// SPDX-License-Identifier: BSD-3-Clause
//
// 能力探测与上下文。
//
// 契约（docs/00_架构总纲.md §5）：
//   探测在采集之前执行一次，决定管线分支。
//   降级不是分支：探测失败退到 BRANCH_MONO，不存在"低精度模式"。
//
// 平台无关部分在此，平台相关探测由 platform 层注入回调实现。

#ifndef RECON_CORE_CONTEXT_H_
#define RECON_CORE_CONTEXT_H_

#include <atomic>
#include <memory>
#include <string>

#include "recon/orchestrator.h"
#include "recon/params.h"
#include "recon/recon_c_api.h"

namespace recon {

// 平台能力提供者。由 platform/ios 或 platform/android 实现。
struct PlatformProbe {
  bool has_camera = false;
  bool has_imu = false;
  bool has_lidar = false;
  bool has_tof = false;
  bool supports_gpu = false;
  bool has_algo_depth = false;   // ARCore 运动推断深度
  int max_image_width = 0;
  int max_image_height = 0;
  int max_fps = 0;
  int imu_max_rate_hz = 0;
  uint64_t total_memory_bytes = 0;
  uint64_t available_memory_bytes = 0;
};

// 全局平台探测提供者。platform 层在启动时注册。
//
// ⚠️ 仅限**进程内**调用方（单测、嵌入式）。平台层（iOS/Android）**不允许**
//    include 本头（架构铁律：平台层只见 `recon_c_api.h`），故平台层用
//    C ABI 的 `recon_set_probe_data`（缺口 AC），不走本函数。
void SetPlatformProbeProvider(PlatformProbe (*fn)());
const PlatformProbe& ProbePlatform();

// C ABI `recon_set_probe_data` 的落点。平台层上报的事实存这里。
// 与 SetPlatformProbeProvider 并存：回调优先，其次本数据，最后全零兜底。
void SetPlatformProbeData(const PlatformProbe& p);
// 是否已通过 C ABI 上报过（用于区分「上报全零」与「从未上报」）。
bool PlatformProbeDataReported();

// 核心上下文。跨语言边界背后的实现。
class Context {
 public:
  explicit Context(const recon_config* config);
  ~Context();

  // 参数存取
  ParamStore& params() { return params_; }
  const ParamStore& params() const { return params_; }

  // 能力探测结果缓存
  const recon_capability& capability() const { return capability_; }
  bool ProbeCapability();

  // 取消标志
  std::atomic<bool>& cancel_flag() { return cancelled_; }
  bool IsCancelled() const { return cancelled_.load(std::memory_order_relaxed); }

  // 会话状态
  bool session_active() const { return session_active_; }
  void set_session_active(bool v) { session_active_ = v; }

  const std::string& session_id() const { return session_id_; }
  void set_session_id(std::string v) { session_id_ = std::move(v); }

  recon_scale_result& scale() { return scale_; }
  recon_mesh_info& mesh_info() { return mesh_info_; }
  recon_scan_stats& scan_stats() { return scan_stats_; }

  // L5 编排器。生命周期与 Context 绑定（recon_create 构造、recon_destroy 析构）。
  // 用 unique_ptr 是为了让它能在 context.cpp 里构造 —— 此时 Orchestrator
  // 已是完整类型，不需要在这里包含其定义细节。
  Orchestrator& orchestrator() { return *orchestrator_; }
  const Orchestrator& orchestrator() const { return *orchestrator_; }

  // 会话目录（落盘根）。可空。
  const std::string& session_dir() const { return session_dir_; }
  void set_session_dir(std::string v) { session_dir_ = std::move(v); }

  // 重建选项。来源是 recon_config 的 enable_* 字段（构造期固化）。
  // 放在 Context 而不是每次重建现算：整跑与单阶段跑必须看同一份开关。
  ReconOptions& options() { return options_; }
  const ReconOptions& options() const { return options_; }

  // 强制分支（recon_config.forced_branch）。valid=false 表示自动探测。
  bool forced_branch_valid() const { return forced_branch_valid_; }
  recon_pipeline_branch forced_branch() const { return forced_branch_; }

 private:
  ParamStore params_;
  recon_capability capability_{};
  recon_scale_result scale_{};
  recon_mesh_info mesh_info_{};
  recon_scan_stats scan_stats_{};
  std::atomic<bool> cancelled_{false};
  bool session_active_ = false;
  std::string session_id_;
  std::string session_dir_;
  bool capability_probed_ = false;
  ReconOptions options_{};
  recon_pipeline_branch forced_branch_ = RECON_BRANCH_MONO;
  bool forced_branch_valid_ = false;

  // 构造顺序：orchestrator_ 依赖 this（拿到 Context*），必须在成员之后构造。
  // 声明在最后，构造时本对象已完成基本初始化。
  std::unique_ptr<Orchestrator> orchestrator_;
};

}  // namespace recon

#endif  // RECON_CORE_CONTEXT_H_
