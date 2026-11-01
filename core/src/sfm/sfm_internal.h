// SPDX-License-Identifier: BSD-3-Clause
//
// SfM 内部共享的小矩阵数值内核（零依赖）。
//
// 为什么抽成头文件（而不是各文件各写一份）：
//   本项目铁律 —— **同一逻辑写两遍必然漂移**（.workbuddy/memory/MEMORY.md §4）。
//   `JacobiEigenSymmetric` / `Svd3x3` 被 geometry.cpp（E/F/H 估计）与
//   registration.cpp（PnP、BA 精化）同时需要。曾因「按 evecs[k] 读特征向量」
//   在 4 处调用点全部错位而吃过大亏，故连取列都要用 `EigenColumn` 收口。
//
// 仅内部使用（位于 src/，不导出到 include/recon/）。
// 依赖极简：<algorithm> <cmath> <vector> + recon/linalg.h。

#ifndef RECON_CORE_SFM_SFM_INTERNAL_H_
#define RECON_CORE_SFM_SFM_INTERNAL_H_

#include <algorithm>
#include <cmath>
#include <vector>

#include "recon/linalg.h"

namespace recon {
namespace sfm_internal {

// 对称矩阵的 Jacobi 特征分解。输入 n×n 行主序对称矩阵（a_in[i*n+j]），
// 输出特征值与对应的正交特征向量（按特征值**升序**）。
//
// 输出布局（务必记住）：`(*eigenvectors)[r*n + i]` = 第 i 个特征向量
// 的第 r 个分量。i 是按特征值升序的序号，**不是**连续内存。
// 取第 i 个特征向量请用 EigenColumn()。
//
// 为什么用 Jacobi 而非幂迭代：本项目既要「最小特征向量」（求零空间），
// 也要「全部特征值」（SVD of E，3×3），Jacobi 一次给出全部，避免两套代码。
//
// 调用方注意：若调用点可能被多线程同时进入，本函数是纯函数（无静态状态），
// 线程安全。
inline bool JacobiEigenSymmetric(const std::vector<double>& a_in, int n,
                                 std::vector<double>* eigenvalues,
                                 std::vector<double>* eigenvectors) {
  std::vector<double> a = a_in;
  std::vector<double> v(static_cast<size_t>(n) * n, 0.0);
  for (int i = 0; i < n; ++i) v[static_cast<size_t>(i) * n + i] = 1.0;

  for (int sweep = 0; sweep < 100; ++sweep) {
    double off = 0.0;
    for (int i = 0; i < n; ++i) {
      for (int j = i + 1; j < n; ++j) {
        off += a[static_cast<size_t>(i) * n + j] * a[static_cast<size_t>(i) * n + j];
      }
    }
    if (off < 1e-300) break;

    for (int p = 0; p < n; ++p) {
      for (int q = p + 1; q < n; ++q) {
        const double apq = a[static_cast<size_t>(p) * n + q];
        if (std::fabs(apq) < 1e-300) continue;
        const double app = a[static_cast<size_t>(p) * n + p];
        const double aqq = a[static_cast<size_t>(q) * n + q];
        const double theta = (aqq - app) / (2.0 * apq);
        const double t = (theta >= 0.0 ? 1.0 : -1.0) /
                         (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0);
        const double s = t * c;

        for (int k = 0; k < n; ++k) {
          const double akp = a[static_cast<size_t>(k) * n + p];
          const double akq = a[static_cast<size_t>(k) * n + q];
          a[static_cast<size_t>(k) * n + p] = c * akp - s * akq;
          a[static_cast<size_t>(k) * n + q] = s * akp + c * akq;
        }
        for (int k = 0; k < n; ++k) {
          const double apk = a[static_cast<size_t>(p) * n + k];
          const double aqk = a[static_cast<size_t>(q) * n + k];
          a[static_cast<size_t>(p) * n + k] = c * apk - s * aqk;
          a[static_cast<size_t>(q) * n + k] = s * apk + c * aqk;
        }
        for (int k = 0; k < n; ++k) {
          const double vkp = v[static_cast<size_t>(k) * n + p];
          const double vkq = v[static_cast<size_t>(k) * n + q];
          v[static_cast<size_t>(k) * n + p] = c * vkp - s * vkq;
          v[static_cast<size_t>(k) * n + q] = s * vkp + c * vkq;
        }
      }
    }
  }

  std::vector<int> idx(n);
  for (int i = 0; i < n; ++i) idx[i] = i;
  std::sort(idx.begin(), idx.end(), [&](int i, int j) {
    return a[static_cast<size_t>(i) * n + i] < a[static_cast<size_t>(j) * n + j];
  });

  eigenvalues->assign(n, 0.0);
  eigenvectors->assign(static_cast<size_t>(n) * n, 0.0);
  for (int i = 0; i < n; ++i) {
    (*eigenvalues)[i] = a[static_cast<size_t>(idx[i]) * n + idx[i]];
    for (int r = 0; r < n; ++r) {
      (*eigenvectors)[static_cast<size_t>(r) * n + i] =
          v[static_cast<size_t>(r) * n + idx[i]];
    }
  }
  return true;
}

// 取 JacobiEigenSymmetric 输出的「第 i 个特征值对应的特征向量」。
//
// 不要直接写 evecs[k] —— 布局是 evecs[r*n+i]，曾有 4 处调用点据此读错。
inline std::vector<double> EigenColumn(const std::vector<double>& evecs, int n,
                                       int i) {
  std::vector<double> col(static_cast<size_t>(n), 0.0);
  for (int r = 0; r < n; ++r) {
    col[static_cast<size_t>(r)] = evecs[static_cast<size_t>(r) * n + i];
  }
  return col;
}

// 3×3 矩阵的 SVD：A = U·Σ·Vᵀ。返回 U(3×3)、s(3, **降序**)、V(3×3)。
// 用 AᵀA 的 Jacobi 特征分解得到 V 与 σ²，再由 U = A·V/σ 补齐。
inline bool Svd3x3(const Mat3d& a, Mat3d* u_out, Vec3d* s_out, Mat3d* v_out) {
  std::vector<double> ata(9, 0.0);
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      double sv = 0.0;
      for (int k = 0; k < 3; ++k) sv += a(k, i) * a(k, j);
      ata[static_cast<size_t>(i) * 3 + j] = sv;
    }
  }
  std::vector<double> evals;
  std::vector<double> evecs;
  JacobiEigenSymmetric(ata, 3, &evals, &evecs);

  // 降序：Jacobi 升序，故反转。
  Vec3d s;
  Mat3d v;
  for (int i = 0; i < 3; ++i) {
    const int src = 2 - i;
    s(i) = std::sqrt(std::max(0.0, evals[src]));
    for (int r = 0; r < 3; ++r) {
      v(r, i) = evecs[static_cast<size_t>(r) * 3 + src];
    }
  }

  // U 的列：u_i = A·v_i / σ_i；σ_i ≈ 0 时用与已定列正交的任意单位向量。
  Mat3d u;
  for (int i = 0; i < 3; ++i) {
    Vec3d vi(v(0, i), v(1, i), v(2, i));
    Vec3d ui = a * vi;
    if (s(i) > 1e-12) {
      ui = ui / s(i);
    } else {
      // 退化列：取标准基中与前面列最不正交者再正交化。
      Vec3d cand(0.0, 0.0, 0.0);
      double best = -1.0;
      for (int k = 0; k < 3; ++k) {
        Vec3d e(0.0, 0.0, 0.0);
        e(k) = 1.0;
        for (int j = 0; j < i; ++j) {
          Vec3d uj(u(0, j), u(1, j), u(2, j));
          e = e - uj * e.dot(uj);
        }
        const double nn = e.norm();
        if (nn > best) { best = nn; cand = e; }
      }
      ui = cand.norm() > 1e-12 ? cand.normalized() : Vec3d(1.0, 0.0, 0.0);
    }
    u(0, i) = ui(0);
    u(1, i) = ui(1);
    u(2, i) = ui(2);
  }

  *u_out = u;
  *s_out = s;
  *v_out = v;
  return true;
}

}  // namespace sfm_internal
}  // namespace recon

#endif  // RECON_CORE_SFM_SFM_INTERNAL_H_
