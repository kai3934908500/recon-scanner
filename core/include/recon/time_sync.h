// SPDX-License-Identifier: BSD-3-Clause
//
// 时间轴归一化（多时钟源 → 统一 session 时间轴）。
//
// 契约：docs/02_采集与同步.md §3.2 / §3.7 / §3.8。
//
// 背景：手机端每类传感器（相机 / IMU / 平台）各自带一个独立时钟源，
// 采样时刻、单位、时基都不同。核心算法需要一个**统一、单调、以会话开始
// 为原点**的时间轴，才能把 IMU 预积分与相机帧正确配对。
//
// 本模块只做「时间轴」这件事，不碰位姿/几何：
//   1. 仿射映射   T_session = α · T_raw + β（最小二乘拟合，含 5σ 剔除重拟合）；
//   2. 归一化     把平台时间戳映射到 session 纳秒（单调性检查）；
//   3. 异常检测   倒退 / 跳变 / 丢帧计数（§3.8.1–§3.8.3）；
//   4. 长期健康度 IMU 采样率偏差与抖动（§3.8.4，可选）。
//
// 数值口径（全部用 double，输入输出为 int64 纳秒）：
//   · 时间戳统一以**纳秒整数**进出，内部用 double 做拟合（长期会话 < 2^53 ns
//     ≈ 104 天，double 的 53 位尾数足够精确到 ns，无溢出风险）。
//   · α 为**无量纲速率比**（正常 ≈ 1），β 为**纳秒偏移**。

#ifndef RECON_CORE_TIME_SYNC_H_
#define RECON_CORE_TIME_SYNC_H_

#include <cstdint>
#include <vector>

#include "recon/recon_c_api.h"

namespace recon {

// 单条「原始时间戳 → 送达时刻」配对样本。
// 相机时间源拟合用：raw = 平台给的时间戳，deliver = 收到帧的单调时钟时刻。
struct ClockPair {
  int64_t raw_ns = 0;      // 传感器/平台原始时间戳
  int64_t deliver_ns = 0;  // 本机单调时钟（弱观测，含交付抖动）
};

// 仿射映射拟合结果。
struct AffineFit {
  bool ok = false;
  recon_status status = RECON_OK;

  double alpha = 1.0;  // 速率比（无量纲）
  double beta = 0.0;   // 偏移（纳秒）

  int used = 0;            // 最终参与拟合的样本数
  int rejected = 0;        // 被 5σ 剔除的样本数
  double residual_rms_ns = 0.0;  // 拟合残差 RMS（纳秒）

  // 置信度（docs/02 §3.2）：residual_rms ≤ 1e-5 s → HIGH；≤ 1e-3 s → MEDIUM；否则 LOW。
  enum Confidence { kHigh = 0, kMedium = 1, kLow = 2 } confidence = kLow;

  // 速率偏差 abs(α−1)，单位 ppm（1e-6）。
  double drift_ppm = 0.0;

  // 时间戳映射：T_session = α·T_raw + β。
  int64_t Map(int64_t raw_ns) const {
    return static_cast<int64_t>(alpha * static_cast<double>(raw_ns) + beta);
  }
};

// 时间轴归一化器。
//
// 典型用法：
//   TimeAxis axis;
//   axis.SetSessionOrigin(first_platform_ts_ns);
//   axis.FitCameraClock(pairs);                       // 相机会话时钟可解算时
//   axis.ConfigureCamera(/*alpha=*/1.0, /*beta=*/0.0, AffineFit::kHigh);  // 已同源
//   int64_t t_session;
//   axis.NormalizeCamera(raw_ts_ns, &t_session);
class TimeAxis {
 public:
  TimeAxis() = default;

  // 会话时间原点：第一帧/首个 IMU 样本的平台时刻，映射后为 0。
  void SetSessionOrigin(int64_t platform_ts_ns) { session_origin_ns_ = platform_ts_ns; }
  int64_t session_origin_ns() const { return session_origin_ns_; }

  // 直接用已知的仿射参数配置相机时间源（同源场景：α=1, β=0，HIGH）。
  void ConfigureCamera(double alpha, double beta, AffineFit::Confidence conf);

  // 由配对样本拟合相机时钟（docs/02 §3.2.2）：
  //   1. 取最近 sample_limit 帧；2. 最小二乘；3. 剔除残差 > sigma_kill·σ 后重拟合
  //   （最多 refit_rounds 轮）；4. 更新 α, β 与置信度。
  // 注意：deliver 是弱观测，拟合出的 α 是有偏估计；真实速率由长期互相关复核
  // （§3.8.4，本模块暂不实现）。
  AffineFit FitCameraClock(const std::vector<ClockPair>& pairs,
                           int sample_limit = 240, double sigma_kill = 5.0,
                           int refit_rounds = 3);

  // 归一化一条相机（或平台）时间戳到 session 时间。
  //
  // 成功：out_session_ns 写回，返回 RECON_OK。
  // 单条轻度倒退：返回 RECON_ERR_SYNC_TIME_NONMONOTONIC，不计入。
  // 未配置映射：用恒等映射 + 原点（α=1,β=0），返回 RECON_OK 但计一次 fallback。
  //
  // `enforce_monotonic`：置 true 时对相机帧做倒退检测（§3.8.1）。
  recon_status NormalizeCamera(int64_t raw_or_platform_ns, int64_t* out_session_ns,
                               bool enforce_monotonic = true);

  // 归一化一条 IMU 时间戳（无仿射，只减原点；做倒退检测）。
  recon_status NormalizeImu(int64_t raw_ns, int64_t* out_session_ns);

  // 重锚定：用于 AP 息屏恢复 / 严重跳变后（§3.8.1 severe、§3.4.1 步骤 3）。
  // 传入新的原点（当前平台时刻），按「延续性」修正 β（保证时间轴无台阶），
  // 并清空单调性基线。
  //
  // 返回 RECON_OK（重锚定是成功的恢复动作）；调用方据 stats().reanchor 增量
  // 决定是否清空预积分窗口（§3.8.1）。
  recon_status Resync(int64_t new_platform_ts_ns);

  // ---- 统计（供 UI / 遥测 / 会话记录）----
  struct Stats {
    uint64_t frames_normalized = 0;
    uint64_t time_backward = 0;      // 相机时间戳倒退次数
    uint64_t imu_backward = 0;       // IMU 时间戳倒退次数
    uint64_t cam_ts_fallback = 0;    // 未配置映射，回落恒等映射
    uint64_t reanchor = 0;           // 重锚定次数
    uint64_t frame_gaps = 0;         // 帧间隔跳变次数
    uint64_t frames_dropped = 0;     // 估算丢帧数
  };
  const Stats& stats() const { return stats_; }
  void ResetStats() { stats_ = Stats{}; }

  // 当前相机仿射参数（未配置时为恒等）。
  double camera_alpha() const { return cam_alpha_; }
  double camera_beta() const { return cam_beta_; }
  bool camera_configured() const { return cam_configured_; }

  // 帧率提示（用于 §3.8.2 跳变判定「10 × 1/fps」与 §3.8.3 丢帧估算）。
  void SetNominalFrameRate(double fps) { nominal_fps_ = (fps > 0.0 ? fps : nominal_fps_); }
  double nominal_frame_rate() const { return nominal_fps_; }

 private:
  int64_t session_origin_ns_ = 0;

  double cam_alpha_ = 1.0;
  double cam_beta_ = 0.0;
  bool cam_configured_ = false;
  AffineFit::Confidence cam_conf_ = AffineFit::kLow;

  int64_t last_cam_session_ns_ = 0;
  bool has_last_cam_ = false;

  int64_t last_imu_session_ns_ = 0;
  bool has_last_imu_ = false;

  double nominal_fps_ = 30.0;
  Stats stats_;
};

// 最小二乘直线拟合 y = a·x + b（用于相机时钟仿射拟合的数值内核）。
// 返回 false 表示样本不足（< 2）或 x 方差退化（所有 x 相同）。
bool FitLineLeastSquares(const std::vector<double>& x, const std::vector<double>& y,
                         double* out_a, double* out_b);

}  // namespace recon

#endif  // RECON_CORE_TIME_SYNC_H_
