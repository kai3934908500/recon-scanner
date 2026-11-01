// SPDX-License-Identifier: BSD-3-Clause
//
// 时间轴归一化实现。
//
// 契约：docs/02_采集与同步.md §3.2（仿射映射）/ §3.7（误差预算）/ §3.8（异常检测）。
//
// 设计要点：
//   · 拟合在 double 域做（int64 ns 先转 double），精度足够（见头文件说明）；
//   · 5σ 剔除「逐轮重拟合」而非一次性删除，避免一次坏点带偏整体；
//   · 倒退/跳变检测只记统计、不抛异常（采集回调不能被打断）；
//   · 所有返回码取自头文件已定义的 RECON_ERR_SYNC_*（当前 4 个）。

#include "recon/time_sync.h"

#include <algorithm>
#include <cmath>

namespace recon {
namespace {

// 置信度判定阈值（docs/02 §3.2）：残差 RMS 单位秒。
constexpr double kHighResidualS = 1e-5;   // 10 µs
constexpr double kMediumResidualS = 1e-3; // 1 ms

// §3.8.1 倒退阈值：轻度 ≤ 100 ms，超过视为严重。
constexpr int64_t kBackwardMildNs = 100LL * 1000 * 1000;

// §3.8.2 跳变：帧间隔 > max(10/fps, 50 ms) 视为跳变。
constexpr int64_t kGapFloorNs = 50LL * 1000 * 1000;

AffineFit::Confidence ClassifyConfidence(double residual_rms_ns) {
  const double rms_s = residual_rms_ns * 1e-9;
  if (rms_s <= kHighResidualS) return AffineFit::kHigh;
  if (rms_s <= kMediumResidualS) return AffineFit::kMedium;
  return AffineFit::kLow;
}

}  // namespace

bool FitLineLeastSquares(const std::vector<double>& x, const std::vector<double>& y,
                         double* out_a, double* out_b) {
  const size_t n = std::min(x.size(), y.size());
  if (n < 2) return false;

  double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
  for (size_t i = 0; i < n; ++i) {
    sx += x[i];
    sy += y[i];
    sxx += x[i] * x[i];
    sxy += x[i] * y[i];
  }
  const double dn = static_cast<double>(n);
  const double mean_x = sx / dn;
  const double var_x = sxx / dn - mean_x * mean_x;
  // x 方差退化（所有 x 相同）→ 无法定斜率。
  if (var_x <= 0.0 || !std::isfinite(var_x)) return false;

  const double cov_xy = sxy / dn - mean_x * (sy / dn);
  const double a = cov_xy / var_x;
  const double b = (sy / dn) - a * mean_x;
  if (!std::isfinite(a) || !std::isfinite(b)) return false;

  *out_a = a;
  *out_b = b;
  return true;
}

// ---------------------------------------------------------------------------
// TimeAxis
// ---------------------------------------------------------------------------

void TimeAxis::ConfigureCamera(double alpha, double beta,
                               AffineFit::Confidence conf) {
  cam_alpha_ = alpha;
  cam_beta_ = beta;
  cam_configured_ = true;
  cam_conf_ = conf;
}

AffineFit TimeAxis::FitCameraClock(const std::vector<ClockPair>& pairs,
                                   int sample_limit, double sigma_kill,
                                   int refit_rounds) {
  AffineFit result;

  // 1. 取最近的 sample_limit 条。
  std::vector<ClockPair> work;
  const size_t take = std::min(pairs.size(),
                               static_cast<size_t>(std::max(2, sample_limit)));
  if (take < 2) {
    result.status = RECON_ERR_SYNC_CALIBRATION_MISSING;
    return result;
  }
  work.assign(pairs.end() - take, pairs.end());

  // 2. 逐轮 5σ 剔除 + 重拟合。
  int round = 0;
  double a = 1.0, b = 0.0;
  double rms = 0.0;
  int rejected_total = 0;

  while (true) {
    std::vector<double> xs, ys;
    xs.reserve(work.size());
    ys.reserve(work.size());
    for (const auto& p : work) {
      xs.push_back(static_cast<double>(p.raw_ns));
      ys.push_back(static_cast<double>(p.deliver_ns));
    }
    if (!FitLineLeastSquares(xs, ys, &a, &b)) {
      result.status = RECON_ERR_SYNC_CALIBRATION_MISSING;
      return result;
    }

    // 残差
    std::vector<double> resid(work.size());
    double sum_sq = 0.0;
    for (size_t i = 0; i < work.size(); ++i) {
      resid[i] = ys[i] - (a * xs[i] + b);
      sum_sq += resid[i] * resid[i];
    }
    rms = std::sqrt(sum_sq / static_cast<double>(work.size()));

    if (round >= refit_rounds || work.size() <= 3 || rms <= 0.0) break;

    // σ 估计：用平均绝对偏差推 sigma（避免被坏点抬高 RMS 自洽）。
    // sigma ≈ 1.4826 × MAD，稳健。
    std::vector<double> absdev(work.size());
    for (size_t i = 0; i < work.size(); ++i) absdev[i] = std::fabs(resid[i]);
    std::vector<double> sorted = absdev;
    std::sort(sorted.begin(), sorted.end());
    const double mad = sorted[sorted.size() / 2];
    const double sigma = 1.4826 * mad;
    if (sigma <= 0.0) break;

    const double thr = sigma_kill * sigma;
    std::vector<ClockPair> kept;
    kept.reserve(work.size());
    int removed = 0;
    for (size_t i = 0; i < work.size(); ++i) {
      if (std::fabs(resid[i]) <= thr) {
        kept.push_back(work[i]);
      } else {
        ++removed;
      }
    }
    if (removed == 0) break;  // 无新剔除，收敛
    // 剔除后若剩余样本过少（< 3），保留上一轮的稳健拟合结果，
    // 不让「过度剔除」把一次良好的拟合退化成 MISSING。
    if (kept.size() < 3) break;
    rejected_total += removed;
    work.swap(kept);
    ++round;
  }

  result.ok = true;
  result.status = RECON_OK;
  result.alpha = a;
  result.beta = b;
  result.used = static_cast<int>(work.size());
  result.rejected = rejected_total;
  result.residual_rms_ns = rms;
  result.confidence = ClassifyConfidence(rms);
  result.drift_ppm = std::fabs(a - 1.0) * 1e6;

  // 判定 §3.2.1：≥ 100 ppm 拒用该时间戳。
  if (result.drift_ppm >= 100.0) {
    result.status = RECON_ERR_SYNC_CLOCK_DRIFT;
  }

  // 拟合成功后自动配置时间轴。
  ConfigureCamera(result.alpha, result.beta, result.confidence);
  return result;
}

recon_status TimeAxis::NormalizeCamera(int64_t raw_or_platform_ns,
                                       int64_t* out_session_ns,
                                       bool enforce_monotonic) {
  if (out_session_ns == nullptr) return RECON_ERR_INVALID_ARGUMENT;

  if (!cam_configured_) {
    ++stats_.cam_ts_fallback;
    cam_alpha_ = 1.0;
    cam_beta_ = 0.0;
  }

  const int64_t t_session =
      static_cast<int64_t>(cam_alpha_ * static_cast<double>(raw_or_platform_ns) +
                           cam_beta_ - static_cast<double>(session_origin_ns_));

  if (enforce_monotonic && has_last_cam_ && t_session <= last_cam_session_ns_) {
    ++stats_.time_backward;
    // 倒退帧丢弃，返回错误码（调用方按 §3.8.1 处理）：
    // 轻度 = 跳过该帧；严重（> 100 ms）由调用方触发 Resync。
    return RECON_ERR_SYNC_TIME_NONMONOTONIC;
  }

  // 帧间隔跳变 / 丢帧估算（§3.8.2 / §3.8.3）。
  if (has_last_cam_ && nominal_fps_ > 0.0) {
    const int64_t dt = t_session - last_cam_session_ns_;
    const int64_t gap_thr = std::max<int64_t>(
        static_cast<int64_t>(10.0 * 1e9 / nominal_fps_), kGapFloorNs);
    if (dt > gap_thr) {
      ++stats_.frame_gaps;
      const int64_t nominal_dt =
          static_cast<int64_t>(1e9 / nominal_fps_);
      if (nominal_dt > 0) {
        const int64_t lost = dt / nominal_dt - 1;
        if (lost > 0) stats_.frames_dropped += static_cast<uint64_t>(lost);
      }
    }
  }

  last_cam_session_ns_ = t_session;
  has_last_cam_ = true;
  ++stats_.frames_normalized;
  *out_session_ns = t_session;
  return RECON_OK;
}

recon_status TimeAxis::NormalizeImu(int64_t raw_ns, int64_t* out_session_ns) {
  if (out_session_ns == nullptr) return RECON_ERR_INVALID_ARGUMENT;

  const int64_t t_session = raw_ns - session_origin_ns_;
  if (has_last_imu_ && t_session <= last_imu_session_ns_) {
    ++stats_.imu_backward;
    return RECON_ERR_SYNC_IMU_GAP;  // 头文件已定义的最近语义错误码
  }
  last_imu_session_ns_ = t_session;
  has_last_imu_ = true;
  *out_session_ns = t_session;
  return RECON_OK;
}

recon_status TimeAxis::Resync(int64_t new_platform_ts_ns) {
  // 以「延续性」为锚点（§3.8.1 严重倒退 / §3.4.1 步骤 3）：
  // 重锚定后，新的平台时刻应映射到**上一次已输出的 session 时间**，
  // 从而不产生向后台阶。由 t(O) = α·O + β − O = last 得：
  //     β_new = last + (1 − α)·O_new
  // α = 1 时退化为 β_new = last（纯平移，最常用情形）。
  // 注意：β 与 session_origin_ 是**两个独立量**——β 是仿射偏移，
  // origin 是「映射后为 0 的平台时刻」；归一化式为 α·raw + β − origin。
  // 二者必须用同一公式保持一致，否则会出现二次扣减（历史缺陷）。
  const double last_session =
      has_last_cam_ ? static_cast<double>(last_cam_session_ns_) : 0.0;
  session_origin_ns_ = new_platform_ts_ns;
  cam_beta_ =
      last_session + (1.0 - cam_alpha_) * static_cast<double>(new_platform_ts_ns);
  // 清空单调性基线（重锚定后允许时间轴出现台阶）。
  has_last_cam_ = false;
  has_last_imu_ = false;
  ++stats_.reanchor;
  // 重锚定是**成功的恢复动作**，返回 OK；调用方据 stats().reanchor 增量
  // 决定是否需要「清预积分窗口」（docs/02 §3.8.1 严重倒退处理）。
  // **不**复用无关错误码（如 CLOCK_DRIFT）充当「已重锚」信号。
  return RECON_OK;
}

}  // namespace recon
