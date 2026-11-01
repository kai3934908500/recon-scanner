// SPDX-License-Identifier: BSD-3-Clause
//
// S9 纹理烘焙。契约：docs/06_网格化与后处理.md §2.9。
//
// 做什么：把多张关键帧（RGB + 深度 + 位姿 + 内参）投影到 UV 图集上，
// 按视角质量加权融合成一张纹理。
//
// 对每个 texel：
//   1. 由 UV 重心坐标反算出对应的 3D 表面点 x 与法向 n；
//   2. 对每个视图 i：投影 x → 深度一致性测试 → 算权重 w(i)；
//   3. 采样颜色 c_i（双线性）；
//   4. 融合 C = Σ w·c / Σ w（`average` 模式）；
//   5. 未覆盖的 texel 用邻域扩散填充。
//
// **颜色空间**：融合默认在**线性空间**做（`linear_space = true`），
// 输出转回 sRGB。在线性空间平均才符合物理（sRGB 直接平均会让暗部偏亮）。

#ifndef RECON_CORE_TEXTURE_BAKE_H_
#define RECON_CORE_TEXTURE_BAKE_H_

#include <cstdint>
#include <vector>

#include "recon/linalg.h"
#include "recon/uv_unwrap.h"

namespace recon {

struct TriangleMesh;

// 一帧烘焙视图。
//
// `rotation` / `translation` 为 **world → camera**：`p_cam = R · p_world + t`。
struct BakeView {
  const uint8_t* rgb = nullptr;   // RGB8，长度 ≥ height * stride_bytes
  const float* depth = nullptr;   // 深度图（米），长度 ≥ width*height；可为 nullptr
  int width = 0;
  int height = 0;
  // RGB 每行字节数（通常 = width * 3）。
  int stride_bytes = 0;

  Mat3d rotation;                        // world → camera
  Vec3d translation = Vec3d::Zero();     // world → camera
  double fx = 0.0, fy = 0.0, cx = 0.0, cy = 0.0;

  // 运动模糊评分 [0,1]，0 表示清晰。用于降权（`w_blur = 1 - score`）。
  double motion_blur = 0.0;
};

// 烘焙配置。参数名对应 docs/06 §2.9 的 `post_bake_*`。
struct BakeConfig {
  // 输出纹理边长（px），应与 UV 图集尺寸一致。
  int texture_size = 4096;
  // 入射角指数 alpha。越大越偏好正视（快 4 / 标 8 / 高 16）。
  double alpha = 8.0;
  // 深度一致性容差的 σ（× 该处的像素足迹）。快 4 / 标 3 / 高 2。
  //
  // **软权重**：w_res = exp(-0.5·(diff/σ)²)，不是硬阈值。
  // 早期用「diff > tau 就剔除」的一票否决，实测在合成球面上因为
  // 网格与深度图的 3~8 mm 系统性偏差导致覆盖率归零。
  double visibility_tol_mult = 3.0;
  // 残差项的 σ（× 像素足迹）。与 `visibility_tol_mult` 现在等价（软权重只有一个 σ）。
  double tau_vis_mult = 3.0;
  // 目标像素足迹（米/像素）。分辨率比它更粗的视图会被降权。
  // ≤ 0 表示自动取所有视图足迹的中位数。
  double target_footprint = 0.0;
  // 权重和下限，低于此值的 texel 记为「未覆盖」。
  double min_weight_sum = 1e-3;
  // 未覆盖 texel 的扩散填充半径（像素）。0 = 不填充。
  int dilate_pixels = 4;
  // 是否在线性空间融合（推荐 true）。
  bool linear_space = true;
};

// 烘焙结果。
struct BakeResult {
  bool ok = false;
  int width = 0;
  int height = 0;
  // RGB8 纹理（行主序，width*height*3）。
  std::vector<uint8_t> rgb;
  // 每 texel 的权重和（诊断用）。0 = 没有视图看到它。
  std::vector<float> weight_sum;
  // 有权重的 texel 占比（填充前）。
  double covered_ratio = 0.0;
  // 每个已覆盖 texel 的平均有效视图数。
  double mean_views_per_texel = 0.0;
  // 填充后仍为空的 texel 数。
  size_t unfilled_texels = 0;
};

// S9：纹理烘焙。
//
// @param mesh   已展 UV 的几何（顶点位置用 `atlas.source_vertex` 回查原网格）
// @param atlas  S8 的输出
// @param views  关键帧集合
// @param config 配置
BakeResult BakeTexture(const TriangleMesh& mesh, const UvAtlas& atlas,
                       const std::vector<BakeView>& views,
                       const BakeConfig& config);

}  // namespace recon

#endif  // RECON_CORE_TEXTURE_BAKE_H_
