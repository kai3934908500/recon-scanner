// SPDX-License-Identifier: BSD-3-Clause
//
// MVS 接口。契约：docs/05_单目管线.md §5。
//
// MVS 是曲面精度的真正来源（docs/00 §8.8、docs/05 §5.1）：
//   深度误差 δZ = Z²·δd / (f·B)
// 手机典型参数下（f=2500px, B=100mm, Z=0.35m, δd=0.15px）→ δZ ≈ 0.074 mm，
// 优于 ToF 深度的 2–8 mm 与 ARKit sceneDepth 的厘米级。
//
// 因此本模块的每一处设计都围绕「压低 δd」与「消除系统误差」。

#ifndef RECON_CORE_MVS_H_
#define RECON_CORE_MVS_H_

#include <cstdint>
#include <vector>

#include "recon/camera.h"
#include "recon/linalg.h"

namespace recon {

// MVS 配置。默认值取自 docs/05 §5.2–§5.4 与 08 文档 G5 组。
struct MvsConfig {
  // 视图选择
  int num_views = 7;                    // 源视图数（标档 7 / 高 8 / 快 5）
  int ref_view_index = -1;              // 参考帧索引，-1 表示按顺序取
  double ideal_angle_min_deg = 8.0;     // 理想视角夹角下界
  double ideal_angle_max_deg = 35.0;    // 理想视角夹角上界
  double max_view_scale_mismatch = 0.15;  // 尺度变化硬约束 |s_i/s_r − 1| ≤ 0.15
  double view_angle_weight = 1.0;
  double view_texture_weight = 0.8;
  double view_spread_weight = 0.6;
  double view_scale_weight = 0.4;
  double reference_depth_m = 0.40;      // 尺度约束评估用的参考深度

  // PatchMatch
  int patch_size = 11;                  // NCC 窗口边长
  int iterations = 4;                   // 外迭代轮数
  int random_search_count = 5;         // 随机搜索次数
  // 随机搜索初始半径，单位为**逆深度的相对比例**。
  // 0.30 表示在当前深度 ±30% 范围内搜索，随迭代指数收缩。
  double random_search_radius_ratio = 0.30;
  double restart_ratio = 0.05;          // 随机重启比例
  int propagation_directions = 4;       // 传播方向数
  bool propagation_enabled = true;
  int num_threads = 0;                // 并行线程数，0=自动（按硬件并发）

  // 代价
  double photo_threshold = 1.0;         // 1 − NCC 阈值（接受条件 NCC ≥ 0）
  double depth_regularization = 0.02;   // 平滑正则权重
  double geo_consistency_threshold = 1.2;  // 相对深度偏差百分数（%）
  double weak_texture_variance = 12.0;  // 参考窗口方差下限，低于判为弱纹理

  // 亚像素细化
  int subpixel_iterations = 4;
  int subpixel_fit_points = 5;
  double subpixel_window_ratio = 0.005;  // 细化窗口（逆深度相对比例）

  // 深度范围（米）
  double min_depth_m = 0.10;
  double max_depth_m = 1.50;

  // 融合
  int min_view_consistency = 2;         // 有效支撑视图数下限
  double fusion_voxel_size_m = 0.001;   // 点云融合体素边长
};

// NCC 计算结果。
struct NccResult {
  bool valid = false;
  double ncc = 0.0;
  bool weak_texture = false;  // 参考窗口方差过小
  int valid_samples = 0;
};

// 视图选择结果。
struct ViewSelection {
  int ref_index = 0;
  std::vector<int> source_indices;  // 源视图索引
  std::vector<double> weights;      // 与 source_indices 对齐的权重
};

// 深度图。
struct DepthMap {
  int width = 0;
  int height = 0;
  // 沿光轴的深度（米）。无效像素为 NaN。
  std::vector<float> depth;
  // 每像素的有效支撑视图数。
  std::vector<uint8_t> support_count;
  // 每像素的置信度 [0,1]。
  std::vector<float> confidence;

  void Allocate(int w, int h) {
    width = w;
    height = h;
    const size_t n = static_cast<size_t>(w) * h;
    depth.assign(n, std::nanf(""));
    support_count.assign(n, 0);
    confidence.assign(n, 0.0f);
  }

  bool IsValid(int x, int y) const {
    if (x < 0 || y < 0 || x >= width || y >= height) return false;
    const float d = depth[static_cast<size_t>(y) * width + x];
    return std::isfinite(d);
  }
};

// 融合后的点云。
struct PointCloud {
  std::vector<Vec3d> positions;
  std::vector<Vec3d> normals;
  std::vector<float> colors;   // RGB，0-255
  std::vector<float> confidences;

  size_t size() const { return positions.size(); }
  void Clear() {
    positions.clear();
    normals.clear();
    colors.clear();
    confidences.clear();
  }
};

/**
 * 视图选择。
 *
 * 对参考帧 ref_index，从其余位姿中按几何评分取 top-N。
 * 尺度变化超限的源视图被硬排除。
 */
std::vector<int> SelectSourceViews(const CameraPose& ref_pose,
                                    const std::vector<CameraPose>& poses,
                                    const CameraIntrinsics& intrinsics,
                                    const MvsConfig& config);

/**
 * 计算 NCC（零均值归一化互相关）。
 *
 * ref/src 为灰度图。窗口越界过多时返回 valid=false。
 * 弱纹理（参考窗口方差过小）时 weak_texture=true，调用方应降权而非丢弃。
 */
NccResult ComputeNcc(const uint8_t* ref_img, int ref_w, int ref_h,
                      const uint8_t* src_img, int src_w, int src_h, double ref_u,
                      double ref_v, double src_u, double src_v, int patch_size);

/**
 * PatchMatch 立体匹配：求参考帧的深度图。
 *
 * **索引契约（必须严格遵守，曾因混淆导致越界崩溃）**：
 *
 * | 数组 | 长度 | 索引基准 | 说明 |
 * |---|---|---|---|
 * | ref_gray | W×H | — | 参考帧 |
 * | src_gray / src_w / src_h | num_views | **相对索引 k** | 只含源视图 |
 * | src_poses / src_intrinsics | 全部关键帧数 | **全局索引 si** | 完整列表 |
 * | source_indices | num_views | 元素是**全局索引** | 指向 src_poses |
 *
 * 即：source_indices[k] 给出第 k 个源视图在 src_poses 中的全局下标。
 * src_gray[k] 与 src_poses[source_indices[k]] 必须指向**同一个视图**。
 *
 * @param ref_gray     参考帧灰度图
 * @param src_gray     源视图灰度图列表（长度 = source_indices.size()）
 * @param src_w/src_h  各源视图的宽高
 * @param ref_pose     参考帧位姿（世界→相机）
 * @param src_poses    全部关键帧的位姿（全局数组）
 * @param ref_intr     参考帧内参
 * @param src_intrinsics 全部关键帧的内参（全局数组）
 * @param source_indices 源视图在全局数组中的下标
 * @param seed_points  可选：SfM 稀疏点播种 (u, v, depth_m)，为空则纯随机初始化
 * @param out_depth    输出深度图
 *
 * 返回值：成功填充的像素数。
 */
uint32_t PatchMatchStereo(const uint8_t* ref_gray, int ref_w, int ref_h,
                           const std::vector<const uint8_t*>& src_gray,
                           const std::vector<int>& src_w,
                           const std::vector<int>& src_h,
                           const CameraPose& ref_pose,
                           const std::vector<CameraPose>& src_poses,
                           const CameraIntrinsics& ref_intr,
                           const std::vector<CameraIntrinsics>& src_intrinsics,
                           const std::vector<int>& source_indices,
                           const MvsConfig& config,
                           const std::vector<std::array<double, 3>>* seed_points,
                           DepthMap* out_depth);

/**
 * 亚像素细化。
 *
 * 三级：抛物线拟合 → 连续局部搜索 → 投影一致性半像素补偿。
 * 精度收益：δd 从 0.5 px（整数）降到 0.05–0.20 px，深度误差改善 4–5 倍。
 */
uint32_t RefineSubpixel(const uint8_t* ref_gray, int ref_w, int ref_h,
                         const std::vector<const uint8_t*>& src_gray,
                         const std::vector<int>& src_w,
                         const std::vector<int>& src_h,
                         const CameraPose& ref_pose,
                         const std::vector<CameraPose>& src_poses,
                         const CameraIntrinsics& ref_intr,
                         const std::vector<CameraIntrinsics>& src_intrinsics,
                         const std::vector<int>& source_indices,
                         const MvsConfig& config, DepthMap* depth);

/**
 * 多视图融合：把深度图转为点云。
 *
 * 随机误差按 1/√N_eff 衰减；系统误差不衰减（docs/05 §5.1.3）。
 */
PointCloud FuseDepthMaps(const std::vector<const DepthMap*>& depth_maps,
                          const std::vector<CameraPose>& poses,
                          const std::vector<CameraIntrinsics>& intrinsics,
                          const MvsConfig& config);

}  // namespace recon

#endif  // RECON_CORE_MVS_H_
