// SPDX-License-Identifier: BSD-3-Clause
//
// 稳健非线性最小二乘的共用原语（实现）。契约见 robust_fit.h。

#include "recon/robust_fit.h"

#include <algorithm>

namespace recon {

bool SolveSymmetricDamped(const double* h_row_major, const double* g, int n,
                          double lambda, double* dx_out) {
  if (h_row_major == nullptr || g == nullptr || dx_out == nullptr || n <= 0) {
    return false;
  }

  // 就地 LDLᵀ 分解。L 的下三角存回 h 的严格下三角；D 存 diag[]。
  std::vector<double> h(h_row_major, h_row_major + static_cast<size_t>(n) * n);
  std::vector<double> diag(static_cast<size_t>(n), 0.0);

  for (int j = 0; j < n; ++j) {
    // D_j = H_jj − Σ_{k<j} L_jk² D_k
    double d = h[static_cast<size_t>(j) * n + j] + lambda;
    for (int k = 0; k < j; ++k) {
      const double ljk = h[static_cast<size_t>(j) * n + k];
      d -= ljk * ljk * diag[static_cast<size_t>(k)];
    }

    // 对角保护：非正（含数值噪声导致的微小负值）时注入一个与当前尺度
    // 相称的正值，而不是直接失败 —— 见头文件里「不可观方向不更新」的说明。
    double scale = std::fabs(h[static_cast<size_t>(j) * n + j]) + lambda;
    if (scale <= 0.0) scale = 1.0;
    if (!(d > scale * 1e-12)) {
      d = scale * 1e-12;
    }
    diag[static_cast<size_t>(j)] = d;

    // L_ij = (H_ij − Σ_{k<j} L_ik L_jk D_k) / D_j   for i > j
    for (int i = j + 1; i < n; ++i) {
      double s = h[static_cast<size_t>(i) * n + j];
      for (int k = 0; k < j; ++k) {
        s -= h[static_cast<size_t>(i) * n + k] * h[static_cast<size_t>(j) * n + k] *
             diag[static_cast<size_t>(k)];
      }
      h[static_cast<size_t>(i) * n + j] = s / d;
    }
  }

  // 前代：L·y = g
  std::vector<double> y(static_cast<size_t>(n), 0.0);
  for (int i = 0; i < n; ++i) {
    double s = g[i];
    for (int k = 0; k < i; ++k) {
      s -= h[static_cast<size_t>(i) * n + k] * y[static_cast<size_t>(k)];
    }
    y[static_cast<size_t>(i)] = s;
  }

  // 对角：D·z = y
  std::vector<double> z(static_cast<size_t>(n), 0.0);
  for (int i = 0; i < n; ++i) {
    z[static_cast<size_t>(i)] = y[static_cast<size_t>(i)] / diag[static_cast<size_t>(i)];
  }

  // 回代：Lᵀ·dx = z
  for (int i = n - 1; i >= 0; --i) {
    double s = z[static_cast<size_t>(i)];
    for (int k = i + 1; k < n; ++k) {
      s -= h[static_cast<size_t>(k) * n + i] * dx_out[k];
    }
    dx_out[i] = s;
  }

  for (int i = 0; i < n; ++i) {
    if (!std::isfinite(dx_out[i])) return false;
  }
  return true;
}

}  // namespace recon
