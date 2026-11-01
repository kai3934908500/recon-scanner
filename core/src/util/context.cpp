// SPDX-License-Identifier: BSD-3-Clause
//
// 能力探测与上下文实现。
//
// 分支判定逻辑见 docs/00_架构总纲.md §5.2 决策表。
// 关键：两条分支平级，均为一等精度路径（见 §4.5）。

#include "recon/context.h"

namespace recon {
namespace {

using ProbeProvider = PlatformProbe (*)();

ProbeProvider g_probe_provider = nullptr;

}  // namespace

void SetPlatformProbeProvider(PlatformProbe (*fn)()) {
  g_probe_provider = fn;
}

const PlatformProbe& ProbePlatform() {
  static const PlatformProbe kFallback{};
  if (g_probe_provider == nullptr) {
    return kFallback;
  }
  // 探测结果按值返回后存入静态存储，避免返回临时对象的引用。
  static thread_local PlatformProbe cached;
  cached = g_probe_provider();
  return cached;
}

Context::Context(const recon_config* config) {
  params_ = BuildDefaultParams();

  if (config != nullptr) {
    if (config->preset >= 0 && config->preset <= 2) {
      params_.ApplyPreset(static_cast<Preset>(config->preset));
    }
    if (config->max_threads > 0) {
      params_.Set(keys::kPerfThreadPoolSize, config->max_threads);
    }
    if (config->voxel_size_m > 0.0) {
      params_.Set(keys::kRgbdTsdfVoxelSize, config->voxel_size_m);
    }
    if (config->poisson_depth > 0.0) {
      params_.Set(keys::kMeshPoissonDepth, config->poisson_depth);
    }
    if (config->mvs_min_depth_m > 0.0) {
      params_.Set(keys::kMvsMinDepthM, config->mvs_min_depth_m);
    }
    if (config->mvs_max_depth_m > 0.0) {
      params_.Set(keys::kMvsMaxDepthM, config->mvs_max_depth_m);
    }

    // 会话目录：UTF-8 路径，原样保存（平台层保证编码正确）。
    if (config->session_dir != nullptr) {
      session_dir_ = config->session_dir;
    }

    // 强制分支。recon_config 的注释规定「0xFFFFFFFF 表示自动探测」，但该
    // 字段的类型是 enum —— 平台层传入越界值后，回读的数值结果依赖实现。
    // 因此**按数值判定**：只有恰好等于合法枚举值时才是强制分支，其余
    // （含 0xFFFFFFFF / -1 / 任何越界值）一律走自动探测。
    const int64_t fb =
        static_cast<int64_t>(static_cast<int32_t>(config->forced_branch));
    if (fb == static_cast<int64_t>(RECON_BRANCH_RGBD) ||
        fb == static_cast<int64_t>(RECON_BRANCH_MONO)) {
      forced_branch_ = static_cast<recon_pipeline_branch>(fb);
      forced_branch_valid_ = true;
    }

    // 重建开关。**注意语义**：`enable_rgbd` 是「是否启用 RGBD 分支」，
    // 不是「是否启用 TSDF」。TSDF 开关由 MVS 精化的存在与否共同决定；
    // 这里把它映射到 enable_tsdf 的「上限」——分支不符时由编排层再判。
    // 负值统一视为「未指定」（保持默认 true）。
    if (config->enable_mvs == 0) options_.enable_mvs = false;
    if (config->enable_texture == 0) options_.enable_texture = false;
    if (config->enable_rgbd == 0) options_.enable_tsdf = false;
  }

  // 编排器必须在所有成员就绪之后构造 —— 它持有 `this`。
  orchestrator_.reset(new Orchestrator(this));
}

Context::~Context() = default;

bool Context::ProbeCapability() {
  if (capability_probed_) {
    return true;
  }

  const PlatformProbe& p = ProbePlatform();

  capability_ = recon_capability{};
  capability_.has_camera = p.has_camera ? 1 : 0;
  capability_.has_imu = p.has_imu ? 1 : 0;
  capability_.has_lidar = p.has_lidar ? 1 : 0;
  capability_.has_tof = p.has_tof ? 1 : 0;
  capability_.supports_gpu = p.supports_gpu ? 1 : 0;
  capability_.max_image_width = p.max_image_width;
  capability_.max_image_height = p.max_image_height;
  capability_.max_fps = p.max_fps;
  capability_.imu_max_rate_hz = p.imu_max_rate_hz;
  capability_.total_memory_bytes = p.total_memory_bytes;
  capability_.available_memory_bytes = p.available_memory_bytes;

  // 深度层级判定。优先级：LiDAR > ToF > 运动推断深度 > 无。
  if (p.has_lidar) {
    capability_.depth_tier = RECON_DEPTH_LIDAR;
    capability_.depth_source = RECON_DEPTH_SRC_LIDAR;
  } else if (p.has_tof) {
    capability_.depth_tier = RECON_DEPTH_TOF;
    capability_.depth_source = RECON_DEPTH_SRC_TOF;
  } else if (p.has_algo_depth) {
    capability_.depth_tier = RECON_DEPTH_ALGO;
    capability_.depth_source = RECON_DEPTH_SRC_ALGO;
  } else {
    capability_.depth_tier = RECON_DEPTH_NONE;
    capability_.depth_source = RECON_DEPTH_SRC_NONE;
  }

  // 分支判定。仅有运动推断深度（ALGO_DEPTH）时也走 RGBD 分支，
  // 因为它仍提供弱纹理区覆盖；但其尺度必须经参照物复核（见 docs/04 §2）。
  const bool has_any_depth = capability_.depth_tier != RECON_DEPTH_NONE;
  capability_.branch =
      has_any_depth ? RECON_BRANCH_RGBD : RECON_BRANCH_MONO;

  capability_probed_ = true;
  return true;
}

}  // namespace recon
