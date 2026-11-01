// SPDX-License-Identifier: BSD-3-Clause
//
// TSDF 融合的**共用内核**。单块（TsdfVolume）与分块（TsdfTiledVolume）两条路径
// 必须复用本文件的实现，不得各写一遍。
//
// 原因（项目铁律「同一逻辑写两遍必然漂移」）：
//   分块 TSDF 的核心验收判据是「分块融合与单块融合的结果**逐位一致**」
//   （docs/14 §5.1 TSDF 行 ③）。若两条路径各写一份体素遍历 / SDF 计算 /
//   零交叉梯度，任何一处细微差别都会让该判据失效，且极难定位（历史上
//   MC 表错位、TSDF 世界→相机公式写错皆属此类）。故把「像素→体素遍历」
//   与「表面梯度提取」抽为本头文件的唯一实现，两条路径只提供存储访问回调。
#ifndef RECON_SRC_RGBD_TSDF_INTERNAL_H_
#define RECON_SRC_RGBD_TSDF_INTERNAL_H_

#include <algorithm>
#include <cmath>

#include "recon/linalg.h"
#include "recon/tsdf_fusion.h"

namespace recon {
namespace tsdf_internal {

// 向下取整的整数除法（b > 0）。用于块归属计算，负坐标也正确。
inline int FloorDiv(int a, int b) {
  int q = a / b;
  const int r = a % b;
  if (r != 0 && ((r < 0) != (b < 0))) --q;
  return q;
}

inline int FloorToIntScaled(float a, float b) {
  return static_cast<int>(std::floor(a / b));
}

// 融合一帧深度。
//
// 对每个有效像素，沿视线法向在截断范围内遍历体素，对每个落入截断带的体素调用
// `write(ix, iy, iz, sdf, w_new)`；`in_bounds(ix, iy, iz)` 决定该体素是否属于
// 调用方的存储范围。
//
// 返回参与融合的像素数（与 docs/04 §4 的 `integrated` 语义一致）。
template <typename InBoundsFn, typename WriteFn>
inline uint32_t IntegrateFrame(const TsdfConfig& cfg, const Vec3d& origin,
                               float voxel, const uint16_t* depth_mm,
                               const uint8_t* confidence, int width, int height,
                               const Mat4d& world_to_camera,
                               float depth_uncertainty_m,
                               InBoundsFn&& in_bounds, WriteFn&& write) {
  if (depth_mm == nullptr || width <= 0 || height <= 0) return 0u;
  if (voxel <= 0.0f) return 0u;

  const float trunc = static_cast<float>(cfg.truncation_m);
  if (trunc <= 0.0f) return 0u;

  // world_to_camera 为 4x4：R_wc, t_wc。
  Mat3d R_wc;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) R_wc(i, j) = world_to_camera(i, j);
  }
  const Vec3d t_wc(world_to_camera(0, 3), world_to_camera(1, 3),
                   world_to_camera(2, 3));

  // 相机系 → 世界系：R_cw = R_wc^T，t_cw = −R_cw·t_wc
  const Mat3d R_cw = R_wc.transpose();
  const Vec3d t_cw = -(R_cw * t_wc);

  const double sigma = std::max(1e-4, static_cast<double>(depth_uncertainty_m));
  const float w_new = static_cast<float>(1.0 / (sigma * sigma));

  // 沿法向的往返步数。
  const int steps = static_cast<int>(trunc / voxel) + 1;

  uint32_t integrated = 0;

  for (int v = 0; v < height; ++v) {
    for (int u = 0; u < width; ++u) {
      const size_t pix = static_cast<size_t>(v) * width + u;
      const uint16_t d_raw = depth_mm[pix];
      if (d_raw == 0) continue;  // 无效深度

      // 置信度门限：低置信像素不参与融合（但不删除数据）。
      if (confidence != nullptr && confidence[pix] < cfg.min_confidence) {
        continue;
      }

      const double dx = (static_cast<double>(u) - cfg.cx) / cfg.fx;
      const double dy = (static_cast<double>(v) - cfg.cy) / cfg.fy;

      // 深度图的计量口径是**光轴投影**（ARCore/ARKit 特性，见 docs/04 §2）：
      // 融合全程使用同一口径，否则 TSDF 会有系统偏差。
      const double d_axis_m = static_cast<double>(d_raw) * 1e-3;
      if (d_axis_m < cfg.min_depth_m || d_axis_m > cfg.max_depth_m) continue;

      const Vec3d p_cam(dx * d_axis_m, dy * d_axis_m, d_axis_m);
      const Vec3d p_world = R_cw * p_cam + t_cw;

      // 表面法向沿视线方向（相机指向点）。
      const Vec3d ray_cam = p_cam.normalized();
      const Vec3d normal_world = (R_cw * ray_cam).normalized();

      const int cx_v = FloorToIntScaled(
          static_cast<float>(p_world(0) - origin(0)), voxel);
      const int cy_v = FloorToIntScaled(
          static_cast<float>(p_world(1) - origin(1)), voxel);
      const int cz_v = FloorToIntScaled(
          static_cast<float>(p_world(2) - origin(2)), voxel);

      const float nx_w = static_cast<float>(normal_world(0));
      const float ny_w = static_cast<float>(normal_world(1));
      const float nz_w = static_cast<float>(normal_world(2));

      for (int s = -steps; s <= steps; ++s) {
        const float dist = static_cast<float>(s) * voxel;
        const int ix = cx_v + FloorToIntScaled(dist * nx_w, voxel);
        const int iy = cy_v + FloorToIntScaled(dist * ny_w, voxel);
        const int iz = cz_v + FloorToIntScaled(dist * nz_w, voxel);
        if (!in_bounds(ix, iy, iz)) continue;

        // 体素中心 → 世界系（与 TsdfVolume::VoxelToWorld 同式）。
        const float wx =
            static_cast<float>(origin(0)) + (static_cast<float>(ix) + 0.5f) * voxel;
        const float wy =
            static_cast<float>(origin(1)) + (static_cast<float>(iy) + 0.5f) * voxel;
        const float wz =
            static_cast<float>(origin(2)) + (static_cast<float>(iz) + 0.5f) * voxel;

        // 体素 → 相机系。**必须是 `R_wc · v_w + t_wc`**（与世界→相机约定
        // `R·p + t` 及上面的逆变换严格互逆）。详见 tsdf_fusion.cpp 的历史注释：
        // 曾误写 `R_wc·(v_w − t_wc)`，等价于相机中心被旋转两次（R²C），
        // 导致点云 Z 向塌成薄板。
        const Vec3d v_w(wx, wy, wz);
        const Vec3d v_c = R_wc * v_w + t_wc;
        if (v_c(2) <= 1e-6) continue;

        const int px_i = static_cast<int>(std::lround(
            cfg.fx * v_c(0) / v_c(2) + cfg.cx));
        const int py_i = static_cast<int>(std::lround(
            cfg.fy * v_c(1) / v_c(2) + cfg.cy));
        if (px_i < 0 || px_i >= width || py_i < 0 || py_i >= height) continue;

        // 投影深度：取该像素的**光轴深度**（与源深度同口径）。
        const double proj_depth_m = v_c(2);
        const double diff = d_axis_m - proj_depth_m;
        if (std::fabs(diff) > trunc) continue;

        const float sdf = static_cast<float>(diff / trunc);
        write(ix, iy, iz, sdf, w_new);
      }
      ++integrated;
    }
  }

  return integrated;
}

// 零交叉表面点的梯度判据。
//
// `get(ix, iy, iz, &tsdf, &weight)` 返回该体素是否在调用方的存储范围内。
// 输出累加的外法向分量与模长；返回 false 表示该体素不是表面点。
//
// 外法向 ∝ +∇tsdf（tsdf 的正方向 = 自由空间 = 表面前方）。邻居为正 → 该邻居
// 方向即梯度方向 → **累加** offset（若写 -= 得到的是朝内法向，违反
// docs/06 §1.2「输入点云法向必须全部朝外」）。
template <typename GetFn>
inline bool SurfaceGradient(GetFn&& get, int ix, int iy, int iz, float* out_nx,
                            float* out_ny, float* out_nz, float* out_len) {
  static const int kOffsets[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                     {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};

  float c = 0.0f, cw = 0.0f;
  if (!get(ix, iy, iz, &c, &cw)) return false;
  if (cw <= 0.0f) return false;   // 从未融合
  if (c > 0.0f) return false;     // 只在 tsdf ≤ 0（表面内侧/表面）起算

  float nx = 0.0f, ny = 0.0f, nz = 0.0f;
  bool has_positive = false;
  for (const auto& off : kOffsets) {
    float nt = 0.0f, nw = 0.0f;
    if (!get(ix + off[0], iy + off[1], iz + off[2], &nt, &nw)) continue;
    // **必须同时要求邻居已融合（weight > 0）**：未融合体素的 tsdf 默认值为 +1，
    // 若只判 `tsdf > 0` 会把「从未观测过的空体素」误当自由空间，给表面点法向
    // 注入大量幻影分量。
    if (nw > 0.0f && nt > 0.0f) {
      has_positive = true;
      nx += static_cast<float>(off[0]);
      ny += static_cast<float>(off[1]);
      nz += static_cast<float>(off[2]);
    }
  }
  if (!has_positive) return false;

  const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
  if (len < 1e-9f) return false;

  *out_nx = nx / len;
  *out_ny = ny / len;
  *out_nz = nz / len;
  *out_len = len;
  return true;
}

}  // namespace tsdf_internal
}  // namespace recon

#endif  // RECON_SRC_RGBD_TSDF_INTERNAL_H_
