// SPDX-License-Identifier: BSD-3-Clause
//
// IMU 预积分对零偏的雅可比校验。
//
// 契约：
//   · docs/02_采集与同步.md §3.5.5 P-1：中心差分校验，相对误差 < 1e-5，
//     Δt ∈ {0.004, 0.005, 0.01} s，‖ω‖ ∈ {0, 1, 3} rad/s。
//   · docs/05_单目管线.md §3.3.5 J-1：同一判据；J-2：不得省略 ∂Δp/∂b_a
//     中的 ã 项（该项直接决定尺度可观性）。
//
// 本测试**不**断言雅可比等于文档里的某条闭式（那两张表互相矛盾，见
// imu_preintegration.cpp 的注释），而是断言「预积分量对零偏的一阶修正
// 与真实重新积分之间，中心差分意义下一致」—— 这是唯一正确的、可验证的
// 判据，也是优化里真正依赖的性质。

#include "recon/imu_preintegration.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::printf("  [FAIL] %s\n", what);
    ++g_failures;
  }
}

// 用给定的零偏重新积分一段合成 IMU 序列，返回 (Δp, Δv, ΔR)。
void IntegrateWithBias(double dt, int steps, const recon::Vec3d& accel_body,
                       const recon::Vec3d& gyro_body,
                       const recon::Vec3d& bias_g, const recon::Vec3d& bias_a,
                       recon::Vec3d* dp, recon::Vec3d* dv, recon::Mat3d* dR) {
  recon::ImuPreintegrator pre;
  pre.Reset(bias_g, bias_a, recon::Vec3d::Zero(), recon::Vec3d::Zero(),
            recon::Vec3d::Zero(), recon::Vec3d::Zero());
  for (int i = 0; i <= steps; ++i) {
    const double t = static_cast<double>(i) * dt * 1e9;
    pre.AddSample(t, accel_body, gyro_body);
  }
  recon::PreintegratedMeasurement m;
  pre.Finalize(&m);
  *dp = m.delta_p;
  *dv = m.delta_v;
  *dR = m.delta_q;
}

// 相对误差：‖a−b‖ / max(1, ‖b‖)。
double RelError(const recon::Vec3d& a, const recon::Vec3d& b) {
  const double denom = std::max(1.0, b.norm());
  return (a - b).norm() / denom;
}

// 对单组 (dt, ω, ã, b̂) 做中心差分校验。
//
// 判据（三式，对应 docs/05 §3.3.5）：
//   ΔR̂(b̂+δ) ≈ ΔR̂ · Exp( ∂Δθ/∂b_g · δ )       —— 只对 b_g
//   Δv̂(b̂+δ) ≈ Δv̂ + ∂Δv/∂b_g·δ_g + ∂Δv/∂b_a·δ_a
//   Δp̂(b̂+δ) ≈ Δp̂ + ∂Δp/∂b_g·δ_g + ∂Δp/∂b_a·δ_a
void CheckOneCase(double dt, const recon::Vec3d& gyro_body, double accel_z,
                  const recon::Vec3d& bias_g, const recon::Vec3d& bias_a) {
  const int steps = 10;
  const recon::Vec3d accel_body(0.2, -0.3, accel_z);

  // ① 以「积分时零偏」重新积分，得到基准测量与雅可比。
  recon::ImuPreintegrator pre;
  pre.Reset(bias_g, bias_a, recon::Vec3d::Zero(), recon::Vec3d::Zero(),
            recon::Vec3d::Zero(), recon::Vec3d::Zero());
  for (int i = 0; i <= steps; ++i) {
    const double t = static_cast<double>(i) * dt * 1e9;
    pre.AddSample(t, accel_body, gyro_body);
  }
  recon::PreintegratedMeasurement pm;
  pre.Finalize(&pm);

  // ② 用有限差分扰动零偏，逐轴比较。
  const double h = 1e-6;
  for (int axis = 0; axis < 3; ++axis) {
    // —— b_g 的第 axis 轴 ——
    {
      recon::Vec3d bg_p = bias_g, bg_m = bias_g;
      bg_p(axis) += h;
      bg_m(axis) -= h;
      recon::Vec3d dpp, dpm, dvp, dvm;
      recon::Mat3d dRp, dRm;
      IntegrateWithBias(dt, steps, accel_body, gyro_body, bg_p, bias_a, &dpp,
                        &dvp, &dRp);
      IntegrateWithBias(dt, steps, accel_body, gyro_body, bg_m, bias_a, &dpm,
                        &dvm, &dRm);

      // 数值导数（中心差分）。
      const recon::Vec3d num_dv_dbg = (dvp - dvm) / (2.0 * h);
      const recon::Vec3d num_dp_dbg = (dpp - dpm) / (2.0 * h);
      // 姿态：δ 相对量的对数差 / (2h)。
      const recon::Vec3d dlog =
          recon::LogSO3(dRm.transpose() * dRp) / (2.0 * h);

      // 解析值：修正公式给出的一阶导数。
      const recon::Vec3d ana_dv_dbg = pm.d_delta_v_d_bg * recon::Vec3d(
          axis == 0 ? 1.0 : 0.0, axis == 1 ? 1.0 : 0.0, axis == 2 ? 1.0 : 0.0);
      const recon::Vec3d ana_dp_dbg = pm.d_delta_p_d_bg * recon::Vec3d(
          axis == 0 ? 1.0 : 0.0, axis == 1 ? 1.0 : 0.0, axis == 2 ? 1.0 : 0.0);
      const recon::Vec3d ana_dr_dbg = pm.d_delta_r_d_bg * recon::Vec3d(
          axis == 0 ? 1.0 : 0.0, axis == 1 ? 1.0 : 0.0, axis == 2 ? 1.0 : 0.0);

      char buf[160];
      std::snprintf(buf, sizeof(buf),
                    "∂Δv/∂b_g 中心差分一致 (dt=%.3f, ‖ω‖=%.1f, axis=%d)", dt,
                    gyro_body.norm(), axis);
      Check(RelError(ana_dv_dbg, num_dv_dbg) < 1e-5, buf);

      std::snprintf(buf, sizeof(buf),
                    "∂Δp/∂b_g 中心差分一致 (dt=%.3f, ‖ω‖=%.1f, axis=%d)", dt,
                    gyro_body.norm(), axis);
      Check(RelError(ana_dp_dbg, num_dp_dbg) < 1e-5, buf);

      std::snprintf(buf, sizeof(buf),
                    "∂Δθ/∂b_g 中心差分一致 (dt=%.3f, ‖ω‖=%.1f, axis=%d)", dt,
                    gyro_body.norm(), axis);
      Check(RelError(ana_dr_dbg, dlog) < 1e-4, buf);
    }

    // —— b_a 的第 axis 轴 ——
    {
      recon::Vec3d ba_p = bias_a, ba_m = bias_a;
      ba_p(axis) += h;
      ba_m(axis) -= h;
      recon::Vec3d dpp, dpm, dvp, dvm;
      recon::Mat3d dRp, dRm;
      IntegrateWithBias(dt, steps, accel_body, gyro_body, bias_g, ba_p, &dpp,
                        &dvp, &dRp);
      IntegrateWithBias(dt, steps, accel_body, gyro_body, bias_g, ba_m, &dpm,
                        &dvm, &dRm);

      const recon::Vec3d num_dv_dba = (dvp - dvm) / (2.0 * h);
      const recon::Vec3d num_dp_dba = (dpp - dpm) / (2.0 * h);

      const recon::Vec3d e(axis == 0 ? 1.0 : 0.0, axis == 1 ? 1.0 : 0.0,
                           axis == 2 ? 1.0 : 0.0);
      const recon::Vec3d ana_dv_dba = pm.d_delta_v_d_ba * e;
      const recon::Vec3d ana_dp_dba = pm.d_delta_p_d_ba * e;

      char buf[160];
      std::snprintf(buf, sizeof(buf),
                    "∂Δv/∂b_a 中心差分一致 (dt=%.3f, ‖ω‖=%.1f, axis=%d)", dt,
                    gyro_body.norm(), axis);
      Check(RelError(ana_dv_dba, num_dv_dba) < 1e-5, buf);

      std::snprintf(buf, sizeof(buf),
                    "∂Δp/∂b_a 中心差分一致 (dt=%.3f, ‖ω‖=%.1f, axis=%d)", dt,
                    gyro_body.norm(), axis);
      Check(RelError(ana_dp_dba, num_dp_dba) < 1e-5, buf);
    }
  }
}

// 一事一验：`Correct()` 在 δb=0 时必须逐位返回原值。
void TestCorrectIdentity() {
  std::printf("[TEST] Correct() 零增量恒等\n");
  recon::ImuPreintegrator pre;
  const recon::Vec3d bg(0.01, -0.02, 0.005);
  const recon::Vec3d ba(0.1, 0.05, -0.08);
  pre.Reset(bg, ba, recon::Vec3d::Zero(), recon::Vec3d::Zero(),
            recon::Vec3d::Zero(), recon::Vec3d::Zero());
  for (int i = 0; i < 20; ++i) {
    const double t = static_cast<double>(i) * 5e6;
    pre.AddSample(t, recon::Vec3d(0.2, -0.3, 9.8), recon::Vec3d(0.1, 0.2, -0.1));
  }
  recon::PreintegratedMeasurement pm;
  pre.Finalize(&pm);

  recon::Vec3d dp, dv;
  recon::Mat3d dR;
  pm.Correct(&dp, &dR, &dv, bg, ba);  // 传入与积分时相同的零偏

  Check((dp - pm.delta_p).norm() < 1e-15, "δb=0 时 Δp 不变");
  Check((dv - pm.delta_v).norm() < 1e-15, "δb=0 时 Δv 不变");
  double dR_err = 0.0;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      dR_err = std::max(dR_err, std::fabs(dR(i, j) - pm.delta_q(i, j)));
    }
  }
  Check(dR_err < 1e-15, "δb=0 时 ΔR 不变");
}

// J-2：∂Δp/∂b_a 不得为零（该项决定尺度可观性）。
void TestScaleObservabilityTerm() {
  std::printf("[TEST] ∂Δp/∂b_a 非零（尺度可观性，J-2）\n");
  const double dt = 0.005;
  const int steps = 20;
  recon::Vec3d bg = recon::Vec3d::Zero();
  recon::Vec3d ba = recon::Vec3d::Zero();
  recon::ImuPreintegrator pre;
  pre.Reset(bg, ba, recon::Vec3d::Zero(), recon::Vec3d::Zero(),
            recon::Vec3d::Zero(), recon::Vec3d::Zero());
  for (int i = 0; i <= steps; ++i) {
    const double t = static_cast<double>(i) * dt * 1e9;
    // 有旋转 + 有比力 → ∂Δp/∂b_a 含 ã 项，非零。
    pre.AddSample(t, recon::Vec3d(0.2, -0.3, 9.8), recon::Vec3d(0.5, 0.1, 0.0));
  }
  recon::PreintegratedMeasurement pm;
  pre.Finalize(&pm);

  // 对角线元素应全部非零（三个零偏轴都对位置有影响）。
  double frob = 0.0;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      frob += pm.d_delta_p_d_ba(i, j) * pm.d_delta_p_d_ba(i, j);
    }
  }
  Check(frob > 1e-8, "∂Δp/∂b_a 的 Frobenius 范数应显著非零");
}

// 小角度数值稳定性（docs/02 §3.5.2 的「必须实现」稳定化）。
//
// 判据：`RightJacobianSO3` 的**非恒等部分** `(J_r − I)` 在 θ ∈ [1e-6, 1e-4]
// 上必须与解析级数一致到相对误差 1e-5 以内。
//
// 为什么判据是「非恒等部分的相对误差」而非「J 的绝对误差」：
// 直接算 `1−cosθ` 在 θ~1e-6 时灾难性相消（`1−cosθ ≈ θ²/2 ~ 1e-12`，
// 两个 ~1 的数相减只剩约 4 位有效数字），使系数 c1 的相对误差达 ~1e-4。
// 但 c1 只经 `−c1·[ω]×` 进入 J，而该块量级 ~0.5θ ~ 5e-7 —— 若拿 J 的
// **绝对误差**衡量（~1e-10，被对角 1 淹没）会误判为"没问题"。真正被污染
// 的是 `J − I` 这块，相对误差 ~2e-4 > 1e-5，正是需要级数展开的原因。
void TestRightJacobianSmallAngle() {
  std::printf("[TEST] 小角度右雅可比数值稳定\n");
  // 覆盖点必须**包含 θ∈(1e-6, 3e-6) 这段窄区间**：旧实现阈值取 1e-12 时，
  // 只有 θ 刚越过 1e-6、`1−cosθ` 的灾难性相消最严重的窄带里，c1 相对误差
  // 才 >1e-5（实测峰值在 θ≈1.4e-6 处约 1.4e-4）。只测 1e-6 与 5e-6 会
  // 恰好跳过该带（前者卡边界走级数、后者误差已降到 8e-8）→ 漏判。
  const double thetas[] = {1e-7, 5e-7, 1e-6,       1.2e-6, 1.5e-6,
                           2.0e-6, 3e-6, 5e-6,      1e-5,   5e-5, 9e-5};
  for (double th : thetas) {
    // ω 沿单位轴，模长 = θ。
    const recon::Vec3d axis = recon::Vec3d(0.3, -0.5, 0.81).normalized();
    const recon::Vec3d omega = axis * th;
    const recon::Mat3d J = recon::RightJacobianSO3(omega);

    // 解析参考（级数，θ 很小时截断误差 ~θ⁴/120 ≈ 1e-26，可作真值）。
    const double th2 = th * th;
    const double c1 = 0.5 - th2 / 24.0;
    const double c2 = 1.0 / 6.0 - th2 / 120.0;
    recon::Mat3d W;
    W(0, 1) = -omega(2);
    W(0, 2) = omega(1);
    W(1, 0) = omega(2);
    W(1, 2) = -omega(0);
    W(2, 0) = -omega(1);
    W(2, 1) = omega(0);
    const recon::Mat3d W2 = W * W;
    const recon::Mat3d ref = recon::Mat3d::Identity() - c1 * W + c2 * W2;

    // 非恒等部分 (J − I) 的 Frobenius 相对误差。
    double num = 0.0, den = 0.0;
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        const double ref_off = ref(i, j) - (i == j ? 1.0 : 0.0);
        const double got_off = J(i, j) - (i == j ? 1.0 : 0.0);
        num += (got_off - ref_off) * (got_off - ref_off);
        den += ref_off * ref_off;
      }
    }
    const double rel = std::sqrt(num / std::max(den, 1e-300));
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "J_r(θ=%.1e) 的非恒等部分相对误差应 < 1e-5 (rel=%.2e)", th,
                  rel);
    Check(rel < 1e-5, buf);
  }
}

}  // namespace

int main() {
  std::printf("=== recon 预积分零偏雅可比测试 ===\n");

  // docs/02 P-1 规定的参数网格。
  const double dts[] = {0.004, 0.005, 0.01};
  const double omegas[] = {0.0, 1.0, 3.0};  // rad/s（沿某轴）

  for (double dt : dts) {
    for (double w : omegas) {
      // 三个轴向的旋转分别测（0 时只有一个方向有意义，但遍历无妨）。
      const recon::Vec3d gyros[] = {
          recon::Vec3d(w, 0.0, 0.0),
          recon::Vec3d(0.0, w, 0.0),
          recon::Vec3d(0.0, 0.0, w),
      };
      for (const recon::Vec3d& gb : gyros) {
        const recon::Vec3d bias_g(0.005, -0.008, 0.003);
        const recon::Vec3d bias_a(0.02, -0.03, 0.015);
        CheckOneCase(dt, gb, 9.8, bias_g, bias_a);
      }
    }
  }

  TestCorrectIdentity();
  TestScaleObservabilityTerm();
  TestRightJacobianSmallAngle();

  if (g_failures == 0) {
    std::printf("\n全部通过\n");
    return 0;
  }
  std::printf("\n失败 %d 项\n", g_failures);
  return 1;
}
