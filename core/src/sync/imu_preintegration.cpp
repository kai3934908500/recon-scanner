// SPDX-License-Identifier: BSD-3-Clause
//
// IMU 预积分（Manifold-Euler / SO(3) 右雅可比形式）。
//
// 契约：docs/05_单目管线.md §3.3、docs/02_采集与同步.md §3.5。
//
// 噪声离散化的正确形式（docs/02 §3.5.4 已修正）：
//   白噪声（rate noise）项：量级 σ²/Δt，Δt 为离散步长。
//   随机游走零偏项：量级 σ²·Δt³，作用于区间位移通道。
// 早期版本把随机游走写成 σ²·Δt²（作用于速率通道），已废弃。
//
// 注意：这里实现的是**形式化预积分**，雅可比与噪声传播按一阶近似给出，
// 完整二阶雅可比推导见 docs/05_单目管线.md §3.3.2。

#include "recon/imu_preintegration.h"

#include <algorithm>
#include <cmath>

namespace recon {
namespace {

constexpr double kGravity = 9.80665;

// 3×3 反对称矩阵 [v]×。
Mat3d Skew(const Vec3d& v) {
  Mat3d m;
  m(0, 1) = -v(2);
  m(0, 2) = v(1);
  m(1, 0) = v(2);
  m(1, 2) = -v(0);
  m(2, 0) = -v(1);
  m(2, 1) = v(0);
  return m;
}

}  // namespace

// ---------------------------------------------------------------------------
// SO(3) 工具
// ---------------------------------------------------------------------------

Mat3d ExpSO3(const Vec3d& omega) {
  const double theta = omega.norm();
  if (theta < 1e-12) {
    // 一阶近似：小角度下 J ≈ I + 0.5*[omega]x
    Mat3d J = Mat3d::Identity();
    J(0, 1) = -omega(2) * 0.5;
    J(0, 2) = omega(1) * 0.5;
    J(1, 0) = omega(2) * 0.5;
    J(1, 2) = -omega(0) * 0.5;
    J(2, 0) = -omega(1) * 0.5;
    J(2, 1) = omega(0) * 0.5;
    return J;
  }
  const Vec3d axis = omega / theta;
  // 反对称矩阵 [axis]x
  Mat3d Omega;
  Omega(0, 1) = -axis(2);
  Omega(0, 2) = axis(1);
  Omega(1, 0) = axis(2);
  Omega(1, 2) = -axis(0);
  Omega(2, 0) = -axis(1);
  Omega(2, 1) = axis(0);

  return Mat3d::Identity() + std::sin(theta) * Omega +
         (1.0 - std::cos(theta)) * (Omega * Omega);
}

Vec3d LogSO3(const Mat3d& R) {
  const double trace = R(0, 0) + R(1, 1) + R(2, 2);
  double cos_theta = 0.5 * (trace - 1.0);
  cos_theta = std::clamp(cos_theta, -1.0, 1.0);
  const double theta = std::acos(cos_theta);

  if (theta < 1e-12) {
    Vec3d v;
    v(0) = 0.5 * (R(2, 1) - R(1, 2));
    v(1) = 0.5 * (R(0, 2) - R(2, 0));
    v(2) = 0.5 * (R(1, 0) - R(0, 1));
    return v;
  }
  if (std::abs(recon::kPi - theta) < 1e-6) {
    // 接近 π 的退化情形：用 (R + I)/2 的列向量提取轴。
    Mat3d A = R + Mat3d::Identity();
    Vec3d v(A(0, 0), A(1, 0), A(2, 0));
    const double n = v.norm();
    if (n < 1e-12) return Vec3d::Zero();
    v /= n;
    return v * theta;
  }

  const double factor = theta / (2.0 * std::sin(theta));
  Vec3d v;
  v(0) = factor * (R(2, 1) - R(1, 2));
  v(1) = factor * (R(0, 2) - R(2, 0));
  v(2) = factor * (R(1, 0) - R(0, 1));
  return v;
}

Mat3d RightJacobianSO3(const Vec3d& omega) {
  // J_r(ω) = I − (1−cosθ)/θ² · [ω]× + (θ−sinθ)/θ³ · [ω]×²
  //
  // **数值稳定化（docs/02 §3.5.2 的「必须实现」）**：
  //   θ < 1e-4  → 用级数（c1 = ½ − θ²/24，c2 = ⅙ − θ²/120）。
  // 阈值取 1e-4 而**不是** 1e-6：θ 在 [1e-6, 1e-4] 区间时，直接算 `1−cosθ`
  // 会灾难性相消（`1−cosθ ≈ θ²/2 ~ 1e-12`，两个 ~1 的数相减只剩约 4 位
  // 有效数字），实测 θ=1.4e-6 处相对误差约 1.4e-4，**超过契约的 1e-5**。
  // 级数在该区间截断误差 ~θ⁴ ≈ 1e-16，完全足够。
  const double theta2 = omega.squaredNorm();
  Mat3d W;  // [ω]×
  W(0, 1) = -omega(2);
  W(0, 2) = omega(1);
  W(1, 0) = omega(2);
  W(1, 2) = -omega(0);
  W(2, 0) = -omega(1);
  W(2, 1) = omega(0);

  double c1, c2;
  if (theta2 < 1e-8) {  // θ < 1e-4
    c1 = 0.5 - theta2 / 24.0;
    c2 = (1.0 / 6.0) - theta2 / 120.0;
  } else {
    const double theta = std::sqrt(theta2);
    c1 = (1.0 - std::cos(theta)) / theta2;
    c2 = (theta - std::sin(theta)) / (theta2 * theta);
  }
  const Mat3d W2 = W * W;
  return Mat3d::Identity() - c1 * W + c2 * W2;
}

// ---------------------------------------------------------------------------
// 预积分
// ---------------------------------------------------------------------------

PreintegrationResult ImuPreintegrator::Reset(const Vec3d& bias_gyro,
                                             const Vec3d& bias_acc,
                                             const Vec3d& noise_gyro,
                                             const Vec3d& noise_acc,
                                             const Vec3d& noise_bias_gyro,
                                             const Vec3d& noise_bias_acc) {
  delta_p_ = Vec3d::Zero();
  delta_q_ = Mat3d::Identity();
  delta_v_ = Vec3d::Zero();

  // 零偏雅可比（docs/05 §3.3.5）：初值 ΔR=I 时 dr/db_g = 0，
  // 其余四项的初值同为 0。P-3 要求 bias reset 后清空全部预积分量，
  // 雅可比随之一并清零。
  dr_dbg_ = Mat3d::Zero();
  dv_dbg_ = Mat3d::Zero();
  dv_dba_ = Mat3d::Zero();
  dp_dbg_ = Mat3d::Zero();
  dp_dba_ = Mat3d::Zero();

  bias_gyro_ = bias_gyro;
  bias_acc_ = bias_acc;
  noise_gyro_ = noise_gyro;
  noise_acc_ = noise_acc;
  noise_bias_gyro_ = noise_bias_gyro;
  noise_bias_acc_ = noise_bias_acc;

  // 9x9 协方差：陀螺 3 + 加计 3 + 陀螺零偏 3 + 加计零偏 3。
  covariance_ = Mat9d::Zero();
  gravity_world_ = Vec3d::Zero();
  prev_time_ns_ = 0.0;
  duration_s_ = 0.0;
  has_prev_ = false;
  return PreintegrationResult{true, RECON_OK};
}

PreintegrationResult ImuPreintegrator::AddSample(double timestamp_ns,
                                                  const Vec3d& accel,
                                                  const Vec3d& gyro) {
  if (!has_prev_) {
    prev_time_ns_ = timestamp_ns;
    prev_accel_ = accel;
    prev_gyro_ = gyro;
    has_prev_ = true;
    return PreintegrationResult{true, RECON_OK};
  }

  const double dt = (timestamp_ns - prev_time_ns_) * 1e-9;
  if (dt <= 0.0) {
    return PreintegrationResult{false, RECON_ERR_SYNC_TIME_NONMONOTONIC};
  }
  duration_s_ += dt;

  // 去偏后的角速度与比力。
  //
  // **量纲修正**（原先写作 `bias_gyro_ * dt`，是错的）：
  // 零偏是可加量（rad/s），去偏就是简单相减；
  // 乘 Δt 会把零偏的影响缩到 1/Δt 倍（30 Hz 下相当于放大 30 倍），
  // 使预积分量对零偏的敏感度完全错乱。
  const Vec3d omega = gyro - bias_gyro_;
  // 加速度计测的是**比力**（body 系）。
  //
  // **参考系修正**（原先写作 `accel − bias_acc_·dt − gravity_world_`，是错的）：
  //   · 零偏同样不该乘 Δt；
  //   · 重力不能在这里扣 —— 重力作用在**世界系**，
  //     而下面的 `delta_q_ * f` 是 body 系到 body 系的旋转。
  //     世界系重力穿过一个 body 系姿态增量再积分，量纲与含义都不对。
  //
  // 正确形式（docs/05 §3.2.1「a_m = R_WIᵀ (a_W − g_W)」、
  // §3.7.2 的 IMU 残差）：
  //   预积分只积分 body 系比力：  f = a_m − b_a
  //   重力在世界系残差里显式出现： r_p 含 −½ g_W Δt²，r_v 含 −g_W Δt
  // 因此 `SetGravity` 不参与本函数，只用于上层构造残差
  // （VIO 估计器与 BA 因子都按此约定）。
  //
  // 自洽性校验（静止、水平放置，a_m = (0,0,+g)，g_W = (0,0,−g)）：
  //   Δv = (0,0,+g)Δt
  //   r_v = Rᵀ(v₁−v₀−g_W Δt) − Δv = (0,0,+g)Δt − (0,0,+g)Δt = 0 ✓
  //   Δp = ½(0,0,+g)Δt²
  //   r_p = Rᵀ(0−0−½g_W Δt²) − Δp = (0,0,½gΔt²) − (0,0,½gΔt²) = 0 ✓
  const Vec3d f = accel - bias_acc_;

  // 一阶欧拉更新（docs/05 §3.3.3 采用形式化形式，此处给出可实现版本）。
  const Mat3d dR = ExpSO3(omega * dt);

  // -------------------------------------------------------------------------
  // 零偏雅可比递推（docs/05 §3.3.5、docs/02 §3.5.5）
  // -------------------------------------------------------------------------
  //
  // **重要**：以下递推是**从上面的实际递推式直接求导**得到的，而非照抄
  // docs/05 §3.3.5 与 docs/02 §3.5.5 的闭式表 —— 因为那两张表**互相矛盾**
  // （05 写 `∂Δv/∂b_g ≈ −ΔR·(ω̃Δt)`，02 写 `≈ −ΔR·(ãΔt)`），且按本递推
  // 求导，两处的 `∂Δv/∂b_g` 形式都不对（正确形式含 `R_{k-1}·[f]×·(∂Δθ/∂b_g)`）。
  // docs/05 J-1 / docs/02 P-1 只要求「以中心差分校验相对误差 < 1e-5」，
  // 故以**能通过差分校验**的形式为准，并在 test_preint_jacobian.cpp 中锁死。
  //
  // 记 δb_g = b_g − b̂_g（积分时零偏），右扰动 R_k = R̂_k·Exp(δθ_k)，
  // δθ_k = dr_dbg_k·δb_g。逐项求导：
  //
  //   dr_dbg_k = Exp(−ω̂ Δt)·dr_dbg_{k-1} − J_r(ω̂ Δt)·Δt
  //   dv_dbg_k = dv_dbg_{k-1} − R̂_{k-1}·[f]×·dr_dbg_{k-1}·Δt
  //   dv_dba_k = dv_dba_{k-1} − R̂_{k-1}·Δt
  //   dp_dbg_k = dp_dbg_{k-1} + dv_dbg_{k-1}·Δt − ½·R̂_{k-1}·[f]×·dr_dbg_{k-1}·Δt²
  //   dp_dba_k = dp_dba_{k-1} + dv_dba_{k-1}·Δt − ½·R̂_{k-1}·Δt²
  //
  // 其中 R̂_{k-1} = 更新前的 delta_q_，ω̂ = gyro − b̂_g，f = accel − b̂_a。
  // 自洽性：首步 R̂_0 = I、dr_dbg_0 = 0，故 dv_dbg_1 = dp_dbg_1 = 0
  // （零偏在第一步还不能通过姿态影响速度/位置），而
  // dv_dba_1 = −Δt、dp_dba_1 = −½Δt² —— 与 docs 的 `∂Δv/∂b_a`、`∂Δp/∂b_a`
  // 在无旋转时退化一致。
  {
    const Mat3d R_prev = delta_q_;              // R̂_{k-1}
    const Mat3d dR_neg = dR.transpose();        // Exp(−ω̂ Δt)
    const Mat3d Jr = RightJacobianSO3(omega * dt);
    const Mat3d Fx = Skew(f);                   // [f]×

    const Mat3d dr_dbg_new = dR_neg * dr_dbg_ - Jr * dt;
    const Mat3d dv_dbg_new = dv_dbg_ - R_prev * Fx * dr_dbg_ * dt;
    const Mat3d dv_dba_new = dv_dba_ - R_prev * dt;
    const Mat3d dp_dbg_new =
        dp_dbg_ + dv_dbg_ * dt - (0.5 * (R_prev * Fx * dr_dbg_)) * (dt * dt);
    const Mat3d dp_dba_new =
        dp_dba_ + dv_dba_ * dt - (0.5 * R_prev) * (dt * dt);

    dr_dbg_ = dr_dbg_new;
    dv_dbg_ = dv_dbg_new;
    dv_dba_ = dv_dba_new;
    dp_dbg_ = dp_dbg_new;
    dp_dba_ = dp_dba_new;
  }

  delta_p_ += delta_v_ * dt + 0.5 * (delta_q_ * f) * dt * dt;
  delta_v_ += (delta_q_ * f) * dt;
  delta_q_ = delta_q_ * dR;

  // 噪声传播。白噪声项 σ²/Δt，随机游走零偏项 σ²·Δt³（位移通道）。
  const double dt3 = dt * dt * dt;

  // 陀螺白噪声对旋转通道的贡献：σ_g²·Δt。
  for (int i = 0; i < 3; ++i) {
    covariance_(i, i) += noise_gyro_(i) * dt;
  }
  // 加计白噪声对速度通道的贡献：σ_a²·Δt。
  for (int i = 0; i < 3; ++i) {
    covariance_(3 + i, 3 + i) += noise_acc_(i) * dt;
  }
  // 陀螺零偏随机游走：σ_bg²·Δt³。
  for (int i = 0; i < 3; ++i) {
    covariance_(i, i) += noise_bias_gyro_(i) * dt3;
  }
  // 加计零偏随机游走：σ_ba²·Δt³。
  for (int i = 0; i < 3; ++i) {
    covariance_(3 + i, 3 + i) += noise_bias_acc_(i) * dt3;
  }

  prev_time_ns_ = timestamp_ns;
  prev_accel_ = accel;
  prev_gyro_ = gyro;
  return PreintegrationResult{true, RECON_OK};
}

PreintegrationResult ImuPreintegrator::Finalize(
    PreintegratedMeasurement* out) const {
  if (out == nullptr) {
    return PreintegrationResult{false, RECON_ERR_INVALID_ARGUMENT};
  }
  if (!has_prev_) {
    return PreintegrationResult{false, RECON_ERR_SYNC_IMU_GAP};
  }
  out->delta_p = delta_p_;
  out->delta_q = delta_q_;
  out->delta_v = delta_v_;
  out->covariance = covariance_;
  out->bias_gyro = bias_gyro_;
  out->bias_acc = bias_acc_;
  out->duration_s = duration_s_;
  out->d_delta_r_d_bg = dr_dbg_;
  out->d_delta_v_d_bg = dv_dbg_;
  out->d_delta_v_d_ba = dv_dba_;
  out->d_delta_p_d_bg = dp_dbg_;
  out->d_delta_p_d_ba = dp_dba_;
  return PreintegrationResult{true, RECON_OK};
}

void PreintegratedMeasurement::Correct(Vec3d* delta_p_out, Mat3d* delta_q_out,
                                       Vec3d* delta_v_out,
                                       const Vec3d& bias_gyro_now,
                                       const Vec3d& bias_acc_now) const {
  const Vec3d dbg = bias_gyro_now - bias_gyro;
  const Vec3d dba = bias_acc_now - bias_acc;
  if (delta_q_out != nullptr) {
    // ΔR̂' = ΔR̂ · Exp( ∂Δθ/∂b_g · δb_g )
    *delta_q_out = delta_q * ExpSO3(d_delta_r_d_bg * dbg);
  }
  if (delta_v_out != nullptr) {
    *delta_v_out = delta_v + d_delta_v_d_bg * dbg + d_delta_v_d_ba * dba;
  }
  if (delta_p_out != nullptr) {
    *delta_p_out = delta_p + d_delta_p_d_bg * dbg + d_delta_p_d_ba * dba;
  }
}

// ---------------------------------------------------------------------------
// 静止检测与尺度可观性
// ---------------------------------------------------------------------------

StaticDetectionResult DetectStatic(const std::vector<ImuSample>& samples,
                                   double accel_tol, double gyro_tol,
                                   double duration_s) {
  StaticDetectionResult r;
  if (samples.size() < 2) {
    r.reason = "样本不足";
    return r;
  }

  const double window_ns = duration_s * 1e9;
  const double t_start = static_cast<double>(samples.front().timestamp_ns);
  const double t_end = static_cast<double>(samples.back().timestamp_ns);
  if (t_end - t_start < window_ns) {
    r.reason = "观测窗口不足";
    return r;
  }

  // 统计窗口内的加速度模长偏差与角速度幅值。
  double max_accel_dev = 0.0;
  double max_gyro = 0.0;
  int64_t count = 0;
  for (const auto& s : samples) {
    const double t = static_cast<double>(s.timestamp_ns);
    if (t < t_end - window_ns) continue;
    const double accel_norm = s.accel.norm();
    max_accel_dev = std::max(max_accel_dev, std::abs(accel_norm - kGravity));
    max_gyro = std::max(max_gyro, s.gyro.norm());
    ++count;
  }

  r.count = count;
  r.max_accel_deviation = max_accel_dev;
  r.max_gyro_norm = max_gyro;
  r.is_static = (max_accel_dev <= accel_tol) && (max_gyro <= gyro_tol);
  if (!r.is_static) {
    if (max_accel_dev > accel_tol) {
      r.reason = "加速度模长偏离重力过多，设备在移动";
    } else {
      r.reason = "角速度过大，设备在旋转";
    }
  }
  return r;
}

ScaleObservabilityResult CheckScaleObservability(
    const std::vector<ImuSample>& samples, double window_s) {
  ScaleObservabilityResult r;

  if (samples.size() < 10) {
    r.reason = "样本不足，无法判定";
    return r;
  }

  // 计算运动方向一致性 ρ_dir：轨迹方向是否单一。
  // 若 |ρ_dir| > 0.95，说明运动近似直线，加速度与尺度不可分离。
  const int64_t window_ns = static_cast<int64_t>(window_s * 1e9);
  Vec3d mean_accel = Vec3d::Zero();
  int n = 0;
  for (const auto& s : samples) {
    if (static_cast<int64_t>(s.timestamp_ns) <
        samples.back().timestamp_ns - window_ns) {
      continue;
    }
    mean_accel += s.accel;
    ++n;
  }
  if (n == 0) {
    r.reason = "窗口内无有效样本";
    return r;
  }
  mean_accel /= static_cast<double>(n);

  // 方向一致性：各样本加速度与均值方向的夹角余弦均值。
  double dir_cos_sum = 0.0;
  int dir_n = 0;
  for (const auto& s : samples) {
    if (static_cast<int64_t>(s.timestamp_ns) <
        samples.back().timestamp_ns - window_ns) {
      continue;
    }
    const double na = s.accel.norm();
    const double nm = mean_accel.norm();
    if (na > 1e-9 && nm > 1e-9) {
      dir_cos_sum += s.accel.dot(mean_accel) / (na * nm);
      ++dir_n;
    }
  }
  r.direction_consistency =
      dir_n > 0 ? std::abs(dir_cos_sum / static_cast<double>(dir_n)) : 0.0;

  // 加速度方差：直线匀速运动时趋近零。
  double var_sum = 0.0;
  for (const auto& s : samples) {
    if (static_cast<int64_t>(s.timestamp_ns) <
        samples.back().timestamp_ns - window_ns) {
      continue;
    }
    var_sum += (s.accel - mean_accel).squaredNorm();
  }
  r.accel_variance =
      var_sum / static_cast<double>(std::max(1, n));

  // 旋转激励：累计旋转角度。绕重力轴旋转对尺度可观性无贡献。
  double rotation_rad = 0.0;
  for (size_t i = 1; i < samples.size(); ++i) {
    rotation_rad += samples[i].gyro.norm();
  }
  r.total_rotation_deg = rotation_rad * 180.0 / recon::kPi;

  // 判定：直线 + 低加速度方差 → 尺度退化。
  const bool linear_motion = r.direction_consistency > 0.95;
  const bool low_accel_variance = r.accel_variance < 0.05 * 0.05;
  r.degenerate = linear_motion && low_accel_variance;

  if (r.degenerate) {
    r.reason = "运动近似直线且加速度变化过小，尺度在此段不可观测";
  } else {
    r.reason = "运动激励充分，尺度可观";
  }
  return r;
}

}  // namespace recon
