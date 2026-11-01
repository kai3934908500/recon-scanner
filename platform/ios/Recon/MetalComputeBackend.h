// SPDX-License-Identifier: BSD-3-Clause
//
// MetalComputeBackend.h — iOS 侧 YUV→BGRA8 转换门面（docs/11 §693 IOS-AR-5）。
//
// 契约原文（docs/11_平台权限与集成.md §693，逐字引用）：
//
//   「IOS-AR-5 | ARKit 模式下的 RGB 缓冲为 YUV
//     （`kCVPixelFormatType_420YpCbCr8BiPlanarFullRange`）。为保持
//     `recon_frame_packet.rgb_data = BGRA8` 契约不变，转换在平台层的
//     Metal compute kernel 中完成（`platform/ios/MetalComputeBackend.mm`
//     的 `yuv_to_bgra`），1080p 单帧 GPU 转换 < 0.4 ms」
//
// ── 本文件补的是哪个洞 ────────────────────────────────────────────────────
//
// 在此之前，`FrameConverter.bgra8(from:)` 是**空壳**（恒返回 nil），理由写在
// 它的文档注释里：`ARFrame.capturedImage` 是 YUV 双平面，`CVPixelBufferGetBaseAddress`
// 对它返回 NULL，而把 plane 0（Y 平面）当 BGRA8 喂给内核会读到跨像素乱码。
// 于是那一版**诚实地拒绝**了每一帧 → RGB 一帧都进不去 → MVS 主线（依赖灰度）
// 无输入（docs/14 §5.1.3 缺口 J）。
//
// 本文件即把这条链路打通，分三层：
//
//   ① **GPU 路径**（首选）：`CVMetalTextureCache` 由 `CVPixelBuffer` **零拷贝**
//      取得 Y 与 CbCr 纹理 → `recon_yuv_to_bgra`（`shaders/metal/yuv.metal`）
//      → BGRA8 输出纹理 → 读回**紧密排列**的字节。诉求是 docs/11 的 < 0.4 ms。
//
//   ② **CPU 回退**（GPU 不可用时）：调 `platform/common/src/yuv_convert.cpp`。
//      为什么会不可用 —— 这不是理论情形：`makeDefaultLibrary()` 要求
//      `default.metallib` 已由 `tools/build_metal_shaders.sh` 编进 bundle；
//      在没有这个产物的构建（或模拟器上无 Metal 设备）里，GPU 路径必须
//      **明确失败**而不是产出乱码。回退是**显式**的：结果里带
//      `usedGPU == NO` 与 `fallbackReason`，调用方必须上报
//      （docs/14 CT-9「无静默降级」）。
//
//   ③ **色彩数学的唯一来源**：系数（`y_bias/y_gain/c_bias/c_gain` 与四个
//      矩阵增益）由 `recon::platform::MakeYuvToBgraParams` 算出 —— 同一份
//      C++ 既供 CPU 路径直接算，也供 GPU 参数块（`ReconYuvToBgraGpuParams`）
//      填值。**着色器里因此没有任何数值字面量**（除 workgroup 尺寸，
//      MT-4 的唯一例外）。「系数算错」这类错误由桌面单测
//      `test_platform_yuv.cpp` 锁定，真机只剩「GPU 是否忠实执行这几行 FMA」。
//
// ══════════════════════════════════════════════════════════════════════════
// ⚠️ 实现状态（必须如实阅读）
// ══════════════════════════════════════════════════════════════════════════
//   · 本文件与 `.mm` **从未被编译过**：本机是 Windows，无 Xcode / 无 Metal 工具链。
//     语言层正确性目前只有人工复核 + `check_ios_contract.py` 的静态判据。
//   · **GPU 路径从未运行过**：无真机。`< 0.4 ms` 是 docs/11 的**目标值**，
//     不是实测值 —— 本仓库迄今**没有任何** GPU 性能实测数据。
//   · 数值正确性由 `test_platform_yuv.cpp` 在桌面锁定（同一套系数与同一套
//     数学式）；**未经真机验证**的只剩「纹理采样/写出的字节序」与「GPU
//     对这几行 FMA 的忠实性」。
//   · 真机验收（docs/14 §5.3 GPU 行）需要 macOS + 一台 iPhone，**本轮未执行**。
// ══════════════════════════════════════════════════════════════════════════

#import <Foundation/Foundation.h>
#import <CoreVideo/CoreVideo.h>

NS_ASSUME_NONNULL_BEGIN

/// 一帧 BGRA8 结果（由 GPU 或 CPU 路径产出）。
///
/// `width` / `height` 与 `data.length` 的**唯一事实来源是本对象** ——
/// 调用方**不得**再从 `CVPixelBuffer` 读一次尺寸（第二事实来源必然漂移，
/// 见 MEMORY.md；本项目已在 `DepthFrame` / `ReconDepthMM` 上确立同一约定）。
@interface ReconBgraFrame : NSObject

/// 转换是否成功。`NO` 时其余字段无意义。
@property (nonatomic, readonly) BOOL ok;

@property (nonatomic, readonly) uint32_t width;
@property (nonatomic, readonly) uint32_t height;

/// 行优先、**紧密排列**（`bytesPerRow == width × 4`）的 BGRA8。
/// 字节序 **B,G,R,A** —— 与内核 `BgraToGray` 的取值口径一致（docs/00 §7.2）。
@property (nonatomic, readonly, copy) NSData *data;

/// 是否走了 GPU 路径。`NO` 表示走了 CPU 回退 → 调用方**必须**上报降级
/// （docs/14 CT-9：不得静默降级）。
@property (nonatomic, readonly) BOOL usedGPU;

/// GPU 不可用 / 回退的机器可读原因（`usedGPU == YES` 时为空串）。
/// **不是**界面文案 —— 不要直接塞进 `Text(...)`（那会绕过本地化，G9）。
@property (nonatomic, readonly, copy) NSString *fallbackReason;

/// 被钳位到边界（0 或 255）的通道数。
///
/// 诊断信号：**持续**偏大意味着色彩矩阵或 range 判定与实际流不匹配
/// （例如把 limited 当 full 会得到低对比 + 灰雾，且**不报任何错**）。
@property (nonatomic, readonly) NSUInteger clampedChannelCount;

@end

/// YUV → BGRA8 转换门面。iOS 对等物见 Android 侧的 JNI 桥（待 Camera2 路径落地）。
@interface ReconYuvConverter : NSObject

/// 从 ARKit 的 `ARFrame.capturedImage` 产出 BGRA8。
///
/// 内部完成：① 判平面数；② `GetBaseAddressOfPlane` / `GetBytesPerRowOfPlane`
/// （**不是** `GetBaseAddress` —— 对平面缓冲它返回 NULL，是本项目缺口 J 的根因）；
/// ③ 读 `kCVImageBufferYCbCrMatrix` 选 BT.601/709；④ 由像素格式判 full/limited；
/// ⑤ 先试 GPU 路径；⑥ 失败则 CPU 回退并记录原因。
///
/// @param capturedImage `ARFrame.capturedImage`。
/// @return 结果；`nil` 表示**该缓冲不是本函数能处理的 YUV 双平面**
///         （调用方必须显式处理，不得静默继续）。
+ (nullable ReconBgraFrame *)bgra8FromCapturedImage:(CVPixelBufferRef)capturedImage;

/// 处理「已经是 BGRA8」的缓冲（模拟器 / 部分非 ARKit 配置下的兼容路径）。
///
/// 只做一件事：按 `bytesPerRow` **重排为紧密 stride**（内核按 `width × 4`
/// 紧排消费）。不做任何色彩变换。
+ (nullable ReconBgraFrame *)bgra8FromBgraPixelBuffer:(CVPixelBufferRef)pixelBuffer;

/// GPU 路径是否可用（设备 + `default.metallib` + kernel）。
///
/// ⚠️ 这是**当前**可用性，不是"曾经成功过"：若 `default.metallib` 缺 kernel，
/// 本属性为 `NO` 且 `metalUnavailableReason` 给出原因。
+ (BOOL)metalAvailable;

/// GPU 不可用的原因（可用时为空串）。用于诊断与降级上报。
+ (NSString *)metalUnavailableReason;

@end

NS_ASSUME_NONNULL_END
