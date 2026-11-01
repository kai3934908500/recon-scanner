// SPDX-License-Identifier: BSD-3-Clause
//
// 轻量线性代数实现。见 core/include/recon/linalg.h 的说明。

#include "recon/linalg.h"

#include <cmath>

namespace recon {

double Determinant3x3(const Mat3d& m) {
  return m(0, 0) * (m(1, 1) * m(2, 2) - m(1, 2) * m(2, 1)) -
         m(0, 1) * (m(1, 0) * m(2, 2) - m(1, 2) * m(2, 0)) +
         m(0, 2) * (m(1, 0) * m(2, 1) - m(1, 1) * m(2, 0));
}

bool Inverse3x3(const Mat3d& m, Mat3d* out) {
  if (out == nullptr) return false;

  const double det = Determinant3x3(m);
  // 奇异判定阈值。相对量级：与矩阵元素量级相关。
  if (std::abs(det) < 1e-15) return false;

  const double inv_det = 1.0 / det;

  // 伴随矩阵（转置的余子式矩阵）。
  Mat3d inv;
  inv(0, 0) = (m(1, 1) * m(2, 2) - m(1, 2) * m(2, 1)) * inv_det;
  inv(0, 1) = (m(0, 2) * m(2, 1) - m(0, 1) * m(2, 2)) * inv_det;
  inv(0, 2) = (m(0, 1) * m(1, 2) - m(0, 2) * m(1, 1)) * inv_det;
  inv(1, 0) = (m(1, 2) * m(2, 0) - m(1, 0) * m(2, 2)) * inv_det;
  inv(1, 1) = (m(0, 0) * m(2, 2) - m(0, 2) * m(2, 0)) * inv_det;
  inv(1, 2) = (m(0, 2) * m(1, 0) - m(0, 0) * m(1, 2)) * inv_det;
  inv(2, 0) = (m(1, 0) * m(2, 1) - m(1, 1) * m(2, 0)) * inv_det;
  inv(2, 1) = (m(0, 1) * m(2, 0) - m(0, 0) * m(2, 1)) * inv_det;
  inv(2, 2) = (m(0, 0) * m(1, 1) - m(0, 1) * m(1, 0)) * inv_det;

  *out = inv;
  return true;
}

}  // namespace recon
