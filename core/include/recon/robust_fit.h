// SPDX-License-Identifier: BSD-3-Clause
//
// 稳健非线性最小二乘的**共用原语**。
//
// 为什么单独抽一个模块（而不是在 VIO 里就地写一遍）：
//   本项目的三条优化路径都要用同一套东西 ——
//     · VIO / MSCKF（docs/05 §3.7）      —— 视觉零空间 + IMU 残差
//     · SfM 全局 BA（docs/05 §4.6）      —— 重投影残差
//     · 外部标定 / 尺度解算（docs/07）   —— 小组残差
//   项目铁律：**同一逻辑写两遍必然漂移**（见 .workbuddy/memory/MEMORY.md §4）。
//   阻尼线性求解与稳健权重是数值行为最敏感的两块，各写一遍迟早出现
//   「VIO 收敛而 BA 不收敛」这类无法归因的差异。
//
// 这里刻意**不依赖 Ceres / Eigen**：
//   · 去掉 Ceres 后 core 仍须能独立编译（CI 环境无 Ceres，见 CMakeLists）；
//   · VIO 的滑窗规模很小（K×6+6 ≤ 78 维），自实现正定求解完全够用，
//     且移动端不引入额外体积。
//   Ceres 可用时，**大**问题（SfM 全局 BA，状态数千维）仍走 Ceres。

#ifndef RECON_CORE_ROBUST_FIT_H_
#define RECON_CORE_ROBUST_FIT_H_

#include <cmath>
#include <vector>

namespace recon {

// ---------------------------------------------------------------------------
// 阻尼正定方程求解
// ---------------------------------------------------------------------------

// 求解对称方程 (H + λI)·dx = g。
//
// @param h_row_major  H，长度 n²，**行主序**（H(i,j) 在 i*n+j）
// @param g            右端项，长度 n
// @param n            维度
// @param lambda       阻尼（Levenberg 项）。取 0 即纯牛顿步。
// @param dx_out       输出增量，长度 n
// @return false 表示分解失败（矩阵严重不正定 / 退化）
//
// 实现为 LDLᵀ（带小对角保护），不是 Cholesky：
//   LM 的 H 在参数不可观时只是**半**正定（docs/05 §3.6.2 的 λ₃/λ₁ 判据
//   就是在检测这种情况）。Cholesky 遇到零特征值直接失败，
//   而 LDLᵀ 可以靠对角保护继续给出一个「沿不可观方向不更新」的步长 ——
//   这正是我们想要的语义（不可观方向不该被数值噪声推动）。
//
// 失败时 dx_out 不被保证有效。
bool SolveSymmetricDamped(const double* h_row_major, const double* g, int n,
                          double lambda, double* dx_out);

// ---------------------------------------------------------------------------
// 稳健核权重
// ---------------------------------------------------------------------------

// 高斯核权重（对应负对数似然为 r²/(2σ²)）。
//
// 选高斯而非 Huber/Cauchy 作 VIO 的视觉核，理由见 docs/05 §3.4.3：
//   零空间投影后的残差**已经**白化（N_fᵀ Σ_J N_f），量纲统一为 σ_v；
//   此时的最小二乘就是最大似然，再加 Huber 会引入无谓偏差。
//   真正的成片外点（错误三角化）已由 λ₃/λ₁ 与视差判据在构造阶段剔除
//   （BuildNullspaceConstraint 的 Reject 枚举），不需要靠核去兜底。
//
// @param r2    残差平方（**未**白化）
// @param sigma 观测噪声标准差
inline double RobustWeightGaussian(double r2, double sigma) {
  if (!(sigma > 0.0)) return 0.0;
  // 半径 4σ 之外直接置零：避免极端外点把权重的浮点尾数带进 Hessian。
  const double s2 = sigma * sigma;
  if (r2 > 16.0 * s2) return 0.0;
  return std::exp(-r2 / (2.0 * s2));
}

// Huber 权重（作用在平方残差上）。SfM 全局 BA 用（docs/05 §3.6.2 的核选择）。
inline double RobustWeightHuber(double r2, double delta) {
  if (!(delta > 0.0)) return 0.0;
  const double norm = std::sqrt(r2);
  if (norm <= delta) return 1.0;
  return delta / norm;
}

// Cauchy 权重（等价于 docs/05 §3.7.1 的 ρ(x)=log(1+x/δ²) 的导数形式）。
// 保留给「外点成片」的场合（例如不经过零空间构造的直接重投影残差）。
inline double RobustWeightCauchy(double r2, double delta) {
  if (!(delta > 0.0)) return 0.0;
  const double d2 = delta * delta;
  return 1.0 / (1.0 + r2 / d2);
}

}  // namespace recon

#endif  // RECON_CORE_ROBUST_FIT_H_
