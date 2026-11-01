// SPDX-License-Identifier: BSD-3-Clause
//
// CaptureEventBridge.kt — 采集状态机的 Android ↔ native 门面。
//
// 规则本体在 native 侧 platform/common/src/capture_state.cpp（跨端共享）。
// 本对象只提供稳定的事件/状态整数常量与转发调用，避免 Kotlin 侧散落魔数。

package com.recon.scan

object CaptureEventBridge {

    // ===== 事件（与 capture_state.h 的 CaptureEvent 序号一致）=====
    const val START_SCAN = 0
    const val PROBE_COMPLETE = 1
    const val PROBE_CAMERA_UNAVAILABLE = 2
    const val PROBE_INTERNAL_ERROR = 3
    const val PERMISSIONS_GRANTED = 4
    const val CAMERA_PERMISSION_DENIED = 5
    const val MOTION_PERMISSION_DENIED = 6
    const val PHOTO_PERMISSION_DENIED = 7
    const val USER_START = 8
    const val USER_CANCEL_FROM_PREVIEW = 9
    const val FINISH_SCAN = 10
    const val CANCEL_GESTURE = 11
    const val FRAME_CAP_OR_DISK_LOW = 12
    const val APP_BACKGROUNDED = 13
    const val CAMERA_BUSY = 14
    const val SELF_CHECK_SEVERE_FAIL = 15
    const val USER_CONFIRM_RECONSTRUCT = 16
    const val USER_CONTINUE_SCAN = 17
    const val REVIEW_CANCEL = 18
    const val RECONSTRUCTION_SUCCEEDED = 19
    const val RECONSTRUCTION_FAILED = 20
    const val USER_CANCEL_RECONSTRUCTION = 21
    const val PROCESS_KILLED = 22
    const val RETRY = 23
    const val BACK_TO_HOME = 24
    const val EXPORT_MODEL = 25
    const val RECAPTURE = 26
    const val OPEN_SETTINGS = 27
    const val MODIFY_CAPTURE_PARAMS = 28
    const val MODIFY_CLASS_C_AND_REBUILD = 29

    // ===== 状态（与 capture_state.h 的 CaptureState 序号一致）=====
    const val STATE_IDLE = 0
    const val STATE_CAPABILITY_PROBE = 1
    const val STATE_REQUESTING_PERMISSION = 2
    const val STATE_PREVIEWING = 3
    const val STATE_CAPTURING = 4
    const val STATE_REVIEWING = 5
    const val STATE_PROCESSING = 6
    const val STATE_COMPLETED = 7
    const val STATE_FAILED = 8
    const val STATE_CANCELLED = 9

    // ===== 失败阶段 =====
    const val FAILURE_NONE = 0
    const val FAILURE_CAMERA = 1
    const val FAILURE_RECONSTRUCTION = 2
    const val FAILURE_CALIBRATION = 3

    /** 应用事件，返回下一状态（对应 capture_state.h::Apply().next）。 */
    external fun apply(event: Int): Int

    /** 当前状态。 */
    external fun currentState(): Int

    /** 最近失败阶段（供 T-24 重试路由）。 */
    external fun failureStage(): Int

    /** 主动记录失败阶段。 */
    external fun setFailureStage(stage: Int)

    /** 采集是否处于暂停变体（T-14）。 */
    external fun isPaused(): Boolean

    /** 清除暂停标志（回前台）。 */
    external fun clearPaused()
}
