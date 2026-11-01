// SPDX-License-Identifier: BSD-3-Clause
//
// imu-bridge.cpp — platform/common 层 IMU 环形缓冲的 JNI 桥接。
//
// 桥接对象：`ImuRingBuffer`（Kotlin object）。
// 规则本体在 platform/common/src/imu_window.cpp，**跨 iOS/Android 共享**；
// iOS 的对等物是 platform/ios/Recon/PlatformCaptureBridge.mm（ObjC++ 薄封装）。
//
// ── 为什么这里**不是**「同一逻辑写两遍」 ────────────────────────────────────
//
// 本文件只做两件平台侧的事，规则本身一行没有：
//   ① JNI 类型搬运（jlong/jdouble ↔ C++ POD）；
//   ② 把窗口样本**重排为 `recon_frame_packet.imu` 要求的交错布局**
//      `[ts_ns, ax, ay, az, gx, gy, gz] × count` —— 这是**传输层约定**
//      （与 native-lib.cpp 的 pushFrame 入参一致），不是切窗规则。
// 切窗边界（`<=` 含等于）、消费后移出、乱序只排序不丢弃，全部在规则本体。
// 本文件若长出任何 `if (ts <= ...)` 之类的判断，就是漂移的开始 ——
// 由 tools/check_platform_parity.py 的闸门 8 静态守住。
//
// ── 为什么用单例而非句柄 ──────────────────────────────────────────────────
//
// 与 platform-bridge.cpp 的 CaptureStateMachine 同理由：**每进程同时只有一次
// 采集会话**（docs/09 §4.1 的状态机保证不会有两个 Capturing）。故单例即可，
// 无需在 Kotlin 侧管理生命周期句柄。Android 的 Activity 重建（旋转屏幕）会
// 重新 `reset()`，语义正确。

#include <jni.h>

#include <cstring>
#include <vector>

#include "recon_platform/imu_window.h"

using recon::platform::ImuRingBuffer;

namespace {

// 默认容量与 iOS 侧（IMURingBuffer(capacity: 512)）**必须一致** ——
// 否则两端在「帧流长时中断」时的丢弃行为会不同（docs/02 §3.8）。
// 该容量由规则本体的 kDefaultCapacity 提供，此处直接引用以免第二事实来源。
ImuRingBuffer& Buffer() {
  static ImuRingBuffer instance;
  return instance;
}

// 交错布局每样本占的 double 数：ts, ax, ay, az, gx, gy, gz。
constexpr int kStridePerSample = 7;

}  // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_com_recon_scan_ImuRingBuffer_nativeAppend(JNIEnv*, jobject, jlong ts_ns,
                                         jdouble ax, jdouble ay, jdouble az,
                                         jdouble gx, jdouble gy, jdouble gz) {
  recon_imu_sample s;
  std::memset(&s, 0, sizeof(s));
  s.timestamp_ns = static_cast<int64_t>(ts_ns);
  s.accel[0] = ax; s.accel[1] = ay; s.accel[2] = az;
  s.gyro[0] = gx;  s.gyro[1] = gy;  s.gyro[2] = gz;
  Buffer().Append(s);
}

/**
 * 取「ts_ns 之前（含等于）」的窗口，并把已消费样本移出。
 *
 * @return 交错数组 `[ts_ns, ax, ay, az, gx, gy, gz] × count`（升序），
 *         可直接投给 NativeBridge.pushFrame 的 imuInterleaved；
 *         空窗返回**零长度数组**（不是 null —— 让 Kotlin 侧无需判空，
 *         且 null 在 pushFrame 里另有语义「本帧无 IMU」）。
 */
JNIEXPORT jdoubleArray JNICALL
Java_com_recon_scan_ImuRingBuffer_nativeWindowBefore(JNIEnv* env, jobject, jlong ts_ns) {
  recon::platform::ImuWindow win =
      Buffer().WindowBefore(static_cast<int64_t>(ts_ns));

  const size_t n = win.samples.size();
  jdoubleArray out = env->NewDoubleArray(static_cast<jsize>(n * kStridePerSample));
  if (out == nullptr) {
    return nullptr;  // OOM：交由 Kotlin 侧按 null 处理（不得静默继续）。
  }
  if (n == 0) {
    return out;  // 零长度数组。
  }

  std::vector<double> flat(n * kStridePerSample);
  for (size_t i = 0; i < n; ++i) {
    const recon_imu_sample& s = win.samples[i];
    double* p = &flat[i * kStridePerSample];
    p[0] = static_cast<double>(s.timestamp_ns);
    p[1] = s.accel[0]; p[2] = s.accel[1]; p[3] = s.accel[2];
    p[4] = s.gyro[0];  p[5] = s.gyro[1];  p[6] = s.gyro[2];
  }
  env->SetDoubleArrayRegion(out, 0, static_cast<jsize>(flat.size()), flat.data());
  return out;
}

JNIEXPORT void JNICALL
Java_com_recon_scan_ImuRingBuffer_nativeReset(JNIEnv*, jobject) {
  Buffer().Reset();
}

JNIEXPORT jint JNICALL
Java_com_recon_scan_ImuRingBuffer_nativeCount(JNIEnv*, jobject) {
  return static_cast<jint>(Buffer().Count());
}

JNIEXPORT jint JNICALL
Java_com_recon_scan_ImuRingBuffer_nativeDroppedByCapacity(JNIEnv*, jobject) {
  return static_cast<jint>(Buffer().DroppedByCapacity());
}

JNIEXPORT jint JNICALL
Java_com_recon_scan_ImuRingBuffer_nativeTimestampRegressions(JNIEnv*, jobject) {
  return static_cast<jint>(Buffer().TimestampRegressions());
}

}  // extern "C"
