//
//  CameraCapture.swift
//  Recon — iOS 相机与深度采集（AVFoundation + ARKit）
//
//  契约：docs/11_平台权限与集成.md §1.3（申请时序）、§1.4（Info.plist）、
//       §3（平台相机/AR/IMU API 对接）、docs/02_采集与同步.md §5.5 F-1
//       （`ARFrame.sceneDepth` 沿光轴 z）、docs/13（热与电降级）。
//  以及 docs/09_UI规格.md §3.3（预览层、异常提示）。
//
//  ── 三个关键设计决定（各有理由）─────────────────────────────────────────
//
//  1) **双路采集成像**：ARKit 的 `ARFrame.capturedImage`（RGB）与
//     `ARFrame.sceneDepth`（深度）来自**同一帧**，时间戳天然一致。
//     因此首选 ARKit 而非「AVCaptureSession + 单独 depth session」——
//     后者会引入两路之间的时间对齐误差，直接吃掉 02 文档的 τ_pipeline 预算。
//
//  2) **不做插值/平滑**：深度与位姿**原样**交给内核（`recon_push_frame`）。
//     任何 UI 层的滤波都会污染后续 VIO/TSDF 的统计假设。
//
//  3) **零网络**：本文件不引用任何网络 API（00 R-LOCAL / docs/11 §1.6）。
//
//  ── 诚实声明（当前实现边界）────────────────────────────────────────────
//  本文件实现了 ARKit 会话的生命周期、配置、帧回调、深度/位姿提取与热状态
//  降级。但**真机验证尚未进行**（无 macOS 构建环境、无 LiDAR 设备）。
//  因此以下各项**未经验证**，不得视为已通过：
//    · `sceneDepth` 在实机上的帧率与 `ARFrame.timestamp` 单调性；
//    · 热降级阈值（docs/13 §5.1）在真实升温曲线下的行为；
//    · `depthMap` 的字节布局与 `kCVPixelFormatType_DepthFloat32` 的一致性。
//
//  ── 像素格式转换的归属（不要让本文件重新长出这段逻辑）────────────────────
//
//  `capturedImage`（YUV 双平面）与 `depthMap`（Float32 单平面）→ C ABI 载荷的
//  全部转换在 `FrameConverter.swift`，本文件**只负责把缓冲原样取出来**
//  （`ARFrame.sceneDepth?.depthMap` / `frame.capturedImage`）。理由见
//  `ReconSessionController` 文件头：转换牵涉 vImage/Metal，是独立可测单元。
//
//  **已知**：`capturedImage` 的 `CVPixelBufferGetBaseAddress` 对其**返回 NULL**
//  （平面格式），故 RGB 路当前**不可用**（`FrameConverter.bgra8` 恒返回 nil）。
//  这是**显式登记的缺口**（docs/14 §5.1.3 缺口 J），不是静默失败。
//

import Foundation
import ARKit
import CoreMotion
import UIKit
import simd

// ===========================================================================
// MARK: - 采集能力探测（对应 U-02 与 recon_probe_capability）
// ===========================================================================

/// ARKit 能力探测结果。输出与 docs/09 §3.2 的「探测到的传感器」区块一致。
public struct ARCapability {
    public let worldTrackingSupported: Bool
    public let sceneDepthSupported: Bool
    public let deviceMotionAvailable: Bool
    public let gyroAvailable: Bool
    public let accelerometerAvailable: Bool
    public let videoResolution: (w: Int, h: Int)?
    public let thermalState: ProcessInfo.ThermalState

    /// 是否走 RGBD 分支（有可用深度）。
    public var hasDepth: Bool { sceneDepthSupported }

    /// 依 §3.2 取值表推导管线。
    public var pipeline: CapabilityPipeline {
        if sceneDepthSupported {
            // LiDAR 与 ToF 的区分需读取 `ARWorldTrackingConfiguration` 的
            // 设备能力标志；此处保守按 LiDAR 处理，实机验证后细化。
            return .rgbdLidar
        }
        return .monoMVS
    }

    /// 探测当前设备能力。**不申请任何权限**（docs/11 §1.3：能力探测不需要权限）。
    public static func probe() -> ARCapability {
        let motion = CMMotionManager()
        return ARCapability(
            worldTrackingSupported: ARWorldTrackingConfiguration.isSupported,
            sceneDepthSupported: ARWorldTrackingConfiguration.supportsFrameSemantics(.sceneDepth),
            deviceMotionAvailable: motion.isDeviceMotionAvailable,
            gyroAvailable: motion.isGyroAvailable,
            accelerometerAvailable: motion.isAccelerometerAvailable,
            videoResolution: nil,      // 需在会话运行的 `ARFrame.camera.imageResolution` 读取
            thermalState: ProcessInfo.processInfo.thermalState
        )
    }
}

// ===========================================================================
// MARK: - 采集会话（ARKit）
// ===========================================================================

/// 一帧采集数据（交给内核的载荷）。
///
/// 与 C ABI 的 `recon_frame_packet` + `recon_imu_sample` 对应。时间戳单位统一为
/// **秒**（`ARFrame.timestamp` 即秒）。
public struct CaptureFramePacket {
    /// RGB 像素缓冲（由 `ARFrame.capturedImage` 提供）。
    ///
    /// ⚠️ 真机上是 `420YpCbCr8BiPlanarFullRange`（**YUV 双平面**），
    /// **不是** BGRA8。转换见 `FrameConverter.bgra8(from:)`（当前返回 nil）。
    public let pixelBuffer: CVPixelBuffer
    /// 深度图（`ARFrame.sceneDepth.depthMap`，单通道 Float32 米，沿光轴 z）。
    ///
    /// 尺寸**独立于** RGB（docs/11 §3.2.1：通常更低，如 256×192）。转换见
    /// `FrameConverter.depthMillimeters(from:)`。
    public let depthMap: CVPixelBuffer?
    /// 相机位姿（world→camera，与 `CameraPose` 约定一致）。
    public let transform: simd_float4x4
    /// 采集时刻（秒）。
    public let timestamp: TimeInterval
    /// 内参（fx, fy, cx, cy）与图像尺寸。
    public let intrinsics: simd_float3x3
    public let imageResolution: CGSize
}

/// ARKit 采集会话。
///
/// 生命周期由外部（`ScanView` + `ScanSessionModel`）驱动，本类不感知 UI 状态 ——
/// 它只暴露「启动 / 停止 / 取帧 / IMU 采样」四个能力，与 C ABI 的分工一致。
public final class CaptureSession: NSObject {

    /// 帧回调（在 ARKit 的串行队列上调用）。调用方负责投递到内核。
    public var onFrame: ((CaptureFramePacket) -> Void)?
    /// IMU 采样回调（CoreMotion 队列）。
    public var onIMU: ((IMUSample) -> Void)?
    /// 位姿跟踪状态变化（`.limited` 时 UI 显示 §5.7 P-07）。
    public var onTrackingStateChange: ((ARCamera.TrackingState) -> Void)?
    /// 热状态变化（docs/13 §5.1；仅降并行度，**不降精度**）。
    public var onThermalStateChange: ((ProcessInfo.ThermalState) -> Void)?

    // IMU 采样类型定义在 `IMUWindow.swift` 的 `IMUSample` —— **不在此重复定义**。
    // 理由：该类型必须与 `recon_imu_sample` 逐字段对应，且 `accel` 的语义
    // （比力/含重力）是正确性关键；两处定义必然漂移（MEMORY.md 铁律）。

    private let session = ARSession()
    private let motion = CMMotionManager()
    /// 独立串行队列：相机帧回调不得占用主线程（docs/11 §5 线程模型）。
    private let sessionQueue = DispatchQueue(label: "recon.capture.arkit")
    private let motionQueue = OperationQueue()
    /// 采样率（docs/09 §3.2 线框图示例为陀螺 200 Hz / 加速度 100 Hz）。
    private let gyroInterval: TimeInterval = 1.0 / 200.0

    public override init() {
        super.init()
        session.delegate = self
        sessionQueue.setSpecific(key: Self.queueKey, value: ())
    }

    private static let queueKey = DispatchSpecificKey<Void>()

    // =======================================================================
    // MARK: - 会话生命周期
    // =======================================================================

    /// 启动采集。调用方须**已获得相机权限**（docs/11 §1.3 时序）。
    ///
    /// - Parameter enableDepth: 是否请求 `sceneDepth`。仅当探测到支持时传 `true`。
    /// - Note: 使用 `.gravity`（不用 `.gravityAndHeading`）—— 02 §5.5 的世界系
    ///   约定为重力对齐，重定向会让重建结果出现非预期旋转。
    public func start(enableDepth: Bool, videoFormat: ARWorldTrackingConfiguration.VideoFormat? = nil) {
        let config = ARWorldTrackingConfiguration()
        config.worldAlignment = .gravity
        config.planeDetection = []                       // 本产品不做平面检测
        if let videoFormat {
            config.videoFormat = videoFormat
        }
        if enableDepth, ARWorldTrackingConfiguration.supportsFrameSemantics(.sceneDepth) {
            config.frameSemantics.insert(.sceneDepth)
        }
        session.run(config, options: [.resetTracking, .removeExistingAnchors])
        startMotion()
    }

    /// 暂停采集（对应状态机 `Capturing` 的 `paused` 变体，T-14 切后台）。
    ///
    /// 刻意**不** `session.pause()` —— 暂停 ARKit 会话会导致重定位；改为只停止
    /// 向内核投递帧与 IMU。这保证回前台后可无缝继续，与 §4.2 T-14 的语义一致。
    public func setPaused(_ paused: Bool) {
        isPaused = paused
    }

    /// 停止采集并释放资源。
    public func stop() {
        session.pause()
        stopMotion()
    }

    private var isPaused = false
    /// 上一次观察到的热状态（用于只在变化时回调）。
    fileprivate var lastThermal: ProcessInfo.ThermalState = .nominal

    // =======================================================================
    // MARK: - IMU（CoreMotion）
    // =======================================================================

    /// 启动 deviceMotion 与原始陀螺仪。
    ///
    /// docs/11 §1.2：使用 `CMDeviceMotion` / `CMGyro` **不触发授权弹窗**。
    /// 因此这里不做任何权限检查 —— 这不是遗漏。
    ///
    /// ## ⚠️ `accel` 语义修正（docs/14 §5.1.3 缺口 H，2026-10-31 第三十四轮）
    ///
    /// 旧实现传 `dm.userAcceleration`（**CoreMotion 已去重力**），并把
    /// `dm.gravity` 塞进一个 C ABI **根本不存在**的 `gravity` 字段 —— 重力信息
    /// 在跨边界时被静默丢弃。而 C ABI 明文规定 `accel` 是**比力（含重力）**、
    /// 由核心层自行减重力（docs/02 §3.5 `a_m = R(a_W − g_W)`）。后果是核心层
    /// 「再减一次重力」：静止时本应 ≈0 的加计残差变成 ≈ −9.81 m/s²，
    /// VIO 预积分被系统性偏置。
    ///
    /// 修正：`accel = gravity + userAcceleration`（两者之和即该时刻的比力），
    /// 与 docs/11 §3.1.5 对接表「已含重力 → 核心层减重力」的口径一致。
    private func startMotion() {
        guard motion.isDeviceMotionAvailable else { return }

        // deviceMotion：带重力与姿态。`rotationRate` 与 `gravity`/`userAcceleration`
        // 均可用 —— 但注意 docs/11 §3.1.5 的 IOS-IMU-x 系列对「是否可用于预积分」
        // 存在**文档内部冲突**（见该文档 §3.1.5 与本文件的 §3.1.5 冲突登记），
        // 本文件按「deviceMotion 用于姿态初值与重力参考」实现，并把该冲突登记在
        // docs/14 §5.1.3 缺口 K，**不擅自裁决**。
        motion.deviceMotionUpdateInterval = gyroInterval
        motion.startDeviceMotionUpdates(using: .xArbitraryZVertical, to: motionQueue) { [weak self] dm, _ in
            guard let self, let dm, !self.isPaused else { return }
            // 比力 = 重力 + 用户加速度（矢量相加，单位 m/s²）。
            let gravity = SIMD3<Double>(dm.gravity.x, dm.gravity.y, dm.gravity.z)
            let userAccel = SIMD3<Double>(dm.userAcceleration.x, dm.userAcceleration.y,
                                          dm.userAcceleration.z)
            let sample = IMUSample(
                timestamp: dm.timestamp,
                gyro: SIMD3<Double>(dm.rotationRate.x, dm.rotationRate.y, dm.rotationRate.z),
                accelIncludingGravity: gravity + userAccel
            )
            self.onIMU?(sample)
        }
    }

    private func stopMotion() {
        motion.stopDeviceMotionUpdates()
    }
}

// ===========================================================================
// MARK: - ARSessionDelegate
// ===========================================================================

extension CaptureSession: ARSessionDelegate {

    public func session(_ session: ARSession, didUpdate frame: ARFrame) {
        // 暂停时不投递（T-14：状态仍是 Capturing，但暂停采帧）。
        guard !isPaused else { return }

        // 热状态变化通知（docs/13 §5.1）。
        let thermal = ProcessInfo.processInfo.thermalState
        if thermal != lastThermal {
            lastThermal = thermal
            onThermalStateChange?(thermal)
        }

        // 位姿跟踪状态（`.limited` → UI 显示 P-07）。
        let state = frame.camera.trackingState
        onTrackingStateChange?(state)

        let packet = CaptureFramePacket(
            pixelBuffer: frame.capturedImage,
            // 深度图：沿光轴 z（与 02 §5.5 F-1 一致）。
            depthMap: frame.sceneDepth?.depthMap,
            // CameraPose 约定：`R` = world→camera、`t = −R·C`。
            // ARFrame.camera.transform 即 world→camera 的 4×4，直接透传，
            // **不做任何转置或求逆**（上游约定一致，反转会静默破坏重建）。
            transform: frame.camera.transform,
            timestamp: frame.timestamp,
            intrinsics: frame.camera.intrinsics,
            imageResolution: frame.camera.imageResolution
        )
        onFrame?(packet)
    }

    public func session(_ session: ARSession, cameraDidChangeTrackingState camera: ARCamera) {
        onTrackingStateChange?(camera.trackingState)
    }
}

// ===========================================================================
// MARK: - docs/13 热与电降级（只降并行度，不降精度）
// ===========================================================================

/// 热状态对应的**允许**动作。
///
/// R-PREC-4 铁律：移动端升温时**只降并行度/分块，不降精度参数**。
/// `mesh_poisson_depth`、`mvs_*` 等精度参数在任何热状态下都**不得**被下调。
public enum ThermalPolicy {
    case normal
    case reducedParallelism    // 降线程数
    case chunked               // 启用分块
    case pauseOptionalStages   // 暂停可选阶段（后处理非必需步骤）

    /// 由热状态推导策略（docs/13 §5.1 的档位划分）。
    public static func policy(for state: ProcessInfo.ThermalState) -> ThermalPolicy {
        switch state {
        case .nominal:  return .normal
        case .fair:     return .reducedParallelism
        case .serious:  return .chunked
        case .critical: return .pauseOptionalStages
        @unknown default: return .normal
        }
    }

    /// 该策略下**不得**触碰的参数前缀（黑名单，供闸门与代码审查引用）。
    public static let precisionParamsNeverTouched = [
        "mesh_poisson_depth", "mesh_poisson_degree",
        "mvs_", "rgbd_",
    ]
}
