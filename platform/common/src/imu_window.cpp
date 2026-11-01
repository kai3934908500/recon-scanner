// SPDX-License-Identifier: BSD-3-Clause
//
// imu_window.cpp — IMU 环形缓冲与时间窗切片的平台无关实现。
//
// 契约与设计理由见同目录 imu_window.h 的文件头。

#include "recon_platform/imu_window.h"

#include <algorithm>
#include <utility>

namespace recon {
namespace platform {

ImuRingBuffer::ImuRingBuffer(std::size_t capacity)
    // 防御：0 会让「满了吗」的判据恒真（每 Append 都先丢再存），退化成一个
    // 只保留最后 1 条却不断丢弃的怪状态。取至少 1，让行为可预测。
    : capacity_(capacity == 0 ? 1 : capacity) {
  storage_.reserve(capacity_);
}

void ImuRingBuffer::Append(const recon_imu_sample& sample) {
  std::lock_guard<std::mutex> lock(mutex_);

  // 倒序检测（docs/02 §3.8 F-10）。**只统计不丢弃** —— 丢弃会掩盖异常。
  if (has_last_ && sample.timestamp_ns < last_timestamp_ns_) {
    ++timestamp_regressions_;
  }
  last_timestamp_ns_ = sample.timestamp_ns;
  has_last_ = true;

  if (storage_.size() >= capacity_) {
    // 丢最旧。被丢的是 IMU 原始流中尚未被任何帧消费的陈旧尾部，
    // 不是已入库的关键帧 —— 故不违反 R-PREC-3。
    storage_.erase(storage_.begin());
    ++dropped_by_capacity_;
  }
  storage_.push_back(sample);
}

ImuWindow ImuRingBuffer::WindowBefore(int64_t timestamp_ns) {
  std::lock_guard<std::mutex> lock(mutex_);

  // 找到第一个「严格晚于 timestamp_ns」的下标；其左侧即为本窗。
  // 边界语义：`<=` 的样本**属于**本窗（C ABI「本帧之前」含等于）。
  std::size_t split = 0;
  while (split < storage_.size() && storage_[split].timestamp_ns <= timestamp_ns) {
    ++split;
  }

  ImuWindow out;
  if (split == 0) {
    return out;  // 空窗。
  }

  out.samples.reserve(split);
  out.samples.assign(storage_.begin(), storage_.begin() + static_cast<long>(split));
  storage_.erase(storage_.begin(), storage_.begin() + static_cast<long>(split));

  // 升序保障（输入可能因平台队列抖动而轻微乱序）。**稳定排序**：当两个样本
  // 时间戳相等时保持其到达顺序，避免排序引入不可复现的抖动。
  std::stable_sort(out.samples.begin(), out.samples.end(),
                   [](const recon_imu_sample& a, const recon_imu_sample& b) {
                     return a.timestamp_ns < b.timestamp_ns;
                   });
  return out;
}

void ImuRingBuffer::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  storage_.clear();
  dropped_by_capacity_ = 0;
  timestamp_regressions_ = 0;
  last_timestamp_ns_ = 0;
  has_last_ = false;
}

std::size_t ImuRingBuffer::Count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return storage_.size();
}

std::size_t ImuRingBuffer::DroppedByCapacity() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dropped_by_capacity_;
}

std::size_t ImuRingBuffer::TimestampRegressions() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return timestamp_regressions_;
}

}  // namespace platform
}  // namespace recon
