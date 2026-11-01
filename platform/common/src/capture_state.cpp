// SPDX-License-Identifier: BSD-3-Clause
//
// capture_state.cpp — 采集状态机（UI 层）实现。
//
// 逐条转写 docs/09_UI规格.md §4.2 的迁移表 T-01..T-30。
// 每个 case 的注释都标出对应的规则编号，便于文档-代码双向对照。

#include "recon_platform/capture_state.h"

namespace recon {
namespace platform {

const char* CaptureStateMachine::StateName(CaptureState s) {
  switch (s) {
    case CaptureState::kIdle:                 return "Idle";
    case CaptureState::kCapabilityProbe:      return "CapabilityProbe";
    case CaptureState::kRequestingPermission: return "RequestingPermission";
    case CaptureState::kPreviewing:           return "Previewing";
    case CaptureState::kCapturing:            return "Capturing";
    case CaptureState::kReviewing:            return "Reviewing";
    case CaptureState::kProcessing:           return "Processing";
    case CaptureState::kCompleted:            return "Completed";
    case CaptureState::kFailed:               return "Failed";
    case CaptureState::kCancelled:            return "Cancelled";
  }
  return "Unknown";
}

// 便捷宏：提交迁移并返回结果。副作用由调用方据此执行。
// 注意：必须**先写回 state_** —— 否则状态机永远停在初始态（本实现曾有过的 bug）。
#define RECON_ACCEPT_RULE(rule_id) \
  do {                             \
    state_ = next;                 \
    return TransitionResult{true, next, rule_id}; \
  } while (0)

TransitionResult CaptureStateMachine::Apply(CaptureEvent ev) {
  // 一次迁移的「下一状态」在多数分支里唯一，先算好；未用到时无副作用。
  CaptureState next = state_;

  switch (state_) {
    // -------------------------------------------------------------- //
    // Idle                                                            //
    // -------------------------------------------------------------- //
    case CaptureState::kIdle:
      if (ev == CaptureEvent::kStartScan) {  // T-01
        next = CaptureState::kCapabilityProbe;
        RECON_ACCEPT_RULE("T-01");
      }
      // T-23 进程被杀后重启：下次启动按 02 §5.9 扫描 manifest.json。
      // 由 App 层在构造时把初始状态设为 Idle 并调用本事件即可（状态不变）。
      if (ev == CaptureEvent::kProcessKilled) {
        RECON_ACCEPT_RULE("T-23");
      }
      break;

    // -------------------------------------------------------------- //
    // CapabilityProbe                                                 //
    // -------------------------------------------------------------- //
    case CaptureState::kCapabilityProbe:
      if (ev == CaptureEvent::kProbeComplete) {  // T-02
        next = CaptureState::kRequestingPermission;
        RECON_ACCEPT_RULE("T-02");
      }
      if (ev == CaptureEvent::kProbeCameraUnavailable) {  // T-03
        failure_stage_ = FailureStage::kCamera;
        next = CaptureState::kFailed;
        RECON_ACCEPT_RULE("T-03");
      }
      if (ev == CaptureEvent::kProbeInternalError) {  // T-04
        // 按 00 §5.2 降级到 BRANCH_MONO 并重试一次。状态机停留在探测态，
        // 由调用方决定是否重试；此处保持状态不变（accepted 表示已处理）。
        RECON_ACCEPT_RULE("T-04");
      }
      break;

    // -------------------------------------------------------------- //
    // RequestingPermission                                            //
    // -------------------------------------------------------------- //
    case CaptureState::kRequestingPermission:
      if (ev == CaptureEvent::kPermissionsGranted) {  // T-05
        next = CaptureState::kPreviewing;
        RECON_ACCEPT_RULE("T-05");
      }
      if (ev == CaptureEvent::kCameraPermissionDenied) {  // T-06
        failure_stage_ = FailureStage::kCamera;
        next = CaptureState::kFailed;
        RECON_ACCEPT_RULE("T-06");
      }
      if (ev == CaptureEvent::kMotionPermissionDenied) {  // T-07
        // 位姿退化，仍进入 Previewing。降级标志由调用方记录（cap_imu_enabled=0）。
        next = CaptureState::kPreviewing;
        RECON_ACCEPT_RULE("T-07");
      }
      if (ev == CaptureEvent::kPhotoPermissionDenied) {  // T-08
        // 相册权限非必需，保持状态不变（继续申请其余权限）。
        RECON_ACCEPT_RULE("T-08");
      }
      break;

    // -------------------------------------------------------------- //
    // Previewing                                                      //
    // -------------------------------------------------------------- //
    case CaptureState::kPreviewing:
      if (ev == CaptureEvent::kUserStart) {  // T-09
        next = CaptureState::kCapturing;
        RECON_ACCEPT_RULE("T-09");
      }
      if (ev == CaptureEvent::kUserCancelFromPreview) {  // T-10
        next = CaptureState::kCancelled;
        RECON_ACCEPT_RULE("T-10");
      }
      if (ev == CaptureEvent::kModifyCaptureParams) {  // T-29
        RECON_ACCEPT_RULE("T-29");
      }
      if (ev == CaptureEvent::kOpenSettings) {  // T-28
        RECON_ACCEPT_RULE("T-28");
      }
      break;

    // -------------------------------------------------------------- //
    // Capturing                                                       //
    // -------------------------------------------------------------- //
    case CaptureState::kCapturing:
      if (ev == CaptureEvent::kFinishScan) {  // T-11
        paused_ = false;
        next = CaptureState::kReviewing;
        RECON_ACCEPT_RULE("T-11");
      }
      if (ev == CaptureEvent::kCancelGesture) {  // T-12
        // 触发取消确认弹窗；确认后 → Cancelled（下一状态 Reviewing 承载弹窗）。
        next = CaptureState::kReviewing;
        RECON_ACCEPT_RULE("T-12");
      }
      if (ev == CaptureEvent::kFrameCapOrDiskLow) {  // T-13
        next = CaptureState::kReviewing;
        RECON_ACCEPT_RULE("T-13");
      }
      if (ev == CaptureEvent::kAppBackgrounded) {  // T-14
        // 状态仍是 Capturing，仅暂停相机与 AR。
        paused_ = true;
        RECON_ACCEPT_RULE("T-14");
      }
      if (ev == CaptureEvent::kCameraBusy) {  // T-15
        failure_stage_ = FailureStage::kCamera;
        next = CaptureState::kFailed;
        RECON_ACCEPT_RULE("T-15");
      }
      if (ev == CaptureEvent::kModifyCaptureParams) {  // T-29
        RECON_ACCEPT_RULE("T-29");
      }
      if (ev == CaptureEvent::kOpenSettings) {  // T-28
        RECON_ACCEPT_RULE("T-28");
      }
      // 回前台（与 T-14 对应）：状态不变，恢复相机。用 kUserContinueScan 之外
      // 无专属事件，故复用 kPermissionsGranted 的语义不成立 —— 这里不定义额外
      // 事件，由 App 层在回前台时直接清 paused_（见 clear_paused()）。
      break;

    // -------------------------------------------------------------- //
    // Reviewing                                                       //
    // -------------------------------------------------------------- //
    case CaptureState::kReviewing:
      if (ev == CaptureEvent::kSelfCheckSevereFail) {  // T-16
        // 确认按钮禁用；状态保持 Reviewing。
        RECON_ACCEPT_RULE("T-16");
      }
      if (ev == CaptureEvent::kUserConfirmReconstruct) {  // T-17
        next = CaptureState::kProcessing;
        RECON_ACCEPT_RULE("T-17");
      }
      if (ev == CaptureEvent::kUserContinueScan) {  // T-18
        paused_ = false;
        next = CaptureState::kCapturing;
        RECON_ACCEPT_RULE("T-18");
      }
      if (ev == CaptureEvent::kReviewCancel) {  // T-19
        next = CaptureState::kCancelled;
        RECON_ACCEPT_RULE("T-19");
      }
      break;

    // -------------------------------------------------------------- //
    // Processing                                                      //
    // -------------------------------------------------------------- //
    case CaptureState::kProcessing:
      if (ev == CaptureEvent::kReconstructionSucceeded) {  // T-20
        next = CaptureState::kCompleted;
        RECON_ACCEPT_RULE("T-20");
      }
      if (ev == CaptureEvent::kReconstructionFailed) {  // T-21
        failure_stage_ = FailureStage::kReconstruction;
        next = CaptureState::kFailed;
        RECON_ACCEPT_RULE("T-21");
      }
      if (ev == CaptureEvent::kUserCancelReconstruction) {  // T-22
        failure_stage_ = FailureStage::kReconstruction;
        next = CaptureState::kFailed;
        RECON_ACCEPT_RULE("T-22");
      }
      if (ev == CaptureEvent::kModifyClassCAndRebuild) {  // T-30
        // 先取消当前重建（若在跑），状态仍为 Processing。
        RECON_ACCEPT_RULE("T-30");
      }
      break;

    // -------------------------------------------------------------- //
    // Failed                                                          //
    // -------------------------------------------------------------- //
    case CaptureState::kFailed:
      if (ev == CaptureEvent::kRetry) {  // T-24
        switch (failure_stage_) {
          case FailureStage::kCamera:
            next = CaptureState::kRequestingPermission;
            break;
          case FailureStage::kReconstruction:
            next = CaptureState::kProcessing;
            break;
          case FailureStage::kCalibration:
            next = CaptureState::kPreviewing;
            break;
          case FailureStage::kNone:
          default:
            // 未记录失败阶段：保守回到权限申请（最稳妥的重入口）。
            next = CaptureState::kRequestingPermission;
            break;
        }
        RECON_ACCEPT_RULE("T-24");
      }
      if (ev == CaptureEvent::kBackToHome) {  // T-25
        failure_stage_ = FailureStage::kNone;
        next = CaptureState::kIdle;
        RECON_ACCEPT_RULE("T-25");
      }
      break;

    // -------------------------------------------------------------- //
    // Completed                                                       //
    // -------------------------------------------------------------- //
    case CaptureState::kCompleted:
      if (ev == CaptureEvent::kExportModel) {  // T-26
        // 进入 U-10，状态不变。
        RECON_ACCEPT_RULE("T-26");
      }
      if (ev == CaptureEvent::kRecapture) {  // T-27
        next = CaptureState::kPreviewing;
        RECON_ACCEPT_RULE("T-27");
      }
      if (ev == CaptureEvent::kModifyClassCAndRebuild) {  // T-30
        next = CaptureState::kProcessing;
        RECON_ACCEPT_RULE("T-30");
      }
      if (ev == CaptureEvent::kOpenSettings) {  // T-28
        RECON_ACCEPT_RULE("T-28");
      }
      break;

    // -------------------------------------------------------------- //
    // Cancelled（终态）：无出边                                          //
    // -------------------------------------------------------------- //
    case CaptureState::kCancelled:
      break;
  }

  // 走到这里说明该 (state, event) 组合未在 docs/09 §4.2 中定义。
  return TransitionResult{false, state_, "—"};
}

}  // namespace platform
}  // namespace recon
