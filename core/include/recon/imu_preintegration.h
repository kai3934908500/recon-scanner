// SPDX-License-Identifier: BSD-3-Clause
//
// IMU 预积分接口。
//
// 契约：docs/05_单目管线.md §3.3、docs/02_采集与同步.md §3.5。

#ifndef RECON_CORE_IMU_PREINTEGRATION_H_
#define RECON_CORE_IMU_PREINTEGRATION_H_

#include <cstdint>
#include <vector>

#include "recon/linalg.h"
#include "recon/recon_c_api.h"

namespace recon {

// 单个 IMU 采样（核心内部表示）。
struct ImuSample {
  int64_t timestamp_ns = 0;
  Vec3d accel = Vec3d::Zero();  // 比力，m/s^2（含重力）
  Vec3d gyro = Vec3d::Zero();   // 角速度，rad/s
};

// 预积分测量。
//
// `bias_gyro` / `bias_acc` 是**积分时所用**的零偏（线性化点）。
// 优化过程中零偏会被更新，此时用下面五个雅可比做**一阶修正**，无需重新积分
// （docs/02 §3.5.5、docs/05 §3.3.5）—— 这正是「零偏可优化」的实现基础。
struct PreintegratedMeasurement {
  Vec3d delta_p = Vec3d::Zero();  // 位置增量
  Mat3d delta_q = Mat3d::Identity();  // 姿态增量
  Vec3d delta_v = Vec3d::Zero();  // 速度增量
  Mat9d covariance = Mat9d::Zero();  // 9x9 协方差
  Vec3d bias_gyro = Vec3d::Zero();
  Vec3d bias_acc = Vec3d::Zero();
  double duration_s = 0.0;

  // 预积分量对零偏的雅可比（docs/05 §3.3.5、docs/02 §3.5.5）。
  //
  // 语义（右扰动，δ 相对**积分时零偏**的增量）：
  //   ΔR̂(b_g+δ)          ≈ ΔR̂ · Exp( d_delta_r_d_bg · δ )
  //   Δv̂(b_g+δ, b_a+δ')  ≈ Δv̂ + d_delta_v_d_bg·δ + d_delta_v_d_ba·δ'
  //   Δp̂(b_g+δ, b_a+δ')  ≈ Δp̂ + d_delta_p_d_bg·δ + d_delta_p_d_ba·δ'
  //
  // 收敛校验：`tests/unit/test_preint_jacobian.cpp` 以**中心差分**核验
  // 上述三式，相对误差 < 1e-5（docs/02 P-1、docs/05 J-1）。
  Mat3d d_delta_r_d_bg = Mat3d::Zero();
  Mat3d d_delta_v_d_bg = Mat3d::Zero();
  Mat3d d_delta_v_d_ba = Mat3d::Zero();
  Mat3d d_delta_p_d_bg = Mat3d::Zero();
  Mat3d d_delta_p_d_ba = Mat3d::Zero();

  // 用**新的零偏**对预积分量做一阶修正（docs/05 §3.3.5）。
  //
  // 传入的是新的绝对零偏值（`bias_gyro_now` / `bias_acc_now`），内部与积分时
  // 的 `bias_gyro` / `bias_acc` 相减得到增量 `δb`，再按上面的雅可比修正：
  //   ΔR̂' = ΔR̂ · Exp( d_delta_r_d_bg · δb_g )
  //   Δv̂' = Δv̂ + d_delta_v_d_bg·δb_g + d_delta_v_d_ba·δb_a
  //   Δp̂' = Δp̂ + d_delta_p_d_bg·δb_g + d_delta_p_d_ba·δb_a
  // 当新零偏等于积分时零偏时，三个输出与原始值**逐位相同**（δb=0）。
  void Correct(Vec3d* delta_p_out, Mat3d* delta_q_out, Vec3d* delta_v_out,
               const Vec3d& bias_gyro_now, const Vec3d& bias_acc_now) const;
};

// 统一的结果类型。
struct PreintegrationResult {
  bool ok = false;
  recon_status status = RECON_OK;
};

// 静止检测结果。
struct StaticDetectionResult {
  bool is_static = false;
  int count = 0;                       // 参与判定的样本数
  double max_accel_deviation = 0.0;    // |‖a‖-g| 最大值
  double max_gyro_norm = 0.0;
  const char* reason = "";
};

// 尺度可观性检查结果。
//
// 背景（docs/00 §8.7）：单目 VIO 在一般运动下尺度可观，
// 但匀速/匀加速直线运动下尺度退化不可观测。
// 本检查用于在采集过程中给出引导，避免用户只做直线平移。
struct ScaleObservabilityResult {
  bool degenerate = false;
  double direction_consistency = 0.0;  // ρ_dir，越接近 1 越像直线运动
  double accel_variance = 0.0;
  double total_rotation_deg = 0.0;
  const char* reason = "";
};

// SO(3) 指数与对数映射。
Mat3d ExpSO3(const Vec3d& omega);
Vec3d LogSO3(const Mat3d& R);

// SO(3) 右雅可比：
//   J_r(ω) = I − (1−cosθ)/θ²·[ω]× + (θ−sinθ)/θ³·[ω]×²,  θ = ‖ω‖
// 满足 Exp(ω + δ) ≈ Exp(ω)·Exp(J_r(ω)·δ)，是预积分零偏雅可比的必备原语
// （docs/05 §3.3.5）。小角度用泰勒展开，θ→0 数值稳定。
Mat3d RightJacobianSO3(const Vec3d& omega);

// 预积分器。
class ImuPreintegrator {
 public:
  ImuPreintegrator() = default;

  /**
   * 重置并设置零偏与噪声。
   *
   * 噪声参数口径：**各轴方差**（σ²），不是标准差。
   *
   * | 参数 | 物理含义 | 单位 |
   * |---|---|---|
   * | noise_gyro | 陀螺白噪声方差 σ_g² | rad²/s² |
   * | noise_acc | 加计白噪声方差 σ_a² | m²/s⁴ |
   * | noise_bias_gyro | 陀螺零偏随机游走方差 σ_bg² | rad²/s⁴ |
   * | noise_bias_acc | 加计零偏随机游走方差 σ_ba² | m²/s⁵ |
   *
   * 离散化（docs/02 §3.5.4）：
   *   白噪声项     σ²·Δt
   *   随机游走项   σ²·Δt³（作用于位移通道）
   */
  PreintegrationResult Reset(const Vec3d& bias_gyro, const Vec3d& bias_acc,
                             const Vec3d& noise_gyro, const Vec3d& noise_acc,
                             const Vec3d& noise_bias_gyro,
                             const Vec3d& noise_bias_acc);

  // 累加一个采样。
  PreintegrationResult AddSample(double timestamp_ns, const Vec3d& accel,
                                  const Vec3d& gyro);

  /**
   * 设置重力方向（世界系）。
   *
   * 必须先完成重力估计再调用 AddSample，否则预积分结果无意义。
   * 静止检测可直接给出重力方向：静止时加速度均值的归一化即为重力方向。
   */
  void SetGravity(const Vec3d& gravity_world) { gravity_world_ = gravity_world; }

  const Vec3d& gravity() const { return gravity_world_; }

  // 输出测量结果。
  PreintegrationResult Finalize(PreintegratedMeasurement* out) const;

  // 已累积时长。
  double duration_s() const { return duration_s_; }

 private:
  Vec3d delta_p_ = Vec3d::Zero();
  Mat3d delta_q_ = Mat3d::Identity();
  Vec3d delta_v_ = Vec3d::Zero();

  // 零偏雅可比累加器（docs/05 §3.3.5、docs/02 §3.5.5）。
  // 递推在 AddSample 中与预积分同步进行，Finalize 导出。
  Mat3d dr_dbg_ = Mat3d::Zero();
  Mat3d dv_dbg_ = Mat3d::Zero();
  Mat3d dv_dba_ = Mat3d::Zero();
  Mat3d dp_dbg_ = Mat3d::Zero();
  Mat3d dp_dba_ = Mat3d::Zero();

  Vec3d gravity_world_ = Vec3d::Zero();
  Vec3d bias_gyro_ = Vec3d::Zero();
  Vec3d bias_acc_ = Vec3d::Zero();
  Vec3d noise_gyro_ = Vec3d::Zero();
  Vec3d noise_acc_ = Vec3d::Zero();
  Vec3d noise_bias_gyro_ = Vec3d::Zero();
  Vec3d noise_bias_acc_ = Vec3d::Zero();

  Mat9d covariance_ = Mat9d::Zero();

  double prev_time_ns_ = 0.0;
  double duration_s_ = 0.0;
  bool has_prev_ = false;
  Vec3d prev_accel_ = Vec3d::Zero();
  Vec3d prev_gyro_ = Vec3d::Zero();
};

// 静止检测。
StaticDetectionResult DetectStatic(const std::vector<ImuSample>& samples,
                                   double accel_tol, double gyro_tol,
                                   double duration_s);

// 尺度可观性检查。
ScaleObservabilityResult CheckScaleObservability(
    const std::vector<ImuSample>& samples, double window_s);

}  // namespace recon

#endif  // RECON_CORE_IMU_PREINTEGRATION_H_
