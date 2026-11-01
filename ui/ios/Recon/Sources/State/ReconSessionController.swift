//
//  ReconSessionController.swift
//  Recon — 内核编排层（C ABI → UI 的唯一接线点，iOS）
//
//  契约：
//    · core/include/recon/recon_c_api.h          —— 唯一跨语言边界（00 §7.1）
//    · docs/09_UI规格.md §5.5（重建文案 R-*）、§5.7（权限文案 P-*）、
//      §3.6.1（14 段进度）、§3.7（结果页字段）
//    · docs/11_平台权限与集成.md §3.7（平台 → 核心层字段对接）
//    · docs/13_内存与降级.md（只降并行度，不降精度 — R-PREC-4）
//
//  ── 这一层是什么、不是什么 ────────────────────────────────────────────────
//
//  这一层是**唯一**把下面三者接起来的地方：
//      ① `ReconCore`（platform/ios/Recon/ReconCore.swift，C ABI 门面）
//      ② `CaptureSession`（Sources/Platform/CameraCapture.swift，ARKit 采集）
//      ③ `ScanSessionModel`（Sources/State/ScanSessionModel.swift，UI 状态机）
//  UI 的 `View` 只读本对象发布的 `@Published` 状态，**不直接调 C ABI**
//  （这样 UI 与内核解耦，也便于注入假内核做 UI 测试）。
//
//  它**不做**的事（各有理由）：
//    · **不重写状态机**：迁移规则只有一份（platform/common/src/capture_state.cpp），
//      本层把事件转给 `ScanSessionModel`，只按新状态驱动副作用。
//    · **不重写 14 段表**：段名唯一来源是 `RebuildStage`（Screens/RebuildView.swift）；
//      段号映射唯一来源是 `ReconStageMap`。本层只做「C ABI 阶段 → 段号」的查询。
//    · **不做像素格式转换的魔法**：CVPixelBuffer → BGRA8 的转换放在
//      `FrameConverter`，因为它牵涉 vImage/CoreImage，是独立可测单元。
//    · **不伪造进度**：`recon_progress_cb` 给什么就显示什么；段号在组边界跳变是
//      诚实的（见 ReconStageMap 文件头）。
//
//  ── 线程模型（docs/11 §5）─────────────────────────────────────────────────
//
//  · 本类 `@MainActor` —— 所有 `@Published` 只在主线程写，避免 SwiftUI 竞态。
//  · `recon_run_reconstruction` **同步阻塞**，必须在后台队列调用；回调在
//    内核线程触发，本层用 `Task { @MainActor in ... }` 弹回主线程后再写状态。
//  · `recon_push_frame` 在 ARKit 帧回调线程调用；本层不做跨线程 `@Published`
//    写入 —— 帧计数用原子累加器，主线程定时读取。
//

import Foundation
import SwiftUI
import CoreVideo
import simd

// ===========================================================================
// MARK: - 面向 UI 的进度/结果模型
// ===========================================================================

/// 重建过程中的进度快照（`recon_progress_cb` → UI）。
///
/// `uiSegment` 可能为 `nil`：当 C ABI 上报的阶段不在 §3.6.1 的 14 段内
/// （`scale` / `export`）或为终态时。UI 应回退到「不显示段号，只显示进度条」，
/// **不得**为了让数字好看而clamp 到某个段（那是伪造）。
public struct ReconProgress: Equatable {
    /// 原始 C ABI 阶段号。
    public let abiStage: Int32
    /// 对应的 §3.6.1 段号（1..14）；`nil` 表示该阶段无对应段。
    public let uiSegment: Int?
    /// 段名（经 `RebuildStage` 单一来源取值）；无段时为 `nil`。
    public let segmentName: String?
    /// 全局进度 [0,1]（直接来自 C ABI，未做任何平滑）。
    public let fraction: Double
    /// 内核附带的进度消息（可为空）。
    public let message: String?
}

/// 重建结果摘要（`recon_get_mesh_info` → U-07 结果页）。
public struct ReconResultSummary: Equatable {
    public let vertexCount: UInt64
    public let triangleCount: UInt64
    public let textureCount: UInt64
    public let nonManifoldEdges: UInt32
    public let boundaryLoops: UInt32
    public let holeCount: UInt32
    public let holeAreaRatio: Double
    public let scaleFactor: Double
    public let meanEdgeLengthM: Double
    public let degraded: Bool
    /// 包围盒（米）。U-07 的「长 / 宽 / 高」与 `volumeCM3` 都从这里派生 ——
    /// 不另存一份尺寸，避免「同一逻辑写两遍」。
    public let bboxMin: SIMD3<Double>
    public let bboxMax: SIMD3<Double>

    init(_ m: ReconMeshInfo) {
        vertexCount = m.vertexCount
        triangleCount = m.triangleCount
        textureCount = m.textureCount
        nonManifoldEdges = m.nonManifoldEdges
        boundaryLoops = m.boundaryLoops
        holeCount = m.holeCount
        holeAreaRatio = m.holeAreaRatio
        scaleFactor = m.scaleFactor
        meanEdgeLengthM = m.meanEdgeLengthM
        degraded = m.degraded
        bboxMin = m.bboxMin
        bboxMax = m.bboxMax
    }

    /// 三轴跨度（毫米），顺序为 (x, y, z)。
    ///
    /// ⚠️ **不做「长宽高」的语义重排**：把 x/y/z 映射成「长/宽/高」需要知道
    /// 物体的朝向约定（哪个轴是「上」）。该约定属内核坐标系定义，
    /// 本层无权替它决定 —— 故这里只给三轴跨度，由 UI 按「降序 = 长/宽/高」
    /// 呈现（那是纯呈现约定，不改变数据）。
    public var extentsMM: SIMD3<Double> {
        (bboxMax - bboxMin) * 1000.0
    }

    /// 三轴跨度降序排列。U-07 的「长 / 宽 / 高」用此顺序。
    public var sortedExtentsMM: [Double] {
        let e = extentsMM
        return [e.x, e.y, e.z].sorted(by: >)
    }

    /// 轴对齐包围盒体积（cm³）。
    public var volumeCM3: Double {
        let e = extentsMM     // mm
        return (e.x * e.y * e.z) / 1000.0   // mm³ → cm³（÷1000）
    }
}

/// 重建结局（成功 / 失败 / 取消），供 UI 决定跳转与文案。
public enum ReconOutcome: Equatable {
    case running
    case succeeded
    /// 失败：携带已解析的 `ReconStatus`（**必定**是真实枚举，见 §5.5 R-* 映射）
    /// 与展示文案键。
    case failed(status: ReconStatus, messageKey: L10nKey?)
    case cancelled
}

/// 一个已完成/已中断的会话记录（U-01 首页列表的数据项）。
///
/// **不伪造**：本结构仅在**真实发生了**采集或重建时由控制器登记，
/// 不做任何目录扫描推测（那是本地持久化层的职责，尚未实现）。
/// 因此 App 冷启动时列表为空 —— 这是诚实的空状态，不是缺陷。
public struct ReconSessionRecord: Identifiable, Equatable {
    public let id: UUID
    /// 会话目录名（作为展示名）。
    public let name: String
    /// 是否有可导出的重建结果。
    public let hasResult: Bool
    /// 上次是否被中断（进程被杀 / 切后台未完成）。
    public let interrupted: Bool
    /// 登记时刻。
    public let createdAt: Date
    /// 结果摘要（有结果时非 nil）。
    public let summary: ReconResultSummary?

    /// 列表副标题：`1.2 MB · GLB · 标准档`。无结果时显示帧数与档位。
    ///
    /// 体积为**估算**（三角面数 × 单面字节 + 纹理），与 `ExportView` 的
    /// 预估口径一致；非实测文件大小 —— UI 不得把估算值说成实测值。
    public var detailText: String {
        if let s = summary {
            let bytes = ReconSessionRecord.estimateBytes(triangles: s.triangleCount,
                                                         textures: s.textureCount)
            return "\(ReconSessionRecord.byteText(bytes)) · GLB · \(presetLabel)"
        }
        return "\(L10n.t(.homeNotRebuilt)) · \(presetLabel)"
    }

    /// 预设档位（登记时冻结，不随后续切换而变）。
    public let presetLabel: String

    /// 时间戳 `10-30 14:20`。
    public var timestampText: String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "MM-dd HH:mm"
        return f.string(from: createdAt)
    }

    /// 粗略体积估算。**同一口径**只此一处（ExportView 复用本函数，
    /// 避免两处各算一遍而漂移）。
    public static func estimateBytes(triangles: UInt64, textures: UInt64) -> Int64 {
        // 每个三角面：3 索引（4B）+ 顶点数据摊薄；经验系数取 24 B/面。
        let mesh = Int64(triangles) * 24
        // 每张贴图按 2048² 未压缩 PNG 的 1/8 估（PNG 有压缩）。
        let tex = Int64(textures) * 1_048_576
        return max(mesh + tex, 32_768)
    }

    static func byteText(_ bytes: Int64) -> String {
        let gb = Double(bytes) / 1_073_741_824
        if gb >= 1 { return String(format: "%.1f GB", gb) }
        let mb = Double(bytes) / 1_048_576
        if mb >= 1 { return String(format: "%.0f MB", mb) }
        return String(format: "%.0f KB", Double(bytes) / 1024)
    }
}

// ===========================================================================
// MARK: - 错误码 → §5.5 R-* 文案（唯一映射表）
// ===========================================================================
//
//  为什么单列成一张表：docs/09 §5.5 的「错误码」列写的是**规格码名**
//  （`RECON_ERR_MESH_EMPTY_INPUT` 等），而 `recon_c_api.h` 里的**实现代码**
//  是另一套（`RECON_ERR_MESH_POISSON_FAILED=700` 等）。二者**不是同一套**
//  （docs/01 §4 有 168 项规格、实现只有 47 项）。
//
//  因此本表以**实现代码为键**（那是运行期真实会收到的值），映射到 §5.5 中
//  语义最贴近的文案键。**凡是实现代码里没有的规格码，一律不许在此出现**
//  —— 那会让 UI 引用一个永远收不到的分支（假分支）。
//  未命中的实现码 → 回退到「通用失败」文案（见 `messageKey(for:)` 末尾），
//  **不静默吞掉**：调用方仍拿到真实 `ReconStatus` 供日志与 bug 报告使用。

public enum ReconStatusPresentation {

    /// `ReconStatus` → §5.5 文案键。`nil` 表示「不显示专用文案，用通用失败」。
    ///
    /// 覆盖范围 = `core/include/recon/recon_c_api.h` 全 47 项中**用户可见**的那些。
    /// 由 `tools/check_ios_contract.py` 的 G11 保证 `ReconStatus` 与头文件逐值对等，
    /// 故此处不会出现「引用了不存在的错误码」。
    public static func messageKey(for status: ReconStatus) -> L10nKey? {
        switch status {
        // --- 通用 ---
        case .ok:                       return nil
        case .cancelled:                return .rebuildCancelled        // R-13
        case .outOfMemory:              return .rebuildPoissonOom       // R-03
        case .invalidArgument,
             .notImplemented,
             .internalError,
             .io,
             .unsupportedPlatform,
             .versionMismatch,
             .noCapability:             return nil                    // 归入通用失败

        // --- 采集 ---
        //   采集期的错误由 ScanView 按 §3.3 处理，不在重建页出现；
        //   若真在重建期收到，归入通用失败。
        case .captureCameraUnavailable,
             .capturePermissionDenied,
             .captureFrameDropped,
             .captureStalled,
             .captureInterrupted:       return nil

        // --- 同步 / VIO / SfM / MVS / RGBD ---
        //   这些属于前置数据质量问题，UI 用通用失败 + 真实错误码呈现，
        //   §5.5 未为其单列 R-* 文案。**不编造**映射。
        case .syncTimeNonmonotonic, .syncClockDrift, .syncImuGap,
             .syncCalibrationMissing,
             .vioTrackingLost, .vioNotInitialized, .vioScaleUnobservable,
             .sfmInsufficientMatches, .sfmInitializationFailed,
             .sfmBundleAdjustmentFailed, .sfmInsufficientParallax,
             .mvsNoDepth, .mvsFilterExhausted, .mvsGpuFailure,
             .rgbdNoDepth, .rgbdDepthMisaligned, .rgbdLowConfidence:
            return nil

        // --- 网格 ---
        case .meshPoissonFailed:        return .rebuildPoissonFailed    // R-04
        case .meshOutOfMemory:          return .rebuildPoissonOom       // R-03
        case .meshNonManifoldLimit:     return .rebuildNonmanifoldUnfixed // R-05

        // --- 后处理 ---
        case .postHoleFillFailed:       return nil                      // 参考量，非硬闸门
        case .postUvFailed:             return .rebuildUvFailed         // R-06
        case .postTextureFailed:        return .rebuildBakeNoCoverage   // R-07

        // --- 标定 ---
        //   §5.7 权限/标定文案另有 P-*；此处归入通用失败由 U-09 自行提示。
        case .calibReferenceNotFound, .calibDegenerate,
             .calibNoReference, .calibInconsistent:
            return nil

        // --- 导出 ---
        case .exportWriteFailed:        return .exportFailed            // §5.8 E-02
        case .exportValidationFailed:   return .exportValidationFailed  // §5.8 E-03
        case .exportUnsupportedFormat:  return .exportFailed

        // --- 内存 ---
        case .memoryLimitReached:       return .rebuildPoissonOom       // R-03
        case .memoryBlockTooLarge:      return nil
        }
    }

    /// 该状态是否属于「成功」（`ok`）。
    public static func isSuccess(_ status: ReconStatus) -> Bool { status.isOK }
}

// ===========================================================================
// MARK: - 编排器
// ===========================================================================

/// 内核编排器：把 `ReconCore`（C ABI）、`CaptureSession`（采集）与
/// `ScanSessionModel`（状态机）接在一起，并向 UI 发布可观察状态。
@MainActor
public final class ReconSessionController: ObservableObject {

    // MARK: 发布的 UI 状态

    /// 采集会话状态机（UI 直接绑定它；本层只负责驱动迁移的副作用）。
    @Published public private(set) var session: ScanSessionModel
    /// 当前重建进度；未在重建时为 `nil`。
    @Published public private(set) var progress: ReconProgress?
    /// 重建结局。
    @Published public private(set) var outcome: ReconOutcome = .running
    /// 重建结果摘要；`nil` 表示尚未产出。
    @Published public private(set) var result: ReconResultSummary?
    /// 最近一次内核调用的状态（**不静默吞错**：即便有专用文案，原码仍在此）。
    @Published public private(set) var lastStatus: ReconStatus = .ok
    /// 设备能力（`recon_probe_capability`）。
    @Published public private(set) var capability: ReconCapability?
    /// ARKit 层能力探测结果。
    @Published public private(set) var arCapability: ARCapability?
    /// 采集统计（`recon_get_scan_stats`）。采集开始后周期刷新。
    @Published public private(set) var scanStats: ReconScanStats?

    /// U-01 首页的会话列表（本次运行期内真实发生过的会话，倒序）。
    @Published public private(set) var records: [ReconSessionRecord] = []
    /// 当前精度预设（U-08 与 `recon_apply_preset` 联动）。
    @Published public private(set) var preset: Preset = .standard
    /// 导出完成提示（U-E-4：一行文字）。`nil` 表示无。
    @Published public private(set) var lastExportNote: String?
    /// 最近一次导出/标定的真实状态（**不静默吞错**）。
    @Published public private(set) var lastExportStatus: ReconStatus = .ok

    // MARK: 依赖

    private var core: ReconCore?
    private let capture = CaptureSession()
    private var sessionDir: String?
    /// 当前会话在 `records` 中的下标（用于重建成功后回填摘要）。
    private var currentRecordIndex: Int?
    /// 采集统计的周期刷新定时器（2 Hz；仅用于 U-03 的「覆盖方位」）。
    private var statsTimer: Timer?

    /// 后台重建队列（`recon_run_reconstruction` 阻塞，不得在主线程跑）。
    private let worker = DispatchQueue(label: "recon.reconstruction", qos: .userInitiated)

    public init(session: ScanSessionModel = ScanSessionModel()) {
        self.session = session
        wireCaptureCallbacks()
    }

    // =======================================================================
    // MARK: - 生命周期
    // =======================================================================

    /// 打开一个会话目录并创建内核上下文。
    ///
    /// - Returns: 是否成功创建。失败时 `lastStatus` 携带原因。
    @discardableResult
    public func open(sessionDir: String, preset: Int32 = 1) -> Bool {
        self.sessionDir = sessionDir
        guard let c = ReconCore(sessionDir: sessionDir, preset: preset) else {
            lastStatus = .internalError
            return false
        }
        core = c
        // 把句柄交给投递管线（否则帧回调进来时管线没有句柄可用，
        // 表现为「采集中但一帧都不投」—— 正是缺口 J 的静默形态之一）。
        framePipeline.setCore(c)
        framePipeline.reset()
        lastStatus = .ok
        capability = c.probeCapability()
        arCapability = ARCapability.probe()
        // 登记一条真实会话记录（U-01 列表的数据来源）。
        registerRecord(dir: sessionDir)
        return true
    }

    /// 关闭并释放内核上下文。
    public func close() {
        // 先解绑管线再取消 —— 否则帧回调（另一队列）可能拿到已释放的 context。
        // 顺序不可交换：`setCore(nil)` 让 handleFrame 下次进来直接 return。
        framePipeline.setCore(nil)
        framePipeline.reset()
        // 先取消（若在跑），再释放句柄 —— 否则后台线程会用到已释放的 context。
        _ = core?.cancel()
        core = nil
        rejectedFrameCount = 0
        progress = nil
        outcome = .running
        result = nil
        stopStatsTimer()
    }

    // =======================================================================
    // MARK: - 精度预设（U-08 ↔ recon_apply_preset）
    // =======================================================================

    /// 切换精度预设：写内核（`recon_apply_preset`）并记录。
    ///
    /// 内核返回非 `ok` 时**不静默**：`lastStatus` 记原码，预设不更新
    /// （避免 UI 显示「已切到高精度」而内核其实没切）。
    @discardableResult
    public func applyPreset(_ p: Preset) -> Bool {
        guard let core else {
            lastStatus = .invalidArgument
            return false
        }
        let st = core.applyPreset(p.rawIndex)
        lastStatus = st
        guard st.isOK else { return false }
        preset = p
        return true
    }

    /// 导出会话参数摘要 JSON（审计 / 复现）。`nil` 表示无句柄或调用失败。
    public func exportParamsJSON() -> String? { core?.exportParams() }

    // =======================================================================
    // MARK: - 采集统计（U-03「覆盖方位」的真实来源）
    // =======================================================================

    /// 开始周期刷新采集统计（2 Hz）。
    ///
    /// 为什么轮询而非回调：`recon_get_scan_stats` 是**拉取式**接口，
    /// C ABI 未提供统计回调。轮询频率取 2 Hz ——「覆盖方位」以 1% 粒度显示，
    /// 高于 2 Hz 的刷新对用户不可见，纯属浪费电（docs/13 热与电铁律）。
    private func startStatsTimer() {
        stopStatsTimer()
        statsTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) {
            [weak self] _ in
            Task { @MainActor in self?.refreshStats() }
        }
    }

    private func stopStatsTimer() {
        statsTimer?.invalidate()
        statsTimer = nil
    }

    /// 拉取一次采集统计并发布。
    public func refreshStats() {
        guard let core else { return }
        guard let s = core.scanStats() else { return }
        scanStats = s
        // 帧计数以内核统计为准（ARKit 回调计数可能因丢帧而偏大）。
        session.capturedFrames = Int(s.framesCaptured)
        session.coverageFraction = s.azimuthCoverage
    }

    // =======================================================================
    // MARK: - 会话记录（U-01 列表）
    // =======================================================================

    /// 登记一条新会话记录（`open` 时调用）。返回其 id。
    @discardableResult
    private func registerRecord(dir: String) -> UUID {
        let id = UUID()
        let rec = ReconSessionRecord(
            id: id,
            name: (dir as NSString).lastPathComponent,
            hasResult: false,
            interrupted: false,
            createdAt: Date(),
            summary: nil,
            presetLabel: preset.label)
        records.insert(rec, at: 0)
        currentRecordIndex = 0
        return id
    }

    /// 用重建结果回填当前会话记录。
    private func updateCurrentRecord(summary: ReconResultSummary?, interrupted: Bool) {
        guard let idx = currentRecordIndex, records.indices.contains(idx) else { return }
        let old = records[idx]
        records[idx] = ReconSessionRecord(
            id: old.id,
            name: old.name,
            hasResult: summary != nil,
            interrupted: interrupted,
            createdAt: old.createdAt,
            summary: summary,
            presetLabel: old.presetLabel)
    }

    // =======================================================================
    // MARK: - 导出（U-10 ↔ recon_export）
    // =======================================================================

    /// 导出模型。返回真实 `ReconStatus`；成功时同时发布一行完成提示（U-E-4）。
    ///
    /// 体积提示用 `estimatedBytes` **估算**（与 U-10 的预估口径同源），
    /// 不谎称是实测文件大小 —— 真实大小需读取文件系统，本层不做。
    @discardableResult
    public func exportModel(format: ExportFormat, outPath: String,
                            applyScale: Bool = true,
                            embedTexture: Bool = true) -> ReconStatus {
        guard let core else {
            lastExportStatus = .invalidArgument
            lastExportNote = nil
            return .invalidArgument
        }
        // 格式序号与 `recon_export_format` 一致。**显式映射**，不靠 rawValue 撞运气。
        let fmt: Int32
        switch format {
        case .glb:    fmt = 0
        case .objMtl: fmt = 1
        case .ply:    fmt = 2
        }
        let st = core.export(format: fmt, outPath: outPath,
                             applyScale: applyScale, embedTexture: embedTexture)
        lastExportStatus = st
        if st.isOK {
            let bytes = ReconSessionRecord.estimateBytes(
                triangles: result?.triangleCount ?? 0,
                textures: result?.textureCount ?? 0)
            lastExportNote = L10n.fill(.fmtExportedSize,
                                       ReconSessionRecord.byteText(bytes))
        } else {
            lastExportNote = nil
        }
        return st
    }

    /// 清除导出完成提示（用户离开 U-10 时调用）。
    public func clearExportNote() { lastExportNote = nil }

    // =======================================================================
    // MARK: - 尺度标定（U-09 ↔ recon_calibrate_scale / recon_set_manual_scale）
    // =======================================================================

    /// 手动设定尺度因子。
    ///
    /// U-09 的输入是**长边毫米值**；内核接口要的是**尺度因子**
    /// `scale = 真实长度 / 重建长度`。后者需知道重建长度（网格包围盒），
    /// 故此处先取 `recon_get_mesh_info` 的包围盒长边，再求比值。
    /// 若尚无网格信息（未重建），返回 `nil` —— **不猜**。
    public func applyManualScale(longEdgeMM: Double) -> ReconScaleResult? {
        guard let core, let info = core.meshInfo() else {
            lastStatus = .calibNoReference
            return nil
        }
        let span = info.bboxMax - info.bboxMin
        let longEdgeM = max(span.x, max(span.y, span.z))
        guard longEdgeM > 1e-9 else {
            lastStatus = .calibDegenerate
            return nil
        }
        let scale = (longEdgeMM / 1000.0) / longEdgeM
        guard let res = core.setManualScale(scale) else {
            lastStatus = .calibInconsistent
            return nil
        }
        lastStatus = .ok
        return res
    }

    /// 当前已重建网格的长边（毫米）。用于 U-09 预填与 U-07 尺寸行。
    public func reconstructedLongEdgeMM() -> Double? {
        guard let info = core?.meshInfo() else { return nil }
        let span = info.bboxMax - info.bboxMin
        let m = max(span.x, max(span.y, span.z))
        return m > 1e-9 ? m * 1000.0 : nil
    }

    // =======================================================================
    // MARK: - 参数读取（U-08 展开行）
    // =======================================================================

    /// 读取一个参数的整型值。`nil` 表示键不存在（**不返回 0 冒充**）。
    public func paramInt(_ key: String) -> Int32? { core?.getParam(key)?.int }

    /// 读取一个参数的浮点值。
    public func paramDouble(_ key: String) -> Double? { core?.getParam(key)?.double }

    /// 读取全部已知键的前缀分组计数（U-08 分组标题的 `(N)`）。
    ///
    /// 数据来源：`recon_export_params` 导出的 JSON（含 `params` / `adjustable` /
    /// `expert_only` 三份键清单）。这是**真实读取注册表**，不是猜键名。
    ///
    /// 为什么不在 Swift 里另抄一份可调性表：那会产生**第二事实来源**，
    /// 一旦内核注册表变动就会漂移 —— 本项目「同一逻辑写两遍必然漂移」的
    /// 铁律正是为此。因此这里只按前缀归类，键名与可见性全部来自内核。
    ///
    /// 分组归属（docs/09 §3.8.1 的 11 组）：
    /// 组 9 有两个并列前缀 `scale_` / `calib_`，其余组单前缀。
    ///
    /// ⚠️ 已知文档缺口（如实登记，不自造）：内核注册表中还有 `scan_` 前缀
    /// （docs/08 的 **G12** 扫描与关键帧类，实测 16 项），而 docs/09 参数面板
    /// 只定义了 G1–G11 —— G12 在该面板中**无归属**。此处**不臆造第 12 组**，
    /// 该前缀的参数不进入任何分组计数，缺口登记于 docs/14。
    public func groupCounts() -> [Int: Int] {
        guard let raw = exportParamsJSON(),
              let data = raw.data(using: .utf8),
              let outer = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let keys = outer["params"] as? [String: Any] else {
            return [:]
        }
        // 可见性清单：只统计用户能看到的键（常规 + 专家），
        // 与 `否` 类（GPU 线程组尺寸等，用户不可见）保持一致。
        let adjustable = Set((outer["adjustable"] as? [String]) ?? [])
        let expertOnly = Set((outer["expert_only"] as? [String]) ?? [])
        let visible = adjustable.union(expertOnly)

        var counts: [Int: Int] = [:]
        for key in keys.keys where visible.contains(key) {
            guard let g = ParamGroup.groupIndex(forKey: key) else { continue }
            counts[g, default: 0] += 1
        }
        return counts
    }

    /// 某个分组的展开行清单（真实键 + 真实当前值）。
    ///
    /// 键与值全部来自内核 `recon_export_params`；`displayName` 由内核注册表的
    /// `note`（中文备注）承载 —— C ABI 未导出 `note`，故此处**不伪造中文名**，
    /// 直接用键名呈现（键名即 docs/08 的唯一定义处，机器可读且不会漂移）。
    ///
    /// - Parameter expert: 专家模式开关。关闭时只列常规可调项。
    public func groupItems(groupId: Int, expert: Bool) -> [ParamItem] {
        guard let raw = exportParamsJSON(),
              let data = raw.data(using: .utf8),
              let outer = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let params = outer["params"] as? [String: Any] else {
            return []
        }
        let adjustable = Set((outer["adjustable"] as? [String]) ?? [])
        let expertOnly = Set((outer["expert_only"] as? [String]) ?? [])
        let visible = adjustedVisibleSet(adjustable: adjustable,
                                        expertOnly: expertOnly,
                                        expert: expert)

        return params.keys
            .filter { visible.contains($0) }
            .filter { ParamGroup.groupIndex(forKey: $0) == groupId }
            .sorted()
            .map { key in
                ParamItem(key: key,
                          displayName: key,
                          value: Self.formatValue(params[key]))
            }
    }

    /// 当前模式下可见的键集合。专家模式 = 常规 ∪ 专家。
    private func adjustedVisibleSet(adjustable: Set<String>,
                                    expertOnly: Set<String>,
                                    expert: Bool) -> Set<String> {
        expert ? adjustable.union(expertOnly) : adjustable
    }

    /// 把内核 JSON 里的数值渲染成单行文本。整数值不带小数点。
    private static func formatValue(_ v: Any?) -> String {
        if let b = v as? Bool { return b ? "true" : "false" }
        if let i = v as? Int { return String(i) }
        if let d = v as? Double {
            if d == d.rounded() && abs(d) < 1e15 { return String(Int(d)) }
            return String(format: "%g", d)
        }
        return "—"
    }

    /// 预设切换差异（U-08「预设差异提示」行）。
    ///
    /// `overridden` 取内核 `overridden` 清单的真实长度 —— 切预设后**用户覆盖
    /// 项会被保留**（R-PREC-6），所以「保留你的 N 项」恰是覆盖项数。
    /// `kept` 是本次切换未改变的值项数；由于 C ABI 不导出「上一次预设」，
    /// 无法计算真实 diff，故**返回 0 并在 UI 侧显示为「已应用」**，
    /// 不编造一个看起来精确的数字。
    ///
    /// - Returns: 覆盖项数；无句柄或解析失败返回 `nil`（UI 显示占位）。
    public func presetOverriddenCount() -> Int? {
        guard let raw = exportParamsJSON(),
              let data = raw.data(using: .utf8),
              let outer = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let overridden = outer["overridden"] as? [String] else {
            return nil
        }
        return overridden.count
    }

    // =======================================================================
    // MARK: - 采集（状态机事件 + 副作用）
    // =======================================================================

    /// 开始扫描：走状态机 `StartScan`，并按新状态启动 ARKit。
    ///
    /// 副作用分工：状态机只回答「去哪」，本层按目标状态执行（docs/09 §4.2 注释）。
    public func startScan(guided: Bool) {
        let next = session.send(.startScan)
        switch next {
        case .capabilityProbe:
            handleCapabilityProbe()
        default:
            break
        }
        _ = guided   // U-04 定点引导由 ScanView 的引导点序列承载，与内核无关
    }

    /// 能力探测完成/失败的迁移入口（由 `handleCapabilityProbe` 或 UI 调用）。
    private func handleCapabilityProbe() {
        guard let cap = arCapability else {
            session.send(.probeInternalError)
            return
        }
        if !cap.worldTrackingSupported {
            session.send(.probeCameraUnavailable)
            return
        }
        session.send(.probeComplete)
    }

    /// 权限全部授予后：启动 ARKit 会话（`enableDepth` 取决于探测结果）。
    public func onPermissionsGranted() {
        let next = session.send(.permissionsGranted)
        if next == .previewing {
            capture.start(enableDepth: arCapability?.hasDepth ?? false)
        }
    }

    /// 用户按下「开始采集」：状态机 `UserStart`，恢复帧投递。
    public func beginCapturing() {
        let next = session.send(.userStart)
        if next == .capturing {
            capture.setPaused(false)
            // 通知内核开始采集会话（sessionId 用会话目录名）。
            if let dir = sessionDir {
                _ = core?.startSession((dir as NSString).lastPathComponent)
            }
            startStatsTimer()          // 开始 2 Hz 拉取「覆盖方位」
        }
    }

    /// 暂停 / 继续采集（§3.3「暂停」按钮）。
    public func setPaused(_ paused: Bool) {
        capture.setPaused(paused)
        // 暂停期间不刷新统计（省电；恢复时再拉一次）。
        if paused { stopStatsTimer() } else { startStatsTimer() }
    }

    /// 结束采集：状态机 `FinishScan` → `Reviewing`，并停止投递。
    public func finishCapture() {
        capture.setPaused(true)
        stopStatsTimer()
        _ = session.send(.finishScan)
        _ = core?.finishCapture()
        refreshStats()                 // 冻结最终统计值（帧数 / 覆盖方位）
    }

    /// 应用切后台（T-14）。返回是否已接受。
    @discardableResult
    public func appDidBackground() -> Bool {
        let next = session.send(.appBackgrounded)
        if session.isPaused { capture.setPaused(true) }
        return next == .capturing
    }

    /// 回到前台：清除暂停标志（状态不变，故无独立事件）。
    public func appWillForeground() {
        session.clearPaused()
        capture.setPaused(false)
    }

    // =======================================================================
    // MARK: - 重建
    // =======================================================================

    /// 用户确认重建：状态机 `UserConfirmReconstruct` → `Processing`，后台跑内核。
    public func confirmReconstruct() {
        let next = session.send(.userConfirmReconstruct)
        guard next == .processing else { return }
        capture.stop()

        progress = ReconProgress(abiStage: 0, uiSegment: nil, segmentName: nil,
                                 fraction: 0, message: nil)
        outcome = .running

        let core = self.core
        worker.async { [weak self] in
            guard let core else {
                Task { @MainActor in self?.finishFailure(.invalidArgument) }
                return
            }
            let status = core.runReconstruction(delegate: self?.progressProxy)
            Task { @MainActor in
                self?.finishReconstruction(status)
            }
        }
    }

    /// 取消重建（线程安全，可在任意线程调用内核的 `recon_cancel`）。
    public func cancelReconstruction() {
        _ = core?.cancel()
    }

    /// 重建收尾：按 status 决定状态机事件与 UI 结局。
    private func finishReconstruction(_ status: ReconStatus) {
        lastStatus = status
        switch status {
        case .ok:
            result = core?.meshInfo().map(ReconResultSummary.init)
            outcome = .succeeded
            session.send(.reconstructionSucceeded)
            updateCurrentRecord(summary: result, interrupted: false)
        case .cancelled:
            outcome = .cancelled
            session.send(.userCancelReconstruction)
            updateCurrentRecord(summary: nil, interrupted: true)
        default:
            outcome = .failed(status: status,
                              messageKey: ReconStatusPresentation.messageKey(for: status))
            session.send(.reconstructionFailed)
            updateCurrentRecord(summary: nil, interrupted: true)
        }
    }

    private func finishFailure(_ status: ReconStatus) {
        lastStatus = status
        outcome = .failed(status: status,
                          messageKey: ReconStatusPresentation.messageKey(for: status))
        session.send(.reconstructionFailed)
    }

    // =======================================================================
    // MARK: - 进度回调接线
    // =======================================================================

    /// 传给 `ReconCore.runReconstruction(delegate:)` 的代理。
    ///
    /// 内核在其线程调用 `onProgress`；这里立刻把 `@Published` 写入
    /// **排到主线程**（`Task { @MainActor }`）。
    private lazy var progressProxy: ReconProgressDelegate = ProgressProxy { [weak self] stage, frac, msg in
        Task { @MainActor [weak self] in
            self?.applyProgress(stage: stage, fraction: frac, message: msg)
        }
    }

    private func applyProgress(stage: Int32, fraction: Double, message: String?) {
        // 未知阶段不静默归类（ReconStage.init?(abi:) 返回 nil）。
        let mapped = ReconStage(abi: stage)
        let segment = mapped.flatMap(ReconStageMap.uiSegment(for:))
        let name = segment.map(RebuildStage.name(_:))
        progress = ReconProgress(abiStage: stage,
                                 uiSegment: segment,
                                 segmentName: name,
                                 fraction: min(max(fraction, 0), 1),
                                 message: message)
    }

    // =======================================================================
    // MARK: - 采集帧 → 内核
    // =======================================================================

    // =======================================================================
    // MARK: - 采集帧 → 内核（薄接线，实现在 FramePipeline）
    // =======================================================================
    //
    // 全部跨队列状态与「帧 → C ABI」的打包都在 `FramePipeline`（独立文件）。
    // 本层**只做三件事**：① 把内核句柄交给它；② 把 IMU 采样转给它；
    // ③ 接收它的计数快照并弹回主线程发布。
    //
    // 为什么不在本类里直接实现：本类是 `@MainActor`，而帧回调（ARKit 队列，
    // 30 Hz）与 IMU 回调（CoreMotion 队列，100 Hz）都不在主线程。若在此实现，
    // 每个回调都要跨 actor 跳到主线程 —— 11 §3.1.3 的背压正因此而来。
    // 详见 `FramePipeline.swift` 文件头。

    /// 帧 / IMU → C ABI 的投递管线（线程安全，自带锁）。
    private let framePipeline = FramePipeline()

    /// RGB 路当前不可用（缺口 J）导致的拒绝计数快照，供诊断可见。
    ///
    /// **刻意是 `@Published`**：若 RGB 转换始终返回 nil（真机 YUV 平面格式），
    /// `capturedFrames` 会停在 0 而状态显示「采集中」—— 那正是缺口 J 在旧实现
    /// 中的表现（静默）。这里让拒绝数**在 UI 可见**，杜绝同类静默。
    @Published public private(set) var rejectedFrameCount: Int = 0

    private func wireCaptureCallbacks() {
        // 先绑定管线回调（闭包内自行弹回主线程，见 FramePipeline 文档）。
        framePipeline.onCountsChanged = { [weak self] accepted, rejected in
            Task { @MainActor [weak self] in
                guard let self else { return }
                // 帧计数以本管线的成功投递数为准（内核统计更权威，由
                // refreshStats 覆盖；这里先保证「采集中」时有即时反馈）。
                self.session.capturedFrames = accepted
                self.rejectedFrameCount = rejected
            }
        }
        capture.onFrame = { [weak self] packet in
            self?.framePipeline.handleFrame(packet)
        }
        capture.onIMU = { [weak self] sample in
            // IMU 采样**独立于帧**到达（CoreMotion 队列，约 100 Hz）。
            // 先入环形缓冲；帧到达时按时间戳切出「本帧之前的窗口」一并投递
            // （实现见 FramePipeline.handleFrame 与 IMUWindow.swift）。
            self?.framePipeline.appendIMU(sample)
        }
    }
}

// ===========================================================================
// MARK: - 进度代理适配
// ===========================================================================

/// 把闭包适配为 `ReconProgressDelegate`。
///
/// `progressProxy` 是 `lazy var`，在后台线程首次访问理论上可能竞态；实际调用点
/// 在 `confirmReconstruct()`（主线程）先取好引用再进入 worker，故安全。
/// 这里显式标注该前提，避免后续改动无声破坏它。
private final class ProgressProxy: ReconProgressDelegate {
    private let handler: (Int32, Double, String?) -> Void
    init(_ handler: @escaping (Int32, Double, String?) -> Void) {
        self.handler = handler
    }
    func onProgress(stage: Int32, progress: Double, message: String?) {
        handler(stage, progress, message)
    }
}
