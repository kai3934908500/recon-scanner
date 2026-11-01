//
//  IMUWindow.swift
//  Recon — IMU 采样输入类型（平台边界）与缓冲门面
//
//  契约：
//    · core/include/recon/recon_c_api.h —— `recon_imu_sample`
//      （`accel` = **比力，含重力**，m/s²；`gyro` = rad/s；`timestamp_ns` 单调纳秒）
//    · docs/02_采集与同步.md §3.5（IMU 预积分，核心层自行减重力）
//    · docs/11_平台权限与集成.md §3.1.5（CoreMotion 对接）与 §4.3（时间戳对齐）
//
//  ── 本文件在第三十五轮被收窄（重要）────────────────────────────────────────
//
//  原先本文件**自带一份** IMU 环形缓冲与时间窗切片的 Swift 实现。那违反铁律
//  「同一逻辑写两遍必然漂移」：Android 的 SensorManager 侧若照写一份 Kotlin，
//  两端的**边界语义**（`<=` 含等于）与**消费语义**（切窗后必须移出，否则预积分
//  双重计数）就会各写各的，且漂移后**无编译期信号**。
//
//  现在规则本体在 `platform/common/`（纯 C++17，双端同源）：
//    · `platform/common/include/recon_platform/imu_window.h`
//    · `platform/common/src/imu_window.cpp`
//  iOS 经 `platform/ios/Recon/PlatformCaptureBridge.mm`（ObjC++ 薄封装）调用，
//  Android 经 JNI 薄封装调用。**本文件不再含任何缓冲/切窗逻辑**。
//
//  ── 为什么还需要这一层 ────────────────────────────────────────────────────
//
//  ① `IMUSample` 是**平台输入表示**：CoreMotion 的 `CMDeviceMotion` 把加速度拆成
//     `gravity + userAcceleration`，采样点在 `CameraCapture.startMotion()`。该
//     结构体唯一的职责是把这 7 个数搬进缓冲，并在类型层面**拒绝**「去重力值」
//     的误用（`accelIncludingGravity` 的命名即该守卫，见缺口 H 的成因）。
//  ② `IMURingBuffer` 是**门面**：把 ObjC++ 桥的 `ReconImuRingBuffer` 包成
//     Swift 友好接口。它**不实现任何规则** —— 若它长出 `if`，就是漂移的开始。
//
//  ── accel 语义（重要，见 docs/14 §5.1.3 缺口 H）──────────────────────────
//
//  C ABI 明文规定 `recon_imu_sample.accel` 是**比力（含重力）**，且核心层
//  自行减重力（docs/02 §3.5 `a_m = R(a_W − g_W)`）。iOS 的 `CMDeviceMotion`
//  把加速度拆成了 `gravity` + `userAcceleration`（后者**已去重力**），
//  两者相加才是含重力的原始比力。`IMUSample` 因此**只接收含重力值**。
//

import Foundation
import simd

// ===========================================================================
// MARK: - IMU 采样（平台输入表示）
// ===========================================================================

/// 单个 IMU 采样，**平台边界的输入表示**。
///
/// 与 `recon_imu_sample` 字段语义一致；本类型存在的唯一理由是把 CoreMotion
/// 的回调载荷搬进 `IMURingBuffer`，并携带 `accel` 的语义守卫。
public struct IMUSample {
    /// 采样时刻（秒，mach 绝对时间基）。与 `ARFrame.timestamp` 同基，可直接相减。
    public let timestamp: TimeInterval
    /// 陀螺仪角速度（rad/s）。
    public let gyro: SIMD3<Double>
    /// 加速度计**比力（含重力）**（m/s²）。
    ///
    /// ⚠️ **不是** `userAcceleration`。`CMDeviceMotion` 的 `userAcceleration`
    /// 已去重力，直接送它会令核心层「再减一次重力」，静止时的加计残差
    /// 从 ≈0 变成 ≈ −9.81 m/s² → VIO 预积分被系统性偏置（静默失效）。
    /// 正确来源见 `IMUSample.from(deviceMotion:)` 与 `CameraCapture.startMotion()`。
    public let accelIncludingGravity: SIMD3<Double>

    public init(timestamp: TimeInterval,
                gyro: SIMD3<Double>,
                accelIncludingGravity: SIMD3<Double>) {
        self.timestamp = timestamp
        self.gyro = gyro
        self.accelIncludingGravity = accelIncludingGravity
    }
}

// ===========================================================================
// MARK: - 环形缓冲门面
// ===========================================================================

/// IMU 采样环形缓冲（**规则本体在 platform/common**，本类只做转发）。
///
/// 线程安全由规则本体（`ImuRingBuffer` 的 `std::mutex`）保证，故本类可在
/// CoreMotion 队列（`append`）与 ARKit 帧队列（`window`）上并发调用。
///
/// 容量上限的存在理由：若相机长时间不来帧（例如用户把手机放下），IMU 仍以
/// 100 Hz 持续灌入；不设上限则内存无界增长。上限按 docs/02 §3.8 的 IMU 缺口
/// 容忍推导：默认 512 样本 ≈ 100 Hz 下 5.1 s，足以覆盖任何正常帧间隔，
/// 超过则说明帧流已实质中断（应由采集状态机处理，而非靠缓冲兜底）。
public final class IMURingBuffer {

    private let impl: ReconImuRingBuffer

    /// 环形容量（样本数）。必须为正（规则本体会把 0 提升为 1）。
    public let capacity: Int

    /// 因容量上限而被丢弃的样本累计数（诊断用；不为 0 表示帧流曾长时中断）。
    public var droppedByCapacity: Int { Int(impl.droppedByCapacity) }

    /// 时间戳倒序次数（诊断用；docs/02 §3.8 F-10 须检出的异常）。
    public var timestampRegressions: Int { Int(impl.timestampRegressions) }

    public init(capacity: Int = 512) {
        // 防御：0 或负数会让环形逻辑退化。取至少 1（与规则本体一致）。
        let cap = max(1, capacity)
        self.capacity = cap
        self.impl = ReconImuRingBuffer(capacity: UInt(cap))
    }

    /// 追加一个采样。在 **CoreMotion 队列**调用。
    ///
    /// ⚠️ 传入的 `accelIncludingGravity` 必须是**含重力**的比力 —— 类型名即守卫。
    public func append(_ sample: IMUSample) {
        let a = sample.accelIncludingGravity
        let g = sample.gyro
        // ⚠️ 本文件**不再自行推断** ObjC 选择子的 Swift 名字：`PlatformCaptureBridge.h`
        //    已用显式 `NS_SWIFT_NAME` 钉死为 `append(timestampNs:…)` 与
        //    `window(beforeNs:)`。理由（一次真实教训）：本机无 Swift 编译器，
        //    靠 Clang importer 猜命名无法本地验证 —— 曾据此写下
        //    `windowBeforeNs(_:)` 与 `append(timestampNs:…)` 两处推断，前者恰好
        //    正确、后者错误（`append` 后无介词，不触发「动词+介词短语」拆分），
        //    CI run 41275447507 报 "has no member 'append'"。
        //    显式命名 = 纯类型检查、零推断（与 L10n 类型化重载同一方针）。
        impl.append(timestampNs: Int64(sample.timestamp * 1_000_000_000),
                    accelX: a.x, accelY: a.y, accelZ: a.z,
                    gyroX: g.x, gyroY: g.y, gyroZ: g.z)
    }

    /// 取「`timestamp` 之前（含等于）的窗口」，并把已消费样本移出缓冲。
    ///
    /// - Parameter timestamp: 帧时间戳（秒，与 IMU 同基）。
    /// - Returns: 桥接层的窗口对象；其 `samplesRaw` 可直接投递给 C ABI 的
    ///   `recon_push_frame`（`const recon_imu_sample*`），**无需逐样本拷贝**。
    ///   窗口内**不含** `timestamp` 之后的样本（C ABI：「**本帧之前**的 IMU 时间窗」）。
    ///
    /// ## 消费后移出（由规则本体保证）
    ///
    /// 每帧只需「自上一帧以来新到的 IMU」。若不移出，窗口会随采集时长线性增长，
    /// 第 300 帧的窗口将携带全部历史样本 —— 内核的预积分是**增量式**的
    /// （docs/02 §3.5），重复预积分已算过的区间会**双重计数**。
    public func window(before timestamp: TimeInterval) -> ReconImuWindow {
        // 名字由 `PlatformCaptureBridge.h` 的 `NS_SWIFT_NAME(window(beforeNs:))`
        // 显式钉死（该处注释说明了为什么不靠 importer 推断）。
        impl.window(beforeNs: Int64(timestamp * 1_000_000_000))
    }

    /// 清空缓冲（停止采集时调用）。
    public func reset() {
        impl.reset()
    }

    /// 当前缓冲的样本数（诊断用）。
    public var count: Int { Int(impl.count) }
}
