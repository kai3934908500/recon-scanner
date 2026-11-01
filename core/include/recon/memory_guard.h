// SPDX-License-Identifier: BSD-3-Clause
//
// 内存保护与降级器（docs/13_内存与降级.md §3.4 / §3.5 / §4）。
//
// 铁律（本文件的存在理由）：
//   - R-PREC-4：降级**只**调整并行度 / 批大小 / 分块粒度 / 内存池预分配
//     （白名单 W-1..W-10），**绝不**触碰任何精度阈值（黑名单 B-1..B-10）。
//   - §5.3 不叠加：降级器维护**单一** effective_level，每次都从**基线值**
//     （档位默认值）一次性算出目标值，而不是在当前值上再乘系数 —— 否则
//     多触发源叠加会导致过度降级、重建跑不完。
//   - D-1..D-6：每次降级与恢复都必须记录，reason 为受控枚举，不得静默。
//   - `blacklist_touched` 必须**恒为空** —— 它是 docs/14 §7 P0 判定的运行时证据。
//
// 边界（如实披露）：本模块只负责「按水位算出白名单目标值并写入 ParamStore」。
// W-3/W-3b/W-3c 的分块尺寸下调（`mvs_gpu_tile_size` / `rgbd_voxel_block_size` /
// `mesh_chunked_enabled`）属**分块调度决策**，由各阶段按 §3.2 的内存预算自行
// 判定并调用本模块的 `AddDegradation` 登记，本模块不越权代劳。
#ifndef RECON_CORE_MEMORY_GUARD_H_
#define RECON_CORE_MEMORY_GUARD_H_

#include <cstdint>
#include <string>
#include <vector>

#include "recon/params.h"

namespace recon {

// 内存水位级别（docs/13 §3.4.2）。
enum class MemoryLevel : uint8_t {
  kNormal = 0,          // w < 0.60        正常
  kWarning = 1,         // 0.60 ≤ w < 0.75 预警（记录 + 预读收敛，不改并发度）
  kReduceParallel = 2,  // 0.75 ≤ w < 0.85 L1 降并行
  kStrongReduce = 3,    // 0.85 ≤ w < 0.92 L2 强降
  kEmergency = 4,       // w ≥ 0.92        L3 紧急
  kOom = 5,             // w ≥ 1.00        溢出（主动抛 OUT_OF_MEMORY）
};

// 级别名（"L0".."L3" / "OOM"），用于日志与 degradations 记录。
const char* MemoryLevelName(MemoryLevel level);

// 降级原因受控枚举（docs/13 §4.5 D-2）。新增取值必须同步本文档与 docs/13。
enum class DegradeReason : uint8_t {
  kMemoryHighWater = 0,
  kMemoryWarning,
  kGpuOom,
  kThermalThrottle,
  kThermalCritical,
  kPowerSaver,
  kLowBattery,
  kBackgroundSuspend,
  kCacheSpill,
  kChunkedPoisson,
  kChunkedTsdf,
  kChunkedMvs,
  kUserConfirmedScaleDown,
};

const char* DegradeReasonName(DegradeReason reason);
// 该原因对应的降级级别（1/2/3），用于 degradations 记录的 level 字段。
int DegradeReasonLevel(DegradeReason reason);
// 该原因对精度的影响："none" / "seam_local" / "user_approved"（docs/13 D-4）。
const char* DegradeReasonPrecisionImpact(DegradeReason reason);

// 一条降级记录（docs/13 §4.5 的 degradations 数组元素）。
struct DegradationRecord {
  const char* stage = "";
  DegradeReason reason = DegradeReason::kMemoryHighWater;
  std::string detail;
  int level = 1;                        // 1=L1 / 2=L2 / 3=L3
  const char* precision_impact = "none";
  bool recovered = false;               // 恢复记录（R-2）标 true
  int64_t t_ms = 0;
};

// 降级汇总（docs/13 §4.5 D-5 的 degradation_summary）。
struct DegradationSummary {
  int max_level = 0;                    // 0=L0 / 1=L1 / 2=L2 / 3=L3
  int64_t duration_ms = 0;
  const char* precision_impact = "none";
  std::vector<std::string> blacklist_touched;  // 必须恒空
};

// 水位阈值与迟滞（docs/13 §3.4.2 / §3.4.3）。
struct MemoryGuardConfig {
  double warn_ratio = 0.60;
  double reduce_ratio = 0.75;
  double strong_ratio = 0.85;
  double emergency_ratio = 0.92;
  double oom_ratio = 1.00;
  double hysteresis = 0.08;             // 恢复需降到「阈值 − 0.08」以下
  int64_t recover_hold_ms = 60000;      // 且持续 60 s；每 60 s 至多恢复一级
};

class MemoryGuard {
 public:
  MemoryGuard();
  explicit MemoryGuard(const MemoryGuardConfig& cfg);

  // 用当前 RSS 与内存预算更新水位。返回**水位级别**（不含平台告警叠加）。
  // rss_mb/budget_mb 单位为 MB；budget_mb ≤ 0 视为无预算信息（按 0 水位处理）。
  MemoryLevel Update(double rss_mb, double budget_mb, int64_t now_ms);

  // 注入平台内存告警（docs/13 §3.5）。
  // severity: 0 = 低（iOS didReceiveMemoryWarning / Android TRIM_MEMORY_RUNNING_LOW）
  //           1 = 严重（Android TRIM_MEMORY_RUNNING_CRITICAL）
  //           2 = 完全（Android TRIM_MEMORY_COMPLETE）
  void NotifyPlatformWarning(int severity, int64_t now_ms);

  // 清除平台告警叠加（平台告警解除时调用）。
  void ClearPlatformWarning(int64_t now_ms);

  // 把 effective_level 对应的白名单目标值（从基线值算出）写入 ParamStore。
  // 只写白名单键；未注册的键跳过（不静默篡改他人的值）。
  void ApplyToParams(ParamStore* params) const;

  // 记录一条降级。返回新记录的索引（供调用方后续回填 detail）。
  int AddDegradation(const char* stage, DegradeReason reason,
                     const std::string& detail, int64_t now_ms);

  MemoryLevel level() const { return level_; }
  MemoryLevel effective_level() const { return effective_level_; }
  bool ShouldAbortForOom() const { return level_ >= MemoryLevel::kOom; }
  double water_ratio() const { return water_ratio_; }

  const std::vector<DegradationRecord>& degradations() const {
    return degradations_;
  }
  DegradationSummary summary(int64_t now_ms) const;

  // 记录一次黑名单触碰。**任何调用都应导致测试失败**——保留此接口是为了让
  // 「误触碰」可被显式登记并被 P0 断言捕获，而不是被静默掩盖。
  void RecordBlacklistTouch(const std::string& key);
  const std::vector<std::string>& blacklist_touched() const {
    return blacklist_touched_;
  }

  // 允许自动降级触碰的键集合（白名单 W-1..W-10 的承载键）。
  static const std::vector<std::string>& WhitelistKeys();
  // 严禁自动降级触碰的键集合（黑名单 B-1..B-10）。
  static const std::vector<std::string>& BlacklistKeys();

 private:
  MemoryGuardConfig cfg_;
  MemoryLevel level_ = MemoryLevel::kNormal;
  MemoryLevel effective_level_ = MemoryLevel::kNormal;
  MemoryLevel platform_level_ = MemoryLevel::kNormal;
  double water_ratio_ = 0.0;
  int64_t recover_candidate_since_ms_ = -1;
  int64_t last_recover_ms_ = -1;
  int64_t first_degrade_ms_ = -1;
  std::vector<DegradationRecord> degradations_;
  std::vector<std::string> blacklist_touched_;
};

}  // namespace recon

#endif  // RECON_CORE_MEMORY_GUARD_H_
