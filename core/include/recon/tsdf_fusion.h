// SPDX-License-Identifier: BSD-3-Clause
//
// TSDF 体素网格接口。契约：docs/04_RGBD管线.md §4。

#ifndef RECON_CORE_TSDF_FUSION_H_
#define RECON_CORE_TSDF_FUSION_H_

#include <cstdint>
#include <vector>

#include "recon/linalg.h"

namespace recon {

// TSDF 配置。默认值取自 docs/04 §4 与 08 文档 G6 组。
struct TsdfConfig {
  double voxel_size_m = 0.001;      // 体素边长，米
  double truncation_m = 0.008;      // 截断距离，米
  double min_depth_m = 0.05;        // 融合深度下界，米
  double max_depth_m = 3.0;         // 融合深度上界，米

  // 相机内参（像素）。
  double fx = 1.0;
  double fy = 1.0;
  double cx = 0.0;
  double cy = 0.0;

  // 网格包围盒（米）。
  double origin_x = 0.0;
  double origin_y = 0.0;
  double origin_z = 0.0;
  double bbox_max_x = 1.0;
  double bbox_max_y = 1.0;
  double bbox_max_z = 1.0;

  // 置信度门限（0-255）。
  int min_confidence = 30;
};

// TSDF 体素网格。
class TsdfVolume {
 public:
  explicit TsdfVolume(const TsdfConfig& config);

  /**
   * 融合一帧深度。
   *
   * depth_mm:  uint16 毫米，沿光轴投影（ARCore/ARKit 口径）。
   * confidence: uint8 置信度，可为 NULL。
   * world_to_camera: 4x4 世界→相机变换。
   * depth_uncertainty_m: 该帧深度不确定度，决定融合权重。
   *
   * 返回参与融合的像素数。
   */
  uint32_t IntegrateDepthFrame(const uint16_t* depth_mm,
                               const uint8_t* confidence, int width, int height,
                               const Mat4d& world_to_camera,
                               float depth_uncertainty_m);

  // 提取等值面点（零交叉检测）。
  void ExtractPoints(std::vector<Vec3d>* out_positions,
                     std::vector<Vec3d>* out_normals,
                     std::vector<float>* out_confidence) const;

  // 网格尺寸。
  const int* dims() const { return dims_; }

  // 内存占用。
  void MemoryUsageBytes(uint64_t* out_bytes) const;

  // 坐标变换。
  void WorldToVoxel(float wx, float wy, float wz, int* ix, int* iy,
                    int* iz) const;
  void VoxelToWorld(int ix, int iy, int iz, float* wx, float* wy,
                    float* wz) const;

 private:
  size_t Index(int ix, int iy, int iz) const;
  bool InBounds(int ix, int iy, int iz) const;

  TsdfConfig config_;
  int dims_[3];
  Vec3d origin_;
  float voxel_size_ = 0.001f;

  std::vector<float> tsdf_;
  std::vector<float> weight_;
  std::vector<float> color_;
};

// ---------------------------------------------------------------------------
// 分块 TSDF 体素网格（docs/13 §3.2.1）。
//
// 用途：移动端内存预算下，全局体素网格无法一次驻留内存，必须把包围盒切成
//       边长 `block_voxels` 的立方块，只保留相机视野内的块。
//
// 硬约束（docs/13 §3.2.1）：
//   - 分块**不得**改变 `voxel_size`（黑名单 B-1）与 `truncation`。分块只改变
//     调度与内存组织。
//   - 验收判据：「分块融合与单块融合的结果一致」（docs/14 §5.1 TSDF 行 ③）。
//
// **实现口径（与 docs/13 §3.2.1 的 halo 方案的差异，如实说明）**：
//   docs/13 描述的 halo 方案是「相邻块重叠 1 层体素、重叠区按 TSDF 距离与权重
//   加权平均」。本实现改用**全局索引精确划分**：所有块共享同一全局体素坐标系
//   （原点 = cfg.origin，步长 = voxel_size），块是索引空间的**互不重叠**窗口，
//   每个全局体素**唯一归属**一个块。
//
//   这是在**目标**上更强、在**机制**上更简的选择：
//     - 目标（逐位一致）：精确划分下，同一全局体素在分块与单块的数值天然相同，
//       一致性**平凡成立**，无需依赖「加权平均是否恰好对称」。
//     - 若采用 halo 复制存储，重叠体素被写两份，必须靠加权平均在浮点上**恰好**
//       还原单块结果 —— 这在「每帧权重相同、写入顺序一致」时可达成，但引入
//       一个「平均是否对称」的额外依赖，是更脆弱的设计。
//   halo 语义在本实现中体现为**邻域查询跨块**：`SurfaceGradient` 需要的邻居若
//   落在相邻块，由 `FindBlock` 定位到该块的对应体素。
//
//   注意：本实现**不做块淘汰**（所有被触达的块常驻）。块淘汰是相机视锥相关的
//   调度决策（docs/13 §3.2.1「相机离开块的视锥范围后该块可被淘汰」），属阶段
//   调度层职责，不在本类内。`resident_blocks()` 供调度层观测内存占用。
class TsdfTiledVolume {
 public:
  TsdfTiledVolume(const TsdfConfig& config, int block_voxels);

  // 融合一帧。等价于对全局网格的融合（结果与 TsdfVolume 一致）。
  uint32_t IntegrateDepthFrame(const uint16_t* depth_mm,
                               const uint8_t* confidence, int width, int height,
                               const Mat4d& world_to_camera,
                               float depth_uncertainty_m);

  // 提取表面点。输出顺序按全局体素索引 (iz, iy, ix) 升序，与 TsdfVolume 一致。
  void ExtractPoints(std::vector<Vec3d>* out_positions,
                     std::vector<Vec3d>* out_normals,
                     std::vector<float>* out_confidence) const;

  // 全局网格尺寸与常驻块数。
  const int* dims() const { return dims_; }
  int block_voxels() const { return block_voxels_; }
  size_t resident_blocks() const { return blocks_.size(); }

  // 当前内存占用（只计常驻块的 tsdf+weight）。
  void MemoryUsageBytes(uint64_t* out_bytes) const;

 private:
  struct Block {
    int bx = 0;
    int by = 0;
    int bz = 0;
    int nx = 0;   // 本块在全局索引下的体素数（含 halo）
    int ny = 0;
    int nz = 0;
    int x0 = 0;   // 全局索引下界
    int y0 = 0;
    int z0 = 0;
    std::vector<float> tsdf;
    std::vector<float> weight;
  };

  // 取得（必要时创建）覆盖全局体素 (gx, gy, gz) 的块。
  Block* TouchBlock(int gx, int gy, int gz);
  // 查找覆盖全局体素 (gx, gy, gz) 的**已存在**块；不存在返回 nullptr。
  const Block* FindBlock(int gx, int gy, int gz) const;

  TsdfConfig config_;
  int dims_[3];
  Vec3d origin_;
  float voxel_size_ = 0.001f;
  int block_voxels_ = 64;

  std::vector<Block> blocks_;
};

}  // namespace recon

#endif  // RECON_CORE_TSDF_FUSION_H_
