// SPDX-License-Identifier: BSD-3-Clause
//
// CaptureStateBridge.h — 采集状态机的 Objective-C++ 门面（给 Swift 用）。
//
// 规则本体在 platform/common/src/capture_state.cpp（跨端共享，见 docs/09 §4.1）。
// Swift 无法直接消费 C++，故用一层 Objective-C++ 薄封装暴露。这是 Android 侧
// JNI 桥接（platform-bridge.cpp）的 iOS 对等物 —— 两端功能对等由
// tools/check_platform_parity.py 校验。

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// 与 recon::platform::CaptureState 序号一致。
typedef NS_ENUM(NSInteger, ReconCaptureState) {
    ReconCaptureStateIdle = 0,
    ReconCaptureStateCapabilityProbe = 1,
    ReconCaptureStateRequestingPermission = 2,
    ReconCaptureStatePreviewing = 3,
    ReconCaptureStateCapturing = 4,
    ReconCaptureStateReviewing = 5,
    ReconCaptureStateProcessing = 6,
    ReconCaptureStateCompleted = 7,
    ReconCaptureStateFailed = 8,
    ReconCaptureStateCancelled = 9,
};

/// 与 recon::platform::CaptureEvent 序号一致。
typedef NS_ENUM(NSInteger, ReconCaptureEvent) {
    ReconCaptureEventStartScan = 0,
    ReconCaptureEventProbeComplete = 1,
    ReconCaptureEventProbeCameraUnavailable = 2,
    ReconCaptureEventProbeInternalError = 3,
    ReconCaptureEventPermissionsGranted = 4,
    ReconCaptureEventCameraPermissionDenied = 5,
    ReconCaptureEventMotionPermissionDenied = 6,
    ReconCaptureEventPhotoPermissionDenied = 7,
    ReconCaptureEventUserStart = 8,
    ReconCaptureEventUserCancelFromPreview = 9,
    ReconCaptureEventFinishScan = 10,
    ReconCaptureEventCancelGesture = 11,
    ReconCaptureEventFrameCapOrDiskLow = 12,
    ReconCaptureEventAppBackgrounded = 13,
    ReconCaptureEventCameraBusy = 14,
    ReconCaptureEventSelfCheckSevereFail = 15,
    ReconCaptureEventUserConfirmReconstruct = 16,
    ReconCaptureEventUserContinueScan = 17,
    ReconCaptureEventReviewCancel = 18,
    ReconCaptureEventReconstructionSucceeded = 19,
    ReconCaptureEventReconstructionFailed = 20,
    ReconCaptureEventUserCancelReconstruction = 21,
    ReconCaptureEventProcessKilled = 22,
    ReconCaptureEventRetry = 23,
    ReconCaptureEventBackToHome = 24,
    ReconCaptureEventExportModel = 25,
    ReconCaptureEventRecapture = 26,
    ReconCaptureEventOpenSettings = 27,
    ReconCaptureEventModifyCaptureParams = 28,
    ReconCaptureEventModifyClassCAndRebuild = 29,
};

/// 与 recon::platform::FailureStage 序号一致。
typedef NS_ENUM(NSInteger, ReconFailureStage) {
    ReconFailureStageNone = 0,
    ReconFailureStageCamera = 1,
    ReconFailureStageReconstruction = 2,
    ReconFailureStageCalibration = 3,
};

/// 采集状态机（单例，非线程安全；仅在主线程使用）。
@interface ReconCaptureStateMachine : NSObject

+ (instancetype)shared;

- (ReconCaptureState)applyEvent:(ReconCaptureEvent)event;
- (ReconCaptureState)currentState;
- (ReconFailureStage)failureStage;
- (void)setFailureStage:(ReconFailureStage)stage;
- (BOOL)isPaused;
- (void)clearPaused;

@end

NS_ASSUME_NONNULL_END
