// SPDX-License-Identifier: BSD-3-Clause
//
// 时间轴归一化测试。契约：docs/02_采集与同步.md §3.2 / §3.7 / §3.8。
//
// 覆盖：
//   · FitLineLeastSquares —— 数值内核与退化拒绝；
//   · FitCameraClock      —— 已知速率偏差下的 α 恢复、5σ 剔除、置信度分级、漂移拒用；
//   · NormalizeCamera     —— 恒等回落、单调性倒退、帧间隔跳变与丢帧估算；
//   · NormalizeImu        —— 减原点 + 倒退检测；
//   · Resync              —— 重锚定的**延续性**（重锚定后新平台时刻映射到上次 session 时刻）。

#include "recon/time_sync.h"

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

void CheckNear(double a, double b, double tol, const char* what) {
  if (std::fabs(a - b) > tol) {
    std::printf("  [FAIL] %s (got %.9g, want %.9g, tol %.3g)\n", what, a, b, tol);
    ++g_failures;
  }
}

// 确定性伪随机（避免依赖 <random> 的实现差异，测试可复现）。
struct Lcg {
  uint64_t s;
  explicit Lcg(uint64_t seed) : s(seed) {}
  // [-1, 1) 之间的双精度噪声。
  double Sym() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const double u = static_cast<double>((s >> 11) & ((1ULL << 53) - 1)) /
                     static_cast<double>(1ULL << 53);  // [0,1)
    return 2.0 * u - 1.0;
  }
};

using recon::AffineFit;
using recon::ClockPair;
using recon::FitLineLeastSquares;
using recon::TimeAxis;

constexpr int64_t kNsPerMs = 1000000LL;

// 构造相机时钟样本：deliver = alpha·raw + beta + noise。
std::vector<ClockPair> MakePairs(double alpha, double beta, int n,
                                 double noise_ns, Lcg* rng) {
  std::vector<ClockPair> pairs;
  pairs.reserve(static_cast<size_t>(n));
  const int64_t dt = 33 * kNsPerMs;  // 30 fps
  for (int k = 0; k < n; ++k) {
    const int64_t raw = k * dt;
    const double d =
        alpha * static_cast<double>(raw) + beta + noise_ns * rng->Sym();
    ClockPair p;
    p.raw_ns = raw;
    p.deliver_ns = static_cast<int64_t>(d);
    pairs.push_back(p);
  }
  return pairs;
}

// ---------------------------------------------------------------------------
// 最小二乘内核
// ---------------------------------------------------------------------------

void TestFitLine() {
  std::printf("[TEST] FitLineLeastSquares\n");
  double a = 0.0, b = 0.0;

  // 精确直线 y = 3x + 7。
  {
    std::vector<double> x = {0, 1, 2, 3, 4};
    std::vector<double> y = {7, 10, 13, 16, 19};
    Check(FitLineLeastSquares(x, y, &a, &b), "精确直线应拟合成功");
    CheckNear(a, 3.0, 1e-12, "斜率应精确");
    CheckNear(b, 7.0, 1e-12, "截距应精确");
  }
  // 样本不足。
  {
    std::vector<double> x = {1.0};
    std::vector<double> y = {2.0};
    Check(!FitLineLeastSquares(x, y, &a, &b), "单样本应被拒绝");
  }
  // x 方差退化（所有 x 相同）。
  {
    std::vector<double> x = {5, 5, 5, 5};
    std::vector<double> y = {1, 2, 3, 4};
    Check(!FitLineLeastSquares(x, y, &a, &b), "x 退化应被拒绝");
  }
  // 长度不等取较小者。
  {
    std::vector<double> x = {0, 1, 2, 3, 4};
    std::vector<double> y = {0, 2, 4};
    Check(FitLineLeastSquares(x, y, &a, &b), "长度不等应取公共前缀");
    CheckNear(a, 2.0, 1e-12, "长度不等时斜率应对");
  }
}

// ---------------------------------------------------------------------------
// 相机时钟仿射拟合
// ---------------------------------------------------------------------------

void TestFitCameraClockRateRecovery() {
  std::printf("[TEST] FitCameraClock 速率恢复（各档 ppm）\n");
  const double drift_ppm[] = {0.0, 10.0, 50.0, 80.0};
  for (double ppm : drift_ppm) {
    const double alpha_true = 1.0 + ppm * 1e-6;
    Lcg rng(1234u);
    // 干净场景：噪声 200 ns（远小于 10 µs HIGH 门限），足以让 MAD > 0。
    const auto pairs = MakePairs(alpha_true, 0.0, 240, 200.0, &rng);

    TimeAxis axis;
    const AffineFit fit = axis.FitCameraClock(pairs);
    Check(fit.ok, "拟合应成功");
    Check(fit.status == RECON_OK, "低漂移不应报错");
    CheckNear(fit.alpha, alpha_true, 1e-7, "α 恢复应准确");
    CheckNear(fit.drift_ppm, ppm, 1.0, "ppm 估计应准确");
    Check(fit.rejected <= 2, "干净数据不应大量剔除");
    // 240 帧拟合成功。
    Check(fit.used >= 230, "参与拟合样本数应接近 240");
    // 残差 ~200 ns RMS → HIGH（≤ 10 µs）。
    Check(fit.confidence == AffineFit::kHigh, "低噪声应判 HIGH");
    // 拟合成功后时间轴应被自动配置，且 Map 与 α 一致。
    Check(axis.camera_configured(), "拟合后应自动配置时间轴");
    CheckNear(axis.camera_alpha(), alpha_true, 1e-7, "时间轴 α 应同步");
  }
}

void TestFitCameraClockRejectsDrift() {
  std::printf("[TEST] FitCameraClock 漂移拒用（≥ 100 ppm）\n");
  const double alpha_true = 1.0 + 200e-6;  // 200 ppm
  Lcg rng(99u);
  const auto pairs = MakePairs(alpha_true, 0.0, 240, 200.0, &rng);

  TimeAxis axis;
  const AffineFit fit = axis.FitCameraClock(pairs);
  Check(fit.ok, "拟合本身应成功（数值上可解）");
  Check(fit.drift_ppm >= 100.0, "应识别为超拒用线");
  Check(fit.status == RECON_ERR_SYNC_CLOCK_DRIFT, "应返回漂移错误码");
}

void TestFitCameraClockOutlierRejection() {
  std::printf("[TEST] FitCameraClock 5σ 剔除\n");
  Lcg rng(7u);
  auto pairs = MakePairs(1.0, 0.0, 120, 100.0, &rng);
  // 注入 6 个粗大离群点（+0.1 s）。
  for (int i = 0; i < 6; ++i) {
    const size_t idx = static_cast<size_t>(i * 20 + 5);
    pairs[idx].deliver_ns += 100 * kNsPerMs;
  }

  TimeAxis axis;
  const AffineFit fit = axis.FitCameraClock(pairs);
  Check(fit.ok, "含离群点仍应拟合成功");
  Check(fit.rejected >= 6, "6 个离群点应全部被剔除");
  Check(fit.used <= 114, "剔除后样本数应下降");
  CheckNear(fit.alpha, 1.0, 1e-6, "剔除后 α 应恢复准确");
  // 剔除后残差应回到噪声量级 → HIGH。
  Check(fit.confidence == AffineFit::kHigh, "剔净离群点后应判 HIGH");
}

void TestFitCameraClockConfidenceTiers() {
  std::printf("[TEST] FitCameraClock 置信度分级\n");
  // MEDIUM：噪声 ~ 400 µs RMS（介于 10 µs 与 1 ms）。
  {
    Lcg rng(31u);
    const auto pairs = MakePairs(1.0, 0.0, 240, 700.0 * 1000.0, &rng);
    TimeAxis axis;
    const AffineFit fit = axis.FitCameraClock(pairs);
    Check(fit.residual_rms_ns > 1e4 && fit.residual_rms_ns <= 1e6,
          "残差应落在 MEDIUM 区间");
    Check(fit.confidence == AffineFit::kMedium, "中等噪声应判 MEDIUM");
    Check(fit.status == RECON_OK, "中等噪声不应报错");
  }
  // LOW：噪声 ~ 20 ms。
  {
    Lcg rng(53u);
    const auto pairs = MakePairs(1.0, 0.0, 240, 30.0 * kNsPerMs, &rng);
    TimeAxis axis;
    const AffineFit fit = axis.FitCameraClock(pairs);
    Check(fit.residual_rms_ns > 1e6, "残差应超过 1 ms");
    Check(fit.confidence == AffineFit::kLow, "大噪声应判 LOW");
  }
}

void TestFitCameraClockInsufficient() {
  std::printf("[TEST] FitCameraClock 样本不足\n");
  TimeAxis axis;
  std::vector<ClockPair> none;
  const AffineFit f0 = axis.FitCameraClock(none);
  Check(!f0.ok, "零样本应失败");
  Check(f0.status == RECON_ERR_SYNC_CALIBRATION_MISSING, "应报标定缺失");

  std::vector<ClockPair> one = {{0, 0}};
  const AffineFit f1 = axis.FitCameraClock(one);
  Check(!f1.ok, "单样本应失败");
}

// ---------------------------------------------------------------------------
// 归一化
// ---------------------------------------------------------------------------

void TestNormalizeFallbackAndMonotonic() {
  std::printf("[TEST] NormalizeCamera 回落与单调性\n");

  // 未配置映射：恒等回落 + 计一次 fallback。
  {
    TimeAxis axis;
    axis.SetSessionOrigin(1000);
    int64_t out = -1;
    const recon_status st = axis.NormalizeCamera(5000, &out);
    Check(st == RECON_OK, "未配置时应回落恒等并返回 OK");
    Check(out == 4000, "恒等回落 = raw − origin");
    Check(axis.stats().cam_ts_fallback == 1, "应计一次 fallback");
    Check(axis.stats().frames_normalized == 1, "应计一次归一化");
  }

  // 配置后前进正常。
  {
    TimeAxis axis;
    axis.SetSessionOrigin(0);
    axis.ConfigureCamera(1.0, 0.0, AffineFit::kHigh);
    int64_t out = -1;
    Check(axis.NormalizeCamera(1000, &out) == RECON_OK && out == 1000, "首帧");
    Check(axis.NormalizeCamera(2000, &out) == RECON_OK && out == 2000, "前进帧");
    // 倒退 → 错误码 + 计数。
    Check(axis.NormalizeCamera(1500, &out) == RECON_ERR_SYNC_TIME_NONMONOTONIC,
          "倒退应返回非单调码");
    Check(axis.stats().time_backward == 1, "应计一次倒退");
    // 倒退帧不应推进 last（下一次 2200 仍算前进）。
    Check(axis.NormalizeCamera(2200, &out) == RECON_OK && out == 2200,
          "倒退后应能继续前进");
    // 空指针守卫。
    Check(axis.NormalizeCamera(3000, nullptr) == RECON_ERR_INVALID_ARGUMENT,
          "空输出指针应被拒");
  }
}

void TestNormalizeGapAndDrop() {
  std::printf("[TEST] NormalizeCamera 跳变与丢帧\n");
  TimeAxis axis;
  axis.SetSessionOrigin(0);
  axis.ConfigureCamera(1.0, 0.0, AffineFit::kHigh);
  axis.SetNominalFrameRate(30.0);  // nominal_dt ≈ 33.3 ms，gap 阈值 = 10/fps ≈ 333 ms

  int64_t out = -1;
  Check(axis.NormalizeCamera(0, &out) == RECON_OK, "首帧");
  // 正常间隔 33 ms：不算跳变。
  Check(axis.NormalizeCamera(33 * kNsPerMs, &out) == RECON_OK, "正常帧");
  Check(axis.stats().frame_gaps == 0, "正常间隔不应计跳变");

  // 400 ms 缺口 → 跳变；lost = 400/33.33 − 1 = 11。
  Check(axis.NormalizeCamera(433 * kNsPerMs, &out) == RECON_OK, "跳变后帧");
  Check(axis.stats().frame_gaps == 1, "应计一次跳变");
  Check(axis.stats().frames_dropped >= 10, "应估算出丢帧");
}

void TestNormalizeImu() {
  std::printf("[TEST] NormalizeImu\n");
  TimeAxis axis;
  axis.SetSessionOrigin(1 * kNsPerMs);
  int64_t out = -1;
  Check(axis.NormalizeImu(2 * kNsPerMs, &out) == RECON_OK, "首样本");
  Check(out == 1 * kNsPerMs, "应减去 origin");
  Check(axis.NormalizeImu(3 * kNsPerMs, &out) == RECON_OK, "前进样本");
  Check(axis.NormalizeImu(2 * kNsPerMs, &out) == RECON_ERR_SYNC_IMU_GAP,
        "IMU 倒退应返回错误码");
  Check(axis.stats().imu_backward == 1, "应计一次 IMU 倒退");
  Check(axis.NormalizeImu(4 * kNsPerMs, nullptr) == RECON_ERR_INVALID_ARGUMENT,
        "空输出指针应被拒");
}

// ---------------------------------------------------------------------------
// 重锚定（关键：延续性）
// ---------------------------------------------------------------------------

void TestResyncContinuity() {
  std::printf("[TEST] Resync 延续性\n");

  // α = 1 情形：重锚定后，新平台时刻应映射到**上一次 session 时刻**（无台阶）。
  {
    TimeAxis axis;
    axis.SetSessionOrigin(0);
    axis.ConfigureCamera(1.0, 0.0, AffineFit::kHigh);
    int64_t out = -1;
    Check(axis.NormalizeCamera(5000, &out) == RECON_OK, "锚定前帧");
    const int64_t before = out;

    const recon_status rs = axis.Resync(1 * kNsPerMs * 1000);  // 新原点 +1000 s
    Check(rs == RECON_OK, "重锚定是成功的恢复动作，返回 OK");
    Check(axis.stats().reanchor == 1, "应计一次重锚定");

    // 重锚定后立即以新平台时刻取帧 → 应等于重锚定前的 session 时刻。
    Check(axis.NormalizeCamera(1 * kNsPerMs * 1000, &out) == RECON_OK,
          "重锚定后首帧");
    Check(out == before, "重锚定应保持时间轴连续（无台阶）");
  }

  // α ≠ 1 情形：验证 β 修正公式 β_new = last + (1−α)·O_new。
  {
    TimeAxis axis;
    axis.SetSessionOrigin(0);
    axis.ConfigureCamera(1.1, 0.0, AffineFit::kHigh);
    int64_t out = -1;
    Check(axis.NormalizeCamera(1000, &out) == RECON_OK, "锚定前帧");
    const int64_t before = out;  // = 1100

    axis.Resync(2000);
    Check(axis.NormalizeCamera(2000, &out) == RECON_OK, "重锚定后首帧");
    Check(out == before, "α≠1 时重锚定仍应保持连续");
  }
}

}  // namespace

int main() {
  std::printf("=== recon 时间轴归一化测试 ===\n");
  TestFitLine();
  TestFitCameraClockRateRecovery();
  TestFitCameraClockRejectsDrift();
  TestFitCameraClockOutlierRejection();
  TestFitCameraClockConfidenceTiers();
  TestFitCameraClockInsufficient();
  TestNormalizeFallbackAndMonotonic();
  TestNormalizeGapAndDrop();
  TestNormalizeImu();
  TestResyncContinuity();

  if (g_failures == 0) {
    std::printf("\n全部通过\n");
    return 0;
  }
  std::printf("\n失败 %d 项\n", g_failures);
  return 1;
}
