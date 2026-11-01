// SPDX-License-Identifier: BSD-3-Clause
//
// capture_state.h — 采集状态机（UI 层）的平台无关实现。
//
// 契约：docs/09_UI规格.md §4.1（状态图）、§4.2（迁移表 T-01..T-30）、
//       §4.3（异常态处理路径）。
//
// 为什么放在 platform/common（而不是 core/）：
//   · 这是 **UI 层** 状态机，不属于重建核心。core/ 只关心「喂进来的帧」，
//     不关心用户点了哪个按钮。把它下沉到 core 会污染内核的职责边界。
//   · 但它 **平台无关**：iOS（SwiftUI）与 Android（Compose）两端共用同一套
//     迁移规则，避免「同一逻辑写两遍必然漂移」（铁律）。
//   · 因此放 platform/common，用纯 C++17 实现，两端各自薄封装（JNI / JNI-free
//     Objective-C++），桌面也能编译并单测 —— 保证规则本身可验证。

#ifndef RECON_PLATFORM_CAPTURE_STATE_H_
#define RECON_PLATFORM_CAPTURE_STATE_H_

#include <cstdint>
#include <functional>
#include <string>

namespace recon {
namespace platform {

/// UI 状态（docs/09 §4.1）。与 03 文档核心状态（L5 编排层）的对应见 §4.1 表。
enum class CaptureState : uint8_t {
  kIdle = 0,                 ///< 首页 / 无进行中的会话
  kCapabilityProbe,          ///< 探测深度/分辨率/IMU/内存
  kRequestingPermission,     ///< 相机 / 运动 / 相册（按需）
  kPreviewing,               ///< 预览不采帧；模式选择、放置物体
  kCapturing,                ///< 采帧 / 门限 / 关键帧写入
  kReviewing,                ///< 自检清单
  kProcessing,               ///< 重建 + 后处理
  kCompleted,                ///< 终态：会话已完成并落盘
  kFailed,                   ///< 终态：任一阶段不可恢复失败
  kCancelled,                ///< 终态：用户取消并经 8 步清理
};

/// 触发迁移的事件（docs/09 §4.2 的「事件」列）。命名对应表内行号 T-01..T-30，
/// 便于测试与文档一一对照。
enum class CaptureEvent : uint8_t {
  kStartScan = 0,            ///< T-01 用户点「开始扫描」
  kProbeComplete,            ///< T-02 探测完成，判定分支
  kProbeCameraUnavailable,   ///< T-03 探测完成，相机不可用
  kProbeInternalError,       ///< T-04 探测失败（内部异常）
  kPermissionsGranted,       ///< T-05 相机+运动权限已授予
  kCameraPermissionDenied,   ///< T-06 相机权限被拒
  kMotionPermissionDenied,   ///< T-07 运动权限被拒（继续，位姿退化）
  kPhotoPermissionDenied,    ///< T-08 相册权限被拒（非必需，跳过）
  kUserStart,                ///< T-09 用户点「开始」
  kUserCancelFromPreview,    ///< T-10 用户点「取消」
  kFinishScan,               ///< T-11 用户点「完成扫描」
  kCancelGesture,            ///< T-12 用户点 ✕ / 返回手势
  kFrameCapOrDiskLow,        ///< T-13 帧数达上限 / 磁盘不足严重
  kAppBackgrounded,          ///< T-14 用户切后台
  kCameraBusy,               ///< T-15 相机被其他 App 占用
  kSelfCheckSevereFail,      ///< T-16 自检有 severe 未通过
  kUserConfirmReconstruct,   ///< T-17 自检有 warn 未通过，用户确认重建
  kUserContinueScan,         ///< T-18 用户点「继续扫描」
  kReviewCancel,             ///< T-19 用户点 ✕
  kReconstructionSucceeded,  ///< T-20 重建成功
  kReconstructionFailed,     ///< T-21 重建失败（不可恢复错误码）
  kUserCancelReconstruction, ///< T-22 用户取消重建
  kProcessKilled,            ///< T-23 App 被系统杀死（状态不落盘为终态）
  kRetry,                    ///< T-24 用户点「重试」（按失败阶段）
  kBackToHome,               ///< T-25 用户点「回到首页」
  kExportModel,              ///< T-26 用户点「导出模型」（状态不变）
  kRecapture,                ///< T-27 用户点「重新采集」
  kOpenSettings,             ///< T-28 用户点「设置」（状态不变）
  kModifyCaptureParams,      ///< T-29 用户修改 cap_* 参数（状态不变）
  kModifyClassCAndRebuild,   ///< T-30 用户修改 C 类参数并重建
};

/// 失败原因（用于 T-24「按失败阶段」重试的判定）。
enum class FailureStage : uint8_t {
  kNone = 0,
  kCamera,       ///< 相机失败 → 重试回 RequestingPermission
  kReconstruction, ///< 重建失败 → 重试回 Processing
  kCalibration,  ///< 标定失败 → 重试回 Previewing
};

/// 一次迁移的结果。
struct TransitionResult {
  bool accepted;         ///< 该 (state, event) 组合是否为合法迁移
  CaptureState next;     ///< accepted 时的下一状态；否则为原状态
  const char* rule;      ///< 对应 docs/09 §4.2 的规则编号（如 "T-05"）或 "—"
};

/// 采集状态机的纯逻辑实现。**无线程、无 IO、无平台依赖。**
///
/// 副作用（启动相机、写 manifest、调 recon_*）由调用方在收到迁移结果后执行 ——
/// 本类只回答「这个事件在此状态下合法吗？合法的话去哪？」。
class CaptureStateMachine {
 public:
  CaptureStateMachine() = default;

  /// 当前状态。
  CaptureState state() const { return state_; }

  /// 最近一次失败阶段（供 T-24 重试路由使用）。
  FailureStage failure_stage() const { return failure_stage_; }

  /// 采集是否处于「暂停」变体（T-14 切后台：状态仍是 Capturing，但暂停相机）。
  bool paused() const { return paused_; }

  /// 主动记录失败阶段。供调用方在因错误码进入 Failed 前调用（T-21/T-15/T-03）。
  void set_failure_stage(FailureStage s) { failure_stage_ = s; }

  /// 清除暂停标志。与 T-14（切后台置 paused）配对：回前台时由 App 层调用
  /// —— docs/09 §4.2 未为该动作定义独立事件（状态本就不变）。
  void clear_paused() { paused_ = false; }

  /**
   * 应用一个事件。
   *
   * @param ev 触发事件。
   * @return 迁移结果。`accepted=false` 表示该组合在 docs/09 §4.2 中不存在，
   *         状态保持不变（调用方应忽略或记录非法迁移）。
   *
   * 规则完全来自 docs/09 §4.2；本函数不含任何额外判断逻辑。
   */
  TransitionResult Apply(CaptureEvent ev);

  /// 状态名（用于日志 / 声明式 UI）。与 docs/09 §4.2 的标识一致。
  static const char* StateName(CaptureState s);

 private:
  CaptureState state_ = CaptureState::kIdle;
  FailureStage failure_stage_ = FailureStage::kNone;
  bool paused_ = false;
};

}  // namespace platform
}  // namespace recon

#endif  // RECON_PLATFORM_CAPTURE_STATE_H_
