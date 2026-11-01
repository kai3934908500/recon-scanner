// SPDX-License-Identifier: BSD-3-Clause
//
// SfM 增量式注册 + 全局 BA。契约：docs/05_单目管线.md §4.4、§4.6。
//
// 零依赖路径：不引用 Ceres / Eigen。数值内核（Jacobi 特征分解 / SVD）复用
// sfm_internal.h，阻尼正定求解复用 robust_fit.h 的 SolveSymmetricDamped ——
// 与 VIO 共用同一套求解语义（铁律：同一逻辑写两遍必然漂移）。
//
// 与 docs/05 §4.6 的差异（如实披露，不假装等价）：
//   文档的完整 BA 含 IMU 预积分因子、重力/零偏变量、全局尺度 s。但本模块
//   服务于 **MONO 无 IMU** 分支 —— 有 IMU 时位姿由 VIO 提供（见 docs/00 §5），
//   SfM 根本不启动。故本实现只保留：
//     · 视觉重投影残差（Huber）+ 可选 VIO 位姿强先验；
//     · gauge 固定：首视图全固定（GA-1）+ 阻尼对角保护锁定尺度 gauge。
//   尺度不作为优化变量：SfM 输出的是 ru，尺度由 docs/07 在 BA 之后注入
//   （07 §2.8「不得 BA 之后再乘 s」针对的是含深度的 RGBD 分支；MONO+无 IMU
//   无米制观测量，尺度只能来自参照物标定）。
//
// 踩坑记录：
//   1. **PnP 用 DLT 而非 P3P**：P3P 的多解与「点共面」退化分支多，
//      噪声下不稳；DLT + RANSAC 对 >4 点天然更稳，代价是需 ≥ 6 对点。
//   2. **BA 必须固定 gauge**。首视图固定（GA-1）消掉 6 自由度；剩余尺度
//      gauge 由阻尼对角保护处理（沿不可观方向不更新）。若不固定首视图，
//      法方程奇异、LM 会随机漂。
//   3. **残差在归一化平面算，再乘焦距折算像素**。观测先用 PixelToNormalized
//      去畸变；投影侧只做 x/z —— 若两侧畸变处理不一致会引入系统性偏差。

#include "recon/sfm.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "recon/imu_preintegration.h"
#include "recon/robust_fit.h"
#include "sfm_internal.h"

namespace recon {
namespace {

using sfm_internal::JacobiEigenSymmetric;
using sfm_internal::Svd3x3;

// 极简线性同余 RNG（确定性，便于复现；不依赖 <random> 的分布对象）。
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  int uniform(int n) { return n > 0 ? static_cast<int>(next() % static_cast<uint64_t>(n)) : 0; }
};

// 采样 k 个互不相同的下标。
void SampleDistinct(Rng* rng, int n, int k, std::vector<int>* out) {
  out->clear();
  if (k >= n) {
    for (int i = 0; i < n; ++i) out->push_back(i);
    return;
  }
  // 蓄水池：保证互异。
  for (int i = 0; i < k; ++i) out->push_back(i);
  for (int i = k; i < n; ++i) {
    const int j = rng->uniform(i + 1);
    if (j < k) (*out)[static_cast<size_t>(j)] = i;
  }
}

// 归一化坐标下的投影：x_n = p.x/p.z。
bool ProjectNormalized(const Vec3d& p_cam, Vec2d* out) {
  if (!(p_cam(2) > 1e-9)) return false;
  (*out)(0) = p_cam(0) / p_cam(2);
  (*out)(1) = p_cam(1) / p_cam(2);
  return true;
}

// 观测像素 → 归一化坐标（去畸变）。
Vec2d ObsToNormalized(const CameraIntrinsics& k, const Vec2d& uv) {
  double x = 0.0, y = 0.0;
  k.PixelToNormalized(uv(0), uv(1), &x, &y);
  return Vec2d(x, y);
}

// ---------------------------------------------------------------------------
// PnP DLT：由对应 (X_world, x_norm) 解 [R|t]（up to scale）。
// ---------------------------------------------------------------------------
// 参数向量布局（**必须与下面的提取代码一致，否则 R 与 t 交错错位**）：
//   p = [ r11 r12 r13  r21 r22 r23  r31 r32 r33  t1 t2 t3 ]
//       └── idx 0..2  ──┘└── 3..5  ──┘└── 6..8  ──┘└  9..11 ┘
// 即「先 R 行主序、后 t」，**不是** [R|t] 的 3×4 行主序。
//
// 曾踩坑：系数行按 3×4 行主序 [r11 r12 r13 t1 | r21 ...] 构造，而提取按
// 「先 R 后 t」读 → R 的 (1,0) 位置拿到 t1 → 解出的位姿完全错误、
// RANSAC 内点数恒为最小样本数、PnP 恒失败。教训：齐次 DLT 的参数布局
// 必须在**同一处**定义并被构造与提取共同引用。
//
// 每个对应给两行（归一化投影 x_n = (R X + t)_x / (R X + t)_z）：
//   [ X Y Z 0 0 0 −x·X −x·Y −x·Z  1 0 −x ] · p = 0
//   [ 0 0 0 X Y Z −y·X −y·Y −y·Z  0 1 −y ] · p = 0
// 取 AᵀA 最小特征向量。
bool PnpDlt(const std::vector<Vec3d>& Xw, const std::vector<Vec2d>& xn,
            Mat3d* out_r, Vec3d* out_t) {
  const size_t n = Xw.size();
  // 需 ≥ 8 点：见 EstimateAbsolutePose 里 kMinSample 的说明。
  if (n < 8 || xn.size() != n) return false;

  std::vector<double> ata(144, 0.0);
  auto add_row = [&](const double* row) {
    for (int i = 0; i < 12; ++i) {
      for (int j = 0; j < 12; ++j) {
        ata[static_cast<size_t>(i) * 12 + j] += row[i] * row[j];
      }
    }
  };
  for (size_t k = 0; k < n; ++k) {
    const double X = Xw[k](0), Y = Xw[k](1), Z = Xw[k](2);
    const double x = xn[k](0), y = xn[k](1);
    double r0[12] = {X, Y, Z, 0.0, 0.0, 0.0, -x * X, -x * Y, -x * Z, 1.0, 0.0, -x};
    double r1[12] = {0.0, 0.0, 0.0, X, Y, Z, -y * X, -y * Y, -y * Z, 0.0, 1.0, -y};
    add_row(r0);
    add_row(r1);
  }

  std::vector<double> evals, evecs;
  JacobiEigenSymmetric(ata, 12, &evals, &evecs);
  const std::vector<double> p = sfm_internal::EigenColumn(evecs, 12, 0);
  for (int i = 0; i < 12; ++i) {
    if (!std::isfinite(p[static_cast<size_t>(i)])) return false;
  }

  // 提取：p 的 0..8 是 R 行主序，9..11 是 t。
  Mat3d m;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) m(i, j) = p[static_cast<size_t>(i) * 3 + j];
  }
  Vec3d t4(p[9], p[10], p[11]);

  Mat3d u, v;
  Vec3d s;
  Svd3x3(m, &u, &s, &v);
  // R 若退化（s 太小）说明点分布病态。
  if (!(s(0) > 1e-12)) return false;
  Mat3d r = u * v.transpose();
  if (Determinant3x3(r) < 0.0) {
    for (int i = 0; i < 3; ++i) u(i, 2) = -u(i, 2);
    r = u * v.transpose();
  }
  // 尺度 = M 的平均奇异值（正交时 s_i = scale）。t = t4 / scale。
  const double scale = (s(0) + s(1) + s(2)) / 3.0;
  if (!(scale > 1e-12)) return false;
  *out_r = r;
  *out_t = t4 / scale;
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 公开：绝对位姿（PnP + RANSAC）
// ---------------------------------------------------------------------------

bool EstimateAbsolutePose(const std::vector<Vec3d>& points_world,
                          const std::vector<Vec2d>& obs_px,
                          const CameraIntrinsics& intr, const SfmConfig& config,
                          CameraPose* out_pose,
                          std::vector<uint8_t>* out_inlier_mask) {
  const size_t n = points_world.size();
  if (n < 8 || obs_px.size() != n || !(intr.fx > 0.0)) return false;

  std::vector<Vec2d> xn(n);
  for (size_t i = 0; i < n; ++i) xn[i] = ObsToNormalized(intr, obs_px[i]);

  // 像素重投影误差（用归一化残差 × 焦距折算）。
  auto reproj = [&](const CameraPose& pose, size_t i) {
    const Vec3d pc = pose.WorldToCamera(points_world[i]);
    Vec2d pr;
    if (!ProjectNormalized(pc, &pr)) return false;
    const double du = (pr(0) - xn[i](0)) * intr.fx;
    const double dv = (pr(1) - xn[i](1)) * intr.fy;
    return std::sqrt(du * du + dv * dv) <= config.pnp_ransac_error_px;
  };

  Rng rng(0x51F3ull ^ static_cast<uint64_t>(n));
  // 最小样本 = 8 点（16 方程）。**不用 6 点**：6 点给出恰好 12 个方程 = 12 个
  // 未知数，齐次系统 A·p=0 的 A 恰好奇异，ATA 的最小特征向量会被浮点噪声
  // 主导（实测：6 点时解出近似平凡解 → 尺度塌缩 → 全部外点）。8 点超定后
  // 最小奇异方向有明确几何意义，数值稳定。
  const int kMinSample = 8;
  if (static_cast<int>(n) < kMinSample) return false;
  const int iters = std::max(1, config.pnp_ransac_iterations);
  int best_inliers = -1;
  Mat3d best_r;
  Vec3d best_t;
  std::vector<uint8_t> best_mask;
  int eval = 0;

  std::vector<int> sample;
  for (int it = 0; it < iters; ++it) {
    SampleDistinct(&rng, static_cast<int>(n), kMinSample, &sample);
    std::vector<Vec3d> Xs(kMinSample);
    std::vector<Vec2d> xs(kMinSample);
    for (int s = 0; s < kMinSample; ++s) {
      Xs[static_cast<size_t>(s)] = points_world[static_cast<size_t>(sample[static_cast<size_t>(s)])];
      xs[static_cast<size_t>(s)] = xn[static_cast<size_t>(sample[static_cast<size_t>(s)])];
    }
    Mat3d r;
    Vec3d t;
    if (!PnpDlt(Xs, xs, &r, &t)) continue;

    CameraPose pose;
    pose.R = r;
    pose.t = t;
    int inl = 0;
    std::vector<uint8_t> mask(n, 0);
    for (size_t i = 0; i < n; ++i) {
      if (reproj(pose, i)) { ++inl; mask[i] = 1; }
    }
    ++eval;
    if (inl > best_inliers) {
      best_inliers = inl;
      best_r = r;
      best_t = t;
      best_mask = mask;
      // 自适应迭代（与 geometry.cpp 的 RANSAC 同口径）。
      const double ratio = static_cast<double>(inl) / static_cast<double>(n);
      const double denom = 1.0 - std::pow(ratio, kMinSample);
      if (denom > 1e-12) {
        const double need =
            std::log(1.0 - config.ransac_confidence) / std::log(denom);
        if (need > 0.0 && need < static_cast<double>(iters)) {
          const int cap = std::min(iters, static_cast<int>(need) + 1);
          if (cap < iters) { /* 不缩小外层上限，仅用于提前跳出 */ }
        }
      }
    }
  }

  if (best_inliers < std::max(kMinSample, config.ransac_min_inliers)) {
    if (eval == 0) return false;
  }
  if (best_inliers < kMinSample) return false;

  // 用全部内点精化（重解一次 DLT）。
  {
    std::vector<Vec3d> Xi;
    std::vector<Vec2d> xi;
    for (size_t i = 0; i < n; ++i) {
      if (best_mask[i]) { Xi.push_back(points_world[i]); xi.push_back(xn[i]); }
    }
    Mat3d r;
    Vec3d t;
    if (Xi.size() >= 8 && PnpDlt(Xi, xi, &r, &t)) {
      best_r = r;
      best_t = t;
      // 重新计算内点掩码。
      CameraPose pose;
      pose.R = r;
      pose.t = t;
      best_inliers = 0;
      for (size_t i = 0; i < n; ++i) {
        if (reproj(pose, i)) { best_mask[i] = 1; ++best_inliers; }
        else { best_mask[i] = 0; }
      }
    }
  }

  const double inlier_ratio = static_cast<double>(best_inliers) / static_cast<double>(n);
  if (inlier_ratio < config.pnp_min_inlier_ratio) return false;

  out_pose->R = best_r;
  out_pose->t = best_t;
  if (out_inlier_mask != nullptr) *out_inlier_mask = best_mask;
  return true;
}

// ---------------------------------------------------------------------------
// 公开：全局 BA
// ---------------------------------------------------------------------------

BaResult BundleAdjust(Bamap* map, const SfmConfig& config) {
  BaResult res;
  if (map == nullptr) return res;
  const int np = static_cast<int>(map->poses.size());
  if (np < 2 || map->tracks.empty()) return res;

  // 位姿参数块：每帧 6 维 [δθ, δp]。首视图固定（GA-1）→ 不分配参数。
  const int fixed = config.ba_fix_first_pose ? 1 : 0;
  const std::vector<int> pose_param(np, -1);
  std::vector<int> pose_of_param;  // 参数块序号 → 帧号
  std::vector<int>& pp = const_cast<std::vector<int>&>(pose_param);
  for (int i = fixed; i < np; ++i) {
    pp[i] = static_cast<int>(pose_of_param.size());
    pose_of_param.push_back(i);
  }
  const int n_pose_p = static_cast<int>(pose_of_param.size());

  // 点参数：只有 valid 的 track 参与。
  std::vector<int> point_param(map->tracks.size(), -1);
  std::vector<int> point_of_param;
  for (size_t j = 0; j < map->tracks.size(); ++j) {
    if (map->tracks[j].valid && map->tracks[j].observations.size() >= 2) {
      point_param[j] = static_cast<int>(point_of_param.size());
      point_of_param.push_back(static_cast<int>(j));
    }
  }
  const int n_pt_p = static_cast<int>(point_of_param.size());
  if (n_pt_p == 0) return res;

  const int n_param = n_pose_p * 6 + n_pt_p * 3;
  if (n_param <= 0) return res;

  // 先验是否可用。
  const bool use_prior = map->prior_poses.size() == map->poses.size() &&
                         map->has_prior.size() == map->poses.size();
  const double sigma_th = config.prior_rotation_sigma_deg * kPi / 180.0;
  const double sigma_t = config.prior_translation_sigma_m;
  const double w_rot = use_prior && sigma_th > 1e-9 ? 1.0 / (sigma_th * sigma_th) : 0.0;
  const double w_t = use_prior && sigma_t > 1e-9 ? 1.0 / (sigma_t * sigma_t) : 0.0;
  const double w_prior = config.prior_vio_weight;

  const double huber_delta = config.ba_huber_delta_px;

  auto compute_cost = [&]() {
    double cost = 0.0;
    for (size_t j = 0; j < map->tracks.size(); ++j) {
      const SfmTrack& tr = map->tracks[j];
      if (!tr.valid) continue;
      for (const SfmObservation& o : tr.observations) {
        const int i = o.image_id;
        if (i < 0 || i >= np) continue;
        const Vec3d pc = map->poses[static_cast<size_t>(i)].WorldToCamera(tr.point);
        Vec2d pr;
        if (!ProjectNormalized(pc, &pr)) continue;
        const Vec2d on = ObsToNormalized(map->intrinsics[static_cast<size_t>(i)], o.uv);
        const double du = (pr(0) - on(0)) * map->intrinsics[static_cast<size_t>(i)].fx;
        const double dv = (pr(1) - on(1)) * map->intrinsics[static_cast<size_t>(i)].fy;
        const double r2 = du * du + dv * dv;
        const double w = RobustWeightHuber(r2, huber_delta);
        cost += w * r2;
      }
    }
    return cost;
  };

  double lambda = 1e-3;
  res.initial_cost = compute_cost();
  double cost = res.initial_cost;
  int iter = 0;

  for (; iter < config.ba_max_iterations; ++iter) {
    std::vector<double> h(static_cast<size_t>(n_param) * n_param, 0.0);
    std::vector<double> g(static_cast<size_t>(n_param), 0.0);

    // 每个观测的雅可比累加。
    for (size_t j = 0; j < map->tracks.size(); ++j) {
      SfmTrack& tr = map->tracks[j];
      if (!tr.valid) continue;
      const int pj = point_param[j];
      if (pj < 0) continue;
      const int pcol = n_pose_p * 6 + pj * 3;

      for (const SfmObservation& o : tr.observations) {
        const int i = o.image_id;
        if (i < 0 || i >= np) continue;
        const CameraIntrinsics& k = map->intrinsics[static_cast<size_t>(i)];
        const CameraPose& pose = map->poses[static_cast<size_t>(i)];
        const Vec3d pc = pose.WorldToCamera(tr.point);
        if (!(pc(2) > 1e-9)) continue;
        const double invz = 1.0 / pc(2);
        const double x_n = pc(0) * invz;
        const double y_n = pc(1) * invz;
        const Vec2d on = ObsToNormalized(k, o.uv);
        const double rx = (x_n - on(0)) * k.fx;
        const double ry = (y_n - on(1)) * k.fy;
        const double r2 = rx * rx + ry * ry;
        const double w = RobustWeightHuber(r2, huber_delta);
        if (w <= 0.0) continue;

        // ∂(x_n,y_n)/∂p（像素化后）—— 3 维。
        Vec3d dxn_dp( invz, 0.0, -pc(0) * invz * invz);
        Vec3d dyn_dp(0.0, invz, -pc(1) * invz * invz);
        Vec3d dresx_dp = dxn_dp * k.fx;
        Vec3d dresy_dp = dyn_dp * k.fy;

        // ∂p/∂X = R。
        const Mat3d& R = pose.R;
        // ∂res/∂X = dres_dp · R
        Vec3d dresx_dX;
        Vec3d dresy_dX;
        for (int c = 0; c < 3; ++c) {
          dresx_dX(c) = dresx_dp(0) * R(0, c) + dresx_dp(1) * R(1, c) + dresx_dp(2) * R(2, c);
          dresy_dX(c) = dresy_dp(0) * R(0, c) + dresy_dp(1) * R(1, c) + dresy_dp(2) * R(2, c);
        }
        // 累加点块。
        for (int a = 0; a < 3; ++a) {
          for (int b = 0; b < 3; ++b) {
            h[static_cast<size_t>(pcol + a) * n_param + (pcol + b)] +=
                w * (dresx_dX(a) * dresx_dX(b) + dresy_dX(a) * dresy_dX(b));
          }
          g[static_cast<size_t>(pcol + a)] +=
              w * (dresx_dX(a) * rx + dresy_dX(a) * ry);
        }

        // 位姿块（若未固定）。
        const int pi = pose_param[static_cast<size_t>(i)];
        if (pi >= 0) {
          const int col = pi * 6;
          // ∂p/∂δθ = −R·[X]×（右扰动）；∂p/∂δp = I。
          const Vec3d& X = tr.point;
          // [X]× = skew(X)
          double sk[3][3] = {{0.0, -X(2), X(1)},
                             {X(2), 0.0, -X(0)},
                             {-X(1), X(0), 0.0}};
          // M = −R·sk，3×3。∂p/∂δθ 的第 c 列 = M 的第 c 列。
          double dpdth[3][3];
          for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
              double s = 0.0;
              for (int m = 0; m < 3; ++m) s += R(r, m) * sk[m][c];
              dpdth[r][c] = -s;
            }
          }
          // ∂res/∂δθ = dres_dp · dpdth（3 维）；∂res/∂δp = dres_dp。
          Vec3d dresx_dth;
          Vec3d dresy_dth;
          for (int c = 0; c < 3; ++c) {
            dresx_dth(c) = dresx_dp(0) * dpdth[0][c] + dresx_dp(1) * dpdth[1][c] +
                           dresx_dp(2) * dpdth[2][c];
            dresy_dth(c) = dresy_dp(0) * dpdth[0][c] + dresy_dp(1) * dpdth[1][c] +
                           dresy_dp(2) * dpdth[2][c];
          }
          // 6 维雅可比 J = [dth(3) | dp(3)]。
          double J[6][2];
          for (int c = 0; c < 3; ++c) {
            J[c][0] = dresx_dth(c);
            J[c][1] = dresy_dth(c);
            J[3 + c][0] = dresx_dp(c);
            J[3 + c][1] = dresy_dp(c);
          }
          for (int a = 0; a < 6; ++a) {
            for (int b = 0; b < 6; ++b) {
              h[static_cast<size_t>(col + a) * n_param + (col + b)] +=
                  w * (J[a][0] * J[b][0] + J[a][1] * J[b][1]);
            }
            g[static_cast<size_t>(col + a)] +=
                w * (J[a][0] * rx + J[a][1] * ry);
          }
          // 交叉项：点 × 位姿（对称填充）。
          for (int a = 0; a < 6; ++a) {
            for (int b = 0; b < 3; ++b) {
              double v = w * (J[a][0] * dresx_dX(b) + J[a][1] * dresy_dX(b));
              h[static_cast<size_t>(col + a) * n_param + (pcol + b)] += v;
              h[static_cast<size_t>(pcol + b) * n_param + (col + a)] += v;
            }
          }
        }
      }
    }

    // VIO 位姿先验（若可用）。
    if (use_prior && (w_rot > 0.0 || w_t > 0.0)) {
      for (int i = fixed; i < np; ++i) {
        const int pi = pose_param[static_cast<size_t>(i)];
        if (pi < 0) continue;
        if (!map->has_prior[static_cast<size_t>(i)]) continue;
        const int col = pi * 6;
        const CameraPose& vio = map->prior_poses[static_cast<size_t>(i)];
        const CameraPose& cur = map->poses[static_cast<size_t>(i)];
        // 旋转残差：Log(R_vioᵀ R_cur)。
        const Vec3d r_rot = LogSO3(vio.R.transpose() * cur.R);
        for (int c = 0; c < 3; ++c) {
          const double rv = r_rot(c);
          for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) {
              const double jj = (a == c && b == c) ? 1.0 : 0.0;
              h[static_cast<size_t>(col + a) * n_param + (col + b)] += w_prior * w_rot * jj;
            }
            g[static_cast<size_t>(col + a)] += w_prior * w_rot * ((a == c) ? rv : 0.0);
          }
        }
        // 平移残差：R_vioᵀ(t_cur − t_vio)。
        const Vec3d r_tr = vio.R.transpose() * (cur.t - vio.t);
        for (int a = 0; a < 3; ++a) {
          for (int b = 0; b < 3; ++b) {
            double jj = 0.0;
            for (int m = 0; m < 3; ++m) jj += vio.R(m, a) * vio.R(m, b);
            h[static_cast<size_t>(col + 3 + a) * n_param + (col + 3 + b)] += w_prior * w_t * jj;
          }
          double gg = 0.0;
          for (int m = 0; m < 3; ++m) gg += vio.R(m, a) * r_tr(m);
          g[static_cast<size_t>(col + 3 + a)] += w_prior * w_t * gg;
        }
      }
    }

    // 求解 (H + λI)·dx = −g。
    std::vector<double> rhs(static_cast<size_t>(n_param));
    for (int k = 0; k < n_param; ++k) rhs[static_cast<size_t>(k)] = -g[static_cast<size_t>(k)];
    std::vector<double> dx(static_cast<size_t>(n_param), 0.0);
    if (!SolveSymmetricDamped(h.data(), rhs.data(), n_param, lambda, dx.data())) {
      lambda *= 10.0;
      if (lambda > 1e12) break;
      continue;
    }

    // 试探步：备份 → 更新 → 评估。
    std::vector<CameraPose> pose_bak = map->poses;
    std::vector<Vec3d> point_bak(point_of_param.size());
    for (size_t p = 0; p < point_of_param.size(); ++p) {
      point_bak[p] = map->tracks[static_cast<size_t>(point_of_param[p])].point;
    }

    for (int i = fixed; i < np; ++i) {
      const int pi = pose_param[static_cast<size_t>(i)];
      if (pi < 0) continue;
      const int col = pi * 6;
      const Vec3d dth(dx[static_cast<size_t>(col)], dx[static_cast<size_t>(col + 1)],
                      dx[static_cast<size_t>(col + 2)]);
      const Vec3d dp(dx[static_cast<size_t>(col + 3)], dx[static_cast<size_t>(col + 4)],
                     dx[static_cast<size_t>(col + 5)]);
      CameraPose& pose = map->poses[static_cast<size_t>(i)];
      pose.R = pose.R * ExpSO3(dth);
      pose.t = pose.t + dp;
    }
    for (size_t p = 0; p < point_of_param.size(); ++p) {
      const int col = n_pose_p * 6 + static_cast<int>(p) * 3;
      Vec3d& X = map->tracks[static_cast<size_t>(point_of_param[p])].point;
      X = X + Vec3d(dx[static_cast<size_t>(col)], dx[static_cast<size_t>(col + 1)],
                    dx[static_cast<size_t>(col + 2)]);
    }

    const double new_cost = compute_cost();
    if (std::isfinite(new_cost) && new_cost < cost) {
      const double rel = (cost - new_cost) / std::max(1e-300, cost);
      cost = new_cost;
      lambda = std::max(1e-9, lambda * 0.3);
      if (rel < config.ba_convergence_tol) { ++iter; break; }
    } else {
      // 回退。
      map->poses = pose_bak;
      for (size_t p = 0; p < point_of_param.size(); ++p) {
        map->tracks[static_cast<size_t>(point_of_param[p])].point = point_bak[p];
      }
      lambda *= 10.0;
      if (lambda > 1e12) { ++iter; break; }
    }
  }

  res.ok = true;
  res.iterations = iter;
  res.final_cost = cost;
  res.num_points = n_pt_p;

  // 统计最终重投影 RMS（像素）。
  double sum2 = 0.0;
  int cnt = 0;
  for (size_t j = 0; j < map->tracks.size(); ++j) {
    const SfmTrack& tr = map->tracks[j];
    if (!tr.valid) continue;
    for (const SfmObservation& o : tr.observations) {
      const int i = o.image_id;
      if (i < 0 || i >= np) continue;
      const Vec3d pc = map->poses[static_cast<size_t>(i)].WorldToCamera(tr.point);
      Vec2d pr;
      if (!ProjectNormalized(pc, &pr)) continue;
      const CameraIntrinsics& k = map->intrinsics[static_cast<size_t>(i)];
      const Vec2d on = ObsToNormalized(k, o.uv);
      const double du = (pr(0) - on(0)) * k.fx;
      const double dv = (pr(1) - on(1)) * k.fy;
      sum2 += du * du + dv * dv;
      ++cnt;
    }
  }
  res.num_observations = cnt;
  res.rms_reproj_px = cnt > 0 ? std::sqrt(sum2 / static_cast<double>(cnt)) : 0.0;
  return res;
}

// ---------------------------------------------------------------------------
// 公开：增量式 SfM 主入口
// ---------------------------------------------------------------------------

SfmResult RunIncrementalSfm(const std::vector<SfmView>& views,
                            const SfmConfig& config) {
  SfmResult out;
  const int nv = static_cast<int>(views.size());
  if (nv < 2) return out;

  out.poses.resize(static_cast<size_t>(nv));
  out.registered.assign(static_cast<size_t>(nv), 0);

  // 1. 特征提取。
  std::vector<SfmFeatureSet> sets(static_cast<size_t>(nv));
  for (int i = 0; i < nv; ++i) {
    const SfmView& v = views[static_cast<size_t>(i)];
    if (v.gray == nullptr || v.width <= 0 || v.height <= 0) continue;
    sets[static_cast<size_t>(i)] =
        ExtractSfmFeatures(v.gray, v.width, v.height, v.stride, i, config);
    sets[static_cast<size_t>(i)].intrinsics = v.intrinsics;
    sets[static_cast<size_t>(i)].intrinsics_valid = v.intrinsics.fx > 0.0;
  }

  // 2. 匹配图（无位姿初值 → 只走 MM-1 序列邻接）。
  std::vector<SfmCandidatePair> pairs = BuildMatchGraph(sets, nullptr, config);
  if (pairs.empty()) return out;

  // 3. 逐候选对做几何验证 + 相对位姿 + 三角化，选初始对。
  //    打分 = 三角化点数（§4.4.1「新增三角化点数 / 代价」的简化）。
  int best_pair = -1;
  double best_score = -1.0;
  CameraPose best_p0;      // 视图 0 的位姿（世界系 = 该视图相机系）
  CameraPose best_p1;
  std::vector<SfmMatch> best_inliers;
  std::vector<Vec3d> best_points;
  std::vector<int> best_f0, best_f1;  // 每条轨迹在两帧的特征下标

  for (size_t idx = 0; idx < pairs.size(); ++idx) {
    const int i0 = pairs[idx].index0;
    const int i1 = pairs[idx].index1;
    if (sets[static_cast<size_t>(i0)].features.empty() ||
        sets[static_cast<size_t>(i1)].features.empty()) {
      continue;
    }
    const std::vector<SfmMatch> m =
        MatchFeatureSets(sets[static_cast<size_t>(i0)],
                         sets[static_cast<size_t>(i1)], config);
    if (static_cast<int>(m.size()) < config.ransac_min_inliers) continue;

    std::vector<SfmMatch> inl;
    Mat3d model;
    bool is_e = false;
    if (!VerifyPairGeometry(sets[static_cast<size_t>(i0)],
                            sets[static_cast<size_t>(i1)], m, config, &inl,
                            &model, &is_e)) {
      continue;
    }
    if (inl.size() < 8) continue;

    // 相对位姿：用归一化坐标 + E 恢复。
    const CameraIntrinsics& k0 = sets[static_cast<size_t>(i0)].intrinsics;
    const CameraIntrinsics& k1 = sets[static_cast<size_t>(i1)].intrinsics;
    std::vector<Vec2d> n0(inl.size()), n1(inl.size());
    for (size_t s = 0; s < inl.size(); ++s) {
      n0[s] = ObsToNormalized(k0, sets[static_cast<size_t>(i0)]
                                      .features[static_cast<size_t>(inl[s].index0)]
                                      .uv);
      n1[s] = ObsToNormalized(k1, sets[static_cast<size_t>(i1)]
                                      .features[static_cast<size_t>(inl[s].index1)]
                                      .uv);
    }
    CameraPose rel;
    if (!RecoverPose(model, n0, n1, &rel, nullptr)) continue;

    // 世界系 = 视图 i0 相机系 → i0 位姿为单位；i1 位姿 = rel。
    const CameraPose p0;
    CameraPose p1 = rel;
    std::vector<CameraPose> poses2 = {p0, p1};
    std::vector<CameraIntrinsics> intrs2 = {k0, k1};

    // 三角化（注意：obs 要按 (i0,i1) 顺序给出）。
    std::vector<Vec3d> pts;
    std::vector<int> f0, f1;
    for (size_t s = 0; s < inl.size(); ++s) {
      std::vector<Vec2d> obs = {
          sets[static_cast<size_t>(i0)]
              .features[static_cast<size_t>(inl[s].index0)].uv,
          sets[static_cast<size_t>(i1)]
              .features[static_cast<size_t>(inl[s].index1)].uv};
      Vec3d X;
      if (!TriangulateMultiView(poses2, intrs2, obs, config, &X)) continue;
      pts.push_back(X);
      f0.push_back(inl[s].index0);
      f1.push_back(inl[s].index1);
    }

    const double score = static_cast<double>(pts.size());
    if (score > best_score) {
      best_score = score;
      best_pair = static_cast<int>(idx);
      best_p0 = p0;
      best_p1 = p1;
      best_inliers = inl;
      best_points = pts;
      best_f0 = f0;
      best_f1 = f1;
    }
  }

  if (best_pair < 0 || best_points.size() < 8) return out;

  const int seed0 = pairs[static_cast<size_t>(best_pair)].index0;
  const int seed1 = pairs[static_cast<size_t>(best_pair)].index1;
  // 4. 初始化地图（世界系 = seed0 相机系）。
  Bamap map;
  map.poses.assign(static_cast<size_t>(nv), CameraPose());
  map.intrinsics.assign(static_cast<size_t>(nv), CameraIntrinsics());
  for (int i = 0; i < nv; ++i) {
    if (sets[static_cast<size_t>(i)].intrinsics_valid) {
      map.intrinsics[static_cast<size_t>(i)] = sets[static_cast<size_t>(i)].intrinsics;
    }
  }
  map.poses[static_cast<size_t>(seed0)] = best_p0;
  map.poses[static_cast<size_t>(seed1)] = best_p1;
  out.registered[static_cast<size_t>(seed0)] = 1;
  out.registered[static_cast<size_t>(seed1)] = 1;

  // 轨迹：seed 对之间的点。
  for (size_t s = 0; s < best_points.size(); ++s) {
    SfmTrack tr;
    SfmObservation o0;
    o0.image_id = seed0;
    o0.uv = sets[static_cast<size_t>(seed0)]
                .features[static_cast<size_t>(best_f0[s])].uv;
    tr.observations.push_back(o0);
    SfmObservation o1;
    o1.image_id = seed1;
    o1.uv = sets[static_cast<size_t>(seed1)]
                .features[static_cast<size_t>(best_f1[s])].uv;
    tr.observations.push_back(o1);
    tr.point = best_points[s];
    tr.valid = true;
    map.tracks.push_back(tr);
  }

  // 5. 增量注册：对每个未注册视图，尝试与其在匹配图中有边的已注册视图
  //    建立 3D-2D 对应，做 PnP。
  //
  //    3D-2D 对应的来源：本视图特征与已注册视图特征的**直接匹配**，若该
  //    已注册视图的特征已被某条轨迹引用，则该轨迹的 3D 点与当前视图特征
  //    构成一对。为避免 O(V·E) 暴力，这里用「帧 → (特征下标 → 轨迹下标)」
  //    的索引表。
  int registered_count = 2;

  // 帧内特征 → 轨迹下标 的索引（每轮注册后重建，简单可靠）。
  std::vector<std::vector<int>> feat_to_track;  // [frame][feat] → track idx

  auto rebuild_index = [&]() {
    feat_to_track.assign(static_cast<size_t>(nv), {});
    for (int i = 0; i < nv; ++i) {
      feat_to_track[static_cast<size_t>(i)].assign(
          sets[static_cast<size_t>(i)].features.size(), -1);
    }
    for (size_t t = 0; t < map.tracks.size(); ++t) {
      if (!map.tracks[t].valid) continue;
      for (const SfmObservation& o : map.tracks[t].observations) {
        if (o.image_id < 0 || o.image_id >= nv) continue;
        // 反查该观测的像素对应的特征下标（用 uv 精确匹配 —— 观测就是从特征
        // 的 uv 构造的，故浮点完全相等）。
        const auto& feats = sets[static_cast<size_t>(o.image_id)].features;
        for (size_t fidx = 0; fidx < feats.size(); ++fidx) {
          if (feats[fidx].uv(0) == o.uv(0) && feats[fidx].uv(1) == o.uv(1)) {
            feat_to_track[static_cast<size_t>(o.image_id)][fidx] =
                static_cast<int>(t);
            break;
          }
        }
      }
    }
  };

  const int max_rounds = nv;  // 每轮至少注册一帧，故 ≤ nv 轮。
  for (int round = 0; round < max_rounds; ++round) {
    rebuild_index();

    int newly = -1;
    CameraPose newly_pose;
    std::vector<uint8_t> newly_mask;
    std::vector<int> newly_track_ids;   // 与 newly_mask 对齐（仅内点）
    std::vector<int> newly_feat_ids;

    for (int cand = 0; cand < nv && newly < 0; ++cand) {
      if (out.registered[static_cast<size_t>(cand)]) continue;
      if (sets[static_cast<size_t>(cand)].features.empty()) continue;

      // 收集该视图与所有已注册视图的匹配所导出的 3D-2D 对应。
      std::vector<Vec3d> Xw;
      std::vector<Vec2d> obs;
      std::vector<int> tids, fids;
      for (const SfmCandidatePair& p : pairs) {
        int other = -1;
        if (p.index0 == cand && out.registered[static_cast<size_t>(p.index1)]) {
          other = p.index1;
        } else if (p.index1 == cand && out.registered[static_cast<size_t>(p.index0)]) {
          other = p.index0;
        }
        if (other < 0) continue;

        const std::vector<SfmMatch> m =
            MatchFeatureSets(sets[static_cast<size_t>(cand)],
                             sets[static_cast<size_t>(other)], config);
        for (const SfmMatch& mm : m) {
          const int tcand = feat_to_track[static_cast<size_t>(cand)]
                                          [static_cast<size_t>(mm.index0)];
          const int tother = feat_to_track[static_cast<size_t>(other)]
                                           [static_cast<size_t>(mm.index1)];
          // other 侧特征必须已属于某轨迹 → 它有可用的 3D 点。
          if (tother < 0) continue;
          // cand 侧特征必须尚未被任何轨迹引用 → 否则会重复注册同一观测。
          if (tcand >= 0) continue;
          Xw.push_back(map.tracks[static_cast<size_t>(tother)].point);
          obs.push_back(sets[static_cast<size_t>(cand)]
                            .features[static_cast<size_t>(mm.index0)].uv);
          tids.push_back(tother);
          fids.push_back(mm.index0);
        }
      }
      if (Xw.size() < 8) continue;

      CameraPose est;
      std::vector<uint8_t> mask;
      if (!EstimateAbsolutePose(Xw, obs, map.intrinsics[static_cast<size_t>(cand)],
                                config, &est, &mask)) {
        continue;
      }      newly = cand;
      newly_pose = est;
      newly_mask = mask;
      newly_track_ids = tids;
      newly_feat_ids = fids;
    }

    if (newly < 0) break;  // 无法再注册

    // 采纳：写入位姿，扩充轨迹观测。
    map.poses[static_cast<size_t>(newly)] = newly_pose;
    out.registered[static_cast<size_t>(newly)] = 1;
    ++registered_count;
    for (size_t s = 0; s < newly_mask.size(); ++s) {
      if (!newly_mask[s]) continue;
      const int t = newly_track_ids[s];
      if (t < 0 || t >= static_cast<int>(map.tracks.size())) continue;
      SfmObservation o;
      o.image_id = newly;
      o.uv = sets[static_cast<size_t>(newly)]
                 .features[static_cast<size_t>(newly_feat_ids[s])].uv;
      map.tracks[static_cast<size_t>(t)].observations.push_back(o);
    }

    // 每 ba_trigger_every 帧或首次达到 ba_min_num_images 时跑全局 BA。
    const bool trigger = (registered_count == config.ba_min_num_images) ||
                         (registered_count % std::max(1, config.ba_trigger_every) == 0);
    if (trigger) {
      map.prior_poses.clear();
      map.has_prior.clear();
      out.ba = BundleAdjust(&map, config);
    }
  }

  // 6. 最终全局 BA（若尚未跑过）。
  map.prior_poses.clear();
  map.has_prior.clear();
  out.ba = BundleAdjust(&map, config);

  // 7. 输出。
  out.poses = map.poses;
  out.tracks = map.tracks;
  out.num_registered = registered_count;
  int npts = 0;
  for (const SfmTrack& tr : out.tracks) {
    if (tr.valid) ++npts;
  }
  out.num_points = npts;
  out.ok = registered_count >= 2 && npts > 0;
  return out;
}

}  // namespace recon
