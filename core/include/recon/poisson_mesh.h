// SPDX-License-Identifier: BSD-3-Clause
//
// 泊松网格化接口。契约：docs/06_网格化与后处理.md §1、§2.3。
//
// 精度链条：
//   MVS 点云（δ ≈ 0.1 mm）→ 泊松重建 → 网格（分辨率 r）
//   最终形状误差主要由 **r** 决定（δ_mesh ≈ 0.5·r，见 §1.3 的
//   「三角形特征边长与单元边长同量级，实测比值 0.8~1.2」）。
//
// 因此 `r` 是黑名单参数，不可因内存压力降级（docs/00 R-PREC-4）。
// 内存不足时的解法是**分块泊松**（§1.7），不是降分辨率。

#ifndef RECON_CORE_POISSON_MESH_H_
#define RECON_CORE_POISSON_MESH_H_

#include <cstdint>
#include <vector>

#include "recon/linalg.h"
#include "recon/mesh_repair.h"

namespace recon {

// 带法向与权重的输入点云（S0 阶段的输出）。
struct OrientedPointCloud {
  std::vector<Vec3d> positions;
  std::vector<Vec3d> normals;   // 朝外
  std::vector<float> colors;    // RGB 0-255，长度 = 3 × 顶点数
  std::vector<float> weights;   // ≥ 0

  size_t size() const { return positions.size(); }
  void Clear() {
    positions.clear();
    normals.clear();
    colors.clear();
    weights.clear();
  }
};

// 估计点云的**采样间距**（最近邻距离中位数，单位 m）。
//
// 这是泊松深度选取的**硬约束**的输入：八叉树的单元边长若远小于点云间距，
// 点云之间就没有场值支撑 → 等值面断裂成碎片。
// 实测（球面 20000 点，间距 1.25 mm）：
//   单元边长 0.215 mm（depth=9）→ 网格只覆盖球面面积的 **7.3%**，边界环 4.2 万
//   单元边长 0.859 mm（depth=7）→ 覆盖 **87.9%**
//   单元边长 1.719 mm（depth=6）→ 覆盖 **107.9%**（含外扩区）
double EstimatePointSpacing(const OrientedPointCloud& cloud);

// 按点云采样间距反推八叉树深度（保守口径，docs/06 §1.3）。
//
// 求满足 `r = s·L_max/2^D ≤ point_spacing` 的**最小** D：
//   D = ceil( log2( s·L_max / point_spacing ) )
//
// **为什么必须是上界约束**：r 超过点云间距时，点云支撑的单元之间会断开，
// 等值面碎成大量孤立片（实测边界环从 0.9 千涨到 4.2 万）。
//
// @param bbox_longest_edge 点云包围盒最长边（m）
// @param scale             mesh_poisson_scale
// @param point_spacing     点云采样间距（m）
int PoissonDepthForPointSpacing(double bbox_longest_edge, double scale,
                                double point_spacing);

// 泊松配置。默认值取自 docs/06 §1.2。
struct PoissonConfig {
  int depth = 10;                 // 八叉树深度（黑名单参数）
  double scale = 1.1;             // 参数化缩放（黑名单参数）
  double samples_per_node = 1.5;  // 每节点采样密度
  bool point_weight = true;       // 启用逐点权重
  double sharpness = 1.5;         // 边角锐度
  int min_samples = 2;            // 低于此采样数的节点不建面
  bool confidence = true;         // 输出逐顶点置信度
  bool prune = true;              // 剪除空分支
  bool interpolate = false;       // 插值采样（会平滑细节）

  // 求解器
  int solver_iterations = 200;    // PCG 最大迭代数
  double solver_tolerance = 1e-6; // PCG 收敛阈值（相对残差）
  int multigrid_levels = 4;       // 多重网格层数

  // 裁剪（§1.6.2）
  double trim_confidence_quantile = 0.999;  // 置信度分位数
  double trim_distance_factor = 2.5;        // 距输入点云 τ = factor × r
  bool trim_enabled = true;
};

// 重建结果。
struct PoissonResult {
  TriangleMesh mesh;
  // 实际生效的分辨率（米）。= scale × L_max / 2^depth。
  double resolution = 0.0;
  // 点云切平面致密化的展开倍数（1 = 未致密化）。
  // 点云间距远大于网格单元边长时，泊松的活跃单元团会互不连通 →
  // 求解域断开 → 等值面碎裂。致密化把它展开到「等效间距 ≤ r」以连通。
  // **不增加几何信息**：形状精度上限仍是原点云的采样密度。
  int densify_factor = 1;
  // 活跃单元数（稀疏化后的求解规模）与格点场值表大小。
  size_t active_cells = 0;
  size_t grid_value_points = 0;
  // MC 提取时「未定义格点」被查询的次数（返回 0.0 的那些）。
  // 非 0 说明求解域没连成片 → 等值面会碎。
  size_t undefined_corner_queries = 0;
  // 场值分布诊断：等值面阈值 iso 与场值的极值/分位数。
  double iso_threshold = 0.0;
  double value_min = 0.0;
  double value_max = 0.0;
  // 8 个角点跨越 iso 的单元数（= 会产生三角形的单元）。
  size_t cells_crossing_iso = 0;
  // 裁剪前后的三角形数（差值 = 被外推区裁剪删掉的量）。
  size_t triangles_before_trim = 0;
  size_t triangles_after_trim = 0;
  // 裁剪判据的诊断量。
  double trim_tau = 0.0;        // 距离阈值 τ
  double trim_q_thresh = 0.0;   // 分位数阈值
  double trim_dist_median = 0.0;// 顶点到点云距离的中位数
  size_t vertices_before_trim = 0;
  size_t vertices_after_trim = 0;
  // 输入点云包围盒最长边（米）。
  double bbox_longest_edge = 0.0;
  // 求解残差（相对），用于诊断。
  double solver_residual = 0.0;
  // 求解耗时（毫秒）。
  int solve_millis = 0;
  // 内存峰值估算（字节）。
  uint64_t peak_memory_bytes = 0;
};

/**
 * 泊松曲面重建。
 *
 * 算法（Screen Poisson 风格）：
 *   1. 八叉树细分：点云所在区域逐层细分，节点记录法向累积。
 *   2. 梯度场构造：V = Σ w_i · n_i · δ_i（Dirac 梯度场的离散化）。
 *   3. 泊松方程求解：∇²f = ∇·V，用多重网格预条件共轭梯度（PCG）。
 *      screened 项：f(p_i) = 0 在输入点处。
 *   4. 等值面提取：在 f = iso 的位置提取三角形（Marching Cubes，
 *      自适应分辨率：只在有信号的节点上提取）。
 *   5. 裁剪：置信度分位数 + 距输入点云距离场。
 *
 * @param cloud 输入点云（法向必须已定向一致）
 * @param config 配置
 * @param out 结果
 * @return true 表示成功
 */
bool PoissonReconstruct(const OrientedPointCloud& cloud, const PoissonConfig& config,
                        PoissonResult* out);

// 计算八叉树深度对应的实际分辨率（§1.3 的唯一权威关系式）。
double PoissonResolution(double bbox_longest_edge, double scale, int depth);

// 从八叉树深度推导所需深度：给定目标分辨率 r_target，求 D。
int PoissonDepthForResolution(double bbox_longest_edge, double scale,
                              double r_target);

}  // namespace recon

#endif  // RECON_CORE_POISSON_MESH_H_
