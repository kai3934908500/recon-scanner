// SPDX-License-Identifier: BSD-3-Clause
//
// SfM 几何原语：E/F/H 估计 + RANSAC + 位姿恢复 + 多视图三角化。
// 契约：docs/05_单目管线.md §4.3.3、§4.3.4、§4.4.2。
//
// 零依赖：本文件不引用 Eigen / OpenCV。所需的数值内核（对称特征分解）
// 在文件内实现，原因见 sfm.h 文件头。
//
// 踩坑记录（写在这里防止回退）：
//   1. **8 点法必须先在归一化坐标上做 Hartley 归一化**（质心移到原点、
//      均方根距离归到 √2）。不做归一化时，像素量级（~1000）会让 AᵀA 的
//      条件数飙到 1e16 以上，特征向量完全被浮点噪声主导，解出的 F 无意义。
//      归一化坐标天然量级 ~1，但为稳健起见仍做一次内部归一化。
//   2. **E 分解后必须用 cheirality 选解**。E 分解给出 4 组 (R,t)，其中
//      只有一组让全部点在两个相机前方。不选解会随机拿到镜像解。
//   3. **三角化的投影矩阵用完整 R|t**，不是「各自的 bearing」。
//      见 msckf.cpp 文件头结论 3（曾把 bearing 混在同一坐标系，误差 0.95 m）。

#include "recon/sfm.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "recon/robust_fit.h"
#include "sfm_internal.h"

namespace recon {
namespace {

// 数值内核（Jacobi 特征分解 / SVD / 取列）已抽到 sfm_internal.h，供
// geometry.cpp 与 registration.cpp 共用（铁律：同一逻辑写两遍必然漂移）。
using sfm_internal::EigenColumn;
using sfm_internal::JacobiEigenSymmetric;
using sfm_internal::Svd3x3;

// ---------------------------------------------------------------------------
// Hartley 归一化
// ---------------------------------------------------------------------------
struct Normalizer {
  Mat3d t = Mat3d::Identity();  // 作用于原始坐标：x_norm = T·x

  void Build(const std::vector<Vec2d>& pts) {
    const size_t n = pts.size();
    double mx = 0.0, my = 0.0;
    for (const Vec2d& p : pts) { mx += p(0); my += p(1); }
    if (n == 0) return;
    mx /= static_cast<double>(n);
    my /= static_cast<double>(n);
    double mean_d = 0.0;
    for (const Vec2d& p : pts) {
      const double dx = p(0) - mx, dy = p(1) - my;
      mean_d += std::sqrt(dx * dx + dy * dy);
    }
    mean_d /= static_cast<double>(n);
    const double scale = mean_d > 1e-12 ? std::sqrt(2.0) / mean_d : 1.0;
    t = Mat3d::Identity();
    t(0, 0) = scale; t(0, 2) = -scale * mx;
    t(1, 1) = scale; t(1, 2) = -scale * my;
  }

  Vec2d Apply(const Vec2d& p) const {
    return Vec2d(t(0, 0) * p(0) + t(0, 1) * p(1) + t(0, 2),
                 t(1, 0) * p(0) + t(1, 1) * p(1) + t(1, 2));
  }
};

// 把 3×3 矩阵投影到「奇异值 (s, s, 0)」流形（F 的秩 2 约束 / E 的 (1,1,0)）。
void EnforceRankTwo(Mat3d* m, bool equal_singular) {
  Mat3d u, v;
  Vec3d s;
  Svd3x3(*m, &u, &s, &v);
  s(2) = 0.0;
  if (equal_singular && s(0) > 0.0) {
    const double avg = 0.5 * (s(0) + s(1));
    s(0) = avg;
    s(1) = avg;
  }
  Mat3d sm = Mat3d::Zero();
  sm(0, 0) = s(0); sm(1, 1) = s(1);
  *m = u * sm * v.transpose();
}

// ---------------------------------------------------------------------------
// 8 点法（归一化坐标）
// ---------------------------------------------------------------------------
// 解 x1ᵀ·F·x0 = 0。堆叠 8 行以上，取 AᵀA 最小特征向量（9 维）。
//
// **系数顺序**（展开 x1ᵀ·F·x0 逐项，f 按行主序 f00,f01,f02,f10,f11,f12,
// f20,f21,f22）：
//   行 = [x0x·x1x, x0y·x1x, x1x, x0x·x1y, x0y·x1y, x1y, x0x, x0y, 1]
// 曾把 f01/f10 两项写反（写成 [.., x0x·x1y, x0x, ..]），导致 AᵀA 的零空间
// 完全错位、RANSAC 内点恒为 0。测试当场抓到。
bool EightPoint(const std::vector<Vec2d>& n0, const std::vector<Vec2d>& n1,
                Mat3d* out_f) {
  const size_t m = n0.size();
  if (m < 8) return false;

  std::vector<double> ata(81, 0.0);
  for (size_t k = 0; k < m; ++k) {
    const double x0 = n0[k](0), y0 = n0[k](1);
    const double x1 = n1[k](0), y1 = n1[k](1);
    const double row[9] = {x0 * x1, y0 * x1, x1, x0 * y1, y0 * y1,
                           y1,      x0,      y0, 1.0};
    for (int i = 0; i < 9; ++i) {
      for (int j = 0; j < 9; ++j) {
        ata[static_cast<size_t>(i) * 9 + j] += row[i] * row[j];
      }
    }
  }

  std::vector<double> evals, evecs;
  JacobiEigenSymmetric(ata, 9, &evals, &evecs);
  // 最小特征值对应特征向量：第 0 列 → 分量 k 在 evecs[k*9 + 0]。
  Mat3d f;
  for (int k = 0; k < 9; ++k) {
    f(k / 3, k % 3) = evecs[static_cast<size_t>(k) * 9];
  }
  EnforceRankTwo(&f, false);
  *out_f = f;
  return true;
}

// 便于测试与调试的确定性 RNG。
class Rng {
 public:
  explicit Rng(uint64_t seed) : s_(seed ? seed : 0x9e3779b97f4a7c15ull) {}
  uint64_t Next() {
    s_ ^= s_ << 13; s_ ^= s_ >> 7; s_ ^= s_ << 17;
    return s_;
  }
  int Below(int n) { return n > 0 ? static_cast<int>(Next() % static_cast<uint64_t>(n)) : 0; }
 private:
  uint64_t s_;
};

// 从 n 个元素里随机取 k 个不同下标（k 很小，用 rejection）。
void SampleDistinct(Rng* rng, int n, int k, std::vector<int>* out) {
  out->clear();
  if (k >= n) {
    for (int i = 0; i < n; ++i) out->push_back(i);
    return;
  }
  while (static_cast<int>(out->size()) < k) {
    const int c = rng->Below(n);
    bool dup = false;
    for (int v : *out) { if (v == c) { dup = true; break; } }
    if (!dup) out->push_back(c);
  }
}

// 通用 RANSAC 框架：按模型最小样本数重复采样 → 估模型 → 数内点。
// estimator(points_idx, &model) 返回 false 表示该样本退化。
template <typename EstFn, typename DistFn>
RansacResult RansacGeneric(int num_points, int min_sample, int max_iters,
                           double confidence, double threshold,
                           Rng* rng, EstFn estimator, DistFn distance,
                           Mat3d* out_model) {
  RansacResult res;
  res.inlier_mask.assign(static_cast<size_t>(num_points), 0);
  if (num_points < min_sample) return res;

  int best_inliers = 0;
  Mat3d best_model;
  bool has_best = false;

  int iters = max_iters;
  for (int it = 0; it < iters; ++it) {
    std::vector<int> sample;
    SampleDistinct(rng, num_points, min_sample, &sample);
    Mat3d model;
    if (!estimator(sample, &model)) continue;

    int inliers = 0;
    for (int i = 0; i < num_points; ++i) {
      if (distance(model, i) <= threshold) ++inliers;
    }
    if (inliers > best_inliers) {
      best_inliers = inliers;
      best_model = model;
      has_best = true;
      // 自适应迭代上限（§4.3.3 的提前终止置信度）。
      const double w = static_cast<double>(inliers) / static_cast<double>(num_points);
      if (w > 0.0 && w < 1.0) {
        const double p_no_outlier = std::pow(1.0 - w, min_sample);
        const double denom = std::log(p_no_outlier < 1e-300 ? 1e-300 : p_no_outlier);
        if (denom < 0.0) {
          const double need = std::log(1.0 - confidence) / denom;
          if (need > 0.0 && need < static_cast<double>(iters)) {
            iters = std::max(it + 1, static_cast<int>(need) + 1);
          }
        }
      }
    }
    res.iterations_used = it + 1;
  }

  if (!has_best || best_inliers < min_sample) return res;
  for (int i = 0; i < num_points; ++i) {
    if (distance(best_model, i) <= threshold) {
      res.inlier_mask[static_cast<size_t>(i)] = 1;
    }
  }
  res.inliers = best_inliers;
  res.ok = true;
  if (out_model != nullptr) *out_model = best_model;
  return res;
}

}  // namespace

// ---------------------------------------------------------------------------
// 公开：距离度量
// ---------------------------------------------------------------------------

double SampsonDistanceNormalized(const Mat3d& f, const Vec2d& x0,
                                 const Vec2d& x1, const CameraIntrinsics& k) {
  const double x0x = x0(0), x0y = x0(1);
  const double x1x = x1(0), x1y = x1(1);
  const double fx = f(0, 0) * x0x + f(0, 1) * x0y + f(0, 2);
  const double fy = f(1, 0) * x0x + f(1, 1) * x0y + f(1, 2);
  const double fz = f(2, 0) * x0x + f(2, 1) * x0y + f(2, 2);
  const double ex = f(0, 0) * x1x + f(1, 0) * x1y + f(2, 0);
  const double ey = f(0, 1) * x1x + f(1, 1) * x1y + f(2, 1);
  const double num = x1x * fx + x1y * fy + fz;
  const double denom = fx * fx + fy * fy + ex * ex + ey * ey;
  if (denom < 1e-300) return 1e30;
  // 归一化平面距离 → 像素距离：乘平均焦距。
  const double fpx = 0.5 * (k.fx + k.fy);
  return fpx * std::fabs(num) / std::sqrt(denom);
}

double SymmetricTransferDistance(const Mat3d& h, const Vec2d& x0,
                                 const Vec2d& x1, const CameraIntrinsics& k) {
  // 用像素坐标计算对称转移（H 由像素坐标解出）。
  const double u0 = k.fx * x0(0) + k.cx;
  const double v0 = k.fy * x0(1) + k.cy;
  const double u1 = k.fx * x1(0) + k.cx;
  const double v1 = k.fy * x1(1) + k.cy;

  const double px = h(0, 0) * u0 + h(0, 1) * v0 + h(0, 2);
  const double py = h(1, 0) * u0 + h(1, 1) * v0 + h(1, 2);
  const double pz = h(2, 0) * u0 + h(2, 1) * v0 + h(2, 2);
  double d = 1e30;
  if (std::fabs(pz) > 1e-12) {
    const double du = u1 - px / pz;
    const double dv = v1 - py / pz;
    d = std::min(d, std::sqrt(du * du + dv * dv));
  }

  Mat3d hi;
  if (Inverse3x3(h, &hi)) {
    const double qx = hi(0, 0) * u1 + hi(0, 1) * v1 + hi(0, 2);
    const double qy = hi(1, 0) * u1 + hi(1, 1) * v1 + hi(1, 2);
    const double qz = hi(2, 0) * u1 + hi(2, 1) * v1 + hi(2, 2);
    if (std::fabs(qz) > 1e-12) {
      const double du = u0 - qx / qz;
      const double dv = v0 - qy / qz;
      d = std::min(d, std::sqrt(du * du + dv * dv));
    }
  }
  return d;
}

bool ReprojectionErrorPx(const CameraPose& pose, const CameraIntrinsics& k,
                         const Vec3d& point_world, const Vec2d& obs,
                         double* out_err_px) {
  const Vec3d p_cam = pose.WorldToCamera(point_world);
  double u = 0.0, v = 0.0;
  if (!k.ProjectPoint(p_cam, &u, &v)) return false;
  const double du = u - obs(0);
  const double dv = v - obs(1);
  *out_err_px = std::sqrt(du * du + dv * dv);
  return true;
}

// ---------------------------------------------------------------------------
// 公开：E/F/H 估计
// ---------------------------------------------------------------------------

RansacResult EstimateFundamentalMatrix(const std::vector<Vec2d>& pts0,
                                        const std::vector<Vec2d>& pts1,
                                        const CameraIntrinsics& intr,
                                        const SfmConfig& config, Mat3d* out_f) {
  RansacResult res;
  const size_t n = std::min(pts0.size(), pts1.size());
  if (n < 8) return res;

  std::vector<Vec2d> p0(pts0.begin(), pts0.begin() + static_cast<long>(n));
  std::vector<Vec2d> p1(pts1.begin(), pts1.begin() + static_cast<long>(n));

  // Hartley 归一化（对像素坐标；因为调用方传的是归一化坐标，量级已 ~1，
  // 但仍统一处理，保证数值稳健）。
  Normalizer nz0, nz1;
  nz0.Build(p0);
  nz1.Build(p1);
  std::vector<Vec2d> q0(n), q1(n);
  for (size_t i = 0; i < n; ++i) { q0[i] = nz0.Apply(p0[i]); q1[i] = nz1.Apply(p1[i]); }

  // 归一化只在 RANSAC 的 estimator 内做（每个样本 8 点）。
  // 这里不先解一次全点 F —— 全点解会被外点污染，且 RANSAC 会重解。
  Rng rng(0x5f4dcc3b5aa765d1ull);
  const double thresh = config.f_threshold_px;
  auto estimator = [&](const std::vector<int>& idx, Mat3d* model) -> bool {
    std::vector<Vec2d> s0, s1;
    s0.reserve(idx.size());
    s1.reserve(idx.size());
    for (int i : idx) { s0.push_back(q0[static_cast<size_t>(i)]); s1.push_back(q1[static_cast<size_t>(i)]); }
    Mat3d fm;
    if (!EightPoint(s0, s1, &fm)) return false;
    *model = nz1.t.transpose() * fm * nz0.t;
    return true;
  };
  auto distance = [&](const Mat3d& model, int i) -> double {
    return SampsonDistanceNormalized(model, p0[static_cast<size_t>(i)],
                                     p1[static_cast<size_t>(i)], intr);
  };

  Mat3d f_est;
  RansacResult r = RansacGeneric(static_cast<int>(n), 8,
                                 config.ransac_max_iterations,
                                 config.ransac_confidence, thresh, &rng,
                                 estimator, distance, &f_est);
  if (r.ok) {
    // 用全部内点在**归一化坐标**上重解一次 F，再反归一化（提高数值稳定）。
    std::vector<Vec2d> i0, i1;
    for (size_t i = 0; i < n; ++i) {
      if (r.inlier_mask[i]) { i0.push_back(q0[i]); i1.push_back(q1[i]); }
    }
    Mat3d f = f_est;
    if (i0.size() >= 8 && EightPoint(i0, i1, &f)) {
      EnforceRankTwo(&f, false);
      f = nz1.t.transpose() * f * nz0.t;
    }
    *out_f = f;
    res = r;
  }
  return res;
}

RansacResult EstimateEssentialMatrix(const std::vector<Vec2d>& pts0_norm,
                                      const std::vector<Vec2d>& pts1_norm,
                                      const CameraIntrinsics& intr,
                                      const SfmConfig& config, Mat3d* out_e) {
  RansacResult res;
  const size_t n = std::min(pts0_norm.size(), pts1_norm.size());
  if (n < 8) return res;

  std::vector<Vec2d> p0(pts0_norm.begin(), pts0_norm.begin() + static_cast<long>(n));
  std::vector<Vec2d> p1(pts1_norm.begin(), pts1_norm.begin() + static_cast<long>(n));

  Rng rng(0x2545f4914f6cdd1dull);
  const double thresh = config.e_threshold_px;
  auto estimator = [&](const std::vector<int>& idx, Mat3d* model) -> bool {
    std::vector<Vec2d> s0, s1;
    s0.reserve(idx.size());
    s1.reserve(idx.size());
    for (int i : idx) { s0.push_back(p0[static_cast<size_t>(i)]); s1.push_back(p1[static_cast<size_t>(i)]); }
    Mat3d em;
    if (!EightPoint(s0, s1, &em)) return false;
    // E 的奇异值必须为 (s, s, 0)。
    EnforceRankTwo(&em, true);
    *model = em;
    return true;
  };
  auto distance = [&](const Mat3d& model, int i) -> double {
    return SampsonDistanceNormalized(model, p0[static_cast<size_t>(i)],
                                     p1[static_cast<size_t>(i)], intr);
  };

  Mat3d e_est;
  RansacResult r = RansacGeneric(static_cast<int>(n), 8,
                                 config.ransac_max_iterations,
                                 config.ransac_confidence, thresh, &rng,
                                 estimator, distance, &e_est);
  if (r.ok) {
    // 用全部内点做一次最小二乘精化，再投影到 E 流形。
    std::vector<Vec2d> i0, i1;
    for (size_t i = 0; i < n; ++i) {
      if (r.inlier_mask[i]) { i0.push_back(p0[i]); i1.push_back(p1[i]); }
    }
    Mat3d e = e_est;
    if (i0.size() >= 8 && EightPoint(i0, i1, &e)) {
      EnforceRankTwo(&e, true);
    }
    *out_e = e;
    res = r;
  }
  return res;
}

RansacResult EstimateHomography(const std::vector<Vec2d>& pts0,
                                const std::vector<Vec2d>& pts1,
                                const SfmConfig& config, Mat3d* out_h) {
  RansacResult res;
  const size_t n = std::min(pts0.size(), pts1.size());
  if (n < 4) return res;

  // 像素坐标下的 DLT（H 用像素坐标解）。
  auto solve_h = [&](const std::vector<Vec2d>& a, const std::vector<Vec2d>& b,
                     Mat3d* h) -> bool {
    std::vector<double> ata(81, 0.0);
    for (size_t k = 0; k < a.size(); ++k) {
      const double x = a[k](0), y = a[k](1);
      const double X = b[k](0), Y = b[k](1);
      const double r1[9] = {x, y, 1.0, 0.0, 0.0, 0.0, -X * x, -X * y, -X};
      const double r2[9] = {0.0, 0.0, 0.0, x, y, 1.0, -Y * x, -Y * y, -Y};
      for (int i = 0; i < 9; ++i) {
        for (int j = 0; j < 9; ++j) {
          ata[static_cast<size_t>(i) * 9 + j] += r1[i] * r1[j] + r2[i] * r2[j];
        }
      }
    }
    std::vector<double> evals, evecs;
    JacobiEigenSymmetric(ata, 9, &evals, &evecs);
    const std::vector<double> col = EigenColumn(evecs, 9, 0);
    for (int i = 0; i < 9; ++i) (*h)(i / 3, i % 3) = col[static_cast<size_t>(i)];
    return true;
  };

  // 输入为像素坐标（调用方约定，与 §4.3.3 H 的像素口径一致）。
  std::vector<Vec2d> pa = pts0;
  std::vector<Vec2d> pb = pts1;

  Rng rng(0x853c49e6748fea9bull);
  auto estimator = [&](const std::vector<int>& idx, Mat3d* model) -> bool {
    std::vector<Vec2d> s0, s1;
    for (int i : idx) { s0.push_back(pa[static_cast<size_t>(i)]); s1.push_back(pb[static_cast<size_t>(i)]); }
    return solve_h(s0, s1, model);
  };
  // 距离用像素坐标的对称转移。
  auto distance = [&](const Mat3d& model, int i) -> double {
    const Vec2d& u0 = pa[static_cast<size_t>(i)];
    const Vec2d& u1 = pb[static_cast<size_t>(i)];
    const double px = model(0, 0) * u0(0) + model(0, 1) * u0(1) + model(0, 2);
    const double py = model(1, 0) * u0(0) + model(1, 1) * u0(1) + model(1, 2);
    const double pz = model(2, 0) * u0(0) + model(2, 1) * u0(1) + model(2, 2);
    double d = 1e30;
    if (std::fabs(pz) > 1e-12) {
      const double du = u1(0) - px / pz, dv = u1(1) - py / pz;
      d = std::min(d, std::sqrt(du * du + dv * dv));
    }
    Mat3d hi;
    if (Inverse3x3(model, &hi)) {
      const double qx = hi(0, 0) * u1(0) + hi(0, 1) * u1(1) + hi(0, 2);
      const double qy = hi(1, 0) * u1(0) + hi(1, 1) * u1(1) + hi(1, 2);
      const double qz = hi(2, 0) * u1(0) + hi(2, 1) * u1(1) + hi(2, 2);
      if (std::fabs(qz) > 1e-12) {
        const double du = u0(0) - qx / qz, dv = u0(1) - qy / qz;
        d = std::min(d, std::sqrt(du * du + dv * dv));
      }
    }
    return d;
  };

  Mat3d h_est;
  RansacResult r = RansacGeneric(static_cast<int>(n), 4,
                                 config.ransac_max_iterations,
                                 config.ransac_confidence, config.h_threshold_px,
                                 &rng, estimator, distance, &h_est);
  if (r.ok) {
    std::vector<Vec2d> i0, i1;
    for (size_t i = 0; i < n; ++i) {
      if (r.inlier_mask[i]) { i0.push_back(pa[i]); i1.push_back(pb[i]); }
    }
    Mat3d h = h_est;
    if (i0.size() >= 4) solve_h(i0, i1, &h);
    *out_h = h;
    res = r;
  }
  return res;
}

// ---------------------------------------------------------------------------
// 公开：从 E 恢复位姿
// ---------------------------------------------------------------------------

bool RecoverPose(const Mat3d& e, const std::vector<Vec2d>& pts0_norm,
                 const std::vector<Vec2d>& pts1_norm, CameraPose* out_pose1,
                 std::vector<uint8_t>* out_inlier_mask) {
  Mat3d u, v;
  Vec3d s;
  Svd3x3(e, &u, &s, &v);

  // 强制 det(U) = det(V) = +1。
  auto fix_det = [](Mat3d* m) {
    if (Determinant3x3(*m) < 0.0) {
      for (int r = 0; r < 3; ++r) (*m)(r, 2) = -(*m)(r, 2);
    }
  };
  fix_det(&u);
  fix_det(&v);

  // W = [[0,-1,0],[1,0,0],[0,0,1]]
  Mat3d w = Mat3d::Identity();
  w(0, 0) = 0.0; w(0, 1) = -1.0;
  w(1, 0) = 1.0; w(1, 1) = 0.0;

  const Mat3d r1 = u * w * v.transpose();
  const Mat3d r2 = u * w.transpose() * v.transpose();
  // 平移的**方向**取 U 的第 3 列（u33 → U(0,2),U(1,2),U(2,2)）。
  // 曾误写成 u(0,2) 单个标量，导致 t 方向完全错误。
  const Vec3d t1(u(0, 2), u(1, 2), u(2, 2));
  const Vec3d t2 = -t1;

  struct Cand { Mat3d r; Vec3d t; };
  const Cand cands[4] = {{r1, t1}, {r1, t2}, {r2, t1}, {r2, t2}};

  // 位姿 0：R=I，t=0（把视图 0 的相机系当世界系）。
  const CameraPose pose0;

  int best = -1;
  int best_positive = -1;
  std::vector<uint8_t> best_mask;
  for (int c = 0; c < 4; ++c) {
    CameraPose p1;
    p1.R = cands[c].r;
    p1.t = cands[c].t;

    int positive = 0;
    std::vector<uint8_t> mask(pts0_norm.size(), 0);
    for (size_t i = 0; i < pts0_norm.size(); ++i) {
      // 用三角化判断正深度（cheirality）。
      double A[4][4] = {};
      const Vec2d o0 = pts0_norm[i], o1 = pts1_norm[i];
      // 归一化坐标下的投影矩阵：P = [R | t]，行取 R 的第 0/2 行等。
      {
        const Mat3d& R = pose0.R; const Vec3d& tt = pose0.t;
        A[0][0] = R(0, 0) - o0(0) * R(2, 0);
        A[0][1] = R(0, 1) - o0(0) * R(2, 1);
        A[0][2] = R(0, 2) - o0(0) * R(2, 2);
        A[0][3] = tt(0) - o0(0) * tt(2);
        A[1][0] = R(1, 0) - o0(1) * R(2, 0);
        A[1][1] = R(1, 1) - o0(1) * R(2, 1);
        A[1][2] = R(1, 2) - o0(1) * R(2, 2);
        A[1][3] = tt(1) - o0(1) * tt(2);
      }
      {
        const Mat3d& R = p1.R; const Vec3d& tt = p1.t;
        A[2][0] = R(0, 0) - o1(0) * R(2, 0);
        A[2][1] = R(0, 1) - o1(0) * R(2, 1);
        A[2][2] = R(0, 2) - o1(0) * R(2, 2);
        A[2][3] = tt(0) - o1(0) * tt(2);
        A[3][0] = R(1, 0) - o1(1) * R(2, 0);
        A[3][1] = R(1, 1) - o1(1) * R(2, 1);
        A[3][2] = R(1, 2) - o1(1) * R(2, 2);
        A[3][3] = tt(1) - o1(1) * tt(2);
      }
      std::vector<double> ata(16, 0.0);
      for (int i2 = 0; i2 < 4; ++i2) {
        for (int j2 = 0; j2 < 4; ++j2) {
          for (int k2 = 0; k2 < 4; ++k2) {
            ata[static_cast<size_t>(i2) * 4 + j2] += A[k2][i2] * A[k2][j2];
          }
        }
      }
      std::vector<double> evals, evecs;
      JacobiEigenSymmetric(ata, 4, &evals, &evecs);
      // 最小特征值对应特征向量：第 0 列 → 分量 k 在 evecs[k*4 + 0]。
      const double wgt = evecs[3 * 4 + 0];
      if (std::fabs(wgt) < 1e-12) continue;
      const Vec3d X(evecs[0 * 4 + 0] / wgt, evecs[1 * 4 + 0] / wgt,
                    evecs[2 * 4 + 0] / wgt);
      const double z0 = X(2);
      const Vec3d x1 = p1.R * X + p1.t;
      const double z1 = x1(2);
      if (z0 > 0.0 && z1 > 0.0) { ++positive; mask[i] = 1; }
    }
    if (positive > best_positive) {
      best_positive = positive;
      best = c;
      best_mask = mask;
    }
  }

  if (best < 0 || best_positive == 0) return false;

  out_pose1->R = cands[best].r;
  out_pose1->t = cands[best].t;
  if (out_inlier_mask != nullptr) *out_inlier_mask = best_mask;
  return true;
}

// ---------------------------------------------------------------------------
// 公开：多视图三角化
// ---------------------------------------------------------------------------

bool TriangulateMultiView(const std::vector<CameraPose>& poses,
                          const std::vector<CameraIntrinsics>& intr,
                          const std::vector<Vec2d>& obs, const SfmConfig& config,
                          Vec3d* out) {
  const size_t m = poses.size();
  if (m < 2 || obs.size() != m || intr.size() != m) return false;

  // 堆叠 2m 行（完整投影矩阵口径，见 msckf.cpp 文件头结论 3）。
  std::vector<double> ata(16, 0.0);
  for (size_t v = 0; v < m; ++v) {
    const Mat3d& R = poses[v].R;
    const Vec3d& tt = poses[v].t;
    const double du = obs[v](0) - intr[v].cx;
    const double dv = obs[v](1) - intr[v].cy;
    const double fxv = intr[v].fx, fyv = intr[v].fy;

    double r0[4], r1[4];
    r0[0] = fxv * R(0, 0) - du * R(2, 0);
    r0[1] = fxv * R(0, 1) - du * R(2, 1);
    r0[2] = fxv * R(0, 2) - du * R(2, 2);
    r0[3] = fxv * tt(0) - du * tt(2);
    r1[0] = fyv * R(1, 0) - dv * R(2, 0);
    r1[1] = fyv * R(1, 1) - dv * R(2, 1);
    r1[2] = fyv * R(1, 2) - dv * R(2, 2);
    r1[3] = fyv * tt(1) - dv * tt(2);
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) {
        ata[static_cast<size_t>(i) * 4 + j] += r0[i] * r0[j] + r1[i] * r1[j];
      }
    }
  }

  // 条件数检查（用对角比值近似，避免引入完整 SVD）。
  double dmin = 1e300, dmax = 0.0;
  for (int i = 0; i < 4; ++i) {
    const double d = ata[static_cast<size_t>(i) * 4 + i];
    dmin = std::min(dmin, d);
    dmax = std::max(dmax, d);
  }
  if (dmin <= 0.0 || dmax / dmin > config.cond_threshold) return false;

  std::vector<double> evals, evecs;
  JacobiEigenSymmetric(ata, 4, &evals, &evecs);
  // 最小特征值对应特征向量（第 0 列）。**注意输出布局 evecs[r*4 + i]**。
  const double wgt = evecs[3 * 4 + 0];
  if (std::fabs(wgt) < 1e-12) return false;
  const Vec3d X(evecs[0 * 4 + 0] / wgt, evecs[1 * 4 + 0] / wgt,
                evecs[2 * 4 + 0] / wgt);
  if (!X.isFinite()) return false;

  // 正深度检验（cheirality）。
  for (size_t v = 0; v < m; ++v) {
    const Vec3d x = poses[v].WorldToCamera(X);
    if (x(2) <= config.triangulation_cheirality_eps) return false;
  }

  // 视差角检验：任一对视图的视线夹角需 > 阈值。
  bool parallax_ok = false;
  for (size_t a = 0; a < m && !parallax_ok; ++a) {
    for (size_t b = a + 1; b < m; ++b) {
      const Vec3d ca = poses[a].CenterWorld();
      const Vec3d cb = poses[b].CenterWorld();
      const Vec3d ra = (X - ca).normalized();
      const Vec3d rb = (X - cb).normalized();
      double cosang = ra.dot(rb);
      cosang = std::max(-1.0, std::min(1.0, cosang));
      const double ang = std::acos(cosang) * 180.0 / kPi;
      if (ang > config.triangulation_min_angle_deg) { parallax_ok = true; break; }
    }
  }
  if (!parallax_ok) return false;

  *out = X;
  return true;
}

}  // namespace recon
