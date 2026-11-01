// SPDX-License-Identifier: BSD-3-Clause
//
// 内存保护与降级器实现（docs/13 §3.4 / §3.5 / §4）。

#include "recon/memory_guard.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace recon {
namespace {

// 0=auto 的线程池解析规则（docs/08 G11）：CPU 核数 − 1，上限 8，下限 1。
double ResolveAutoThreads() {
  unsigned hw = std::thread::hardware_concurrency();
  if (hw == 0) hw = 4;  // 探测失败时的保守兜底
  double n = static_cast<double>(hw) - 1.0;
  if (n < 1.0) n = 1.0;
  if (n > 8.0) n = 8.0;
  return n;
}

// 读取某键在「当前预设档」下的基线值（档位默认值）。找不到返回 false。
// 为什么用档位默认值而不是当前值：§5.3 明确要求「从基线值一次性算出目标值」，
// 否则多触发源叠加会过度降级。
bool PresetBaseline(const ParamStore& params, const std::string& key, double* out) {
  const Preset preset = params.current_preset();
  for (const auto& s : params.specs()) {
    if (s.key != key) continue;
    switch (preset) {
      case Preset::kFast: *out = s.fast_value; break;
      case Preset::kHighPrecision: *out = s.high_precision_value; break;
      case Preset::kStandard:
      default: *out = s.standard_value; break;
    }
    return true;
  }
  return false;
}

// 级别 → 该级别起始水位（band 下界）。用于迟滞判据。
double ThresholdLower(const MemoryGuardConfig& c, MemoryLevel lvl) {
  switch (lvl) {
    case MemoryLevel::kWarning: return c.warn_ratio;
    case MemoryLevel::kReduceParallel: return c.reduce_ratio;
    case MemoryLevel::kStrongReduce: return c.strong_ratio;
    case MemoryLevel::kEmergency: return c.emergency_ratio;
    case MemoryLevel::kOom: return c.oom_ratio;
    case MemoryLevel::kNormal:
    default: return 0.0;
  }
}

MemoryLevel Classify(const MemoryGuardConfig& c, double w) {
  if (w >= c.oom_ratio) return MemoryLevel::kOom;
  if (w >= c.emergency_ratio) return MemoryLevel::kEmergency;
  if (w >= c.strong_ratio) return MemoryLevel::kStrongReduce;
  if (w >= c.reduce_ratio) return MemoryLevel::kReduceParallel;
  if (w >= c.warn_ratio) return MemoryLevel::kWarning;
  return MemoryLevel::kNormal;
}

// 各白名单键在三个降级级别下的乘数（相对**基线值**，不叠加，§5.3）。
// 行 = 键，列 = {L1, L2, L3}。
struct ScaleRule {
  const char* key_constant;   // keys:: 里的常量名对应的字符串
  double f1;
  double f2;
  double f3;
  double floor_value;
  bool is_auto_threads;       // 0=auto 需先解析
  bool is_bool;               // 布尔：L1 起置 1（true）
};

const ScaleRule kScaleRules[] = {
    // 批大小类：L1 ×0.75、L2 再 ×0.75（=0.5625）、L3 ×0.5，最低 1。
    {"perf_gpu_batch_size", 0.75, 0.5625, 0.5, 1.0, false, false},
    {"perf_block_batch_size", 0.75, 0.5625, 0.5, 1.0, false, false},
    {"perf_max_concurrent_tasks", 0.75, 0.5625, 0.5, 1.0, false, false},
    // 内存池预分配：L2 起 ×0.5，最低 32。
    {"perf_memory_pool_prealloc_mb", 1.0, 0.5, 0.5, 32.0, false, false},
    // TSDF 常驻块数（W-8）：L2 起 ×0.5，最低 8。
    {"rgbd_max_blocks_in_memory", 1.0, 0.5, 0.5, 8.0, false, false},
    // 线程池（W-1）：L2 ×0.75、L3 ×0.5，最低 1；0=auto 先解析。
    {"perf_thread_pool_size", 1.0, 0.75, 0.5, 1.0, true, false},
    // 读盘预读窗口（W-10）：L1 起 ×0.5，最低 1。
    {"perf_read_ahead_frames", 0.5, 0.5, 0.5, 1.0, false, false},
};

// 落盘开关（L1 起置 true）。
const char* kSpillKeys[] = {
    "perf_spill_enabled",
    "perf_tsdf_spill_enabled",
    "perf_poisson_spill_enabled",
};

int LevelIndex(MemoryLevel lvl) {
  switch (lvl) {
    case MemoryLevel::kReduceParallel: return 1;   // L1
    case MemoryLevel::kStrongReduce: return 2;     // L2
    case MemoryLevel::kEmergency: return 3;        // L3
    case MemoryLevel::kOom: return 3;              // 溢出按最严处理
    default: return 0;                             // L0 / 预警
  }
}

}  // namespace

const char* MemoryLevelName(MemoryLevel level) {
  switch (level) {
    case MemoryLevel::kNormal: return "L0";
    case MemoryLevel::kWarning: return "WARN";
    case MemoryLevel::kReduceParallel: return "L1";
    case MemoryLevel::kStrongReduce: return "L2";
    case MemoryLevel::kEmergency: return "L3";
    case MemoryLevel::kOom: return "OOM";
  }
  return "?";
}

const char* DegradeReasonName(DegradeReason reason) {
  switch (reason) {
    case DegradeReason::kMemoryHighWater: return "memory_high_water";
    case DegradeReason::kMemoryWarning: return "memory_warning";
    case DegradeReason::kGpuOom: return "gpu_oom";
    case DegradeReason::kThermalThrottle: return "thermal_throttle";
    case DegradeReason::kThermalCritical: return "thermal_critical";
    case DegradeReason::kPowerSaver: return "power_saver";
    case DegradeReason::kLowBattery: return "low_battery";
    case DegradeReason::kBackgroundSuspend: return "background_suspend";
    case DegradeReason::kCacheSpill: return "cache_spill";
    case DegradeReason::kChunkedPoisson: return "chunked_poisson";
    case DegradeReason::kChunkedTsdf: return "chunked_tsdf";
    case DegradeReason::kChunkedMvs: return "chunked_mvs";
    case DegradeReason::kUserConfirmedScaleDown: return "user_confirmed_scale_down";
  }
  return "unknown";
}

int DegradeReasonLevel(DegradeReason reason) {
  switch (reason) {
    case DegradeReason::kMemoryWarning: return 1;
    case DegradeReason::kMemoryHighWater: return 1;
    case DegradeReason::kCacheSpill: return 1;
    case DegradeReason::kChunkedMvs: return 1;
    case DegradeReason::kGpuOom: return 2;
    case DegradeReason::kThermalThrottle: return 2;
    case DegradeReason::kPowerSaver: return 2;
    case DegradeReason::kLowBattery: return 2;
    case DegradeReason::kBackgroundSuspend: return 2;
    case DegradeReason::kChunkedTsdf: return 2;
    case DegradeReason::kThermalCritical: return 3;
    case DegradeReason::kChunkedPoisson: return 1;
    case DegradeReason::kUserConfirmedScaleDown: return 2;
  }
  return 1;
}

const char* DegradeReasonPrecisionImpact(DegradeReason reason) {
  switch (reason) {
    // 分块泊松的接缝处法向/曲率可能不连续（docs/13 §3.2.3 诚实披露）→ 局部精度影响。
    case DegradeReason::kChunkedPoisson: return "seam_local";
    // 用户显式确认的精度降级（§4.4）。
    case DegradeReason::kUserConfirmedScaleDown: return "user_approved";
    default: return "none";
  }
}

MemoryGuard::MemoryGuard() = default;
MemoryGuard::MemoryGuard(const MemoryGuardConfig& cfg) : cfg_(cfg) {}

MemoryLevel MemoryGuard::Update(double rss_mb, double budget_mb, int64_t now_ms) {
  water_ratio_ = (budget_mb > 0.0) ? (rss_mb / budget_mb) : 0.0;
  const MemoryLevel raw = Classify(cfg_, water_ratio_);

  if (raw > level_) {
    // 升级立即（§3.4.3）。
    level_ = raw;
    recover_candidate_since_ms_ = -1;
    if (first_degrade_ms_ < 0) first_degrade_ms_ = now_ms;
  } else if (raw < level_) {
    // 降级迟滞：需降到「当前级别下界 − 0.08」以下，且持续 60 s，
    // 且距上次恢复 ≥ 60 s（每 60 s 至多恢复一级，R-1）。
    const double gate = ThresholdLower(cfg_, level_) - cfg_.hysteresis;
    const bool low_enough = water_ratio_ <= gate;
    const bool hold_ok = recover_candidate_since_ms_ >= 0 &&
                         (now_ms - recover_candidate_since_ms_) >= cfg_.recover_hold_ms;
    const bool rate_ok = last_recover_ms_ < 0 ||
                         (now_ms - last_recover_ms_) >= cfg_.recover_hold_ms;
    if (low_enough) {
      if (recover_candidate_since_ms_ < 0) recover_candidate_since_ms_ = now_ms;
      if (hold_ok && rate_ok) {
        // 逐级恢复（R-1）。
        const int cur = static_cast<int>(level_);
        level_ = static_cast<MemoryLevel>(cur - 1);
        recover_candidate_since_ms_ = -1;
        last_recover_ms_ = now_ms;
      }
    } else {
      recover_candidate_since_ms_ = -1;
    }
  } else {
    recover_candidate_since_ms_ = -1;
  }

  effective_level_ = std::max(level_, platform_level_);
  return level_;
}

void MemoryGuard::NotifyPlatformWarning(int severity, int64_t now_ms) {
  // docs/13 §3.5：低 → L2（块数/内存池/GPU 批次 ×0.5）；
  //              严重/完全 → L3（再加并发与线程池 ×0.5）。
  const MemoryLevel lvl =
      (severity <= 0) ? MemoryLevel::kStrongReduce : MemoryLevel::kEmergency;
  platform_level_ = std::max(platform_level_, lvl);
  if (first_degrade_ms_ < 0) first_degrade_ms_ = now_ms;
  effective_level_ = std::max(level_, platform_level_);
}

void MemoryGuard::ClearPlatformWarning(int64_t /*now_ms*/) {
  platform_level_ = MemoryLevel::kNormal;
  effective_level_ = std::max(level_, platform_level_);
}

void MemoryGuard::ApplyToParams(ParamStore* params) const {
  if (params == nullptr) return;
  const int idx = LevelIndex(effective_level_);

  // 1) 比例缩放类键。
  for (const auto& rule : kScaleRules) {
    const std::string key = rule.key_constant;
    double baseline = 0.0;
    if (!PresetBaseline(*params, key, &baseline)) continue;  // 未注册 → 跳过

    double value = baseline;
    if (idx > 0) {
      if (rule.is_auto_threads && baseline == 0.0) baseline = ResolveAutoThreads();
      const double factor = (idx == 1) ? rule.f1 : (idx == 2) ? rule.f2 : rule.f3;
      value = std::round(baseline * factor);
      if (value < rule.floor_value) value = rule.floor_value;
    }
    // 写回：L0 时把基线值写回（恢复），非 L0 写缩放值。
    params->Set(key, value);
  }

  // 2) 落盘开关：L1 起置 true；L0 不主动改（保持用户/基线设置）。
  if (idx > 0) {
    for (const char* k : kSpillKeys) {
      double v = 0.0;
      if (params->Get(k, &v)) params->Set(k, 1.0);
    }
  }
}

int MemoryGuard::AddDegradation(const char* stage, DegradeReason reason,
                                const std::string& detail, int64_t now_ms) {
  DegradationRecord rec;
  rec.stage = (stage != nullptr) ? stage : "";
  rec.reason = reason;
  rec.detail = detail;
  rec.level = DegradeReasonLevel(reason);
  rec.precision_impact = DegradeReasonPrecisionImpact(reason);
  rec.recovered = false;
  rec.t_ms = now_ms;
  degradations_.push_back(rec);
  if (first_degrade_ms_ < 0) first_degrade_ms_ = now_ms;
  return static_cast<int>(degradations_.size()) - 1;
}

DegradationSummary MemoryGuard::summary(int64_t now_ms) const {
  DegradationSummary s;
  int max_level = LevelIndex(effective_level_);
  const char* impact = "none";
  for (const auto& r : degradations_) {
    max_level = std::max(max_level, r.level);
    // 优先级：user_approved > seam_local > none。
    if (std::string(r.precision_impact) == "user_approved") {
      impact = "user_approved";
    } else if (std::string(r.precision_impact) == "seam_local" &&
               std::string(impact) != "user_approved") {
      impact = "seam_local";
    }
  }
  s.max_level = max_level;
  s.duration_ms = (first_degrade_ms_ >= 0 && now_ms > first_degrade_ms_)
                      ? (now_ms - first_degrade_ms_)
                      : 0;
  s.precision_impact = impact;
  s.blacklist_touched = blacklist_touched_;  // 必须恒空
  return s;
}

void MemoryGuard::RecordBlacklistTouch(const std::string& key) {
  blacklist_touched_.push_back(key);
}

const std::vector<std::string>& MemoryGuard::WhitelistKeys() {
  // docs/13 §4.2 W-1..W-10 的承载键。
  static const std::vector<std::string> kKeys = {
      "perf_thread_pool_size",         // W-1
      "perf_gpu_batch_size",           // W-2
      "mvs_gpu_tile_size",             // W-3（分块调度决策）
      "rgbd_voxel_block_size",         // W-3b
      "mesh_chunked_enabled",          // W-3c / W-7
      "perf_block_batch_size",         // W-5
      "perf_memory_pool_prealloc_mb",  // W-6
      "rgbd_max_blocks_in_memory",     // W-8
      "perf_max_concurrent_tasks",     // W-9
      "perf_read_ahead_frames",        // W-10
  };
  return kKeys;
}

const std::vector<std::string>& MemoryGuard::BlacklistKeys() {
  // docs/13 §4.3 B-1..B-10。任何自动降级触碰其中任一键即为 P0 缺陷。
  static const std::vector<std::string> kKeys = {
      "rgbd_tsdf_voxel_size",              // B-1
      "sfm_feature_count",                 // B-2
      "vio_feature_count",                 // B-2
      "sfm_ba_max_iterations",             // B-3
      "sfm_ba_convergence_tolerance",      // B-3
      "sfm_ba_gradient_tolerance",         // B-3
      "sfm_ba_parameter_tolerance",        // B-3
      "sfm_ransac_max_iterations",         // B-4
      "sfm_ransac_min_inliers",            // B-4
      "sfm_ransac_max_error_px",           // B-4
      "sfm_ransac_confidence",             // B-4
      "mesh_poisson_depth",                // B-5
      "mesh_poisson_scale",                // B-5
      "mvs_image_scale",                   // B-6
      "sfm_match_ratio_threshold",         // B-8
      "mvs_num_views",                     // B-8
      "mvs_patch_size",                    // B-8
      "mvs_patch_iterations",              // B-8
      "mvs_photo_threshold",               // B-8
      "mvs_geo_consistency_threshold",     // B-8
      "mvs_depth_regularization",          // B-8
      "cap_resolution_width",              // B-9
      "cap_resolution_height",             // B-9
      "cap_frame_rate",                    // B-9
      "post_uv_atlas_size",                // B-10
      "post_bake_texture_size",            // B-10
  };
  return kKeys;
}

}  // namespace recon
