//
//  FramePipeline.swift
//  Recon — 采集帧与 IMU → C ABI 的投递管线（独立于主线程）
//
//  契约：
//    · core/include/recon/recon_c_api.h —— `recon_frame_packet` / `recon_imu_sample`
//    · docs/11_平台权限与集成.md §5（线程模型）、§3.7（字段对接）
//    · docs/02_采集与同步.md §5.5 F-1（深度毫米、沿光轴 z）
//
//  ── 为什么独立于 `ReconSessionController` ──────────────────────────────────
//
//  `ReconSessionController` 是 `@MainActor`（所有 `@Published` 只在主线程写）。
//  但 ARKit 的帧回调与 CoreMotion 的 IMU 回调**都不在主线程**：
//
//    · `ARSession.delegateQueue` —— 专用串行队列（docs/11 §3.2.2）
//    · CoreMotion 的 `OperationQueue`（`maxConcurrentOperationCount = 1`）
//
//  若把「帧 → C ABI」这段放在控制器上，就会出现**跨 actor 越界访问**：
//  要么编译不过（Swift 6 严格并发），要么用 `MainActor.assumeIsolated` 强行
//  跳过去 —— 后者会在每次帧回调（30 Hz）与每次 IMU 回调（100 Hz）上
//  制造一次主线程阻塞，直接吃掉 docs/02 §3.7 的 `τ_pipeline` 预算，
//  并在主线程繁忙时反压相机（docs/11 §3.1.3 的背压正是要避免的）。
//
//  因此本文件承载**全部跨队列状态**，并自带锁。控制器只做两件事：
//  ① 把内核句柄交给它（`setCore`）；② 接收它的计数快照并弹回主线程发布。
//
//  ── `accel` 语义（docs/14 §5.1.3 缺口 H）────────────────────────────────
//
//  `recon_imu_sample.accel` 是**比力（含重力）**，核心层自行减重力
//  （docs/02 §3.5 `a_m = R(a_W − g_W)`）。本管线**期望**收到的
//  `IMUSample.accelIncludingGravity` 已是含重力值；合成点在
//  `CameraCapture.startMotion()`（`gravity + userAcceleration`）。
//
//  ── IMU 缓冲与深度换算规则的归属（第三十五轮）─────────────────────────────
//
//  这两块的规则本体已下沉 `platform/common`（双端同源，经 ObjC++ 桥调用）：
//    · IMU 环形缓冲 + 时间窗切片 → `IMURingBuffer`（门面在 IMUWindow.swift）
//    · 深度单位换算              → `FrameConverter.depthMillimeters`
//  本文件**只做编排**（取 RGB/深度/IMU → 打包字段 → 投递 C ABI → 计数），
//  不再含任何缓冲或换算逻辑 —— 否则就是「同一逻辑写两遍」的开始。
//

import Foundation
import simd

/// 采集帧 / IMU → C ABI 的投递管线。
///
/// 线程安全：可在 ARKit 帧队列（`handleFrame`）、CoreMotion 队列（`appendIMU`）
/// 与主线程（`setCore` / `reset` / 读计数）上并发调用。
/// 所有可变状态要么自带锁（`IMURingBuffer`），要么由 `stateLock` 保护。
final class FramePipeline: @unchecked Sendable {

    // MARK: - 跨队列状态（全部由 stateLock 保护）
    //
    // `@unchecked Sendable` 的依据在本注释，而非「碰巧能编过」：
    //   · `imuBuffer`    —— `IMURingBuffer` 内部用 `NSLock` 保护。
    //   · `core`         —— 句柄，读写在 `stateLock` 内；且 `ReconCore` 的
    //                       C ABI 契约要求「除 recon_cancel 外不跨线程并发调用
    //                       同一 context」，本管线对帧投递是**单队列串行**的
    //                       （ARKit delegate 队列只有一个），故不违反该契约。
    //   · `pushedFrames` / `rejectedFrames` / `lastReject` —— 计数，`stateLock` 内。
    //   · `lastPipelineNote` —— 诊断串，`stateLock` 内（读经 `pipelineNote`）。
    //   · `timestampRegressions` —— **不是存储属性**，是转发 `imuBuffer` 的计算属性
    //                               （规则本体内部加锁），故无需 `stateLock`。
    //   · `onCountsChanged` —— 仅赋值与调用，不持有共享可变状态；调用方负责
    //                       在闭包内自行弹回主线程。

    private let stateLock = NSLock()
    private var core: ReconCore?
    private var pushedFrames = 0
    private var rejectedFrames = 0
    private var lastReject: RejectReason?
    /// 最近一条**管线级诊断**（非拒绝原因），如 RGB 走了 CPU 回退、
    /// YCbCr 矩阵附件缺失等。**机器可读，不是界面文案**（G9）。
    ///
    /// 与 `lastReject` 分开的理由：那是「帧被拒」；这可能是「帧被接受了，
    /// 但路径与预期不同」（例如 GPU 不可用、静默降级到 CPU）。两者语义不同，
    /// 混进一个字段会让「拒绝率」这类判据被污染。
    private var lastPipelineNote: String?

    private let imuBuffer = IMURingBuffer(capacity: 512)

    /// 拒绝投递的原因。
    ///
    /// **刻意用枚举而非中文字符串**（两条理由）：
    ///  1. 这些是**诊断值**，不是界面文案 —— 用字符串会诱导后来者直接把它塞进
    ///     某个 `Text(...)`，从而绕过本地化（G9 的存在意义）。
    ///  2. 枚举在 Swift 里可被 `switch` 穷举，新增原因时**编译期**就会提醒
    ///     每处消费点；字符串不会。
    enum RejectReason: Equatable {
        /// RGB → BGRA8 转换失败：该缓冲不是本层能处理的 YUV 双平面
        /// （或 GPU/CPU 两条路都失败）。**不是**「尚未实现」——
        /// Metal 路径已接入（`MetalComputeBackend.mm`，docs/11 §693）；
        /// 历史上这里曾因缺口 J 对**每一帧**都命中。
        case rgbFormatUnsupported
        /// 内核拒绝该帧，附 C ABI 状态码（不复用 `ReconStatus` 的文案映射，
        /// 因为这里是**诊断上下文**，不是最终呈现给用户的错误摘要）。
        case coreRejected(ReconStatus)

        /// 稳定的机器可读标识（供日志与测试断言；**不是**界面文案）。
        var diagnosticCode: String {
            switch self {
            case .rgbFormatUnsupported: return "RGB_FORMAT_UNSUPPORTED"
            case .coreRejected(let s):  return "CORE_REJECTED_\(s.rawValue)"
            }
        }
    }

    /// 计数变化回调。**在调用它的线程上同步执行**（即 ARKit 帧队列）。
    ///
    /// 实现方**必须**在闭包内自行切回主线程（例如 `Task { @MainActor in }`），
    /// 不得在此直接写 UI 状态 —— 那会在帧回调线程上触发 SwiftUI 更新。
    var onCountsChanged: ((_ accepted: Int, _ rejected: Int) -> Void)?

    /// IMU 采样的时间戳单调性漂移累计（次）。**委托规则本体**，不在本类重算。
    ///
    /// 依据：docs/02 §3.8 F-10「IMU 时间戳倒序」是需要被检出的异常；
    /// 但平台层**不擅自丢样本**（丢样本会掩盖问题），只统计并放行。
    ///
    /// ⚠️ 第三十六轮收窄：此前本类**自带一份** `lastIMUTimestamp` +
    /// `timestampRegressions` 的 Swift 实现，与规则本体 `ImuRingBuffer` 的
    /// `TimestampRegressions()` 是同一条逻辑写了两遍（且 Swift 那份无任何
    /// 外部消费点 = 死码）。现改为直接转发规则本体的计数 —— 规则本体在
    /// `platform/common/src/imu_window.cpp`，与 Android 同源。
    var timestampRegressions: Int { imuBuffer.timestampRegressions }

    // =======================================================================
    // MARK: - 内核句柄
    // =======================================================================

    /// 绑定 / 解绑内核句柄。主线程调用。
    func setCore(_ core: ReconCore?) {
        stateLock.lock()
        self.core = core
        stateLock.unlock()
    }

    // =======================================================================
    // MARK: - IMU（CoreMotion 队列）
    // =======================================================================

    /// 追加一个 IMU 采样。**在 CoreMotion 队列调用**。
    ///
    /// 倒序检测（docs/02 §3.8 F-10）由规则本体在 `Append()` 内完成 —— 本方法
    /// **不再自己比较时间戳**（那会是同一条逻辑的第二份实现）。
    func appendIMU(_ sample: IMUSample) {
        imuBuffer.append(sample)
    }

    // =======================================================================
    // MARK: - 帧（ARKit 帧队列）
    // =======================================================================

    /// 计数快照（供诊断与 UI）。
    var counts: (accepted: Int, rejected: Int, lastReject: RejectReason?) {
        stateLock.lock()
        defer { stateLock.unlock() }
        return (pushedFrames, rejectedFrames, lastReject)
    }

    /// 最近一条管线级诊断（nil = 无）。与 `counts` 分开，语义见 `lastPipelineNote`。
    var pipelineNote: String? {
        stateLock.lock()
        defer { stateLock.unlock() }
        return lastPipelineNote
    }

    /// 投递一帧。**在 ARKit 帧队列调用**。
    ///
    /// 字段打包（`recon_frame_packet`）由 `ReconCore.pushFrame` 一处完成 ——
    /// 本方法**只负责**解包 ARKit 的 `CVPixelBuffer` / `simd_float4x4`
    /// 并切出 IMU 窗口。**不得**在此再次填装 `recon_frame_packet`
    /// （同一逻辑写两遍必然漂移，见 MEMORY.md）。
    func handleFrame(_ packet: CaptureFramePacket) {
        stateLock.lock()
        let core = self.core
        stateLock.unlock()
        guard let core else { return }

        // ── ① RGB：转换失败则拒绝整帧（不降级、不猜格式）────────────────────
        //
        // 旧实现在此处直接 `CVPixelBufferGetBaseAddress(capturedImage)`：
        // 该调用对 ARKit 的**平面格式**缓冲返回 NULL，于是 `guard else { return }`
        // **静默丢弃 100% 的帧**（docs/14 §5.1.3 缺口 J）。
        // 现在改为经 FrameConverter，并**显式计数拒绝**，绝不静默。
        guard let rgb = FrameConverter.bgra8(from: packet.pixelBuffer) else {
            recordRejection(.rgbFormatUnsupported)
            return
        }

        // ── ①b RGB 转换降级上报（docs/14 CT-9「无静默降级」）─────────────────
        //
        // `FrameConverter` 会在 CPU 回退、或 YCbCr 矩阵附件缺失（按 BT.601
        // 假设）时带回 `converterNote`。这里**必须**把它带出去 —— 否则
        // 「GPU 没跑起来、一直在用 CPU」这种状态在界面上完全不可见，
        // 而它恰好会让 docs/11 的 `< 0.4 ms` 目标悄悄不成立。
        if !rgb.usedGPU || !rgb.converterNote.isEmpty {
            // ⚠️ 诊断串刻意用**机器可读的 ASCII**（不是界面文案）：它会被日志
            //    与测试断言消费，也确实不该出现在界面上 —— 一旦此处写中文，
            //    G9（文案去硬编码）会正确地报「视图层存在未接线的中文文案」，
            //    那说明它被误当成文案了。`RejectReason.diagnosticCode` 同此口径。
            recordPipelineNote("RGB " + (rgb.usedGPU ? "GPU_NOTE: " : "CPU_FALLBACK: ")
                               + (rgb.converterNote.isEmpty ? "GPU_UNAVAILABLE"
                                                            : rgb.converterNote))
        }

        // ── ② 深度：原生分辨率、原生单位（毫米），不做重采样（docs/04 IR-1）
        let depth = packet.depthMap.flatMap { FrameConverter.depthMillimeters(from: $0) }

        // ── ③ IMU：切出「本帧之前」的时间窗（C ABI：「本帧之前的 IMU 时间窗」）
        //
        // `imuWindow` 是 ObjC++ 桥对象，其 `samplesRaw` 指向**连续
        // `recon_imu_sample` 数组**（规则本体在 platform/common 产出），
        // 可直接投递给 C ABI，**无需逐样本拷贝**（100 Hz × 数百样本下这对
        // τ_pipeline 预算很重要）。窗口在本对象生命周期内有效。
        let imuWindow = imuBuffer.window(before: packet.timestamp)
        let imuPtr = imuWindow.samplesRaw?.assumingMemoryBound(to: recon_imu_sample.self)
        let imuCount = UInt32(imuWindow.count)

        // ── ④ 时间戳（纳秒，单调时钟），多处复用 ───────────────────────────
        let timestampNs = UInt64(packet.timestamp * 1_000_000_000)

        // ── ⑤ 内参。ARKit 无畸变参数（已在线校正），k/p 置 0 ────────────────
        let w = UInt32(rgb.width)
        let h = UInt32(rgb.height)
        var intr = recon_camera_intrinsics()
        intr.fx = Double(packet.intrinsics[0][0])
        intr.fy = Double(packet.intrinsics[1][1])
        intr.cx = Double(packet.intrinsics[2][0])
        intr.cy = Double(packet.intrinsics[2][1])
        intr.k1 = 0; intr.k2 = 0; intr.k3 = 0; intr.p1 = 0; intr.p2 = 0
        intr.width = Double(w)
        intr.height = Double(h)

        // ── ⑥ 位姿：ARFrame.camera.transform 即 world→camera ───────────────
        // C ABI 的 `recon_pose.r` 是 `double r[9]`（行主序）—— Swift 导入为 9 元组。
        let t = packet.transform
        var pose = recon_pose()
        pose.r = (Double(t[0][0]), Double(t[0][1]), Double(t[0][2]),
                  Double(t[1][0]), Double(t[1][1]), Double(t[1][2]),
                  Double(t[2][0]), Double(t[2][1]), Double(t[2][2]))
        // 约定 t = −R·C（见 CameraPose 说明）。由 4×4 的平移列 t4 反推相机中心
        // C = −Rᵀ·t4，再得 t。**直接透传 t4 是错的**（会与内核约定不一致）。
        let t4 = SIMD3<Double>(Double(t[3][0]), Double(t[3][1]), Double(t[3][2]))
        let r3 = simd_double3x3(
            SIMD3<Double>(pose.r.0, pose.r.1, pose.r.2),
            SIMD3<Double>(pose.r.3, pose.r.4, pose.r.5),
            SIMD3<Double>(pose.r.6, pose.r.7, pose.r.8))
        let c = -(r3.transpose * t4)
        pose.t = (c.x, c.y, c.z)
        // ⚠️ `recon_pose.timestamp_ns` 是 **`double`**（单位仍是纳秒，见
        //    recon_c_api.h:62），而 `recon_frame.timestamp_ns`（同文件:215）是
        //    `uint64_t`。同一个纳秒量在两条结构里类型不同 —— 故此处必须显式
        //    转 `Double`，否则 `UInt64 → Double` 无隐式转换，编译失败。
        //    精度：纳秒级时间戳（~1e18）在 double 下有效位约 15–16 位十进制，
        //    即 ~1e3 ns 分辨率 —— 该字段由内核按 double 契约消费，保持原样。
        pose.timestamp_ns = Double(timestampNs)

        // ── ⑦ 帧号：在锁内自增，保证唯一且单调（帧队列串行，但实现不依赖该前提）
        stateLock.lock()
        let frameId = UInt64(pushedFrames)
        stateLock.unlock()

        // ── ⑧ 投递。三个指针必须在同一作用域内有效 ─────────────────────────
        // `recon_push_frame` 契约：返回后内核不访问调用方内存（深拷贝），
        // 故用 `withUnsafeBufferPointer` 包住整个调用即可。
        let status: ReconStatus = rgb.data.withUnsafeBufferPointer { rgbBuf in
            guard let rgbBase = rgbBuf.baseAddress else { return .invalidArgument }

            func push(depthPtr: UnsafePointer<UInt16>?,
                      depthW: UInt32, depthH: UInt32,
                      imuPtr: UnsafePointer<recon_imu_sample>?,
                      imuCount: UInt32) -> ReconStatus {
                core.pushFrame(
                    frameId: frameId,
                    timestampNs: timestampNs,
                    rgb: rgbBase,
                    rgbWidth: w, rgbHeight: h, rgbStride: UInt32(rgb.bytesPerRow),
                    depthMm: depthPtr,
                    depthWidth: depthW, depthHeight: depthH,
                    // ARKit 的 `sceneDepth` **无逐像素置信度**（docs/04 §2.4.1：
                    // HW_LIFAR 一行明写「无」）。传 nil 是**正确的** ——
                    // 内核会用「局部一致性推导置信度」（§2.4.2）；传全 255
                    // 反而会伪造一个平台并不提供的信息。
                    depthConfidence: nil,
                    depthSource: ReconDepthSource.lidar.rawValue,
                    intrinsics: intr, pose: pose,
                    imu: imuPtr, imuCount: imuCount,
                    exposureUs: 0, iso: 0
                )
            }

            if let depth {
                return depth.millimeters.withUnsafeBufferPointer { dBuf in
                    push(depthPtr: dBuf.baseAddress,
                         depthW: UInt32(depth.width), depthH: UInt32(depth.height),
                         imuPtr: imuPtr, imuCount: imuCount)
                }
            }
            return push(depthPtr: nil, depthW: 0, depthH: 0,
                        imuPtr: imuPtr, imuCount: imuCount)
        }

        // ── ⑨ 计数：只累加成功，失败保留状态（不静默）─────────────────────
        stateLock.lock()
        if status == .ok {
            pushedFrames += 1
        } else {
            rejectedFrames += 1
            lastReject = .coreRejected(status)
        }
        let accepted = pushedFrames
        let rejected = rejectedFrames
        stateLock.unlock()

        onCountsChanged?(accepted, rejected)
    }

    /// 清空（停止采集 / 切换会话时调用）。主线程调用。
    func reset() {
        stateLock.lock()
        pushedFrames = 0
        rejectedFrames = 0
        lastReject = nil
        lastPipelineNote = nil
        stateLock.unlock()
        // 倒序计数随缓冲一并归零（规则本体 `Reset()` 内完成）—— 本类不再单独维护。
        imuBuffer.reset()
    }

    private func recordRejection(_ reason: RejectReason) {
        stateLock.lock()
        rejectedFrames += 1
        lastReject = reason
        let accepted = pushedFrames
        let rejected = rejectedFrames
        stateLock.unlock()
        onCountsChanged?(accepted, rejected)
    }

    /// 记录一条**管线级诊断**（非拒绝）。不递增任何计数。
    ///
    /// 与 `recordRejection` 分开：这条路径上帧**是被接受的**，只是走了
    /// 非预期的路（如 CPU 回退）。混入 `rejectedFrames` 会让「拒绝率」失真。
    private func recordPipelineNote(_ note: String) {
        stateLock.lock()
        lastPipelineNote = note
        stateLock.unlock()
    }
}

/// C ABI `recon_depth_source` 的 Swift 对等（**只列本层实际使用的取值**）。
///
/// 完整枚举在 `recon_c_api.h`；此处**不整份复制** —— 整份复制会立刻变成
/// 「第二事实来源」，需要像 `ReconStatus` 那样再加一条逐值对等闸门。
/// 若未来需要在多处引用其它取值，应改为整份对等表 + 闸门（G11 模式），
/// 而不是零散添加。
enum ReconDepthSource: Int32 {
    case none = 0
    case algo = 1
    case tof = 2
    case lidar = 3
}
