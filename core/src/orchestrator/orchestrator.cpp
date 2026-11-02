// SPDX-License-Identifier: BSD-3-Clause
//
// L5 编排层实现。契约：docs/01_接口契约.md §3.12 / §6。
//
// 本文件把 L4 的模块接成端到端流水线。**每个阶段只做一件事，并且调用真实的
// L4 实现** —— 没有任何 `return RECON_OK;` 式的占位。
//
// 阶段表（docs/01 §6.1 的核心状态 → 阶段映射）：
//   PREPROCESS    BGRA → 灰度、内参/位姿转换、帧合法性校验
//   VIO           有 IMU 且平台无位姿时跑 MSCKF；否则用平台位姿（先验）
//   MVS           PatchMatch 立体 → 亚像素 → 多视图融合出稠密点云
//   RGBD_FUSION   TSDF 加权融合 → 零交叉提取点云（仅 RGBD 分支）
//   MESH          泊松曲面重建（深度由点云采样间距反推，docs/06 §1.3）
//   POSTPROC      RunMeshPipeline（S4/S5/S6/S7，末端整体朝外定向）
//   UV_TEXTURE    S8 UV 展开 + S9 纹理烘焙
//   SCALE         尺度标定结果回写 mesh_info
//
// 为什么不把「导出」放进流水线：C ABI 把导出拆成独立入口 `recon_export`
// （docs/01 §6.1「Completed」状态），因为它可以反复调用（换格式 / 换路径），
// 而重建只跑一次。

#include "recon/orchestrator.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "recon/context.h"
#include "recon/exporter.h"
#include "recon/params.h"
#include "recon/sfm.h"
#include "recon/tsdf_fusion.h"
#include "recon/vio.h"

namespace recon {
namespace {

// 单调时钟毫秒。用于阶段耗时与进度节流。
double NowMillis() {
  using clock = std::chrono::steady_clock;
  static const clock::time_point start = clock::now();
  return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

// BGRA8 → 灰度。BT.601 亮度系数（与 MVS / VIO 的灰度口径一致）。
//
// 为什么不用算术平均：人眼对绿最敏感，RGB 等权平均在彩色纹理上会丢失
// 对比度，直接影响 Shi-Tomasi 的响应强度与 NCC 的峰值锐度。
void BgraToGray(const uint8_t* bgra, int width, int height, int stride,
                std::vector<uint8_t>* out, int* out_stride) {
  out->assign(static_cast<size_t>(width) * height, 0);
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
    uint8_t* dst = out->data() + static_cast<size_t>(y) * width;
    for (int x = 0; x < width; ++x) {
      const int b = row[x * 4 + 0];
      const int g = row[x * 4 + 1];
      const int r = row[x * 4 + 2];
      dst[x] = static_cast<uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
    }
  }
  *out_stride = width;
}

// 4x4 世界→相机变换（供 TsdfVolume::IntegrateDepthFrame 使用）。
Mat4d ToMat4(const CameraPose& p) {
  Mat4d m = Mat4d::Identity();
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) m(i, j) = p.R(i, j);
    m(i, 3) = p.t(i);
  }
  return m;
}

// 点云 + 法向的包围盒（留出 padding）。用于 TSDF 体素网格的边界。
void PointCloudBounds(const std::vector<Vec3d>& pts, double padding,
                      Vec3d* lo, Vec3d* hi) {
  Vec3d a(1e30, 1e30, 1e30);
  Vec3d b(-1e30, -1e30, -1e30);
  for (const auto& p : pts) {
    for (int i = 0; i < 3; ++i) {
      a(i) = std::min(a(i), p(i));
      b(i) = std::max(b(i), p(i));
    }
  }
  if (pts.empty()) {
    a = Vec3d(-padding, -padding, -padding);
    b = Vec3d(padding, padding, padding);
  }
  for (int i = 0; i < 3; ++i) {
    a(i) -= padding;
    b(i) += padding;
  }
  *lo = a;
  *hi = b;
}

// 体素化去重合并：把 src 中与 dst 已有体素重复的点丢弃。
//
// 为什么必须去重：TSDF 提取点云与 MVS 点云覆盖同一表面，直接拼接会让
// 泊松在每个表面位置收到 2× 的点 → 点权重失衡（同一处观测被算两遍）→
// 等值面被"拉向"重复侧。用体素哈希按「最近邻间距的一半」吸附即可。
void MergeDedupe(const UnifiedPointCloud& src, double voxel,
                 UnifiedPointCloud* dst) {
  if (src.size() == 0) return;
  if (voxel <= 0.0) voxel = 1e-4;

  struct Key {
    int64_t x, y, z;
    bool operator<(const Key& o) const {
      if (x != o.x) return x < o.x;
      if (y != o.y) return y < o.y;
      return z < o.z;
    }
  };
  auto key_of = [voxel](const Vec3d& p) -> Key {
    return Key{static_cast<int64_t>(std::llround(p(0) / voxel)),
               static_cast<int64_t>(std::llround(p(1) / voxel)),
               static_cast<int64_t>(std::llround(p(2) / voxel))};
  };

  std::vector<Key> keys(dst->positions.size());
  for (size_t i = 0; i < dst->positions.size(); ++i) {
    keys[i] = key_of(dst->positions[i]);
  }
  std::sort(keys.begin(), keys.end());

  const bool has_colors = src.colors.size() == src.positions.size() * 3;
  for (size_t i = 0; i < src.positions.size(); ++i) {
    const Key k = key_of(src.positions[i]);
    if (std::binary_search(keys.begin(), keys.end(), k)) continue;

    dst->positions.push_back(src.positions[i]);
    dst->normals.push_back(i < src.normals.size() ? src.normals[i]
                                                  : Vec3d(0.0, 0.0, 1.0));
    dst->confidences.push_back(i < src.confidences.size() ? src.confidences[i]
                                                          : 0.5f);
    if (has_colors) {
      dst->colors.push_back(src.colors[i * 3 + 0]);
      dst->colors.push_back(src.colors[i * 3 + 1]);
      dst->colors.push_back(src.colors[i * 3 + 2]);
    } else {
      dst->colors.push_back(128.0f);
      dst->colors.push_back(128.0f);
      dst->colors.push_back(128.0f);
    }
    keys.insert(std::lower_bound(keys.begin(), keys.end(), k), k);
  }
}

// 简短 UTF-8 截断（日志回调上限 512 字节，docs/01 §6.5 CB-5）。
std::string TruncateUtf8(const std::string& s, size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  size_t cut = max_bytes;
  // 回退到 UTF-8 字符边界，避免截出半个码点。
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  return s.substr(0, cut);
}

}  // namespace

const char* OrchestratorStateName(OrchestratorState s) {
  switch (s) {
    case OrchestratorState::kIdle: return "IDLE";
    case OrchestratorState::kCalibrating: return "CALIBRATING";
    case OrchestratorState::kPlacing: return "PLACING";
    case OrchestratorState::kScanning: return "SCANNING";
    case OrchestratorState::kReviewing: return "REVIEWING";
    case OrchestratorState::kReconstructing: return "RECONSTRUCTING";
    case OrchestratorState::kCancelling: return "CANCELLING";
    case OrchestratorState::kCompleted: return "COMPLETED";
    case OrchestratorState::kFailed: return "FAILED";
  }
  return "UNKNOWN";
}

OrientedPointCloud UnifiedPointCloud::ToOrientedCloud() const {
  OrientedPointCloud out;
  out.positions = positions;
  out.normals = normals;
  out.colors = colors;
  // 权重：泊松的 point_weight 期望「≥ 0」。置信度直接可用；
  // 置信度缺失时给 1.0（等权），**不能给 0**（会被当无效点丢弃）。
  out.weights.resize(positions.size(), 1.0f);
  for (size_t i = 0; i < positions.size() && i < confidences.size(); ++i) {
    out.weights[i] = std::max(0.0f, confidences[i]);
  }
  return out;
}

Orchestrator::Orchestrator(Context* ctx) : ctx_(ctx) {}

Orchestrator::~Orchestrator() = default;

// ---------------------------------------------------------------------------
// 状态与回调
// ---------------------------------------------------------------------------

void Orchestrator::SetState(OrchestratorState s) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = s;
}

OrchestratorState Orchestrator::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

void Orchestrator::EmitStatus(int32_t code, const char* text) {
  if (cb_.status != nullptr && text != nullptr) {
    cb_.status(cb_.user_data, code, text);
  }
}

void Orchestrator::EmitLog(int32_t level, const std::string& message) {
  if (cb_.log != nullptr) {
    const std::string t = TruncateUtf8(message, 512);
    cb_.log(cb_.user_data, level, t.c_str());
  }
}

void Orchestrator::FlushProgress(StageId stage, double progress,
                                 const char* message) {
  if (cb_.progress == nullptr) return;
  const double p = std::max(0.0, std::min(1.0, progress));
  cb_.progress(cb_.user_data, stage, p, message == nullptr ? "" : message);
  last_progress_ms_ = NowMillis();
}

void Orchestrator::EmitProgress(StageId stage, double progress,
                                const char* message) {
  // CB-3：进度回调频率上限 10 Hz（内部可高频，100 ms 窗口内合并推送一次）。
  const double now = NowMillis();
  if (now - last_progress_ms_ < 100.0) return;
  FlushProgress(stage, progress, message);
}

double Orchestrator::ParamOr(const char* key, double fallback) const {
  if (ctx_ == nullptr) return fallback;
  double v = 0.0;
  if (ctx_->params().Get(key, &v)) return v;
  return fallback;
}

bool Orchestrator::CheckCancel() {
  if (ctx_ == nullptr || !ctx_->IsCancelled()) return false;
  SetState(OrchestratorState::kCancelling);
  return true;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

void Orchestrator::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  frames_.clear();
  accepted_frames_ = 0;
  // 像素预算计数随 frames_ 一并归零（缺口 AT）—— 不归零会让下一次会话
  // 一开始就误判「已超预算」，把首帧像素立即释放掉。
  resident_pixel_bytes_ = 0;
  released_pixel_frames_ = 0;
  pixel_budget_note_emitted_ = false;
  key_poses_.clear();
  key_intrinsics_.clear();
  keyframe_indices_.clear();
  key_timestamps_ns_.clear();
  cloud_.Clear();
  mesh_.Clear();
  atlas_ = UvAtlas{};
  bake_ = BakeResult{};
  mesh_info_ = recon_mesh_info{};
  records_.clear();
  last_failed_stage_ = RECON_STAGE_IDLE;
  mvs_depth_maps_ = 0;
  tsdf_integrated_pixels_ = 0;
  poisson_resolution_ = 0.0;
  poisson_depth_used_ = 0;
  resume_stage_ = RECON_STAGE_IDLE;
  opts_valid_ = false;
}

recon_status Orchestrator::startSession(const SessionSpec& spec) {
  if (ctx_ == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  if (ctx_->IsCancelled()) return RECON_ERR_CANCELLED;

  Reset();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    spec_ = spec;
    // 会话目录：调用方显式给了就用它，否则回退到 Context 的目录。
    // 两条来源都可能为空（无落盘模式），此时检查点写入会静默跳过。
    if (spec_.session_dir.empty()) {
      spec_.session_dir = ctx_->session_dir();
    }
    if (spec_.session_id.empty()) {
      spec_.session_id = ctx_->session_id();
    }
  }

  // 能力探测决定分支。分支平级，不是降级（docs/00 §4.5）。
  // 调用方强制指定分支时优先于探测结果（用于桌面验证与回归测试）。
  if (ctx_->forced_branch_valid()) {
    branch_ = ctx_->forced_branch();
    EmitLog(1, std::string("分支由调用方强制为 ") +
                   (branch_ == RECON_BRANCH_RGBD ? "RGBD" : "MONO"));
  } else if (!ctx_->ProbeCapability()) {
    // 探测失败退到 MONO（契约：探测失败不存在"低精度模式"）。
    branch_ = RECON_BRANCH_MONO;
    EmitLog(2, "能力探测失败，退到 MONO 分支");
  } else {
    branch_ = ctx_->capability().branch;
  }

  SetState(OrchestratorState::kPlacing);
  EmitLog(1, std::string("会话开始：") + spec.session_id +
                 "，分支=" + (branch_ == RECON_BRANCH_RGBD ? "RGBD" : "MONO"));
  FlushProgress(RECON_STAGE_IDLE, 0.0, "准备扫描");
  return RECON_OK;
}

recon_status Orchestrator::onFramePushed(const recon_frame_packet& packet) {
  if (ctx_ == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  if (ctx_->IsCancelled()) return RECON_ERR_CANCELLED;

  const OrchestratorState s = state();
  if (s != OrchestratorState::kPlacing && s != OrchestratorState::kScanning) {
    return RECON_ERR_CAPTURE_CAMERA_UNAVAILABLE;
  }

  // 边界校验（docs/01 §2.2.5）：rgb_data 可为 NULL，但尺寸必须自洽。
  if (packet.rgb_data != nullptr) {
    if (packet.rgb_width == 0 || packet.rgb_height == 0 ||
        packet.rgb_stride < packet.rgb_width * 4u) {
      return RECON_ERR_INVALID_ARGUMENT;
    }
  }
  if (packet.has_depth != 0) {
    if (packet.depth_data == nullptr || packet.depth_width == 0 ||
        packet.depth_height == 0 || packet.depth_stride < packet.depth_width * 2u) {
      return RECON_ERR_INVALID_ARGUMENT;
    }
  }
  if (packet.imu != nullptr && packet.imu_count == 0) {
    return RECON_ERR_INVALID_ARGUMENT;
  }
  if (packet.imu == nullptr && packet.imu_count != 0) {
    return RECON_ERR_INVALID_ARGUMENT;
  }

  CapturedFrame f;
  f.frame_id = packet.frame_id;
  f.timestamp_ns = packet.timestamp_ns;
  f.exposure_us = packet.exposure_us;
  // 运动模糊：C ABI 的 recon_frame_packet 未携带该字段，置 0 表示清晰。
  // 平台的模糊度估计若可用，应在 PREPROCESS 用 quality_gate 重算
  // （docs/02 §4.8），而不是在这里猜。
  f.motion_blur_score = 0.0;

  // RGB 深拷贝（契约：push_frame 返回后不访问调用方内存）。
  if (packet.rgb_data != nullptr) {
    f.rgb_width = static_cast<int>(packet.rgb_width);
    f.rgb_height = static_cast<int>(packet.rgb_height);
    f.rgb_stride = static_cast<int>(packet.rgb_width) * 4;  // 内部紧排
    f.rgb.resize(static_cast<size_t>(f.rgb_width) * f.rgb_height * 4);
    const uint8_t* src = packet.rgb_data;
    for (int y = 0; y < f.rgb_height; ++y) {
      std::memcpy(f.rgb.data() + static_cast<size_t>(y) * f.rgb_stride,
                  src + static_cast<size_t>(y) * packet.rgb_stride,
                  static_cast<size_t>(f.rgb_stride));
    }
  }

  // 深度深拷贝。
  if (packet.has_depth != 0 && packet.depth_data != nullptr) {
    f.has_depth = true;
    f.depth_source = packet.depth_source;
    f.depth_width = static_cast<int>(packet.depth_width);
    f.depth_height = static_cast<int>(packet.depth_height);
    const int dw = f.depth_width;
    const int dh = f.depth_height;
    f.depth.resize(static_cast<size_t>(dw) * dh);
    for (int y = 0; y < dh; ++y) {
      std::memcpy(f.depth.data() + static_cast<size_t>(y) * dw,
                  packet.depth_data + static_cast<size_t>(y) * packet.depth_stride,
                  static_cast<size_t>(dw) * 2);
    }
    if (packet.depth_confidence != nullptr) {
      f.depth_confidence.resize(static_cast<size_t>(dw) * dh);
      // **置信度按「宽度」跨行，不能用 depth_stride**（真实缺陷，已修）。
      //
      // `depth_stride` 是 **uint16 深度数据**的行字节数（≥ 2×宽度，见
      // docs/01 §3.2 字段表与上面的边界校验）。而 `depth_confidence` 是
      // **uint8**（每像素 1 字节），且契约里**没有**独立的 confidence stride
      // 字段 —— 含义是紧凑排列（行步长 = 宽度）。
      //
      // 两者合用会让第 2 行起每行多跳过 width 字节：160×160 帧、
      // depth_stride=320 时，第 y 行读 `confidence + y*320`，而数组只有
      // 25600 字节 → 第 40 行起就越界，很快踩到不可读页 → **SIGSEGV**。
      // （2026-10-31 由本轮的端到端测试实测暴露：此前的单测从不推带
      //  depth_confidence 的帧，所以一直没被发现。）
      //
      // 与下游一致：`TsdfVolume::IntegrateDepthFrame` 内部就是按
      // `confidence[v * width + u]` 紧凑访问，深拷贝后的缓冲正是该口径。
      for (int y = 0; y < dh; ++y) {
        std::memcpy(f.depth_confidence.data() + static_cast<size_t>(y) * dw,
                    packet.depth_confidence + static_cast<size_t>(y) * dw,
                    static_cast<size_t>(dw));
      }
    }
  }

  // 内参。
  if (packet.intrinsics != nullptr) {
    const recon_camera_intrinsics& k = *packet.intrinsics;
    f.intrinsics.fx = k.fx;
    f.intrinsics.fy = k.fy;
    f.intrinsics.cx = k.cx;
    f.intrinsics.cy = k.cy;
    f.intrinsics.k1 = k.k1;
    f.intrinsics.k2 = k.k2;
    f.intrinsics.k3 = k.k3;
    f.intrinsics.p1 = k.p1;
    f.intrinsics.p2 = k.p2;
    f.intrinsics.width = static_cast<int>(k.width);
    f.intrinsics.height = static_cast<int>(k.height);
    f.intrinsics_valid = k.fx > 0.0 && k.fy > 0.0;
  }

  // 平台位姿先验（world → camera，行主序 R）。
  if (packet.pose != nullptr) {
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) f.pose.R(i, j) = packet.pose->r[i * 3 + j];
      f.pose.t(i) = packet.pose->t[i];
    }
    f.pose_valid = true;
  }

  // IMU 时间窗。
  if (packet.imu != nullptr && packet.imu_count > 0) {
    f.imu.resize(packet.imu_count);
    for (uint32_t i = 0; i < packet.imu_count; ++i) {
      f.imu[i].timestamp_ns = packet.imu[i].timestamp_ns;
      f.imu[i].accel = Vec3d(packet.imu[i].accel[0], packet.imu[i].accel[1],
                             packet.imu[i].accel[2]);
      f.imu[i].gyro = Vec3d(packet.imu[i].gyro[0], packet.imu[i].gyro[1],
                            packet.imu[i].gyro[2]);
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    resident_pixel_bytes_ += FramePixelBytes(f);
    frames_.push_back(std::move(f));
    if (state_ == OrchestratorState::kPlacing) {
      state_ = OrchestratorState::kScanning;
    }
    // ★ OOM 保护：入帧后立即按预算裁决（缺口 AT，2026-11-02）。
    //   必须在 push_back 之后、释放锁之前调用 —— 否则下一帧可能已越过预算。
    //   本函数与调用方同持 mutex_（见其声明的 "Locked" 后缀约定）。
    EnforcePixelBudgetLocked();
  }
  return RECON_OK;
}

// ── 采集期像素内存预算（缺口 AT）────────────────────────────────────────────
//
// 完整背景见 orchestrator.h 中 `frame_pixel_budget_bytes_` 的声明处注释。
// 此处只强调**为什么不能简单地「删掉整帧」**：
//
//   `frames_` 的下标被 `keyframe_indices_` / `key_poses_` / `key_intrinsics_`
//   / `key_timestamps_ns_` 四组平行数组直接引用（`BuildKeyframeArrays`、
//   MVS 的 `BuildKeyframeArrays`、烘焙的 `keyframe_indices_` 遍历都会
//   `frames_[idx]`）。删元素会让这些下标**整体错位**，是比 OOM 更隐蔽的
//   错误（会静默产出错配的位姿/纹理 → 几何错误但**不崩溃**）。
//   故只释放**像素矢量**（`clear()` + `shrink_to_fit()`），下标与元数据不动。
//
//   这正是 docs/13 §0.3 表格第 3 行「关键帧元数据（位姿/特征点）：分块加载」
//   与第 2 行「关键帧图像：落盘后释放引用」的**同一条思路**，只是落盘那一步
//   尚未实现（缺口 AT 的完整修复路径）。

size_t Orchestrator::FramePixelBytes(const CapturedFrame& f) {
  size_t n = f.rgb.size() + f.depth.size() * sizeof(uint16_t)
             + f.depth_confidence.size() + f.gray.size();
  return n;
}

void Orchestrator::EnforcePixelBudgetLocked() {
  const size_t budget = frame_pixel_budget_bytes_;
  if (budget == 0) return;  // 0 = 不设限（测试可用）
  if (resident_pixel_bytes_ <= budget) return;

  // 从最旧帧开始释放，直到回到预算内。`begin()` 是最旧 —— 采集顺序入队。
  //
  // ⚠️ 只释放**还没被释放过**的帧（用 `!rgb.empty()` 判）：
  //    已释放的帧其 `FramePixelBytes` 为 0，重复遍历会让 `released_pixel_frames_`
  //    虚高，也会让循环多绕。深度-only 帧（rgb 为空但有深度）用 depth 判。
  size_t i = 0;
  while (resident_pixel_bytes_ > budget && i < frames_.size()) {
    CapturedFrame& f = frames_[i];
    const bool has_pixels = !f.rgb.empty() || !f.depth.empty()
                            || !f.depth_confidence.empty() || !f.gray.empty();
    if (has_pixels) {
      const size_t freed = FramePixelBytes(f);
      f.rgb.clear();               f.rgb.shrink_to_fit();
      f.depth.clear();             f.depth.shrink_to_fit();
      f.depth_confidence.clear();  f.depth_confidence.shrink_to_fit();
      f.gray.clear();              f.gray.shrink_to_fit();
      resident_pixel_bytes_ -= freed;
      released_pixel_frames_ += 1;
    }
    ++i;
  }

  // 不静默降级：把释放事实与规模记进内核日志（真机可经设备日志看到）。
  // 频率控制：只在**首次**越界时全量记录，之后由 runStage 的 metrics 承担。
  if (released_pixel_frames_ > 0 && !pixel_budget_note_emitted_) {
    pixel_budget_note_emitted_ = true;
    EmitLog(2, "内存保护：像素预算 " + std::to_string(budget / (1024 * 1024))
                + " MiB 触顶，已释放最旧帧像素缓冲（保留全部位姿与元数据）；"
                + "MVS 稠密匹配可用帧数将减少。这是内存保护行为，不是精度参数降级。");
  }
}

recon_status Orchestrator::finishCapture() {
  if (ctx_ == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  if (ctx_->IsCancelled()) return RECON_ERR_CANCELLED;

  const OrchestratorState s = state();
  if (s != OrchestratorState::kScanning && s != OrchestratorState::kPlacing) {
    // 幂等：已在 REVIEWING 时重复调用直接成功。
    if (s == OrchestratorState::kReviewing) return RECON_OK;
    return RECON_ERR_INVALID_ARGUMENT;
  }

  size_t n = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    n = frames_.size();
  }
  if (n == 0) return RECON_ERR_MVS_NO_DEPTH;

  SetState(OrchestratorState::kReviewing);
  EmitLog(1, "采集结束，共 " + std::to_string(n) + " 帧");
  FlushProgress(RECON_STAGE_IDLE, 0.0, "采集完成");
  return RECON_OK;
}

void Orchestrator::cancel() {
  if (ctx_ == nullptr) return;
  ctx_->cancel_flag().store(true, std::memory_order_relaxed);
  SetState(OrchestratorState::kCancelling);
}

// ---------------------------------------------------------------------------
// 阶段 1：预处理
// ---------------------------------------------------------------------------
recon_status Orchestrator::StagePreprocess(const ReconOptions& opt,
                                           StageResult* out) {
  (void)opt;
  size_t n = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    n = frames_.size();
  }

  for (size_t i = 0; i < n; ++i) {
    if ((i & 31u) == 0 && CheckCancel()) {
      return RECON_ERR_CANCELLED;
    }
    CapturedFrame& f = frames_[i];

    if (f.rgb.empty() || f.rgb_width <= 0 || f.rgb_height <= 0) continue;

    BgraToGray(f.rgb.data(), f.rgb_width, f.rgb_height, f.rgb_stride, &f.gray,
               &f.gray_stride);
    f.gray_width = f.rgb_width;
    f.gray_height = f.rgb_height;

    // 内参缺失时按图像尺寸给一个合理默认（fx = fy = 0.9 × 宽，
    // 对应约 58° 水平 FOV 的手机主摄），并记警告 —— 不静默。
    if (!f.intrinsics_valid) {
      f.intrinsics.fx = 0.9 * static_cast<double>(f.rgb_width);
      f.intrinsics.fy = f.intrinsics.fx;
      f.intrinsics.cx = 0.5 * static_cast<double>(f.rgb_width);
      f.intrinsics.cy = 0.5 * static_cast<double>(f.rgb_height);
      f.intrinsics.width = f.rgb_width;
      f.intrinsics.height = f.rgb_height;
      f.intrinsics_valid = true;
      if (i == 0) EmitLog(2, "帧内参缺失，已按 0.9×宽估计焦距");
    }

    EmitProgress(RECON_STAGE_PREPROCESS,
                 static_cast<double>(i + 1) / static_cast<double>(n),
                 "预处理");
  }

  accepted_frames_ = n;
  out->metrics.push_back({"frames", static_cast<double>(n)});
  // 与 C ABI 的采集统计保持一致（单一数据源）。
  if (ctx_ != nullptr) {
    ctx_->scan_stats().frames_accepted = n;
    ctx_->scan_stats().keyframes = n;
  }
  return RECON_OK;
}

// ---------------------------------------------------------------------------
// 阶段 2：位姿（VIO 或平台先验）
//
// 契约：平台位姿是「先验」，VIO 是「补充」。真实产品里二者会在 MSCKF 里
// 联合优化；当前版本的分工是：
//   · 全部帧都有平台位姿 → 直接用（精度已由平台 AR 子系统保证）
//   · 缺平台位姿但有 IMU + 灰度 → 跑 MSCKF
//   · 都没有 → 无法建图，返回 VIO_NOT_INITIALIZED
// ---------------------------------------------------------------------------
recon_status Orchestrator::StageVio(const ReconOptions& opt, StageResult* out) {
  (void)opt;
  size_t n = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    n = frames_.size();
  }

  key_poses_.clear();
  key_intrinsics_.clear();
  keyframe_indices_.clear();
  key_timestamps_ns_.clear();

  size_t with_pose = 0;
  size_t with_imu = 0;
  size_t with_gray = 0;
  for (size_t i = 0; i < n; ++i) {
    const CapturedFrame& f = frames_[i];
    if (f.pose_valid) ++with_pose;
    if (!f.imu.empty()) ++with_imu;
    if (!f.gray.empty()) ++with_gray;
  }

  if (with_pose == n && n > 0) {
    // 平台位姿齐备：直接用。这是主流路径（ARKit / ARCore）。
    key_poses_.reserve(n);
    key_intrinsics_.reserve(n);
    keyframe_indices_.reserve(n);
    key_timestamps_ns_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      const CapturedFrame& f = frames_[i];
      key_poses_.push_back(f.pose);
      key_intrinsics_.push_back(f.intrinsics);
      keyframe_indices_.push_back(static_cast<int>(i));
      key_timestamps_ns_.push_back(f.timestamp_ns);
    }
    out->message = "平台位姿";
    out->metrics.push_back({"keyframes", static_cast<double>(n)});
    EmitLog(1, "位姿：使用平台先验 " + std::to_string(n) + " 帧");
    EmitProgress(RECON_STAGE_VIO, 1.0, "位姿就绪");
    return RECON_OK;
  }

  if (with_gray == 0) {
    return RECON_ERR_VIO_NOT_INITIALIZED;
  }

  // 走 VIO 需要 IMU（MSCKF 是视觉惯性耦合的）。**无 IMU 时退到 SfM**
  // （docs/05 §2.4 C1：C1 不满足 → 跳过 §3 全部 VIO，SfM 直接承担位姿）。
  // 这不是降级为低精度模式，而是同一精度主线的另一条位姿来源
  // （docs/00 §5.3「降级不是分支」）。
  if (with_imu == 0) {
    return StageSfmMonol(opt, out);
  }

  VioConfig vcfg;
  vcfg.feature_count =
      static_cast<int>(ParamOr(keys::kVioFeatureCount, vcfg.feature_count));
  vcfg.window_size =
      static_cast<int>(ParamOr(keys::kVioMsckfWindowSize, vcfg.window_size));
  ViOEstimator vio(vcfg);

  size_t fed = 0;       // 实际喂进估计器的帧数（灰度可用的帧）
  size_t accepted = 0;  // 被接收为关键帧的帧数
  // VIO 的 frame_id 从 1 起、只对**被喂入的帧**自增；而 frames_ 里可能夹着
  // 灰度为空（RGB 缺失）的帧。据 frame_id 反查 frames_ 时二者会错位，
  // 故显式记录「第 k 次喂入对应 frames_ 的哪个下标」。
  std::vector<int> fed_frame_index;
  fed_frame_index.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    if ((i & 7u) == 0 && CheckCancel()) return RECON_ERR_CANCELLED;
    const CapturedFrame& f = frames_[i];
    if (f.gray.empty()) continue;
    ++fed;
    fed_frame_index.push_back(static_cast<int>(i));
    const bool is_kf = vio.AddFrame(f.gray.data(), f.gray_width, f.gray_height,
                                    f.gray_stride,
                                    static_cast<int64_t>(f.timestamp_ns), f.imu);
    if (is_kf) ++accepted;
    EmitProgress(RECON_STAGE_VIO,
                 static_cast<double>(i + 1) / static_cast<double>(n), "视觉惯性求解");
  }

  if (!vio.initialized() || vio.keyframe_count() == 0) {
    EmitLog(2, "VIO 未收敛：喂入 " + std::to_string(fed) + " 帧，接收 " +
                   std::to_string(accepted) + " 个关键帧");
    return RECON_ERR_VIO_NOT_INITIALIZED;
  }

  const VioState& st = vio.state();
  key_poses_.reserve(st.keyframes.size());
  key_intrinsics_.reserve(st.keyframes.size());
  keyframe_indices_.reserve(st.keyframes.size());
  key_timestamps_ns_.reserve(st.keyframes.size());
  for (const KeyframeState& kf : st.keyframes) {
    key_poses_.push_back(kf.pose);
    // VIO 的 frame_id 从 1 开始，故 kf.frame_id-1 是它在**喂入序列**里的序号；
    // 再经 fed_frame_index 映射回 frames_ 的真实下标。
    const size_t fed_ord = static_cast<size_t>(kf.frame_id > 0 ? kf.frame_id - 1 : 0);
    const size_t idx =
        fed_ord < fed_frame_index.size() ? static_cast<size_t>(fed_frame_index[fed_ord])
                                         : 0;
    key_intrinsics_.push_back(idx < frames_.size() ? frames_[idx].intrinsics
                                                   : frames_[0].intrinsics);
    keyframe_indices_.push_back(static_cast<int>(idx));
    // 时间戳直接取关键帧自带的值（VIO 在挂帧时写入本帧曝光中点，§3.10），
    // 不再回查 frames_ —— 单一数据源，避免两处实现漂移。
    key_timestamps_ns_.push_back(static_cast<uint64_t>(kf.timestamp_ns));
  }

  out->message = "VIO 位姿";
  out->metrics.push_back({"fed_frames", static_cast<double>(fed)});
  out->metrics.push_back({"accepted_keyframes", static_cast<double>(accepted)});
  out->metrics.push_back({"keyframes", static_cast<double>(key_poses_.size())});
  EmitLog(1, "位姿：VIO 关键帧 " + std::to_string(key_poses_.size()) + " 个");
  return RECON_OK;
}

// ---------------------------------------------------------------------------
// 阶段 2b：MONO 无 IMU 分支的 SfM 位姿（docs/05 §2.4 C1、§4）
//
// 何时到达这里：既无平台位姿、又无 IMU —— MONO 分支的 C1 门限不满足。
// 按 docs/05 §2.4 C1，此时**跳过全部 VIO**，由 SfM 直接承担位姿；
// 尺度只能来自参照物标定（docs/07），故 SfM 输出的位姿是**重建单位 ru**，
// 与「平台位姿」路径（米制）在编排层的下游（MVS）用法一致 —— MVS 只关心
// 相对几何，尺度在 SCALE 阶段统一注入。
//
// 输入：frames_ 里所有灰度可用的帧（不限于关键帧 —— SfM 自己做选帧）。
// 输出：key_poses_ / key_intrinsics_ / keyframe_indices_ / key_timestamps_ns_。
// ---------------------------------------------------------------------------
recon_status Orchestrator::StageSfmMonol(const ReconOptions& opt,
                                         StageResult* out) {
  (void)opt;
  size_t n = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    n = frames_.size();
  }

  // 收集灰度可用的帧（SfM 的 SfmView 需要裸指针，故先建索引数组，
  // 再在循环里取 frames_[idx] 的缓冲 —— 避免拷贝整幅图）。
  std::vector<int> usable;
  usable.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    if (!frames_[i].gray.empty()) usable.push_back(static_cast<int>(i));
  }
  if (usable.size() < 2) {
    out->message = "SfM：可用灰度帧不足 2（是否漏跑 PREPROCESS？）";
    EmitLog(2, out->message);
    return RECON_ERR_VIO_NOT_INITIALIZED;
  }

  SfmConfig scfg;
  scfg.feature_count =
      static_cast<int>(ParamOr(keys::kSfmFeatureCount, scfg.feature_count));
  scfg.match_ratio_threshold = ParamOr(keys::kSfmMatchRatioThreshold,
                                       scfg.match_ratio_threshold);
  scfg.max_num_neighbors =
      static_cast<int>(ParamOr(keys::kSfmMaxNumNeighbors, scfg.max_num_neighbors));
  scfg.ransac_max_iterations = static_cast<int>(
      ParamOr(keys::kSfmRansacMaxIterations, scfg.ransac_max_iterations));
  scfg.ransac_min_inliers = static_cast<int>(
      ParamOr(keys::kSfmRansacMinInliers, scfg.ransac_min_inliers));
  scfg.ransac_confidence =
      ParamOr(keys::kSfmRansacConfidence, scfg.ransac_confidence);
  scfg.pnp_ransac_error_px =
      ParamOr(keys::kSfmRansacMaxErrorPx, scfg.pnp_ransac_error_px);
  scfg.ba_max_iterations =
      static_cast<int>(ParamOr(keys::kSfmBaMaxIterations, scfg.ba_max_iterations));
  scfg.ba_convergence_tol =
      ParamOr(keys::kSfmBaConvergenceTolerance, scfg.ba_convergence_tol);
  scfg.ba_huber_delta_px =
      ParamOr(keys::kSfmBaHuberDelta, scfg.ba_huber_delta_px);
  scfg.ba_min_num_images =
      static_cast<int>(ParamOr(keys::kSfmBaMinNumImages, scfg.ba_min_num_images));
  scfg.min_track_length =
      static_cast<int>(ParamOr(keys::kSfmMinTrackLength, scfg.min_track_length));

  std::vector<SfmView> views;
  views.reserve(usable.size());
  for (size_t k = 0; k < usable.size(); ++k) {
    const CapturedFrame& f = frames_[static_cast<size_t>(usable[k])];
    SfmView v;
    v.gray = f.gray.data();
    v.width = f.gray_width;
    v.height = f.gray_height;
    v.stride = f.gray_stride;
    v.intrinsics = f.intrinsics;
    views.push_back(v);
    EmitProgress(RECON_STAGE_VIO,
                 static_cast<double>(k + 1) / static_cast<double>(usable.size()),
                 "SfM 稀疏建图");
  }

  const SfmResult sr = RunIncrementalSfm(views, scfg);
  if (!sr.ok || sr.num_registered < 2) {
    out->message = "SfM 未收敛：视图 " + std::to_string(usable.size()) +
                   "，注册 " + std::to_string(sr.num_registered) + "，点 " +
                   std::to_string(sr.num_points);
    out->metrics.push_back({"sfm_views", static_cast<double>(usable.size())});
    out->metrics.push_back({"sfm_registered", static_cast<double>(sr.num_registered)});
    out->metrics.push_back({"sfm_points", static_cast<double>(sr.num_points)});
    EmitLog(2, out->message);
    return RECON_ERR_VIO_NOT_INITIALIZED;
  }

  key_poses_.clear();
  key_intrinsics_.clear();
  keyframe_indices_.clear();
  key_timestamps_ns_.clear();
  key_poses_.reserve(static_cast<size_t>(sr.num_registered));
  key_intrinsics_.reserve(static_cast<size_t>(sr.num_registered));
  keyframe_indices_.reserve(static_cast<size_t>(sr.num_registered));
  key_timestamps_ns_.reserve(static_cast<size_t>(sr.num_registered));

  for (size_t k = 0; k < sr.registered.size(); ++k) {
    if (!sr.registered[k]) continue;
    const int frame_idx = usable[k];
    const CapturedFrame& f = frames_[static_cast<size_t>(frame_idx)];
    key_poses_.push_back(sr.poses[k]);
    key_intrinsics_.push_back(f.intrinsics);
    keyframe_indices_.push_back(frame_idx);
    key_timestamps_ns_.push_back(f.timestamp_ns);
  }

  sfm_registered_frames_ = sr.num_registered;
  sfm_sparse_points_ = sr.num_points;
  sfm_ba_rms_px_ = sr.ba.rms_reproj_px;

  out->message = "SfM 位姿（无 IMU）";
  out->metrics.push_back({"keyframes", static_cast<double>(key_poses_.size())});
  out->metrics.push_back({"sfm_registered", static_cast<double>(sr.num_registered)});
  out->metrics.push_back({"sfm_points", static_cast<double>(sr.num_points)});
  out->metrics.push_back({"sfm_ba_rms_px", sr.ba.rms_reproj_px});
  EmitLog(1, "位姿：SfM 注册 " + std::to_string(sr.num_registered) + " 帧，稀疏点 " +
                 std::to_string(sr.num_points) + "，BA RMS " +
                 std::to_string(sr.ba.rms_reproj_px) + " px");
  return RECON_OK;
}

// ---------------------------------------------------------------------------
// 关键帧数组（供 MVS 的全局索引契约使用）
// ---------------------------------------------------------------------------
void Orchestrator::BuildKeyframeArrays(
    std::vector<CameraPose>* out_poses,
    std::vector<CameraIntrinsics>* out_intrinsics) const {
  *out_poses = key_poses_;
  *out_intrinsics = key_intrinsics_;
}

// ---------------------------------------------------------------------------
// 阶段 3：MVS 稠密重建
//
// PatchMatch 的索引契约（mvs.h 里明确警告过，曾因混淆越界）：
//   src_gray / src_w / src_h 按**相对索引 k**（只含源视图）
//   src_poses / src_intrinsics 按**全局索引**（全部关键帧）
//   source_indices[k] 给出第 k 个源视图在全局数组中的下标
// ---------------------------------------------------------------------------
recon_status Orchestrator::StageMvs(const ReconOptions& opt, StageResult* out) {
  if (!opt.enable_mvs) {
    out->degraded = true;
    out->message = "MVS 已关闭";
    return RECON_OK;
  }
  if (key_poses_.size() < 2) {
    return RECON_ERR_MVS_NO_DEPTH;
  }

  MvsConfig mcfg;
  mcfg.num_views = static_cast<int>(ParamOr(keys::kMvsNumViews, mcfg.num_views));
  mcfg.patch_size = static_cast<int>(ParamOr(keys::kMvsPatchSize, mcfg.patch_size));
  mcfg.iterations =
      static_cast<int>(ParamOr(keys::kMvsPatchIterations, mcfg.iterations));
  mcfg.subpixel_iterations =
      static_cast<int>(ParamOr(keys::kMvsSubpixelIterations, mcfg.subpixel_iterations));
  mcfg.min_depth_m = ParamOr(keys::kMvsMinDepthM, mcfg.min_depth_m);
  mcfg.max_depth_m = ParamOr(keys::kMvsMaxDepthM, mcfg.max_depth_m);
  mcfg.fusion_voxel_size_m =
      ParamOr(keys::kMvsFusionVoxelSizeM, mcfg.fusion_voxel_size_m);
  mcfg.num_threads = static_cast<int>(ParamOr(keys::kPerfThreadPoolSize, 0.0));

  std::vector<CameraPose> all_poses;
  std::vector<CameraIntrinsics> all_intrinsics;
  BuildKeyframeArrays(&all_poses, &all_intrinsics);

  // 灰度图全局数组（与 keyframe_indices_ 对齐）。
  std::vector<const uint8_t*> kf_gray;
  kf_gray.reserve(keyframe_indices_.size());
  for (int idx : keyframe_indices_) {
    const CapturedFrame& f = frames_[static_cast<size_t>(idx)];
    kf_gray.push_back(f.gray.empty() ? nullptr : f.gray.data());
  }

  std::vector<DepthMap> all_depths(all_poses.size());
  size_t produced = 0;
  const size_t total = all_poses.size();

  for (size_t r = 0; r < total; ++r) {
    if (CheckCancel()) return RECON_ERR_CANCELLED;
    if (kf_gray[r] == nullptr) continue;
    const CapturedFrame& ref = frames_[static_cast<size_t>(keyframe_indices_[r])];

    // 1) 视图选择。
    std::vector<int> src_idx =
        SelectSourceViews(all_poses[r], all_poses, all_intrinsics[r], mcfg);
    // 排除自身，并剔除无灰度图的视图。
    std::vector<int> kept;
    for (int si : src_idx) {
      if (si == static_cast<int>(r)) continue;
      if (static_cast<size_t>(si) >= kf_gray.size()) continue;
      if (kf_gray[static_cast<size_t>(si)] == nullptr) continue;
      kept.push_back(si);
    }
    if (kept.empty()) continue;
    if (static_cast<int>(kept.size()) > mcfg.num_views) {
      kept.resize(static_cast<size_t>(mcfg.num_views));
    }

    // 2) 组装按相对索引的源视图数组。
    std::vector<const uint8_t*> src_gray;
    std::vector<int> src_w;
    std::vector<int> src_h;
    src_gray.reserve(kept.size());
    src_w.reserve(kept.size());
    src_h.reserve(kept.size());
    for (int si : kept) {
      const CapturedFrame& sf = frames_[static_cast<size_t>(keyframe_indices_[si])];
      src_gray.push_back(sf.gray.data());
      src_w.push_back(sf.gray_width);
      src_h.push_back(sf.gray_height);
    }

    // 3) PatchMatch + 亚像素。
    DepthMap dm;
    const uint32_t filled =
        PatchMatchStereo(kf_gray[r], ref.gray_width, ref.gray_height, src_gray,
                         src_w, src_h, all_poses[r], all_poses,
                         all_intrinsics[r], all_intrinsics, kept, mcfg, nullptr,
                         &dm);
    if (filled == 0) continue;
    RefineSubpixel(kf_gray[r], ref.gray_width, ref.gray_height, src_gray, src_w,
                   src_h, all_poses[r], all_poses, all_intrinsics[r],
                   all_intrinsics, kept, mcfg, &dm);
    all_depths[r] = std::move(dm);
    ++produced;

    EmitProgress(RECON_STAGE_MVS,
                 static_cast<double>(r + 1) / static_cast<double>(total),
                 "稠密匹配");
  }

  if (produced == 0) return RECON_ERR_MVS_NO_DEPTH;

  // 4) 多视图融合。poses / intrinsics 必须与 depth_maps 逐元素对齐。
  std::vector<const DepthMap*> dm_ptrs;
  std::vector<CameraPose> dm_poses;
  std::vector<CameraIntrinsics> dm_intr;
  for (size_t r = 0; r < total; ++r) {
    if (all_depths[r].width == 0) continue;
    dm_ptrs.push_back(&all_depths[r]);
    dm_poses.push_back(all_poses[r]);
    dm_intr.push_back(all_intrinsics[r]);
  }
  PointCloud cloud = FuseDepthMaps(dm_ptrs, dm_poses, dm_intr, mcfg);

  UnifiedPointCloud uni;
  uni.positions = std::move(cloud.positions);
  uni.normals = std::move(cloud.normals);
  uni.colors = std::move(cloud.colors);
  uni.confidences = std::move(cloud.confidences);

  // MVS 深度是主精度来源，直接作为云端起点（TSDF 稍后并入）。
  cloud_ = std::move(uni);
  mvs_depth_maps_ = produced;

  out->metrics.push_back({"depth_maps", static_cast<double>(produced)});
  out->metrics.push_back({"points", static_cast<double>(cloud_.size())});
  EmitLog(1, "MVS：深度图 " + std::to_string(produced) + " 张 → 点云 " +
                 std::to_string(cloud_.size()) + " 点");
  return RECON_OK;
}

// ---------------------------------------------------------------------------
// 阶段 4：TSDF 融合（RGBD 分支）
//
// 体素网格的边界由**已有点云的包围盒**决定。为什么不用平台给出的体积：
// 平台给出的通常是 AR 会话的参考体，与本次扫描对象无关；而用点云包围盒
// 可以自动贴合物体，避免把 90% 的体素浪费在空区域（直接影响内存与耗时）。
// ---------------------------------------------------------------------------
recon_status Orchestrator::StageTsdf(const ReconOptions& opt, StageResult* out) {
  if (!opt.enable_tsdf || branch_ != RECON_BRANCH_RGBD) {
    out->degraded = true;
    out->message = "TSDF 跳过（非 RGBD 分支或已关闭）";
    return RECON_OK;
  }

  // 收集所有带深度的帧。
  std::vector<size_t> depth_frames;
  for (size_t i = 0; i < frames_.size(); ++i) {
    const CapturedFrame& f = frames_[i];
    if (f.has_depth && !f.depth.empty() && f.pose_valid &&
        f.depth_width > 0 && f.depth_height > 0) {
      depth_frames.push_back(i);
    }
  }
  if (depth_frames.empty()) {
    out->degraded = true;
    out->message = "无可用深度帧";
    return RECON_OK;
  }

  // 包围盒：由已有 MVS 点云 + 深度帧粗采样点共同决定。
  std::vector<Vec3d> bounds_pts;
  if (cloud_.size() > 0) {
    const size_t step = std::max<size_t>(1, cloud_.size() / 20000);
    for (size_t i = 0; i < cloud_.size(); i += step) {
      bounds_pts.push_back(cloud_.positions[i]);
    }
  }
  if (bounds_pts.size() < 32) {
    // MVS 点云不可用，用深度图粗采样（每 8 像素取一个）。
    //
    // **必须用「深度分辨率下的内参」反投影**：`x`/`y` 是深度像素索引，
    // 而 `f.intrinsics` 是彩色内参（分辨率更高，见 docs/11 §3.7）。
    // 二者分辨率不同时直接混用会按 `s` 倍缩放坐标 → 包围盒被系统性撑大
    // （或缩小），进而让 TSDF 体素预算与截断带范围失准。
    // 旧实现正是直接用彩色内参配深度像素（2026-10-31 第三十四轮实测），
    // 与上方内参换算段的缩放口径**保持同一份实现**（MEMORY 铁律：
    // 同一逻辑不写两遍）。
    for (size_t fi : depth_frames) {
      const CapturedFrame& f = frames_[fi];
      CameraIntrinsics k_src = f.intrinsics;
      if (k_src.width <= 0 || k_src.height <= 0) {
        if (f.rgb_width > 0 && f.rgb_height > 0) {
          k_src.width = f.rgb_width;
          k_src.height = f.rgb_height;
        }
      }
      const CameraIntrinsics kd =
          ScaleIntrinsicsToResolution(k_src, f.depth_width, f.depth_height);
      for (int y = 0; y < f.depth_height; y += 8) {
        for (int x = 0; x < f.depth_width; x += 8) {
          const uint16_t d = f.depth[static_cast<size_t>(y) * f.depth_width + x];
          if (d == 0) continue;
          const double z = d * 1e-3;
          if (z < 0.05 || z > 3.0) continue;
          const double px = (x - kd.cx) / kd.fx;
          const double py = (y - kd.cy) / kd.fy;
          bounds_pts.push_back(f.pose.CameraToWorld(Vec3d(px * z, py * z, z)));
        }
      }
    }
  }
  if (bounds_pts.empty()) {
    out->degraded = true;
    out->message = "无法确定融合体积边界";
    return RECON_OK;
  }

  Vec3d lo, hi;
  const double voxel = ParamOr(keys::kRgbdTsdfVoxelSize, 0.001);
  PointCloudBounds(bounds_pts, voxel * 4.0, &lo, &hi);

  TsdfConfig tcfg;
  tcfg.voxel_size_m = voxel;
  tcfg.truncation_m = ParamOr(keys::kRgbdTsdfTruncation, tcfg.truncation_m);
  tcfg.max_depth_m = ParamOr(keys::kRgbdTsdfMaxDepth, tcfg.max_depth_m);
  tcfg.min_confidence =
      static_cast<int>(ParamOr(keys::kRgbdMinDepthConfidence, tcfg.min_confidence));
  tcfg.origin_x = lo(0);
  tcfg.origin_y = lo(1);
  tcfg.origin_z = lo(2);
  tcfg.bbox_max_x = hi(0);
  tcfg.bbox_max_y = hi(1);
  tcfg.bbox_max_z = hi(2);

  // 内参：`TsdfVolume` 的内参在构造期固定，因此要求所有融合帧的深度内参
  // 一致 —— 产品上同一台设备的深度内参恒定，这个前提成立。取第一帧的
  // 深度内参，并校验其余帧是否一致。
  //
  // ── 为什么在这里「按分辨率换算」而不是「假定期望就是 RGB 内参」 ──────────
  //
  // ABI 的 `intrinsics` 语义是「该内参对应的图像尺寸」（见
  // `recon_camera_intrinsics.width/height` 与 docs/01 §2.2.2）。平台侧
  // （docs/11 §3.7）上报的是**彩色相机内参**（iOS `ARFrame.camera.intrinsics`，
  // 分辨率为彩色图）。而深度图通常**分辨率更低**（ARKit 256×192 vs
  // RGB 1920×1440）。TSDF 积分在**深度原生分辨率**上进行（docs/04 §3.6 IR-2），
  // 因此必须把彩色内参换算到深度分辨率，且换算**必须含主点的 +0.5/−0.5 补偿**
  // （docs/02 C-1、docs/04 §3.5.3 明文要求；漏掉会引入 0.5 px 系统偏差，
  // 恰好是 00 §4.6 的验收线量级）。
  //
  // ── 旧实现的三个错误（2026-10-31 第三十四轮实测）───────────────────────
  //   ① 只用**单一标量** `s = depth_width / rgb_width` 同时乘 fx/fy/cx/cy；
  //      宽高比不同时 fy/cy 必错。
  //   ② **主点无补偿**（`cx * s`），违反 C-1。
  //   ③ 缩放守卫写成 `f0.rgb_width > 0` —— 而 C ABI 明确允许
  //      `rgb_data == nullptr`（纯深度帧），此时 `rgb_width` 恒为 0，
  //      `s` 静默停在 1.0，深度内参**完全不缩放** → 点云 X/Y 系统性压缩。
  //      正确的源尺寸应取**内参自身的 width/height**，与 RGB 缓冲是否存在无关。
  {
    const CapturedFrame& f0 = frames_[depth_frames[0]];
    // 源内参尺寸优先用内参自带的 width/height（平台已按 docs/01 §2.2.2 填）；
    // 平台漏填时（width<=0）退回用 RGB 缓冲尺寸，再不行则不缩放并记录。
    CameraIntrinsics k_src = f0.intrinsics;
    if (k_src.width <= 0 || k_src.height <= 0) {
      if (f0.rgb_width > 0 && f0.rgb_height > 0) {
        k_src.width = f0.rgb_width;
        k_src.height = f0.rgb_height;
        EmitLog(2, "平台内参未带图像尺寸，回退用 RGB 缓冲尺寸换算深度内参");
      } else {
        EmitLog(2, "内参与 RGB 缓冲均无可用尺寸，深度内参不做分辨率换算");
      }
    }
    const CameraIntrinsics k_depth = ScaleIntrinsicsToResolution(
        k_src, f0.depth_width, f0.depth_height);
    tcfg.fx = k_depth.fx;
    tcfg.fy = k_depth.fy;
    tcfg.cx = k_depth.cx;
    tcfg.cy = k_depth.cy;

    bool inconsistent = false;
    for (size_t fi : depth_frames) {
      const CapturedFrame& f = frames_[fi];
      if (f.depth_width != f0.depth_width ||
          f.depth_height != f0.depth_height ||
          std::fabs(f.intrinsics.fx - f0.intrinsics.fx) >
              1e-3 * std::fabs(f0.intrinsics.fx)) {
        inconsistent = true;
        break;
      }
    }
    if (inconsistent) {
      // 不静默：内参不一致会让融合产生系统偏差，必须让用户看到。
      EmitLog(2, "深度帧内参/分辨率不一致，TSDF 融合按首帧内参执行");
      out->degraded = true;
    }
    out->metrics.push_back({"tsdf_depth_intrinsics_fx", k_depth.fx});
    out->metrics.push_back({"tsdf_depth_intrinsics_cx", k_depth.cx});
    out->metrics.push_back({"tsdf_intrinsics_scaled", 1.0});
  }

  // 体素总数上限保护：超过 memory_limit 对应的规模就退到更粗的体素
  // （这属于「分块 / 内存组织」类降级，不是精度降级，R-PREC-4）。
  const double vx = (hi(0) - lo(0)) / tcfg.voxel_size_m;
  const double vy = (hi(1) - lo(1)) / tcfg.voxel_size_m;
  const double vz = (hi(2) - lo(2)) / tcfg.voxel_size_m;
  const double cells = vx * vy * vz;
  const double mem_limit_mb = ParamOr(keys::kPerfMemoryLimitMb, 2048.0);
  const double cells_limit = mem_limit_mb * 1024.0 * 1024.0 / 20.0;  // 20 B/体素
  bool coarsened = false;
  if (cells > cells_limit && cells > 0.0) {
    const double scale = std::cbrt(cells_limit / cells);
    tcfg.voxel_size_m /= scale;
    coarsened = true;
    EmitLog(2, "TSDF 体素数超预算，体素边长放宽 " +
                   std::to_string(1.0 / scale) + " 倍（仅内存组织，非精度降级）");
  }

  TsdfVolume volume(tcfg);

  // 逐帧融合。
  size_t integrated = 0;
  for (size_t fi : depth_frames) {
    if (CheckCancel()) return RECON_ERR_CANCELLED;
    const CapturedFrame& f = frames_[fi];

    const Mat4d w2c = ToMat4(f.pose);
    const uint32_t n = volume.IntegrateDepthFrame(
        f.depth.data(), f.depth_confidence.empty() ? nullptr
                                                   : f.depth_confidence.data(),
        f.depth_width, f.depth_height, w2c, 0.002f);
    integrated += n;
    EmitProgress(RECON_STAGE_RGBD_FUSION,
                 static_cast<double>(integrated) /
                     static_cast<double>(depth_frames.size() * 200000u + 1u),
                 "深度融合");
  }

  // 提取零交叉点云。
  UnifiedPointCloud tsdf_cloud;
  volume.ExtractPoints(&tsdf_cloud.positions, &tsdf_cloud.normals,
                       &tsdf_cloud.confidences);
  tsdf_cloud.colors.assign(tsdf_cloud.positions.size() * 3, 128.0f);

  tsdf_integrated_pixels_ = integrated;

  // 合并进主点云（去重）。
  const double merge_voxel =
      std::max(1e-5, 0.5 * ParamOr(keys::kMvsFusionVoxelSizeM, 0.001));
  const size_t before = cloud_.size();
  MergeDedupe(tsdf_cloud, merge_voxel, &cloud_);

  out->degraded = coarsened;
  out->metrics.push_back({"integrated_pixels", static_cast<double>(integrated)});
  out->metrics.push_back({"tsdf_points", static_cast<double>(tsdf_cloud.size())});
  out->metrics.push_back({"merged_new", static_cast<double>(cloud_.size() - before)});
  EmitLog(1, "TSDF：融合 " + std::to_string(integrated) + " 像素，提取 " +
                 std::to_string(tsdf_cloud.size()) + " 点，新增 " +
                 std::to_string(cloud_.size() - before) + " 点");
  return RECON_OK;
}

// ---------------------------------------------------------------------------
// 阶段 5：泊松曲面重建
//
// 深度选取（docs/06 §1.3 的硬约束）：八叉树单元边长 r 必须 ≥ 点云采样间距，
// 否则点云支撑断开会把等值面碎成孤立片。因此 D 由采样间距反推，
// 再由参数区间夹取（参数是用户可覆盖的边界，不是降级开关）。
// ---------------------------------------------------------------------------
recon_status Orchestrator::StagePoisson(const ReconOptions& opt, StageResult* out) {
  (void)opt;
  if (cloud_.size() < 100) {
    return RECON_ERR_MESH_POISSON_FAILED;
  }

  OrientedPointCloud pc = cloud_.ToOrientedCloud();

  Vec3d lo, hi;
  PointCloudBounds(pc.positions, 0.0, &lo, &hi);
  const Vec3d ext = hi - lo;
  const double longest = std::max(ext(0), std::max(ext(1), ext(2)));

  PoissonConfig pcfg;
  pcfg.scale = ParamOr(keys::kMeshPoissonScale, pcfg.scale);
  pcfg.samples_per_node =
      ParamOr(keys::kMeshPoissonSamplesPerNode, pcfg.samples_per_node);
  pcfg.point_weight = ParamOr(keys::kMeshPoissonPointWeight, 1.0) > 0.5;
  pcfg.prune = ParamOr(keys::kMeshPoissonPrune, 1.0) > 0.5;
  pcfg.confidence = ParamOr(keys::kMeshPoissonConfidence, 1.0) > 0.5;
  pcfg.interpolate = ParamOr(keys::kMeshPoissonInterpolate, 1.0) > 0.5;
  pcfg.trim_confidence_quantile =
      ParamOr(keys::kMeshTrimConfQuantile, pcfg.trim_confidence_quantile);

  const double spacing = EstimatePointSpacing(pc);
  const int derived = PoissonDepthForPointSpacing(longest, pcfg.scale, spacing);
  const int dmin =
      static_cast<int>(ParamOr(keys::kMeshPoissonDepthMin, 6.0));
  const int dmax =
      static_cast<int>(ParamOr(keys::kMeshPoissonDepthMax, 14.0));
  pcfg.depth = std::max(dmin, std::min(dmax, derived));

  EmitLog(1, "泊松：点间距 " + std::to_string(spacing * 1000.0) + " mm，反推 depth=" +
                 std::to_string(derived) + "，采用 " + std::to_string(pcfg.depth) +
                 "，分辨率 " +
                 std::to_string(PoissonResolution(longest, pcfg.scale, pcfg.depth) *
                                1000.0) +
                 " mm");
  FlushProgress(RECON_STAGE_MESH, 0.1, "泊松求解");

  PoissonResult result;
  if (!PoissonReconstruct(pc, pcfg, &result)) {
    EmitLog(3, "泊松重建失败");
    return RECON_ERR_MESH_POISSON_FAILED;
  }
  if (CheckCancel()) return RECON_ERR_CANCELLED;

  mesh_ = std::move(result.mesh);
  poisson_resolution_ = result.resolution;
  poisson_depth_used_ = pcfg.depth;

  out->metrics.push_back({"depth", static_cast<double>(pcfg.depth)});
  out->metrics.push_back({"resolution_mm", result.resolution * 1000.0});
  out->metrics.push_back({"triangles_before_trim",
                          static_cast<double>(result.triangles_before_trim)});
  out->metrics.push_back({"triangles_after_trim",
                          static_cast<double>(result.triangles_after_trim)});
  out->metrics.push_back({"mesh_triangles",
                          static_cast<double>(mesh_.triangle_count())});
  return RECON_OK;
}

// ---------------------------------------------------------------------------
// 阶段 6：后处理（S4/S5/S6/S7）
// ---------------------------------------------------------------------------
recon_status Orchestrator::StagePostproc(const ReconOptions& opt, StageResult* out) {
  (void)opt;
  if (mesh_.triangle_count() == 0) {
    return RECON_ERR_POST_HOLE_FILL_FAILED;
  }

  MeshPipelineConfig mpc;
  // 特征边长 = 泊松分辨率（< 0 时回退到平均边长）。
  mpc.resolution = poisson_resolution_ > 0.0 ? poisson_resolution_ : 1e-3;
  mpc.hole.resolution = mpc.resolution;
  mpc.hole.micro_area_ratio = ParamOr(keys::kPostHoleAreaMicroRatio,
                                      mpc.hole.micro_area_ratio);
  mpc.hole.small_area_ratio = ParamOr(keys::kPostHoleAreaSmallRatio,
                                      mpc.hole.small_area_ratio);
  mpc.hole.large_area_ratio = ParamOr(keys::kPostHoleAreaLargeRatio,
                                      mpc.hole.large_area_ratio);
  // 由流水线统一驱动 S4→S5 迭代（见 mesh_repair.h 的说明）。
  mpc.hole.post_repair_enabled = false;
  mpc.orient_outward = true;
  mpc.recompute_normals = true;

  EmitProgress(RECON_STAGE_POSTPROC, 0.2, "拓扑修复");

  const MeshPipelineReport rep = RunMeshPipeline(&mesh_, mpc);
  if (CheckCancel()) return RECON_ERR_CANCELLED;

  if (mesh_.triangle_count() == 0) {
    return RECON_ERR_POST_HOLE_FILL_FAILED;
  }

  out->metrics.push_back({"winding_inconsistent",
                          static_cast<double>(rep.stats.inconsistent_winding_edges)});
  out->metrics.push_back({"non_manifold", static_cast<double>(rep.stats.non_manifold_edges)});
  out->metrics.push_back({"boundary_loops", static_cast<double>(rep.stats.boundary_loops)});
  out->metrics.push_back({"holes_filled", static_cast<double>(rep.holes.filled_count)});
  out->metrics.push_back({"outward_flip_fixed",
                          static_cast<double>(rep.outward_flip_fixed)});
  out->metrics.push_back({"edge_flips", static_cast<double>(rep.edge_flips)});
  out->metrics.push_back({"edge_collapses", static_cast<double>(rep.edge_collapses)});
  EmitLog(1, "后处理：非流形 " + std::to_string(rep.stats.non_manifold_edges) +
                 "，边界环 " + std::to_string(rep.stats.boundary_loops) +
                 "，补洞 " + std::to_string(rep.holes.filled_count) +
                 "，朝外翻转 " + std::to_string(rep.outward_flip_fixed));
  return RECON_OK;
}

BakeView BakeViewStorage::View() const {
  BakeView v;
  v.rgb = rgb8.empty() ? nullptr : rgb8.data();
  v.depth = depth_m.empty() ? nullptr : depth_m.data();
  v.width = width;
  v.height = height;
  v.stride_bytes = width * 3;
  v.rotation = rotation;
  v.translation = translation;
  v.fx = fx;
  v.fy = fy;
  v.cx = cx;
  v.cy = cy;
  v.motion_blur = motion_blur;
  return v;
}

// ---------------------------------------------------------------------------
// 阶段 7：UV 展开 + 纹理烘焙（S8 + S9）
// ---------------------------------------------------------------------------
recon_status Orchestrator::StageUvTexture(const ReconOptions& opt,
                                          StageResult* out) {
  if (!opt.enable_texture) {
    out->degraded = true;
    out->message = "纹理已关闭";
    return RECON_OK;
  }

  UvConfig ucfg;
  ucfg.atlas_size =
      static_cast<int>(ParamOr(keys::kPostTextureAtlasSize, ucfg.atlas_size));
  // `post_uv_padding` 是相对图集尺寸的比例（docs/08），换算成像素。
  const double pad_ratio = ParamOr(keys::kPostUvPadding, 0.005);
  ucfg.padding = static_cast<int>(std::lround(pad_ratio * ucfg.atlas_size));
  ucfg.gutter = ucfg.padding * 2;

  FlushProgress(RECON_STAGE_UV_TEXTURE, 0.05, "UV 展开");
  atlas_ = UnwrapMesh(mesh_, ucfg);
  if (!atlas_.ok) {
    // 契约（docs/06 §5）：UV 失败导出无 UV 几何并告警，**不阻断**。
    out->degraded = true;
    out->message = "UV 展开失败，导出无 UV 几何";
    EmitLog(2, "UV 展开失败，跳过烘焙");
    return RECON_OK;
  }
  if (CheckCancel()) return RECON_ERR_CANCELLED;

  out->metrics.push_back({"charts", static_cast<double>(atlas_.chart_count)});
  out->metrics.push_back({"atlas_used", atlas_.used_area_ratio});
  out->metrics.push_back({"max_stretch", atlas_.max_stretch});

  // 组装烘焙视图：每个关键帧一个。
  //
  // `BakeView` 里是裸指针，必须保证其指向的内存在 `BakeTexture` 调用期间
  // 有效。做法：用 `BakeViewStorage` 持有紧凑 RGB8 与 float 深度，
  // 直到 `bake_` 产出后再释放。
  std::vector<BakeViewStorage> storage;
  storage.reserve(keyframe_indices_.size());
  for (int idx : keyframe_indices_) {
    const size_t fi = static_cast<size_t>(idx);
    if (fi >= frames_.size()) continue;
    const CapturedFrame& f = frames_[fi];
    if (f.rgb.empty() || f.rgb_width <= 0 || f.rgb_height <= 0) continue;
    if (!f.pose_valid) continue;

    BakeViewStorage st;
    st.width = f.rgb_width;
    st.height = f.rgb_height;
    // BGRA8 → 紧凑 RGB8。BakeTexture 的口径是 RGB8（texture_bake.h）。
    st.rgb8.resize(static_cast<size_t>(f.rgb_width) * f.rgb_height * 3);
    for (int y = 0; y < f.rgb_height; ++y) {
      const uint8_t* src = f.rgb.data() + static_cast<size_t>(y) * f.rgb_stride;
      uint8_t* dst = st.rgb8.data() + static_cast<size_t>(y) * f.rgb_width * 3;
      for (int x = 0; x < f.rgb_width; ++x) {
        dst[x * 3 + 0] = src[x * 4 + 2];  // R
        dst[x * 3 + 1] = src[x * 4 + 1];  // G
        dst[x * 3 + 2] = src[x * 4 + 0];  // B
      }
    }
    // 深度：uint16 毫米 → float 米。分辨率与 RGB 不一致时不传深度
    // （BakeView 的 depth 与 width/height 必须同尺度）。
    if (f.has_depth && f.depth_width == f.rgb_width &&
        f.depth_height == f.rgb_height && !f.depth.empty()) {
      st.depth_m.resize(static_cast<size_t>(f.depth_width) * f.depth_height);
      for (size_t i = 0; i < st.depth_m.size(); ++i) {
        st.depth_m[i] = f.depth[i] == 0 ? 0.0f
                                        : static_cast<float>(f.depth[i]) * 1e-3f;
      }
    }
    st.rotation = f.pose.R;
    st.translation = f.pose.t;
    st.fx = f.intrinsics.fx;
    st.fy = f.intrinsics.fy;
    st.cx = f.intrinsics.cx;
    st.cy = f.intrinsics.cy;
    st.motion_blur = f.motion_blur_score;
    storage.push_back(std::move(st));
  }

  if (storage.empty()) {
    out->degraded = true;
    out->message = "无可用 RGB 关键帧，跳过烘焙";
    EmitLog(2, "无可用 RGB 关键帧，跳过烘焙");
    return RECON_OK;
  }

  // 组装 views。`storage` 的容量已 reserve 到位且此后不再 push_back，
  // 因此内部 vector 的 data() 指针稳定。
  std::vector<BakeView> views;
  views.reserve(storage.size());
  for (const BakeViewStorage& st : storage) views.push_back(st.View());

  BakeConfig bcfg;
  bcfg.texture_size = ucfg.atlas_size;
  // `post_texture_occlusion_threshold` 是「深度容差的像素足迹倍数」的
  // 规格口径（docs/06 §2.9），映射到 visibility_tol_mult。
  bcfg.visibility_tol_mult =
      ParamOr(keys::kPostTextureOcclusionThreshold, bcfg.visibility_tol_mult);
  bcfg.tau_vis_mult = bcfg.visibility_tol_mult;
  // 入射角指数 alpha：偏好正视的程度（快 4 / 标 8 / 高 16，docs/06 §2.9）。
  // 从当前预设推导，避免硬编码。
  if (ctx_ != nullptr) {
    switch (ctx_->params().current_preset()) {
      case Preset::kFast: bcfg.alpha = 4.0; break;
      case Preset::kHighPrecision: bcfg.alpha = 16.0; break;
      case Preset::kStandard: default: bcfg.alpha = 8.0; break;
    }
  }

  FlushProgress(RECON_STAGE_UV_TEXTURE, 0.5, "纹理烘焙");
  bake_ = BakeTexture(mesh_, atlas_, views, bcfg);
  if (CheckCancel()) return RECON_ERR_CANCELLED;

  if (!bake_.ok) {
    out->degraded = true;
    out->message = "烘焙失败，导出无纹理";
    EmitLog(2, "纹理烘焙失败，导出无纹理");
    return RECON_OK;
  }

  out->metrics.push_back({"bake_covered", bake_.covered_ratio});
  out->metrics.push_back({"bake_views_per_texel", bake_.mean_views_per_texel});
  out->metrics.push_back({"bake_unfilled", static_cast<double>(bake_.unfilled_texels)});
  EmitLog(1, "烘焙：覆盖 " + std::to_string(bake_.covered_ratio * 100.0) +
                 "%，每 texel 视图 " +
                 std::to_string(bake_.mean_views_per_texel));
  return RECON_OK;
}

// ---------------------------------------------------------------------------
// 阶段 8：尺度
// ---------------------------------------------------------------------------
recon_status Orchestrator::StageScale(const ReconOptions& opt, StageResult* out) {
  (void)opt;
  if (ctx_ == nullptr) return RECON_ERR_INVALID_ARGUMENT;

  const recon_scale_result& sc = ctx_->scale();
  double factor = 1.0;
  if (sc.ok != 0 && sc.scale > 0.0) {
    factor = sc.scale;
    out->message = "已应用标定尺度";
  } else {
    out->degraded = true;
    out->message = "未标定，按重建单位";
  }

  // 回填网格信息（recon_get_mesh_info 的数据源）。
  mesh_info_ = recon_mesh_info{};
  MeshStats stats = AnalyzeMesh(mesh_);
  mesh_info_.vertex_count = stats.vertex_count;
  mesh_info_.triangle_count = stats.triangle_count;
  mesh_info_.texture_count = atlas_.ok ? 1u : 0u;
  mesh_info_.non_manifold_edges = stats.non_manifold_edges;
  mesh_info_.boundary_loops = stats.boundary_loops;
  mesh_info_.hole_count = stats.hole_count;
  mesh_info_.hole_area_ratio = stats.hole_area_ratio;

  Vec3d lo, hi;
  PointCloudBounds(mesh_.positions, 0.0, &lo, &hi);
  for (int i = 0; i < 3; ++i) {
    mesh_info_.bbox_min[i] = lo(i) * factor;
    mesh_info_.bbox_max[i] = hi(i) * factor;
  }
  mesh_info_.scale_factor = factor;
  mesh_info_.mean_edge_length_m = stats.mean_edge_length * factor;
  mesh_info_.poisson_depth = static_cast<uint32_t>(poisson_depth_used_);
  mesh_info_.degraded = 0;

  out->metrics.push_back({"scale", factor});
  out->metrics.push_back({"mean_edge_mm", stats.mean_edge_length * 1000.0});
  return RECON_OK;
}

// ---------------------------------------------------------------------------
// 阶段调度
// ---------------------------------------------------------------------------

recon_status Orchestrator::runStage(StageId stage, StageResult* out) {
  if (out != nullptr) *out = StageResult{};
  if (ctx_ == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  if (CheckCancel()) return RECON_ERR_CANCELLED;

  ReconOptions opt = opts_;
  bool have_opts = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    have_opts = opts_valid_;
  }
  if (!have_opts) {
    // 未经 runReconstruction 直接单跑阶段：退回按参数推断的默认开关。
    const double atlas = ParamOr(keys::kPostTextureAtlasSize, 4096.0);
    opt.enable_texture = atlas > 0.0;
  }

  const double t0 = NowMillis();
  recon_status st = RECON_OK;
  switch (stage) {
    case RECON_STAGE_PREPROCESS: st = StagePreprocess(opt, out); break;
    case RECON_STAGE_VIO:        st = StageVio(opt, out); break;
    case RECON_STAGE_MVS:        st = StageMvs(opt, out); break;
    case RECON_STAGE_RGBD_FUSION:st = StageTsdf(opt, out); break;
    case RECON_STAGE_MESH:       st = StagePoisson(opt, out); break;
    case RECON_STAGE_POSTPROC:   st = StagePostproc(opt, out); break;
    case RECON_STAGE_UV_TEXTURE: st = StageUvTexture(opt, out); break;
    case RECON_STAGE_SCALE:      st = StageScale(opt, out); break;
    default:
      return RECON_ERR_NOT_IMPLEMENTED;
  }

  if (out != nullptr) {
    out->stage = stage;
    out->status = st;
    out->ok = (st == RECON_OK);
    out->millis = NowMillis() - t0;
  }
  return st;
}

recon_status Orchestrator::runReconstruction(const ReconOptions& options,
                                             const OrchestratorCallbacks& callbacks) {
  if (ctx_ == nullptr) return RECON_ERR_INVALID_ARGUMENT;

  // 复位取消标志？**不**：契约规定 cancel 是幂等置位，重建不能再清除它，
  // 否则用户取消后重建会"复活"。
  if (ctx_->IsCancelled()) {
    SetState(OrchestratorState::kCancelling);
    return RECON_ERR_CANCELLED;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = frames_.size();
    if (n == 0) return RECON_ERR_MVS_NO_DEPTH;
    records_.clear();
    last_failed_stage_ = RECON_STAGE_IDLE;
    // 选项合并：调用方显式传入的 options 优先；未指定的项从 Context
    // （即 recon_config）继承。两条来源不同层的开关必须合到一处，
    // 否则「C ABI 关了纹理、编排层又打开」这类漂移会静默发生。
    opts_ = ctx_->options();
    opts_.enable_mvs = options.enable_mvs && opts_.enable_mvs;
    opts_.enable_tsdf = options.enable_tsdf && opts_.enable_tsdf;
    opts_.enable_texture = options.enable_texture && opts_.enable_texture;
    opts_.apply_scale = options.apply_scale && opts_.apply_scale;
    opts_.resume_from_checkpoint = options.resume_from_checkpoint;
    opts_valid_ = true;
  }
  if (options.resume_from_checkpoint && resume_stage_ == RECON_STAGE_IDLE) {
    const StageId cp = ScanCheckpoints();
    if (cp > RECON_STAGE_IDLE) {
      std::lock_guard<std::mutex> lock(mutex_);
      resume_stage_ = cp;
      EmitLog(1, "续算：从已完成的阶段 " + std::to_string(static_cast<int>(cp)) +
                     " 之后开始");
    }
  }

  cb_ = callbacks;
  last_progress_ms_ = 0.0;
  SetState(OrchestratorState::kReconstructing);
  EmitStatus(0, "开始重建");

  // 阶段序列表。每项：阶段号、总体进度下限、进度上限、状态文案。
  struct StageEntry {
    StageId stage;
    double lo;
    double hi;
    const char* label;
  };
  static const StageEntry kStages[] = {
      {RECON_STAGE_PREPROCESS, 0.00, 0.04, "预处理"},
      {RECON_STAGE_VIO, 0.04, 0.10, "位姿估计"},
      {RECON_STAGE_MVS, 0.10, 0.45, "稠密重建"},
      {RECON_STAGE_RGBD_FUSION, 0.45, 0.60, "深度融合"},
      {RECON_STAGE_MESH, 0.60, 0.82, "曲面重建"},
      {RECON_STAGE_POSTPROC, 0.82, 0.90, "拓扑修复"},
      {RECON_STAGE_UV_TEXTURE, 0.90, 0.96, "纹理"},
      {RECON_STAGE_SCALE, 0.96, 1.00, "尺度"},
  };

  bool skip = (resume_stage_ > RECON_STAGE_IDLE);
  for (const StageEntry& e : kStages) {
    if (skip) {
      if (e.stage <= resume_stage_) continue;
      skip = false;
    }
    if (CheckCancel()) {
      SetState(OrchestratorState::kCancelling);
      EmitStatus(RECON_ERR_CANCELLED, "已取消");
      return RECON_ERR_CANCELLED;
    }

    FlushProgress(e.stage, e.lo, e.label);
    StageResult res;
    const recon_status st = runStage(e.stage, &res);

    StageRecord rec;
    rec.stage = e.stage;
    rec.ok = (st == RECON_OK);
    rec.degraded = res.degraded;
    rec.millis = res.millis;
    rec.message = res.message;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      records_.push_back(rec);
    }

    if (st == RECON_ERR_CANCELLED) {
      SetState(OrchestratorState::kCancelling);
      EmitStatus(RECON_ERR_CANCELLED, "已取消");
      return RECON_ERR_CANCELLED;
    }
    // 阶段级空结果按「降级但可继续」处理（如无深度帧、MVS 关闭）。
    if (st != RECON_OK && !res.degraded) {
      std::lock_guard<std::mutex> lock(mutex_);
      last_failed_stage_ = e.stage;
      state_ = OrchestratorState::kFailed;
      EmitStatus(st, "重建失败");
      EmitLog(3, std::string("阶段失败：") + e.label +
                     "（status=" + std::to_string(static_cast<int>(st)) + "）");
      return st;
    }

    FlushProgress(e.stage, e.hi, e.label);
    // 阶段边界写检查点。
    Checkpoint cp;
    cp.stage = e.stage;
    writeCheckpoint(cp);
  }

  // 完成后把网格信息写回 context（recon_get_mesh_info 的数据源）。
  ctx_->mesh_info() = mesh_info_;

  SetState(OrchestratorState::kCompleted);
  FlushProgress(RECON_STAGE_DONE, 1.0, "重建完成");
  EmitStatus(0, "重建完成");
  EmitLog(1, "重建完成：顶点 " + std::to_string(mesh_info_.vertex_count) +
                 "，三角形 " + std::to_string(mesh_info_.triangle_count));
  return RECON_OK;
}

// ---------------------------------------------------------------------------
// 检查点
// ---------------------------------------------------------------------------

recon_status Orchestrator::writeCheckpoint(const Checkpoint& cp) {
  if (ctx_ == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  std::string dir;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    dir = cp.path.empty() ? spec_.session_dir : cp.path;
  }
  if (dir.empty()) return RECON_OK;  // 无落盘目录：静默跳过是契约允许的

  // 极简 JSON：只记阶段与时间，供 resume 判定；不含几何（几何走 spill）。
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "{\n  \"schema_version\": 1,\n  \"session_id\": \"%s\",\n"
                "  \"stage_completed\": %d,\n  \"stage_name\": \"%s\",\n"
                "  \"scale_factor\": %.9g\n}\n",
                spec_.session_id.c_str(), static_cast<int>(cp.stage),
                OrchestratorStateName(state()), mesh_info_.scale_factor);
  const std::string path =
      dir + "/checkpoints/stage_" + std::to_string(static_cast<int>(cp.stage)) +
      ".json";
  if (!WriteFileAtomic(path, std::string(buf))) {
    EmitLog(2, "检查点写入失败：" + path);
    return RECON_ERR_IO;
  }
  return RECON_OK;
}

// 扫描 <session_dir>/checkpoints/ 下的 stage_<N>.json，取最大的 N 作为已完成
// 阶段。刻意不用 <filesystem>：Android NDK 的 libc++ 在部分 API level 上
// 未提供 `std::filesystem`，而本层必须是可移植的纯 C++17。
StageId Orchestrator::ScanCheckpoints() const {
  std::string dir;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    dir = spec_.session_dir;
  }
  if (dir.empty()) return RECON_STAGE_IDLE;

  const std::string ckpt_dir = dir + "/checkpoints";
  int best = static_cast<int>(RECON_STAGE_IDLE);
  for (int s = static_cast<int>(RECON_STAGE_DONE); s > best; --s) {
    const std::string p =
        ckpt_dir + "/stage_" + std::to_string(s) + ".json";
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (f == nullptr) continue;
    std::fclose(f);
    best = s;
    break;
  }
  return static_cast<StageId>(best);
}

recon_status Orchestrator::restoreCheckpoint(const Checkpoint& cp) {
  if (ctx_ == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  StageId stage = cp.stage;
  if (stage <= RECON_STAGE_IDLE) {
    // 未指定阶段：按会话目录里的检查点自动定位。
    stage = ScanCheckpoints();
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    resume_stage_ = stage;
  }
  if (stage <= RECON_STAGE_IDLE) {
    EmitLog(1, "无可用检查点，从第一个阶段开始");
    return RECON_OK;
  }
  EmitLog(1, "从检查点续算：已完成阶段 " + std::to_string(static_cast<int>(stage)));
  return RECON_OK;
}

}  // namespace recon
