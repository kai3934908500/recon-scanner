//
//  FrameConverter.swift
//  Recon — 平台像素缓冲 → C ABI 载荷的唯一转换层
//
//  契约：
//    · core/include/recon/recon_c_api.h —— `recon_frame_packet` 字段语义
//    · docs/02_采集与同步.md §5.5 F-1（深度单位固定为毫米、沿光轴 z）
//    · docs/04_RGBD管线.md §3.6 IR-1~IR-6（保留原始深度副本铁律）
//    · docs/11_平台权限与集成.md §3.7（平台 → 核心层字段对接）
//
//  ── 为什么单独一个文件 ──────────────────────────────────────────────────────
//
//  `ReconSessionController` 的文件头已声明：「不做像素格式转换的魔法 ——
//  CVPixelBuffer → BGRA8 的转换放在 FrameConverter，因为它牵涉 vImage/
//  CoreImage，是独立可测单元」。本文件即该设计意图的落地。
//
//  分层的收益是**可验证性**：本文件的所有函数都是**纯函数**（输入缓冲 → 输出
//  值语义），不依赖 ARKit 会话、不依赖句柄、不写全局状态。因此它们可以被
//  单元测试直接喂合成缓冲验证（无需真机），这是把「平台层错误」从
//  「只能真机发现」变成「可静态/单测发现」的关键一步。
//
//  ── 诚实声明（当前实现边界）────────────────────────────────────────────────
//
//  · 深度换算（`depthMillimeters(from:)`）的**数学**已下沉 `platform/common`
//    （见 `platform/common/src/depth_convert.cpp`，双端同源）；本文件只做
//    「CVPixelBuffer → C++ 桥 → Swift 值类型」的搬运。**未经真机验证**
//    （无 macOS/Xcode，无 LiDAR 设备）。
//  · RGB 换算（`bgra8(from:)`）已接入 —— 见下方该函数的文档。规则本体（色彩
//    数学）下沉 `platform/common/src/yuv_convert.cpp`（双端同源），Metal host
//    侧门面在 `platform/ios/Recon/MetalComputeBackend.mm`。**未经真机验证**
//    （无 macOS/Xcode，无 iPhone；GPU 路径从未运行过）。
//

import Foundation
import CoreVideo
import simd

// ===========================================================================
// MARK: - 深度帧（ARDepthData.depthMap → uint16 毫米）
// ===========================================================================

/// 一帧已换算为毫米的深度数据。
///
/// `width` / `height` 与 `millimeters.count` 的**唯一事实来源是本结构体** ——
/// 调用方不得自行从 `CVPixelBuffer` 再读一次尺寸（第二事实来源必然漂移，
/// 见 MEMORY.md）。
///
/// **换算规则本体在 `platform/common/src/depth_convert.cpp`**（双端同源），
/// 本结构体只是桥接结果在 Swift 侧的值表示。
public struct DepthFrame {
    /// 深度图宽度（像素）。**等于平台原生分辨率**（IR-1：不重采样）。
    public let width: Int
    /// 深度图高度（像素）。
    public let height: Int
    /// 行优先、紧密排列（stride == width）的毫米深度。0 = 无效。
    public let millimeters: [UInt16]
    /// 有效像素数（**深度非零**，含被上限夹取的触顶像素）。
    ///
    /// 口径依据：core `depth_preprocess.h` 的 `valid_input` = 「深度 ≠ 0 且过
    /// 置信硬门槛」。触顶像素深度为 65535（非零），故**计入有效**。
    /// ⚠️ 本注释曾误写为「满足 `0 < d < 上限`」，与下方代码矛盾（被
    /// `platform/common/.../depth_convert.h` 的下沉实现连同单测抓出）。
    public let validPixelCount: Int
    /// 被上限截断的像素数（原始深度 ≥ 65.535 m 后被夹到 65535）。
    ///
    /// **是 `validPixelCount` 的子集**（触顶像素仍然有效）——「有效 vs 无效」
    /// 的判据是「深度是否为零」；「是否触顶」是另一个维度，两者不互补。
    ///
    /// 该值**不应**被静默忽略：持续非零意味着深度图里混入了远超扫描距离的
    /// 数值（噪声或远景），属可诊断信号，故显式带回供上层上报。
    public let truncatedPixelCount: Int

    /// 有效像素占比 [0,1]。供扫描自检与 U-03 的深度质量提示使用。
    public var validRatio: Double {
        let total = width * height
        return total > 0 ? Double(validPixelCount) / Double(total) : 0
    }
}

public enum FrameConverter {

    // 注：本枚举**不再声明** `depthMaxMM` 常量（曾为 `65_535` 的 Swift 镜像）。
    // 理由：那是「第二事实来源」——权威定义在规则本体
    // `platform/common/include/recon_platform/depth_convert.h` 的 `kDepthMaxMM`，
    // 两处各写一份必然漂移；且该常量在 Swift 侧**零调用点**（死码）。
    // 结构上消灭它比事后加「对等闸门」更可靠（MEMORY.md 第十三轮教训）。
    // 若未来 Swift 侧确需引用，应经 ObjC++ 桥暴露，而非再抄一份。

    /// 把 ARKit 的深度图换算为 C ABI 要求的 **uint16 毫米**。
    ///
    /// - Parameter depthMap: `ARFrame.sceneDepth?.depthMap`。
    ///   格式为 `kCVPixelFormatType_DepthFloat32`（**单平面**，单位为**米**，
    ///   沿光轴 z —— 见 docs/04 §3.5.3「同光心」）。
    /// - Returns: 换算结果；`nil` 表示格式/尺寸不受支持（**调用方必须处理**）。
    ///
    /// ## 换算规则（逐像素、无跨像素依赖 ⇒ 分辨率严格不变）
    ///
    /// | 输入 | 输出 | 理由 |
    /// |---|---|---|
    /// | `NaN` / `±Inf` | `0` | 无效；`is_finite` 是唯一可靠判据 |
    /// | `≤ 0` | `0` | ARKit 用 0 表示无回波；负值物理上不可能 |
    /// | `0 < m < 65.535` | `round(m × 1000)` | 米 → 毫米 |
    /// | `≥ 65.535` | `65535` | 夹取（见规则本体的 `kDepthMaxMM`） |
    ///
    /// ## 规则本体的位置（不要让本函数重新长出这段逻辑）
    ///
    /// 上述规则与本函数**曾经的 Swift 实现**都已下沉
    /// `platform/common/src/depth_convert.cpp`，由
    /// `tests/unit/test_platform_capture.cpp` 在桌面验收（含负向注入证伪）。
    /// 本函数只负责：① 校验像素格式；② 取平面/行字节数；③ 调桥；④ 搬运结果。
    /// **任何单位换算/取整/钳位的新逻辑都必须加在规则本体里**，否则双端漂移。
    ///
    /// ## 为什么**不**做任何滤波或降采样
    ///
    /// docs/04 §3.6 IR-1 是**铁律级**规定：「原始深度帧按原生分辨率、原生单位
    /// （uint16 毫米）逐帧落盘……不得重采样、不得转码为 float32、不得丢帧」。
    /// IR-4 进一步规定：任何滤波结果都**不得**被当作「更准的深度」。
    public static func depthMillimeters(from depthMap: CVPixelBuffer) -> DepthFrame? {
        // `ARDepthData.depthMap` 是单平面 Float32。
        let format = CVPixelBufferGetPixelFormatType(depthMap)
        guard format == kCVPixelFormatType_DepthFloat32 else { return nil }

        let width = CVPixelBufferGetWidth(depthMap)
        let height = CVPixelBufferGetHeight(depthMap)
        guard width > 0, height > 0 else { return nil }

        guard CVPixelBufferLockBaseAddress(depthMap, .readOnly) == kCVReturnSuccess else {
            return nil
        }
        defer { CVPixelBufferUnlockBaseAddress(depthMap, .readOnly) }

        // 深度图是**单平面**缓冲，故 `GetBaseAddress` 是正确接口
        // （与 `capturedImage` 的双平面不同 —— 后者必须用 *OfPlane，见缺口 J）。
        guard let base = CVPixelBufferGetBaseAddress(depthMap) else { return nil }
        let stride = CVPixelBufferGetBytesPerRow(depthMap)
        // 每像素 4 字节（Float32）。stride 可能带行填充（AR 深度图通常不带，
        // 但规则本体按字节偏移取值，可同时容忍两种情况）。
        let bytesPerPixel = MemoryLayout<Float32>.size
        guard stride >= width * bytesPerPixel else { return nil }

        // 单位换算的**唯一实现**在 C++（双端同源）。这里只搬运。
        let r = ReconDepthConverter.millimeters(
            fromFloat32Meters: base,
            width: UInt32(width), height: UInt32(height), strideBytes: UInt(stride))
        guard r.ok else { return nil }

        // NSData（紧密 uint16 小端）→ [UInt16]。桥保证字节数 == w*h*2。
        let mmCount = Int(r.width) * Int(r.height)
        var millimeters = [UInt16](repeating: 0, count: mmCount)
        let byteCount = r.millimeters.count
        guard byteCount == mmCount * MemoryLayout<UInt16>.size else { return nil }
        if mmCount > 0 {
            _ = millimeters.withUnsafeMutableBytes { dst in
                r.millimeters.copyBytes(to: dst, count: byteCount)
            }
        }

        return DepthFrame(width: Int(r.width), height: Int(r.height),
                          millimeters: millimeters,
                          validPixelCount: Int(r.validPixelCount),
                          truncatedPixelCount: Int(r.truncatedPixelCount))
    }

    // =======================================================================
    // MARK: - RGB（CVPixelBuffer → BGRA8）
    // =======================================================================

    /// 把 ARKit 的 `ARFrame.capturedImage` 转为 C ABI 要求的 **BGRA8**。
    ///
    /// - Returns: BGRA8 平面（`data` 行优先紧密排列，`bytesPerRow == width × 4`）；
    ///   `nil` 表示**该缓冲不是本层能处理的 YUV 双平面**（调用方必须显式处理）。
    ///
    /// ## 历史（为什么这里曾经恒返回 `nil`）
    ///
    /// `ARFrame.capturedImage` 在真机上是 **`420YpCbCr8BiPlanarFullRange`**
    /// 的**双平面**缓冲，因此：
    ///
    /// 1. **`CVPixelBufferGetBaseAddress` 对该缓冲返回 NULL**
    ///    —— 平面格式必须用 `CVPixelBufferGetBaseAddressOfPlane(buf, i)`。
    ///    更早的实现（`ReconSessionController.pushFrameToCore`）用了前者，
    ///    并在 `guard let base = ... else { return }` 处**静默丢弃 100% 的帧**
    ///    （`pushedFrames` 恒 0，UI 仍显示「采集中」）—— 缺口 J。
    /// 2. **plane 0 是 Y（亮度）平面**，不是 BGRA8。把它当 BGRA8 传给内核会
    ///    读到**跨像素的乱码**（内核 `BgraToGray` 按 `row[x*4 + {0,1,2}]` 取值，
    ///    而 Y 平面每像素仅 1 字节）并越界。
    /// 3. 正确做法需要 GPU（Metal compute）做 YCbCr→RGB 矩阵变换，
    ///    并按 `kCVImageBufferYCbCrMatrix` 选 BT.601/BT.709 系数。
    ///
    /// 在 Metal 方案落地前，本函数**诚实地拒绝**每一帧（宁可不产出，也不产出
    /// 乱码污染重建）。
    ///
    /// ## 当前实现（docs/11 §693 IOS-AR-5）
    ///
    /// 现在委派给 ObjC++ 门面 `ReconYuvConverter`（`MetalComputeBackend.mm`）：
    ///   · **首选** Metal compute（零拷贝 `CVMetalTextureCache` + `recon_yuv_to_bgra`）；
    ///   · Metal 不可用时回退 CPU（`platform/common/src/yuv_convert.cpp`），
    ///     并在结果里带 `fallbackReason`（不静默降级）。
    ///
    /// `nil` 的语义收窄为：**确实是双平面 YUV 之外的格式**（例如未知单平面格式），
    /// 或 GPU/CPU 两条路都失败。调用方仍**必须**显式处理 `nil`。
    ///
    /// ⚠️ **未经真机验证**：本机是 Windows，无 Metal 工具链、无 iPhone。
    /// 色彩数学由 `test_platform_yuv.cpp` 在桌面锁定；**未验证**的只剩
    /// 「GPU 是否忠实执行那几行 FMA」与「纹理读写的字节序」。
    public static func bgra8(from capturedImage: CVPixelBuffer) -> BGRAFrame? {
        guard let r = ReconYuvConverter.bgra8(fromCapturedImage: capturedImage) else {
            return nil
        }
        // `r.ok == false` 时其余字段无意义（契约见 MetalComputeBackend.h）——
        // 不猜、不当空帧用。
        guard r.ok else { return nil }

        let width = Int(r.width)
        let height = Int(r.height)
        let expected = width * height * 4
        guard width > 0, height > 0, r.data.count == expected else { return nil }

        // 结果字段的唯一事实来源是 `ReconBgraFrame` —— 这里只做值搬运，
        // 不从 `capturedImage` 再读一次尺寸（第二事实来源必然漂移）。
        var data = [UInt8](repeating: 0, count: expected)
        _ = data.withUnsafeMutableBytes { dst in
            r.data.copyBytes(to: dst, count: expected)
        }
        // 降级信息**随帧一起**带走（而不是挂在一个进程级全局量上）：后者是
        // 第二事实来源、并发下要额外加锁，且会把「这一帧」的诊断错误地
        // 变成「最近一帧」的诊断。
        return BGRAFrame(width: width, height: height, data: data,
                         usedGPU: r.usedGPU, converterNote: r.fallbackReason)
    }
}

/// BGRA8 帧（紧密排列）。
public struct BGRAFrame {
    public let width: Int
    public let height: Int
    /// 行优先紧密排列的 BGRA8，长度 == `width * height * 4`。
    public let data: [UInt8]

    /// 该帧是否由 GPU（Metal compute）产出。`false` 表示走了 CPU 规则本体。
    public let usedGPU: Bool

    /// 转换器给出的诊断（**机器可读，不是界面文案** —— 不要直接塞进
    /// `Text(...)`，那会绕过本地化，见 G9）。
    ///
    /// 依据 docs/14 CT-9「无静默降级」：CPU 回退、或 YCbCr 矩阵附件缺失
    /// （按 BT.601 假设）等情况都必须能被上层看到。
    public let converterNote: String

    public init(width: Int, height: Int, data: [UInt8],
                usedGPU: Bool = false, converterNote: String = "") {
        self.width = width
        self.height = height
        self.data = data
        self.usedGPU = usedGPU
        self.converterNote = converterNote
    }

    public var bytesPerRow: Int { width * 4 }
}
