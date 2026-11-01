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
///
/// `public`：本类型是 UI 层公开 API 的一部分（`ReconOutcome.failed(status:)`、
/// `ReconStatusPresentation` 等均以其为参数/返回），而 UI 源文件与 `ReconCore`
/// **不在同一 module**（前者属 `Recon` app target，后者属 `ReconCore` 库 target）。
/// 保持 internal 会让所有引用它的 public 声明报
/// 「cannot be declared public because its parameter uses an internal type」。
public enum ReconStatus: Int32 {
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
///
/// `public`：被 `ReconCapability.branch`（`public`）使用，理由同 `ReconCapability`。
/// 取值必须与头文件 `recon_pipeline_branch` 逐值对应（G11 同类判据）。
public enum ReconBranch: Int32 {
    case rgbd = 0
    case mono = 1
}

/// 设备能力（对应 `recon_capability`）。
///
/// `public`：由 UI 层 `ReconSessionController.capability`（`public`）与
/// `CapabilityView.make(capability:)`（`public`）暴露 —— Swift 规则要求
/// `public` 声明的参数/属性类型也必须是 `public`（与是否跨 module 无关，
/// 同一 module 内同样强制）。成员与之同步，否则 memberwise init 不可见。
public struct ReconCapability {
    public var branch: ReconBranch
    public var depthTier: Int32
    public var depthSource: Int32
    public var hasCamera: Bool
    public var hasImu: Bool
    public var hasLidar: Bool
    public var hasTof: Bool
    public var supportsGpu: Bool
    public var maxImageWidth: Int32
    public var maxImageHeight: Int32
    public var maxFps: Int32
    public var imuMaxRateHz: Int32
    public var availableMemoryBytes: UInt64
    public var totalMemoryBytes: UInt64
}

/// 平台能力**事实**上报载荷（对应 `recon_probe_data`，缺口 AC）。
///
/// 与 `ReconCapability` 的分工（**不要混用**）：本类型是平台层**报上去**的事实
/// （`平台 → core`），`ReconCapability` 是 core **判定后**的结果
/// （`core → 平台`，多出 `branch` / `depthTier` / `depthSource` 三个派生字段）。
///
/// ⚠️ 记忆点：`ReconCapability` 里既然有 `maxImageWidth`，为什么这里还要再报一遍？
/// 因为「相机最大分辨率」是**设备事实**，而 `recon_capability` 里的那个字段是
/// 平台**报上去后被 core 原样带回来**的。真机上若不上报，core 读到的就是全零，
/// `recon_capability.max_image_width` 也是 0 —— 那才是缺口 AC 的表象。
public struct ReconProbeData {
    public var hasCamera: Bool
    public var hasImu: Bool
    public var hasLidar: Bool
    public var hasTof: Bool
    /// 算法 / 运动推断深度（ARCore `DEPTH_BACKEND_MOTION_STEREO` 一类）。
    /// 内核据它把 `depth_tier` 判为 `RECON_DEPTH_ALGO` 并以 `BRANCH_RGBD` 跑。
    public var hasAlgoDepth: Bool
    public var supportsGpu: Bool
    public var maxImageWidth: Int32
    public var maxImageHeight: Int32
    public var maxFps: Int32
    public var imuMaxRateHz: Int32
    public var totalMemoryBytes: UInt64
    public var availableMemoryBytes: UInt64

    public init(hasCamera: Bool, hasImu: Bool, hasLidar: Bool, hasTof: Bool,
                hasAlgoDepth: Bool, supportsGpu: Bool,
                maxImageWidth: Int32, maxImageHeight: Int32, maxFps: Int32,
                imuMaxRateHz: Int32,
                totalMemoryBytes: UInt64, availableMemoryBytes: UInt64) {
        self.hasCamera = hasCamera
        self.hasImu = hasImu
        self.hasLidar = hasLidar
        self.hasTof = hasTof
        self.hasAlgoDepth = hasAlgoDepth
        self.supportsGpu = supportsGpu
        self.maxImageWidth = maxImageWidth
        self.maxImageHeight = maxImageHeight
        self.maxFps = maxFps
        self.imuMaxRateHz = imuMaxRateHz
        self.totalMemoryBytes = totalMemoryBytes
        self.availableMemoryBytes = availableMemoryBytes
    }

    /// 把本结构映射为 C ABI 的 `recon_probe_data`。
    ///
    /// `Bool` → `Int32` 必须显式转换：C 侧字段声明为 `int32_t`（头文件注释说明
    /// 不用 `bool` 是为了跨语言宽度确定）。直接塞 `Bool` 会让 Swift 按 1 字节
    /// 写入一个 4 字节字段的高位字节，在 little-endian 下**恰好**能读出 0/1，
    /// 但一旦结构体被 `memcpy` 到别处或编译器插入填充，就会变成随机值 ——
    /// 故此处一律 `? 1 : 0`。
    var cValue: recon_probe_data {
        var d = recon_probe_data()
        d.has_camera = hasCamera ? 1 : 0
        d.has_imu = hasImu ? 1 : 0
        d.has_lidar = hasLidar ? 1 : 0
        d.has_tof = hasTof ? 1 : 0
        d.has_algo_depth = hasAlgoDepth ? 1 : 0
        d.supports_gpu = supportsGpu ? 1 : 0
        d.max_image_width = maxImageWidth
        d.max_image_height = maxImageHeight
        d.max_fps = maxFps
        d.imu_max_rate_hz = imuMaxRateHz
        d.total_memory_bytes = totalMemoryBytes
        d.available_memory_bytes = availableMemoryBytes
        return d
    }
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
///
/// `public`：由 `ReconSessionController.scanStats`（`public`）暴露，理由同上。
public struct ReconScanStats {
    public var framesCaptured: UInt64
    public var framesAccepted: UInt64
    public var keyframes: UInt64
    public var imuSamples: UInt64
    public var durationS: Double
    /// 方位覆盖度 [0,1]（§3.3「覆盖方位」的真实来源，而非估算）。
    public var azimuthCoverage: Double
    public var elevationCoverage: Double
    public var anomalyBlur: UInt32
    public var anomalyFastMotion: UInt32
    public var anomalyLowOverlap: UInt32
    public var anomalySpecular: UInt32
    public var anomalyLowTexture: UInt32
    public var anomalyBlindSpot: UInt32
}

/// 尺度标定结果（对应 `recon_scale_result`）。
///
/// `public`：由 `ReconSessionController.applyManualScale(longEdgeMM:)`
/// （`public`）作为返回值暴露，也被 `CalibrationView` 的 `@State` 持有。
/// 不提升访问级别会报「method cannot be declared public because its result
/// uses an internal type」（CI 首次真编暴露），且**无法靠改调用点绕过** ——
/// 公开 API 的签名必须自洽。成员同步 `public`，与 `ReconScanStats` 同风格。
public struct ReconScaleResult {
    public var ok: Bool
    public var scale: Double
    /// 相对不确定度 σ_s/s（用于 §3.9 置信度三档判定：≤0.02 高 / ≤0.05 中 / 其余低）。
    public var relativeSigma: Double
    public var referenceType: Int32
    public var referenceWidthMM: Double
    public var referenceHeightMM: Double
    public var measuredWidthMM: Double
    public var measuredHeightMM: Double
}

/// 进度回调协议。实现方需自行切回主线程再更新 UI。
///
/// ── 为什么继承 `Sendable`（而不是只 `AnyObject`）────────────────────────
///
/// `ReconSessionController.confirmReconstruct()` 把实现者**捕获进
/// `worker.async { }`（`@Sendable` 闭包）**传给内核（内核在后台线程回调它）。
/// 若协议本身不 `Sendable`，则 **existential 类型 `any ReconProgressDelegate`
/// 也不是 Sendable** —— 即便具体实现类已标注 `@unchecked Sendable`（那是
/// 对**具体类型**的声明，不传递到 `any P` 上）仍会报
/// 「capture of 'proxy' with non-sendable type」（CI 首次真编暴露）。
///
/// 继承 `Sendable` 后，协议要求**每个实现者**都是 Sendable；本工程唯一实现
/// `ProgressProxy` 已标注 `@unchecked Sendable`（依据见其定义处注释），
/// 语义上成立而非放宽。`ProgressBox` 里的 `weak var delegate` 也随之可安全
/// 跨线程弱引用。
protocol ReconProgressDelegate: AnyObject, Sendable {
    func onProgress(stage: Int32, progress: Double, message: String?)
}

/// core C ABI 的 Swift 门面。与 Android 侧 `NativeBridge` 一一对等。
///
/// ── 为什么是 `@unchecked Sendable`（而不是 `Sendable`）──────────────────────
///
/// `ReconSessionController.confirmReconstruct()` 把本对象**捕获进
/// `worker.async { }`（`@Sendable` 闭包）**在后台队列调用
/// `recon_run_reconstruction`（该函数同步阻塞，不得在主线程跑）。不标注
/// `Sendable` 会报「capture of 'core' with non-sendable type 'ReconCore?' in a
/// '@Sendable' closure」（CI run 41274385705 首次真编暴露）。
///
/// 用 `@unchecked`（而非编译器可验证的 `Sendable`）是本文件**已明文声明的
/// 调用纪律**的直接产物 —— 见文件头「设计约束 ③」：
///     **句柄不可并发**：除 `recon_cancel` 外，同一 context 不跨线程并发调用。
/// 即：`ReconCore` 本身**不含**线程同步原语（内部持 C 侧裸句柄），
/// 安全性由**调用方**的串行化保证（重建走单条串行 `worker` 队列、帧投递走
/// 自有队列且与重建不重叠 —— `confirmReconstruct()` 会先 `capture.stop()`）。
/// 这类「正确性由使用约定而非类型系统保证」的情况，正是 `@unchecked Sendable`
/// 的既定用途。**代价是诚实的**：若将来有人新增一条并发调用路径，
/// 编译器**不会**拦 —— 故此处显式复述该前提，供后人核对。
extension ReconCore: @unchecked Sendable {}

final class ReconCore {


    /// 与 core 的 `RECON_ABI_VERSION` 对应。
    static let expectedAbiVersion: UInt32 = 1

    // MARK: - C 枚举 ↔ Swift 枚举桥接（唯一转换点）

    /// 把 C ABI 的 `recon_status` 转成 Swift `ReconStatus`。
    ///
    /// ── 为什么必须有这一层（2026-11-01 CI 首次真编暴露，系统性错误）──────────
    ///
    /// `recon_status` 是**普通 C enum**（既无 `NS_ENUM`、也无
    /// `enum_extensibility` 标注），Clang importer 因此**不**把它导入成 Swift
    /// enum，而是导入成一个 `RawRepresentable` **结构体**（每个成员降级为一个
    /// 全局常量）。更关键的是其 `RawValue` 类型由**枚举成员的正负**决定：
    /// `recon_status` 全部成员非负 → 导入为 `CUnsignedInt` 即 **`UInt32`**，
    /// 且 `init(rawValue:)` **不是** failable 的。
    /// 事实来源：Apple *How Swift Imports C APIs*（「enums with strictly
    /// non-negative members are imported as CUnsignedInt, while those that have
    /// negative members are imported as CInt」）。
    ///
    /// 因此以下两种写法都**无法编译**（本文件原先通篇如此）：
    ///   · `ReconStatus(rawValue: st)` —— `UInt32` ≠ `Int32`；且因初始化器非
    ///     failable，`??` 的左操作数非 Optional（在 `SWIFT_TREAT_WARNINGS_AS_ERRORS`
    ///     下这个警告直接变错误）。
    ///   · `st == ReconStatus.ok.rawValue` —— `recon_status` 与 `Int32` 不可比较。
    ///
    /// 本函数是三处（状态码 / 分支 / 深度源 / 导出格式）之外**唯一**的转换点：
    /// 新增转换需求请复用此处，不要再写第二份位模式转换。
    ///
    /// 未知值**不静默吞掉**：映射到 `.internalError` 并打日志。出现未知值意味着
    /// 头文件新增了错误码而 Swift 表没跟上 —— G11 闸门会拦，此处兜底保持可见。
    static func status(_ s: recon_status) -> ReconStatus {
        let raw = Int32(bitPattern: s.rawValue)
        if let known = ReconStatus(rawValue: raw) { return known }
        NSLog("recon: 未知 recon_status=\(raw)；头文件与 Swift 表可能已漂移（见 G11 闸门）")
        return .internalError
    }

    /// C ABI 的 `recon_status` 是否表示成功（仅 `RECON_OK`）。
    static func isOK(_ s: recon_status) -> Bool { status(s) == .ok }

    /// C 枚举的 `rawValue`（`UInt32`）→ `Int32`。
    ///
    /// 用 `Int32(bitPattern:)` 而非 `Int32(_:)`：后者在 `rawValue > Int32.max`
    /// 时会 **trap**（运行时崩溃），而 C 枚举的位模式不保证落在 `Int32` 域内
    /// —— `recon_config.forced_branch` 正是一个例子（用 `0xFFFFFFFF` 当哨兵）。
    /// 位模式转换与 C 侧 `static_cast<int32_t>` 的语义**逐位一致**，正是跨语言
    /// 传递枚举时所期望的行为。
    static func code(_ raw: UInt32) -> Int32 { Int32(bitPattern: raw) }

    private(set) var handle: UnsafeMutableRawPointer?

    // MARK: - 生命周期

    init?(sessionDir: String, preset: Int32 = 1, maxThreads: UInt32 = 0) {
        var cfg = recon_config()
        cfg.abi_version = ReconCore.expectedAbiVersion
        // 哨兵 `0xFFFFFFFF`（= UINT32_MAX）表示「自动探测」。此处**必须**写位模式，
        // 不能写 0xFFFF_FFFF 之外的字面量：`recon_pipeline_branch` 的 rawValue 是
        // UInt32，而 core 侧判定（core/src/util/context.cpp §62-70）是
        // 「越界/负值一律走自动探测」，故这个哨兵正是被设计成「越界」的。
        cfg.forced_branch = recon_pipeline_branch(rawValue: 0xFFFF_FFFF)
        cfg.preset = preset
        cfg.max_threads = maxThreads
        cfg.enable_rgbd = 1
        cfg.enable_mvs = 1
        cfg.enable_texture = 1

        let created: UnsafeMutableRawPointer? = sessionDir.withCString { cstr in
            cfg.session_dir = cstr
            var ctx: UnsafeMutableRawPointer?
            let st = recon_create(&cfg, &ctx)
            return ReconCore.isOK(st) ? ctx : nil
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

    /// 上报平台能力**事实**（`recon_set_probe_data`，缺口 AC）。
    ///
    /// ── 为什么必须在 `probeCapability()` **之前**调用（顺序不可交换）───────
    ///
    /// core 的 `Context::ProbeCapability()` 有**一次性缓存**（`capability_probed_`，
    /// `core/src/util/context.cpp:108`）：第一次调用时读到的平台事实会被固化，
    /// 之后再调 `recon_set_probe_data` 也不会改变 `recon_capability`。
    /// 因此上报**必须**发生在首次 `recon_probe_capability` 之前。
    ///
    /// ── 为什么是类方法而不是实例方法 ────────────────────────────────────
    ///
    /// `recon_set_probe_data` 是**全局**的（不绑 context，见头文件语义约束）——
    /// 设备能力与「哪个会话」无关。做成实例方法会诱使调用方以为「每个会话配置
    /// 一次」，而实际上重复上报是幂等的、且必须在 create 之前就位。
    /// 做成类方法把「App 启动时一次」这件事写进签名。
    ///
    /// - Returns: 内核返回的状态。**不吞错**：调用方应把非 `RECON_OK` 记入日志，
    ///            因为上报失败意味着后续探测结果可能全为零。
    @discardableResult
    public static func reportProbeData(_ data: ReconProbeData) -> ReconStatus {
        var d = data.cValue
        let st = recon_set_probe_data(&d)
        if !isOK(st) {
            NSLog("recon: recon_set_probe_data 失败 status=\(status(st).rawValue)"
                  + "；能力探测将读到全零事实（缺口 AC 回退）")
        }
        return status(st)
    }

    func probeCapability() -> ReconCapability? {
        guard let h = handle else { return nil }
        var cap = recon_capability()
        guard ReconCore.isOK(recon_probe_capability(h, &cap)) else { return nil }
        return ReconCapability(
            branch: ReconBranch(rawValue: ReconCore.code(cap.branch.rawValue)) ?? .mono,
            depthTier: ReconCore.code(cap.depth_tier.rawValue),
            depthSource: ReconCore.code(cap.depth_source.rawValue),
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
        return ReconCore.status(st)
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
        guard ReconCore.isOK(st) else { return nil }
        return (vInt, vDouble)
    }

    /// 应用预设：0=快速 1=标准 2=高精度。
    @discardableResult
    func applyPreset(_ preset: Int32) -> ReconStatus {
        guard let h = handle else { return .invalidArgument }
        return ReconCore.status(recon_apply_preset(h, preset))
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
        guard ReconCore.isOK(st), let p = out else { return nil }
        defer { recon_free_string(p) }
        return String(cString: p)
    }

    // MARK: - 采集

    @discardableResult
    func startSession(_ sessionId: String) -> ReconStatus {
        guard let h = handle else { return .invalidArgument }
        let st = sessionId.withCString { recon_start_session(h, $0) }
        return ReconCore.status(st)
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

        // `let`（不是 `var`）：下面传给 `withUnsafePointer(to:)` 的 **by-value**
        // 重载，不取 `inout`，故无需可变绑定 —— 写成 `var` 会触发
        // 「variable was never mutated」警告，而本工程开了
        // `SWIFT_TREAT_WARNINGS_AS_ERRORS`（零警告标准），警告即错误。
        let intr = intrinsics
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
        pkt.depth_source = recon_depth_source(rawValue: UInt32(bitPattern: depthSource))
        pkt.has_depth = (depthMm != nil) ? 1 : 0
        pkt.imu = imu
        pkt.imu_count = imuCount
        pkt.exposure_us = exposureUs
        pkt.iso = iso

        // ⚠️ `intrinsics` / `pose` 是**指向本帧栈上局部量的指针**，仅在本次调用
        //    期间有效（C ABI 明文：「核心不持有指针」）。因此两次
        //    `recon_push_frame` 都必须在 `withUnsafePointer` 的**闭包内部**发起：
        //    若照旧写成 `pkt.intrinsics = withUnsafePointer(to: &intr) { $0 }`
        //    再在闭包外调用，指针在闭包返回后即失效（悬垂），属未定义行为 ——
        //    悬垂时读到的是复用后的脏内存，表现为「位姿/内参偶发错乱」，
        //    是最难查的一类。此处用嵌套闭包把「指针有效期」与「调用期」对齐。
        //
        // ⚠️ 两个闭包**都显式标注返回类型 `-> ReconStatus`**：多语句闭包的类型
        //    推断在「外层闭包返回内层闭包结果」的嵌套场景下容易超时/失败
        //    （"unable to type-check this expression in reasonable time"），
        //    显式标注是零成本的消除手段。
        return withUnsafePointer(to: intr) { (intrPtr) -> ReconStatus in
            pkt.intrinsics = intrPtr
            if let p = pose {
                return withUnsafePointer(to: p) { (posePtr) -> ReconStatus in
                    pkt.pose = posePtr
                    return ReconCore.status(recon_push_frame(h, &pkt))
                }
            }
            return ReconCore.status(recon_push_frame(h, &pkt))
        }
    }

    @discardableResult
    func finishCapture() -> ReconStatus {
        guard let h = handle else { return .invalidArgument }
        return ReconCore.status(recon_finish_capture(h))
    }

    /// 读取采集统计。`nil` 表示句柄缺失或调用失败（**不返回全 0 结构体冒充**）。
    func scanStats() -> ReconScanStats? {
        guard let h = handle else { return nil }
        var s = recon_scan_stats()
        guard ReconCore.isOK(recon_get_scan_stats(h, &s)) else { return nil }
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
        // 闭包返回类型写成 `recon_status`（C 结构体），不是 `Int32` —— 见
        // `ReconCore.status(_:)` 的说明：普通 C enum 被导入为 RawRepresentable
        // 结构体，`Int32` 形参会导致 "cannot convert return expression" 报错。
        let st = detectedCorners.withUnsafeBufferPointer { buf -> recon_status in
            var pkt = referenceFrame
            return recon_calibrate_scale(h, &pkt, buf.baseAddress,
                                         referenceType, refWidthMM, refHeightMM, &out)
        }
        guard ReconCore.isOK(st) else { return nil }
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
        guard ReconCore.isOK(st) else { return nil }
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
        return ReconCore.status(st)
    }

    func meshInfo() -> ReconMeshInfo? {
        guard let h = handle else { return nil }
        var info = recon_mesh_info()
        guard ReconCore.isOK(recon_get_mesh_info(h, &info)) else { return nil }
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
        opt.format = recon_export_format(rawValue: UInt32(bitPattern: format))
        opt.apply_scale = applyScale ? 1 : 0
        opt.embed_texture = embedTexture ? 1 : 0
        let st = outPath.withCString { cstr in
            opt.out_path = cstr
            return recon_export(h, &opt)
        }
        return ReconCore.status(st)
    }

    /// 线程安全，可在任意线程调用。
    @discardableResult
    func cancel() -> ReconStatus {
        guard let h = handle else { return .invalidArgument }
        return ReconCore.status(recon_cancel(h))
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
