// SPDX-License-Identifier: BSD-3-Clause
//
// L5 编排层。契约：docs/01_接口契约.md §3.12（接口）、§6（生命周期与线程模型）。
//
// 职责：把 L4 的各个「单测孤岛」接成一条真正的端到端流水线。
//
//   PUSH → PREPROCESS → [VIO] → [MVS] → [TSDF] → POISSON → POSTPROC
//        → UV → BAKE → SCALE →（导出由 recon_export 触发）
//
// 两条分支平级（docs/00 §4.5）：有深度走 RGBD（TSDF 主线 + MVS 精化），
// 无深度走 MONO（VIO + MVS）。分支由 `Context::capability()` 决定，
// **不是降级**。
//
// 设计约束：
//   · 本层不引入任何平台 API（相机 / IMU / 文件权限都在 platform 层）。
//   · 本层不引入网络。
//   · 取消只置原子标志，各阶段在自己的检查点读取（docs/01 §6.4）。
//   · 阶段的**精度阈值**全部来自 ParamStore，本层不硬编码、不降级
//     （R-PREC-4：内存压力只允许降并行度 / 批大小 / 分块）。
//
// 线程：`startSession` / `finishCapture` / `runReconstruction` / `runStage`
// 必须在**后台线程**调用（docs/01 §6.3 的「UI 线程禁令」）；
// `cancel` / `state` 任意线程可调。

#ifndef RECON_CORE_ORCHESTRATOR_H_
#define RECON_CORE_ORCHESTRATOR_H_

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "recon/camera.h"
#include "recon/imu_preintegration.h"
#include "recon/linalg.h"
#include "recon/mesh_repair.h"
#include "recon/mvs.h"
#include "recon/poisson_mesh.h"
#include "recon/recon_c_api.h"
#include "recon/texture_bake.h"
#include "recon/uv_unwrap.h"

namespace recon {

// 前向声明：完整定义见 context.h。orchestrator.h 刻意**不**包含 context.h，
// 以避免 context.h ←（持有 Orchestrator）→ orchestrator.h 的循环依赖。
class Context;

// ---------------------------------------------------------------------------
// 核心状态机（docs/01 §6.1：UI 状态 / 核心状态 / C ABI 入口三者对应）
// ---------------------------------------------------------------------------
enum class OrchestratorState : uint8_t {
  kIdle = 0,            // recon_create 之后
  kCalibrating = 1,     // 能力探测 / 权限
  kPlacing = 2,         // start_session 之后，等待开始采集
  kScanning = 3,        // push_frame 期间
  kReviewing = 4,       // finish_capture 之后
  kReconstructing = 5,  // run_reconstruction 期间
  kCancelling = 6,      // cancel 已置位，等待阶段收敛
  kCompleted = 7,       // 重建成功
  kFailed = 8,          // 失败
};

const char* OrchestratorStateName(OrchestratorState s);

// 阶段标识。对外复用 recon_stage，保持 C ABI 单一口径。
using StageId = int32_t;

// 单个阶段的执行结果。
struct StageResult {
  bool ok = false;
  recon_status status = RECON_OK;
  StageId stage = RECON_STAGE_IDLE;
  // 是否走了降级路径（只允许并行度 / 分块类降级，R-PREC-4）。
  bool degraded = false;
  // 人类可读的短说明。
  std::string message;
  // 本阶段耗时（毫秒）。
  double millis = 0.0;
  // 该阶段产出的诊断键值对（进日志，便于排障）。
  std::vector<std::pair<std::string, double>> metrics;
};

// 阶段级记录（runReconstruction 结束后可整体回读）。
struct StageRecord {
  StageId stage = RECON_STAGE_IDLE;
  bool ok = false;
  bool degraded = false;
  double millis = 0.0;
  std::string message;
};

// ---------------------------------------------------------------------------
// 会话与帧
// ---------------------------------------------------------------------------

// 会话规格。
struct SessionSpec {
  std::string session_id;
  std::string session_dir;  // 为空则不落盘
  bool resume = false;      // true 时允许从最近检查点续算
};

// 采集期内部帧。
//
// **深拷贝**了 C ABI 指针指向的内容 —— 契约要求 `recon_push_frame` 返回后
// 核心层不再访问调用方的内存（docs/01 §2.2.5「内部拷入自有缓冲」）。
struct CapturedFrame {
  uint64_t frame_id = 0;
  uint64_t timestamp_ns = 0;

  // BGRA8（与 recon_frame_packet.rgb_data 同口径）。
  std::vector<uint8_t> rgb;
  int rgb_width = 0;
  int rgb_height = 0;
  int rgb_stride = 0;

  // uint16 毫米，沿光轴 z。
  std::vector<uint16_t> depth;
  std::vector<uint8_t> depth_confidence;
  int depth_width = 0;
  int depth_height = 0;
  bool has_depth = false;
  recon_depth_source depth_source = RECON_DEPTH_SRC_NONE;

  CameraIntrinsics intrinsics;
  bool intrinsics_valid = false;

  // 平台位姿先验（world → camera，docs/01 §2.2.3）。valid=false 时无先验。
  CameraPose pose;
  bool pose_valid = false;

  // 本帧之前的 IMU 时间窗。
  std::vector<ImuSample> imu;

  double motion_blur_score = 0.0;
  double exposure_us = 0.0;

  // PREPROCESS 阶段填充（灰度图，供 VIO / MVS 使用）。
  std::vector<uint8_t> gray;
  int gray_width = 0;
  int gray_height = 0;
  int gray_stride = 0;
};

// ---------------------------------------------------------------------------
// 重建选项
// ---------------------------------------------------------------------------
struct ReconOptions {
  // 是否走 MVS 精化。RGBD 分支默认开启（MVS 是曲面精度的真正来源，
  // docs/00 §8.8）；MONO 分支它是唯一深度来源，必须开。
  bool enable_mvs = true;
  // 是否做 TSDF 融合（仅 RGBD 分支有意义）。
  bool enable_tsdf = true;
  // 是否做 UV 展开 + 纹理烘焙。
  bool enable_texture = true;
  // 是否应用尺度（写入 mesh_info.scale_factor 与导出）。
  bool apply_scale = true;
  // 从最近检查点续算。
  bool resume_from_checkpoint = false;
};

// ---------------------------------------------------------------------------
// 回调集合
// ---------------------------------------------------------------------------
struct OrchestratorCallbacks {
  recon_progress_cb progress = nullptr;
  recon_status_cb status = nullptr;
  recon_log_cb log = nullptr;
  void* user_data = nullptr;
};

// ---------------------------------------------------------------------------
// 烘焙视图暂存
//
// `BakeView` 内的 `rgb` / `depth` 是裸指针，指向的内存必须比 `BakeView`
// 活得久。用这个结构把「紧凑 RGB8 + float 深度」的拥有权集中起来，
// 避免在循环里把指针指向临时容器后失效（项目历史上踩过这个野指针的坑，
// 见 test_bake.cpp 的注释）。
// ---------------------------------------------------------------------------
struct BakeViewStorage {
  std::vector<uint8_t> rgb8;    // 紧凑 RGB8，长度 = width × height × 3
  std::vector<float> depth_m;   // 沿光轴深度（米），无深度时为空
  int width = 0;
  int height = 0;
  Mat3d rotation = Mat3d::Identity();  // world → camera
  Vec3d translation = Vec3d::Zero();
  double fx = 0.0, fy = 0.0, cx = 0.0, cy = 0.0;
  double motion_blur = 0.0;

  // 构造指向自身缓冲的 BakeView。调用后本对象**不得移动/复制**。
  BakeView View() const;
};

// ---------------------------------------------------------------------------
// 检查点
// ---------------------------------------------------------------------------
struct Checkpoint {
  StageId stage = RECON_STAGE_IDLE;  // 已完成到该阶段
  std::string path;                  // 为空时用默认路径
};

// ---------------------------------------------------------------------------
// 统一点云视图
//
// MVS（`PointCloud`）与 TSDF（`ExtractPoints` 的三个平行数组）产出结构不同，
// 但下游（泊松）只需要「位置 + 法向 + 权重」。本结构是二者的共同出口，
// 避免在泊松入口写两套派生代码 —— 同一逻辑写两遍必然漂移（项目铁律）。
// ---------------------------------------------------------------------------
struct UnifiedPointCloud {
  std::vector<Vec3d> positions;
  std::vector<Vec3d> normals;
  std::vector<float> colors;       // RGB 0-255，长度 = 3 × 顶点数
  std::vector<float> confidences;  // ≥ 0

  size_t size() const { return positions.size(); }
  void Clear() {
    positions.clear();
    normals.clear();
    colors.clear();
    confidences.clear();
  }

  // 转成泊松输入。权重直接取置信度（泊松的 point_weight）。
  OrientedPointCloud ToOrientedCloud() const;
};

// ---------------------------------------------------------------------------
// 编排器
// ---------------------------------------------------------------------------
class Orchestrator {
 public:
  // ctx 必须比本对象活得久。Orchestrator 通过 ctx 读写参数、能力、取消标志、
  // 尺度、网格信息与采集统计 —— 单一数据源，不复制状态。
  explicit Orchestrator(Context* ctx);
  ~Orchestrator();

  Orchestrator(const Orchestrator&) = delete;
  Orchestrator& operator=(const Orchestrator&) = delete;

  // --- 生命周期（对应 docs/01 §3.12） ---

  recon_status startSession(const SessionSpec& spec);
  recon_status onFramePushed(const recon_frame_packet& packet);
  recon_status finishCapture();
  recon_status runReconstruction(const ReconOptions& options,
                                 const OrchestratorCallbacks& callbacks);
  recon_status runStage(StageId stage, StageResult* out);
  void cancel();
  OrchestratorState state() const;

  recon_status writeCheckpoint(const Checkpoint& cp);
  recon_status restoreCheckpoint(const Checkpoint& cp);

  // --- 结果访问 ---

  const TriangleMesh& mesh() const { return mesh_; }
  const UvAtlas& atlas() const { return atlas_; }
  const BakeResult& bake() const { return bake_; }
  const UnifiedPointCloud& cloud() const { return cloud_; }
  const std::vector<StageRecord>& stage_records() const { return records_; }

  // 质量门限通过（被接受）的帧数。
  size_t accepted_frame_count() const { return accepted_frames_; }

  // ── 采集期像素内存预算（缺口 AT）─────────────────────────────────────────
  //
  // `@param bytes` 预算字节数；0 = 不设限（测试用）。设置后立即按新预算裁决。
  // 平台层在 `open()` 时按 docs/13 §1.2 的预算公式调用
  // （iOS `os_proc_available_memory()` × 系数）—— 当前由 ABI 消费方
  // 直接调用，尚未经 C ABI 暴露（C ABI 出口见 docs/14 缺口 AT 的后续项）。
  void SetFramePixelBudget(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_pixel_budget_bytes_ = bytes;
    EnforcePixelBudgetLocked();
  }
  size_t frame_pixel_budget_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frame_pixel_budget_bytes_;
  }
  // 当前常驻像素字节数与已释放像素的帧数（诊断 / 测试断言用）。
  size_t resident_pixel_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return resident_pixel_bytes_;
  }
  size_t released_pixel_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return released_pixel_frames_;
  }
  // frames_ 的元素个数（**含像素已被释放的帧** —— 元数据仍全部在册）。
  size_t captured_frame_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.size();
  }

  // 关键帧时间戳（单调时钟纳秒），与 keyframe_indices_ 逐元素对齐。
  //
  // VIO 输出契约要求每关键帧带时间戳（docs/05 §3.10），因为下游按**时间**
  // 而非按序号对齐：
  //   · SfM 的 IMU 因子按关键帧区间切分预积分（docs/05 §4.6）；
  //   · MVS 源视图选择按基线/时间筛除近重复帧；
  //   · 导出的位姿轨迹必须能与原始采集时间轴对表。
  // 时间戳只存在于 KeyframeState 与 CapturedFrame 中、不随 CameraPose 走
  // （CameraPose 是纯几何量），故在此显式保留一份平行数组，避免下游
  // 各自「回查 frames_」而重复实现（本项目铁律：同一逻辑写两遍必然漂移）。
  const std::vector<uint64_t>& keyframe_timestamps_ns() const {
    return key_timestamps_ns_;
  }
  // 本阶段实际跑的分支。
  recon_pipeline_branch branch() const { return branch_; }
  // 最近一次失败所在的阶段（成功时无意义）。
  StageId last_failed_stage() const { return last_failed_stage_; }

  // SfM（MONO 无 IMU 分支）诊断。
  int sfm_registered_frames() const { return sfm_registered_frames_; }
  int sfm_sparse_points() const { return sfm_sparse_points_; }
  double sfm_ba_rms_px() const { return sfm_ba_rms_px_; }

  // 清空所有中间产物（重新采集时调用）。
  void Reset();

 private:
  // 阶段实现。全部返回 recon_status；返回非 RECON_OK 时 runReconstruction
  // 会把状态机切到 kFailed 并记录阶段号。
  recon_status StagePreprocess(const ReconOptions& opt, StageResult* out);
  recon_status StageVio(const ReconOptions& opt, StageResult* out);
  // MONO 无 IMU 分支的位姿来源：SfM（docs/05 §2.4 C1）。
  recon_status StageSfmMonol(const ReconOptions& opt, StageResult* out);
  recon_status StageMvs(const ReconOptions& opt, StageResult* out);
  recon_status StageTsdf(const ReconOptions& opt, StageResult* out);
  recon_status StagePoisson(const ReconOptions& opt, StageResult* out);
  recon_status StagePostproc(const ReconOptions& opt, StageResult* out);
  recon_status StageUvTexture(const ReconOptions& opt, StageResult* out);
  recon_status StageScale(const ReconOptions& opt, StageResult* out);

  // 取消检查：已置位则返回 true，并做状态机迁移。
  bool CheckCancel();

  // 进度 / 状态 / 日志上报。进度按 10 Hz 合并（docs/01 §6.5 CB-3）。
  void EmitProgress(StageId stage, double progress, const char* message);
  void EmitStatus(int32_t code, const char* text);
  void EmitLog(int32_t level, const std::string& message);

  // 参数读取（缺键时回退到给定默认，并记一条 warn）。
  double ParamOr(const char* key, double fallback) const;

  void SetState(OrchestratorState s);
  // 无条件推送一次进度（阶段边界用）。
  void FlushProgress(StageId stage, double progress, const char* message);

  // 把关键帧位姿整理成 MVS 需要的三个平行数组。
  // @param out_poses      全局位姿数组（与 keyframe_indices_ 对齐）
  // @param out_intrinsics 全局内参数组
  void BuildKeyframeArrays(std::vector<CameraPose>* out_poses,
                           std::vector<CameraIntrinsics>* out_intrinsics) const;

  // 扫描会话目录下 checkpoints/ 里已完成的最高阶段号。
  // 只读，不引入 <filesystem>（移动端工具链覆盖不全）。无检查点返回
  // RECON_STAGE_IDLE。
  StageId ScanCheckpoints() const;

  Context* ctx_ = nullptr;

  mutable std::mutex mutex_;
  OrchestratorState state_ = OrchestratorState::kIdle;

  SessionSpec spec_;
  std::vector<CapturedFrame> frames_;
  size_t accepted_frames_ = 0;

  // ── 采集期像素内存预算（OOM 保护；docs/13 §0.3 / §1.3、docs/02 §5.6）────
  //
  // 背景（2026-11-02 真机实测，一次真实崩溃，缺口 AT）：
  //   `frames_` 把每帧的 RGB / 深度**全量深拷贝**且**无任何上限**。
  //   iOS 的 ARKit 默认 videoFormat 是 1920×1440（4:3），BGRA8 单帧
  //   11.06 MiB；加深度约 12.5 MiB。100 余帧即 ~1.3 GiB 常驻 ——
  //   在 iPhone 13（约 4 GB RAM、单 App 上限 ~2 GB）上撞 jetsam 内存
  //   上限被系统直接杀进程，表现为用户反馈的「扫到 100 多帧必闪退」。
  //
  // 正解是 docs/02 §5.6 的**采集期落盘**（`cap_disk_cache_enabled=1`，
  //   `cap_ring_buffer_frames=600`）：图像写入 `frames.bin` 后释放引用，
  //   按 docs/03 §5.3.2 的 L1 层要求「常驻内存 0」。该落盘实现**尚未完成**
  //   （全仓 `grep frames.bin core/` 零命中），故此处先做**止血**。
  //
  // 止血语义（**必须与「降精度」严格区分**）：
  //   · 像素超预算时，按 FIFO 释放**最旧**帧的像素缓冲（rgb / depth /
  //     depth_confidence / gray）；
  //   · **保留全部元数据**：frame_id / 位姿 / 内参 / 时间戳 / 各尺寸字段。
  //     故 `key_poses_` / `keyframe_indices_` / VIO / TSDF 的**索引与位姿
  //     语义完全不变**，几何重建链照常工作（这正是 docs/13 R-PREC-4
  //     「只降并行度/批大小/分块策略」允许的范围）；
  //   · 代价：被释放帧不再参与 MVS 稠密匹配、不提供纹理采样。
  //     这是**真实损失**，故 `released_pixel_frames_` 必须被日志上报
  //     （不静默降级，U-D-7 / docs/13 §0.2）。
  //
  // 预算取值：默认 1024 MiB。依据 —— iOS 单 App 上限约 2 GB，采集期
  //   还需为 TSDF（13 §1.3：1.5 GB 上限，但按需分块）、MVS、纹理留余量；
  //   1024 MiB 约可容纳 110 帧 1080p 像素（7.91+1.5 MiB/帧）。
  //   **待平台上报真实可用内存（`os_proc_available_memory()`）后，应改为
  //   按 docs/13 §1.2 的预算公式动态取值**，此处先用保守常量。
  size_t frame_pixel_budget_bytes_ = static_cast<size_t>(1024) * 1024 * 1024;
  size_t resident_pixel_bytes_ = 0;
  size_t released_pixel_frames_ = 0;
  bool pixel_budget_note_emitted_ = false;

  // 按预算释放最旧帧的像素。**调用方必须已持有 mutex_**。
  void EnforcePixelBudgetLocked();
  // 单帧像素字节数（rgb + depth + depth_confidence + gray）。
  static size_t FramePixelBytes(const CapturedFrame& f);

  // 最近一次 runReconstruction 的选项。runStage 单阶段调用复用它，保证
  // 「整跑」与「单跑某一阶段」在开关上完全一致（否则两条路径会漂移）。
  ReconOptions opts_;
  bool opts_valid_ = false;

  // 关键帧位姿（与 keyframe_indices_ 对齐）。
  std::vector<CameraPose> key_poses_;
  std::vector<CameraIntrinsics> key_intrinsics_;
  std::vector<int> keyframe_indices_;
  // 关键帧时间戳（纳秒），与上面三个数组同长同序。
  std::vector<uint64_t> key_timestamps_ns_;

  // 各阶段产物。
  UnifiedPointCloud cloud_;
  TriangleMesh mesh_;
  UvAtlas atlas_;
  BakeResult bake_;
  recon_mesh_info mesh_info_{};

  // 各阶段的诊断数字，供测试与日志。
  size_t mvs_depth_maps_ = 0;
  size_t tsdf_integrated_pixels_ = 0;
  double poisson_resolution_ = 0.0;
  int poisson_depth_used_ = 0;

  // SfM（MONO 无 IMU 分支）诊断。docs/05 §4。
  int sfm_registered_frames_ = 0;  // 成功注册的关键帧数
  int sfm_sparse_points_ = 0;      // 稀疏点云点数
  double sfm_ba_rms_px_ = 0.0;     // 最终全局 BA 重投影 RMS（像素）

  recon_pipeline_branch branch_ = RECON_BRANCH_MONO;

  std::vector<StageRecord> records_;
  StageId last_failed_stage_ = RECON_STAGE_IDLE;

  // 进度节流。
  double last_progress_ms_ = 0.0;
  OrchestratorCallbacks cb_{};

  // 检查点续算的起算阶段。
  StageId resume_stage_ = RECON_STAGE_IDLE;
};

}  // namespace recon

#endif  // RECON_CORE_ORCHESTRATOR_H_
