// SPDX-License-Identifier: BSD-3-Clause
//
// ReconCore.swift — iOS 侧 core C ABI 门面。
//
// 契约：
//   · core/include/recon/recon_c_api.h —— 唯一跨语言边界（docs/00 §7.1）。
//   · docs/11_平台权限与集成.md §3.7 —— 平台 → 核心层的字段对接契约。
//
// 本文件是 Android 侧 NativeBridge.kt 的 iOS 对等物。两端**必须**保持功能对等
// （铁律：双端同等对待，见 docs/14 §5.1 平台适配层）。功能对等由
// tools/check_platform_parity.py 静态校验。
//
// 设计约束：
//   1. **只经 C ABI**：不 import 任何 C++ 头（Swift 无法直接消费 C++）。
//   2. **不阻塞主线程**：recon_run_reconstruction 同步阻塞，一律在后台队列调用。
//   3. **句柄不可并发**：除 recon_cancel 外，同一 context 不跨线程并发调用。

import Foundation
import CoreVideo
import simd

/// core 的 `recon_status` 取值。
///
/// 事实来源：`core/include/recon/recon_c_api.h` 的 `typedef enum recon_status`。
/// 此处与头文件**逐值一一对应**（共 47 项），由 `tools/check_ios_contract.py`
/// 的 G11 判据静态校验（键集合必须相等），负向注入见
/// `tools/verify_ios_contract_negative.py --case enum-drift`。
///
/// 为什么不引用 C 枚举：Swift 只能 import C 的**整数常量**，无法遍历 C 枚举
/// 成员，故仍以 Swift 枚举承载；一致性靠闸门而非语言特性保证。
/// **改本表必须同步头文件（或反之），CI 会拒收单边改动。**
enum ReconStatus: Int32 {
    case ok = 0

    // 通用
    case invalidArgument = 1
    case outOfMemory = 2
    case notImplemented = 3
    case internalError = 4
    case cancelled = 5
    case io = 6
    case unsupportedPlatform = 7
    case versionMismatch = 8
    case noCapability = 9

    // 采集
    case captureCameraUnavailable = 100
    case capturePermissionDenied = 101
    case captureFrameDropped = 102
    case captureStalled = 103
    case captureInterrupted = 104

    // 同步
    case syncTimeNonmonotonic = 200
    case syncClockDrift = 201
    case syncImuGap = 202
    case syncCalibrationMissing = 203

    // VIO
    case vioTrackingLost = 300
    case vioNotInitialized = 301
    case vioScaleUnobservable = 302

    // SfM
    case sfmInsufficientMatches = 400
    case sfmInitializationFailed = 401
    case sfmBundleAdjustmentFailed = 402
    case sfmInsufficientParallax = 403

    // MVS
    case mvsNoDepth = 500
    case mvsFilterExhausted = 501
    case mvsGpuFailure = 502

    // 深度 / RGBD
    case rgbdNoDepth = 600
    case rgbdDepthMisaligned = 601
    case rgbdLowConfidence = 602

    // 网格
    case meshPoissonFailed = 700
    case meshOutOfMemory = 701
    case meshNonManifoldLimit = 702

    // 后处理
    case postHoleFillFailed = 800
    case postUvFailed = 801
    case postTextureFailed = 802

    // 标定
    case calibReferenceNotFound = 900
    case calibDegenerate = 901
    case calibNoReference = 902
    case calibInconsistent = 903

    // 导出
    case exportWriteFailed = 1000
    case exportValidationFailed = 1001
    case exportUnsupportedFormat = 1002

    // 内存
    case memoryLimitReached = 1100
    case memoryBlockTooLarge = 1101
}

extension ReconStatus {
    /// 该状态是否可视为「成功」（目前仅 `ok`）。
    var isOK: Bool { self == .ok }

    /// 用户可见的分类归属（用于 UI 分组，不影响错误码数值）。
    var category: String {
        switch rawValue {
        case 0: return "通用"
        case 1...9: return "通用"
        case 100...104: return "采集"
        case 200...203: return "同步"
        case 300...302: return "VIO"
        case 400...403: return "SfM"
        case 500...502: return "MVS"
        case 600...602: return "深度"
        case 700...702: return "网格"
        case 800...802: return "后处理"
        case 900...903: return "标定"
        case 1000...1002: return "导出"
        case 1100...1101: return "内存"
        default: return "未知"
        }
    }
}

/// 管线分支（`recon_pipeline_branch`）。
enum ReconBranch: Int32 {
    case rgbd = 0
    case mono = 1
}

/// 设备能力（对应 `recon_capability`）。
struct ReconCapability {
    var branch: ReconBranch
    var depthTier: Int32
    var depthSource: Int32
    var hasCamera: Bool
    var hasImu: Bool
    var hasLidar: Bool
    var hasTof: Bool
    var supportsGpu: Bool
    var maxImageWidth: Int32
    var maxImageHeight: Int32
    var maxFps: Int32
    var imuMaxRateHz: Int32
    var availableMemoryBytes: UInt64
    var totalMemoryBytes: UInt64
}

/// 网格信息（对应 `recon_mesh_info`）。
struct ReconMeshInfo {
    var vertexCount: UInt64
    var triangleCount: UInt64
    var textureCount: UInt64
    var nonManifoldEdges: UInt32
    var boundaryLoops: UInt32
    var holeCount: UInt32
    var holeAreaRatio: Double
    var bboxMin: simd_double3
    var bboxMax: simd_double3
    var scaleFactor: Double
    var meanEdgeLengthM: Double
    var poissonDepth: UInt32
    var degraded: Bool
}

/// 采集统计（对应 `recon_scan_stats`）。
///
/// 与 Android 侧 `NativeBridge.getScanStats` 返回的 DoubleArray 布局**逐项对应**
/// （见 native-lib.cpp 的 `kStatsCount` 注释），字段顺序不得单边调整。
struct ReconScanStats {
    var framesCaptured: UInt64
    var framesAccepted: UInt64
    var keyframes: UInt64
    var imuSamples: UInt64
    var durationS: Double
    /// 方位覆盖度 [0,1]（§3.3「覆盖方位」的真实来源，而非估算）。
    var azimuthCoverage: Double
    var elevationCoverage: Double
    var anomalyBlur: UInt32
    var anomalyFastMotion: UInt32
    var anomalyLowOverlap: UInt32
    var anomalySpecular: UInt32
    var anomalyLowTexture: UInt32
    var anomalyBlindSpot: UInt32
}

/// 尺度标定结果（对应 `recon_scale_result`）。
struct ReconScaleResult {
    var ok: Bool
    var scale: Double
    /// 相对不确定度 σ_s/s（用于 §3.9 置信度三档判定：≤0.02 高 / ≤0.05 中 / 其余低）。
    var relativeSigma: Double
    var referenceType: Int32
    var referenceWidthMM: Double
    var referenceHeightMM: Double
    var measuredWidthMM: Double
    var measuredHeightMM: Double
}

/// 进度回调协议。实现方需自行切回主线程再更新 UI。
protocol ReconProgressDelegate: AnyObject {
    func onProgress(stage: Int32, progress: Double, message: String?)
}

/// core C ABI 的 Swift 门面。与 Android 侧 `NativeBridge` 一一对等。
final class ReconCore {

    /// 与 core 的 `RECON_ABI_VERSION` 对应。
    static let expectedAbiVersion: UInt32 = 1

    private(set) var handle: UnsafeMutableRawPointer?

    // MARK: - 生命周期

    init?(sessionDir: String, preset: Int32 = 1, maxThreads: UInt32 = 0) {
        var cfg = recon_config()
        cfg.abi_version = ReconCore.expectedAbiVersion
        cfg.forced_branch = recon_pipeline_branch(rawValue: 0xFFFF_FFFF)  // 自动探测
        cfg.preset = preset
        cfg.max_threads = maxThreads
        cfg.enable_rgbd = 1
        cfg.enable_mvs = 1
        cfg.enable_texture = 1

        let created: UnsafeMutableRawPointer? = sessionDir.withCString { cstr in
            cfg.session_dir = cstr
            var ctx: UnsafeMutableRawPointer?
            let st = recon_create(&cfg, &ctx)
            return st == ReconStatus.ok.rawValue ? ctx : nil
        }
        guard let context = created else { return nil }
        self.handle = context

        // ABI 版本自检（docs/01 §3）。
        let coreAbi = recon_abi_version()
        if coreAbi != ReconCore.expectedAbiVersion {
            NSLog("recon: ABI 版本不匹配 App=\(ReconCore.expectedAbiVersion) core=\(coreAbi)")
        }
    }

    deinit {
        if let h = handle { recon_destroy(h) }
    }

    // MARK: - 能力探测与参数

    func probeCapability() -> ReconCapability? {
        guard let h = handle else { return nil }
        var cap = recon_capability()
        guard recon_probe_capability(h, &cap) == ReconStatus.ok.rawValue else { return nil }
        return ReconCapability(
            branch: ReconBranch(rawValue: cap.branch) ?? .mono,
            depthTier: cap.depth_tier,
            depthSource: cap.depth_source,
            hasCamera: cap.has_camera != 0,
            hasImu: cap.has_imu != 0,
            hasLidar: cap.has_lidar != 0,
            hasTof: cap.has_tof != 0,
            supportsGpu: cap.supports_gpu != 0,
            maxImageWidth: cap.max_image_width,
            maxImageHeight: cap.max_image_height,
            maxFps: cap.max_fps,
            imuMaxRateHz: cap.imu_max_rate_hz,
            availableMemoryBytes: cap.available_memory_bytes,
            totalMemoryBytes: cap.total_memory_bytes
        )
    }

    @discardableResult
    func setParam(_ key: String, int valueInt: Int32, double valueDouble: Double = 0) -> ReconStatus {
        guard let h = handle else { return .invalidArgument }
        let st = key.withCString { recon_set_param(h, $0, valueInt, valueDouble) }
        return ReconStatus(rawValue: st) ?? .internalError
    }

    /// 读取参数当前值。`nil` 表示句柄缺失或该键不存在（**不返回 0 冒充**）。
    ///
    /// `recon_get_param` 同时写出 int 与 double 两份值；本项目参数体系里
    /// 整型参数用前者、浮点参数用后者，此处两份都带出，交由调用方按注册表
    /// 的值类型取舍。**不 assert 某一份为 0** —— 那会掩盖值类型不匹配。
    func getParam(_ key: String) -> (int: Int32, double: Double)? {
        guard let h = handle else { return nil }
        var vInt: Int32 = 0
        var vDouble: Double = 0
        let st = key.withCString { recon_get_param(h, $0, &vInt, &vDouble) }
        guard st == ReconStatus.ok.rawValue else { return nil }
        return (vInt, vDouble)
    }

    /// 应用预设：0=快速 1=标准 2=高精度。
    @discardableResult
    func applyPreset(_ preset: Int32) -> ReconStatus {
        guard let h = handle else { return .invalidArgument }
        return ReconStatus(rawValue: recon_apply_preset(h, preset)) ?? .internalError
    }

    /// 导出会话参数摘要 JSON（审计 / 复现用）。
    ///
    /// 返回的 `char*` 由 core 分配，**必须**用 `recon_free_string` 释放 ——
    /// 故这里在同一函数内复制成 Swift `String` 后立刻释放，不把所有权外泄
    /// （Swift 侧若持有裸指针，几乎必然漏释放）。
    func exportParams() -> String? {
        guard let h = handle else { return nil }
        var out: UnsafeMutablePointer<CChar>?
        let st = recon_export_params(h, &out)
        guard st == ReconStatus.ok.rawValue, let p = out else { return nil }
        defer { recon_free_string(p) }
        return String(cString: p)
    }

    // MARK: - 采集

    @discardableResult
    func startSession(_ sessionId: String) -> ReconStatus {
        guard let h = handle else { return .invalidArgument }
        let st = sessionId.withCString { recon_start_session(h, $0) }
        return ReconStatus(rawValue: st) ?? .internalError
    }

    /// 推入一帧。所有缓冲在本调用期间有效（docs/01：核心不持有指针）。
    ///
    /// - Parameters:
    ///   - rgb: BGRA8 像素（可空）。
    ///   - depthMm: uint16 毫米（可空）。
    ///   - timestampNs: 与 IMU/位姿同轴（单调时钟）。
    @discardableResult
    func pushFrame(
        frameId: UInt64,
        timestampNs: UInt64,
        rgb: UnsafePointer<UInt8>?, rgbWidth: UInt32, rgbHeight: UInt32, rgbStride: UInt32,
        depthMm: UnsafePointer<UInt16>?, depthWidth: UInt32, depthHeight: UInt32,
        depthConfidence: UnsafePointer<UInt8>?,
        depthSource: Int32,
        intrinsics: recon_camera_intrinsics,
        pose: recon_pose?,
        imu: UnsafePointer<recon_imu_sample>?, imuCount: UInt32,
        exposureUs: Double, iso: Double
    ) -> ReconStatus {
        guard let h = handle else { return .invalidArgument }

        var intr = intrinsics
        var pkt = recon_frame_packet()
        pkt.frame_id = frameId
        pkt.timestamp_ns = timestampNs
        pkt.rgb_data = rgb
        pkt.rgb_width = rgbWidth
        pkt.rgb_height = rgbHeight
        pkt.rgb_stride = rgbStride
        pkt.depth_data = depthMm.map { UnsafeRawPointer($0).assumingMemoryBound(to: UInt8.self) }
        pkt.depth_width = depthWidth
        pkt.depth_height = depthHeight
        pkt.depth_stride = depthWidth * 2  // uint16
        pkt.depth_confidence = depthConfidence
        pkt.depth_source = recon_depth_source(rawValue: depthSource) ?? .init(0)
        pkt.has_depth = (depthMm != nil) ? 1 : 0
        pkt.intrinsics = withUnsafePointer(to: &intr) { $0 }
        pkt.imu = imu
        pkt.imu_count = imuCount
        pkt.exposure_us = exposureUs
        pkt.iso = iso

        if var p = pose {
            pkt.pose = withUnsafePointer(to: &p) { $0 }
            return ReconStatus(rawValue: recon_push_frame(h, &pkt)) ?? .internalError
        }
        return ReconStatus(rawValue: recon_push_frame(h, &pkt)) ?? .internalError
    }

    @discardableResult
    func finishCapture() -> ReconStatus {
        guard let h = handle else { return .invalidArgument }
        return ReconStatus(rawValue: recon_finish_capture(h)) ?? .internalError
    }

    /// 读取采集统计。`nil` 表示句柄缺失或调用失败（**不返回全 0 结构体冒充**）。
    func scanStats() -> ReconScanStats? {
        guard let h = handle else { return nil }
        var s = recon_scan_stats()
        guard recon_get_scan_stats(h, &s) == ReconStatus.ok.rawValue else { return nil }
        return ReconScanStats(
            framesCaptured: s.frames_captured,
            framesAccepted: s.frames_accepted,
            keyframes: s.keyframes,
            imuSamples: s.imu_samples,
            durationS: s.duration_s,
            azimuthCoverage: s.azimuth_coverage,
            elevationCoverage: s.elevation_coverage,
            anomalyBlur: s.anomaly_blur,
            anomalyFastMotion: s.anomaly_fast_motion,
            anomalyLowOverlap: s.anomaly_low_overlap,
            anomalySpecular: s.anomaly_specular,
            anomalyLowTexture: s.anomaly_low_texture,
            anomalyBlindSpot: s.anomaly_blind_spot
        )
    }

    // MARK: - 尺度标定

    /// 用参照物图像解算尺度因子。
    ///
    /// - Parameters:
    ///   - detectedCorners: 8 个角点坐标 `(x0,y0,...,x3,y3)`，顺序为
    ///     左上、右上、右下、左下（`recon_c_api.h` 明示）。
    ///   - referenceType: 见 `recon_scale_result.reference_type`。
    ///
    /// 与 Android 侧 `NativeBridge.calibrateScale` 对等。
    func calibrateScale(
        referenceFrame: recon_frame_packet,
        detectedCorners: [Double],
        referenceType: Int32,
        refWidthMM: Double,
        refHeightMM: Double
    ) -> ReconScaleResult? {
        guard let h = handle, detectedCorners.count >= 8 else { return nil }
        var out = recon_scale_result()
        let st = detectedCorners.withUnsafeBufferPointer { buf -> Int32 in
            var pkt = referenceFrame
            return recon_calibrate_scale(h, &pkt, buf.baseAddress,
                                         referenceType, refWidthMM, refHeightMM, &out)
        }
        guard st == ReconStatus.ok.rawValue else { return nil }
        return ReconScaleResult(
            ok: out.ok != 0,
            scale: out.scale,
            relativeSigma: out.relative_sigma,
            referenceType: out.reference_type,
            referenceWidthMM: out.reference_width_mm,
            referenceHeightMM: out.reference_height_mm,
            measuredWidthMM: out.measured_width_mm,
            measuredHeightMM: out.measured_height_mm
        )
    }

    /// 手动设定尺度因子。与 Android 侧 `NativeBridge.setManualScale` 对等。
    func setManualScale(_ scale: Double) -> ReconScaleResult? {
        guard let h = handle else { return nil }
        var out = recon_scale_result()
        let st = recon_set_manual_scale(h, scale, &out)
        guard st == ReconStatus.ok.rawValue else { return nil }
        return ReconScaleResult(
            ok: out.ok != 0,
            scale: out.scale,
            relativeSigma: out.relative_sigma,
            referenceType: out.reference_type,
            referenceWidthMM: out.reference_width_mm,
            referenceHeightMM: out.reference_height_mm,
            measuredWidthMM: out.measured_width_mm,
            measuredHeightMM: out.measured_height_mm
        )
    }

    // MARK: - 重建

    /// 执行重建。**同步阻塞** —— 必须在后台队列调用。
    func runReconstruction(delegate: ReconProgressDelegate?) -> ReconStatus {
        guard let h = handle else { return .invalidArgument }

        var box: ProgressBox?
        var cb: recon_progress_cb?
        if let d = delegate {
            box = ProgressBox(delegate: d)
            cb = { userData, stage, progress, message in
                guard let userData else { return }
                let b = Unmanaged<ProgressBox>.fromOpaque(userData).takeUnretainedValue()
                let msg = message.map { String(cString: $0) }
                b.delegate?.onProgress(stage: stage, progress: progress, message: msg)
            }
        }

        let userPtr = box.map { UnsafeMutableRawPointer(Unmanaged.passUnretained($0).toOpaque()) }
        let st = recon_run_reconstruction(h, cb, nil, nil, userPtr)
        return ReconStatus(rawValue: st) ?? .internalError
    }

    func meshInfo() -> ReconMeshInfo? {
        guard let h = handle else { return nil }
        var info = recon_mesh_info()
        guard recon_get_mesh_info(h, &info) == ReconStatus.ok.rawValue else { return nil }
        return ReconMeshInfo(
            vertexCount: info.vertex_count,
            triangleCount: info.triangle_count,
            textureCount: info.texture_count,
            nonManifoldEdges: info.non_manifold_edges,
            boundaryLoops: info.boundary_loops,
            holeCount: info.hole_count,
            holeAreaRatio: info.hole_area_ratio,
            bboxMin: simd_double3(info.bbox_min.0, info.bbox_min.1, info.bbox_min.2),
            bboxMax: simd_double3(info.bbox_max.0, info.bbox_max.1, info.bbox_max.2),
            scaleFactor: info.scale_factor,
            meanEdgeLengthM: info.mean_edge_length_m,
            poissonDepth: info.poisson_depth,
            degraded: info.degraded != 0
        )
    }

    // MARK: - 导出与控制

    @discardableResult
    func export(format: Int32, outPath: String, applyScale: Bool, embedTexture: Bool) -> ReconStatus {
        guard let h = handle else { return .invalidArgument }
        var opt = recon_export_options()
        opt.format = recon_export_format(rawValue: format) ?? .init(0)
        opt.apply_scale = applyScale ? 1 : 0
        opt.embed_texture = embedTexture ? 1 : 0
        let st = outPath.withCString { cstr in
            opt.out_path = cstr
            return recon_export(h, &opt)
        }
        return ReconStatus(rawValue: st) ?? .internalError
    }

    /// 线程安全，可在任意线程调用。
    @discardableResult
    func cancel() -> ReconStatus {
        guard let h = handle else { return .invalidArgument }
        return ReconStatus(rawValue: recon_cancel(h)) ?? .internalError
    }

    var isCancelled: Bool {
        guard let h = handle else { return false }
        return recon_is_cancelled(h) != 0
    }
}

/// 持有 delegate 的引用盒，供 C 回调以 userData 传递。
private final class ProgressBox {
    weak var delegate: ReconProgressDelegate?
    init(delegate: ReconProgressDelegate) { self.delegate = delegate }
}
