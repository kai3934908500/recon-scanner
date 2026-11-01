// SPDX-License-Identifier: BSD-3-Clause
//
// imu_window.h — IMU 采样环形缓冲与「按帧时间戳切出此前窗口」的平台无关实现。
//
// 契约：
//   · core/include/recon/recon_c_api.h —— `recon_imu_sample`
//     （`accel` = **比力，含重力**，m/s²；`gyro` = rad/s；`timestamp_ns` 单调纳秒）
//     与 `recon_frame_packet.imu` 的语义：「**本帧之前**的 IMU 时间窗」。
//   · docs/02_采集与同步.md §3.5（IMU 预积分，核心层自行减重力）
//                          §3.8（IMU 缺口容忍；时间戳倒序为须检出异常）
//   · docs/11_平台权限与集成.md §3.1.5（CoreMotion / SensorManager 对接）
//                              §4.3（时间戳对齐）
//
// ── 为什么放在 platform/common，而不是双端各写一份 ──────────────────────────
//
// 铁律：**同一逻辑写两遍必然漂移**。CoreMotion（iOS）与 SensorManager（Android）
// 的回调频率、时间基、队列模型确实不同，但「按帧时间戳切出此前窗口」这条
// **规则本身是同一件事**。若双端各写一份，任何一侧的边界写法漂移
// （`<=` 写成 `<`）都会**静默**改变喂给内核的 IMU 区间 —— 而该区间直接决定
// VIO 预积分的输入，属于「不报错但结果偏」的最难查一类。
//
// 故以**一份 C++17 实现**承载，两端各自薄封装（Objective-C++ / JNI），
// 桌面也能编译并单测 —— 与既有先例 `platform/common/include/recon_platform/
// capture_state.h`（采集状态机）完全同构。
//
// ── accel 语义（重要）──────────────────────────────────────────────────────
//
// `recon_imu_sample.accel` 是**比力（含重力）**，核心层自行减重力
// （docs/02 §3.5 `a_m = R(a_W − g_W)`）。iOS 的 `CMDeviceMotion` 把加速度拆成
// `gravity + userAcceleration`（后者**已去重力**），相加才是含重力原始值；
// Android 的 `TYPE_ACCELEROMETER` 本身即含重力。
//
// 本容器**只做缓冲与切窗**，不对 `accel` 做任何变换 —— 是否已含重力、按哪种
// 传感器口径取值，是各端采集代码的职责，并由 docs/14 §5.1.3 缺口 H 跟踪。
//
// ── 类型真源 ───────────────────────────────────────────────────────────────
//
// 本头**直接使用** C ABI 的 `recon_imu_sample`，**不另定义**同形结构体。
// 理由：另定义一份就是「第二事实来源」，需要额外的字段布局对等闸门去守；
// 结构上消灭它比事后加闸门更可靠（MEMORY.md 第十三轮教训）。
//
// 因此本目标（`recon_platform_common`）**只 include core 的 C ABI 头以取得
// POD 类型，绝不调用任何 `RECON_API` 函数** —— 那是各端桥接层的职责。
// 该边界由 `tools/check_platform_parity.py` 的闸门 8 静态守住（iOS 侧只 link
// `recon_platform_common`、不 link `recon_core`，一旦误调即链接失败）。

#ifndef RECON_PLATFORM_IMU_WINDOW_H_
#define RECON_PLATFORM_IMU_WINDOW_H_

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "recon/recon_c_api.h"

namespace recon {
namespace platform {

/// 一帧的 IMU 时间窗（按 `timestamp_ns` **升序**）。
struct ImuWindow {
  /// 已升序排列的采样。空表示该窗口内无样本。
  std::vector<recon_imu_sample> samples;

  bool empty() const { return samples.empty(); }
  std::size_t count() const { return samples.size(); }

  /// 窗口内首末采样的时间跨度（纳秒）。用于诊断 docs/02 §3.8 的 IMU 缺口。
  /// 窗口不足 2 个样本时返回 0。
  int64_t span_ns() const {
    if (samples.size() < 2) return 0;
    return samples.back().timestamp_ns - samples.front().timestamp_ns;
  }
};

/// IMU 采样环形缓冲。**线程安全**（`Append` 与 `WindowBefore` 可跨队列并发）。
///
/// 容量上限的存在理由：若相机长时间不来帧（例如用户把手机放下），IMU 仍以
/// 100 Hz 持续灌入；不设上限则内存无界增长。上限按 docs/02 §3.8 的 IMU 缺口
/// 容忍推导：默认 512 样本 ≈ 100 Hz 下 5.1 s，足以覆盖任何正常帧间隔；
/// 超过则说明帧流已实质中断（应由采集状态机处理，而非靠缓冲兜底）。
class ImuRingBuffer {
 public:
  /// 默认容量（样本数）。推导见类注释。
  static constexpr std::size_t kDefaultCapacity = 512;

  /// @param capacity 环形容量（样本数）。0 会被提升为 1（防御除零/越界）。
  explicit ImuRingBuffer(std::size_t capacity = kDefaultCapacity);

  /// 追加一个采样。**在传感器回调队列调用**。
  ///
  /// 同时统计「时间戳倒序」异常（docs/02 §3.8 F-10）：本层**不静默丢弃**
  /// 倒序样本（丢样本会掩盖问题），只计数并放行。
  void Append(const recon_imu_sample& sample);

  /// 取「`timestamp_ns` 之前（含等于）的窗口」，并把已消费样本**移出**缓冲。
  ///
  /// @param timestamp_ns 帧时间戳（纳秒，与 IMU 同基）。
  /// @return 按时间升序排列的窗口。窗口内**不含** `timestamp_ns` 之后的样本
  ///         （C ABI 明文：「**本帧之前**的 IMU 时间窗」）。
  ///
  /// ## 为什么消费后必须移出
  ///
  /// 每帧只需「自上一帧以来新到的 IMU」。若不移出，窗口会随采集时长线性增长，
  /// 第 300 帧的窗口将携带全部历史样本 —— 内核的预积分是**增量式**的
  /// （docs/02 §3.5），重复预积分已算过的区间会**双重计数**。
  ///
  /// ## 时间戳单调性
  ///
  /// 若平台给出倒序样本，这里**不静默修正**：仅在输出前排一次序（升序），
  /// 把「是否异常」交给内核的异常检测。平台层擅自丢样本会掩盖问题，
  /// 违反「不静默降级」。
  ImuWindow WindowBefore(int64_t timestamp_ns);

  /// 清空缓冲（停止采集时调用）。
  void Reset();

  /// 当前缓冲的样本数（诊断用）。
  std::size_t Count() const;

  /// 因容量上限而被丢弃的样本累计数（诊断用）。
  ///
  /// 不为 0 表示帧流曾长时中断。注意：这**不违反** R-PREC-3 —— 被丢的是 IMU
  /// 原始流中尚未被任何帧消费的陈旧尾部，不是已入库的关键帧。
  std::size_t DroppedByCapacity() const;

  /// 时间戳倒序次数（诊断用）。依据 docs/02 §3.8 F-10。
  std::size_t TimestampRegressions() const;

  /// 环形容量（样本数）。
  std::size_t capacity() const { return capacity_; }

 private:
  const std::size_t capacity_;
  std::vector<recon_imu_sample> storage_;
  std::size_t dropped_by_capacity_ = 0;
  std::size_t timestamp_regressions_ = 0;
  /// 上一次 Append 的时间戳；用于倒序检测。`has_last_` 为 false 表示尚未有样本。
  int64_t last_timestamp_ns_ = 0;
  bool has_last_ = false;
  mutable std::mutex mutex_;
};

}  // namespace platform
}  // namespace recon

#endif  // RECON_PLATFORM_IMU_WINDOW_H_
