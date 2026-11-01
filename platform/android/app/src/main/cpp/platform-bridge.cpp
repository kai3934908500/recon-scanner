// SPDX-License-Identifier: BSD-3-Clause
//
// platform-bridge.cpp — 平台公共层（capture_state）的 JNI 桥接。
//
// 桥接对象：NativeBridge（其 CaptureEventBridge 方法）。规则本体在
// platform/common/src/capture_state.cpp，跨 iOS/Android 共享。
//
// 状态机是**单实例、非线程安全**的：每进程一个。调用方（Activity）保证
// 只在主线程调用。

#include <jni.h>

#include "recon_platform/capture_state.h"

using recon::platform::CaptureEvent;
using recon::platform::CaptureState;
using recon::platform::CaptureStateMachine;

namespace {
CaptureStateMachine& Machine() {
  static CaptureStateMachine instance;
  return instance;
}
}  // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_com_recon_scan_CaptureEventBridge_apply(JNIEnv*, jobject, jint event) {
  const auto ev = static_cast<CaptureEvent>(event);
  const auto result = Machine().Apply(ev);
  return static_cast<jint>(result.next);
}

JNIEXPORT jint JNICALL
Java_com_recon_scan_CaptureEventBridge_currentState(JNIEnv*, jobject) {
  return static_cast<jint>(Machine().state());
}

JNIEXPORT jint JNICALL
Java_com_recon_scan_CaptureEventBridge_failureStage(JNIEnv*, jobject) {
  return static_cast<jint>(Machine().failure_stage());
}

JNIEXPORT void JNICALL
Java_com_recon_scan_CaptureEventBridge_setFailureStage(JNIEnv*, jobject, jint stage) {
  Machine().set_failure_stage(static_cast<recon::platform::FailureStage>(stage));
}

JNIEXPORT jboolean JNICALL
Java_com_recon_scan_CaptureEventBridge_isPaused(JNIEnv*, jobject) {
  return Machine().paused() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_recon_scan_CaptureEventBridge_clearPaused(JNIEnv*, jobject) {
  Machine().clear_paused();
}

}  // extern "C"
