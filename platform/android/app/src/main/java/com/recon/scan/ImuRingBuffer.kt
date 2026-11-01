// SPDX-License-Identifier: BSD-3-Clause
//
// ImuRingBuffer.kt — IMU 采样环形缓冲（Android 侧门面）。
//
// 契约：
//   · core/include/recon/recon_c_api.h —— `recon_imu_sample`
//     （`accel` = 比力、含重力，m/s²；`gyro` = rad/s；`timestamp_ns` 单调纳秒）
//   · docs/02_采集与同步.md §3.5（IMU 预积分，核心层自行减重力）、
//     §3.8（IMU 缺口容忍；时间戳倒序为须检出异常）
//   · docs/11_平台权限与集成.md §3.6（SensorManager 对接）、§3.7（字段对接表）、
//     §4.3（时间戳对齐：`SensorEvent.timestamp` 与 `Image.getTimestamp()` 同源，
//     均为 `elapsedRealtimeNanos` —— 故 **Android 两端同基，无需换算**）
//   · ⚠️ §3.6 的 AND-IMU-2 与 §3.7 对接表对 accel 口径**相互矛盾**，见下方
//
// ── 规则本体的位置（不要让本类重新长出逻辑）────────────────────────────────
//
// 环形缓冲与「本帧之前」时间窗切片的**规则本体**在
// `platform/common/src/imu_window.cpp`（纯 C++17，**双端同源**）。
// iOS 的对等物是 `platform/ios/Recon/PlatformCaptureBridge.mm` + Swift 的
// `IMURingBuffer` 门面。本类**只做转发** —— 若它长出「找分割点」之类的循环，
// 就是「同一逻辑写两遍必然漂移」的开始（由 tools/check_platform_parity.py
// 的闸门 8 静态拒收）。
//
// ── 为什么需要这一层 ───────────────────────────────────────────────────────
//
//  Android 的 `SensorManager` 回调（`SensorEventListener.onSensorChanged`）与
//  相机帧回调（`ImageReader.OnImageAvailableListener`）在**两个不同线程**上
//  独立触发，频率不同（IMU 100–200 Hz、相机 30 Hz）且互不等待。因此「IMU 采样」
//  不能随帧直接投递 —— 必须缓冲，再在每帧到达时按时间戳切出此前窗口。
//  线程安全由规则本体的 `std::mutex` 保证。
//
// ── accel 语义（⚠️ 契约冲突，不得擅自裁决）──────────────────────────────────
//
//  C ABI 明文：`recon_imu_sample.accel` 是**比力（含重力）**，核心层自行减重力
//  （docs/02 §3.5 `a_m = R(a_W − g_W)`）；docs/11 §3.7 对接表亦把 Android 的
//  `TYPE_ACCELEROMETER` 列为该字段的来源。
//
//  **但 docs/11 §3.6 的 AND-IMU-2 与之相反**，原文要求：
//
//  > 不使用 `TYPE_ACCELEROMETER` 的「含重力」值直接做预积分；必须先减去
//  > `TYPE_GRAVITY` 或用低通估计重力 …
//
//  两者不可能同时为真：若平台层先减重力、核心层再减一次 → **双重减重力**，
//  静止时加计残差从 ≈0 变成 ≈ +9.81 m/s² → VIO 预积分被系统性偏置
//  （这正是 iOS 侧缺口 H 的镜像）。且 AND-IMU-2 自身也已自相矛盾 —— 其末句
//  「减去的重力分量需与 iOS `CMAccelerometerData.acceleration`（**同样含重力**）
//  的处理一致」等于说 iOS 传含重力值。
//
//  按项目铁律（**发现两份契约冲突时不擅自裁决，登记基线锁住现状**）：
//  本类**不实现任何减重力**，也不断言应采用哪一口径；该冲突登记在
//  docs/14 §5.1.3 缺口 O，待真机实测裁决。
//
//  当前状态：Android 的传感器订阅本身**尚未实现**（`CaptureActivity.startPreview`
//  仍是骨架），故**没有任何口径被固化进代码** —— 这是如实声明，不是"已经好使"。
//  本类只保证：**投进 `append` 的 accel 会被原样搬运**，不做任何重力处理。
package com.recon.scan

/**
 * IMU 采样环形缓冲。**规则本体在 platform/common（native）**，本类只做转发。
 *
 * 线程安全：可在 SensorManager 回调线程（[append]）与相机帧线程（[windowBefore]）
 * 上并发调用（由 native 侧 `std::mutex` 保证）。
 *
 * 容量上限（默认 512 样本 ≈ 100 Hz 下 5.1 s）的推导见规则本体注释：
 * 相机长时间不来帧时限制内存增长，超过说明帧流已实质中断。
 */
object ImuRingBuffer {

    /**
     * 追加一个采样。在 **SensorManager 回调线程**调用。
     *
     * @param timestampNs 采样时刻（纳秒，单调时钟，与相机帧时间戳同基）。
     * @param accel 加速度计原始读数（m/s²）。**本类原样搬运、不做重力处理** ——
     *              取 `TYPE_ACCELEROMETER`（含重力）还是先减 `TYPE_GRAVITY`，
     *              取决于 docs/11 §3.6 AND-IMU-2 与 §3.7 对接表的冲突如何裁决
     *              （登记于 docs/14 §5.1.3 缺口 O）。**不要在本类内加减重力**。
     * @param gyro  陀螺仪角速度 rad/s（`TYPE_GYROSCOPE` 的原始读数）。
     */
    fun append(
        timestampNs: Long,
        ax: Double, ay: Double, az: Double,
        gx: Double, gy: Double, gz: Double
    ) {
        nativeAppend(timestampNs, ax, ay, az, gx, gy, gz)
    }

    /**
     * 取「`timestampNs` 之前（含等于）」的窗口，并把已消费样本移出缓冲。
     *
     * @return 交错数组 `[ts_ns, ax, ay, az, gx, gy, gz] × count`（升序），
     *         可直接投给 [NativeBridge.pushFrame] 的 `imuInterleaved`；
     *         空窗返回**零长度数组**。native 侧 OOM 时返回 `null`
     *         （调用方**必须**处理，不得静默继续）。
     *
     * ## 消费后移出（由规则本体保证）
     *
     * 每帧只需「自上一帧以来新到的 IMU」。若不移出，窗口会随采集时长线性增长 ——
     * 内核预积分是**增量式**的（docs/02 §3.5），重复预积分已算区间会**双重计数**。
     */
    fun windowBefore(timestampNs: Long): DoubleArray? = nativeWindowBefore(timestampNs)

    /** 清空缓冲并归零诊断计数（停止采集 / Activity 重建时调用）。 */
    fun reset() = nativeReset()

    /** 当前缓冲样本数（诊断用）。 */
    val count: Int get() = nativeCount()

    /** 因容量上限丢弃的样本累计数（诊断用；不为 0 表示帧流曾长时中断）。 */
    val droppedByCapacity: Int get() = nativeDroppedByCapacity()

    /** 时间戳倒序次数（诊断用；docs/02 §3.8 F-10 须检出的异常）。 */
    val timestampRegressions: Int get() = nativeTimestampRegressions()

    // --------------------------------------------------------------------- //
    // native 声明（由 platform/android/app/src/main/cpp/imu-bridge.cpp 实现）  //
    // --------------------------------------------------------------------- //

    private external fun nativeAppend(
        timestampNs: Long,
        ax: Double, ay: Double, az: Double,
        gx: Double, gy: Double, gz: Double
    )

    private external fun nativeWindowBefore(timestampNs: Long): DoubleArray?
    private external fun nativeReset()
    private external fun nativeCount(): Int
    private external fun nativeDroppedByCapacity(): Int
    private external fun nativeTimestampRegressions(): Int
}
