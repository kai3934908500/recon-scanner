// SPDX-License-Identifier: BSD-3-Clause
//
// yuv_convert.h — YUV 4:2:0 → BGRA8 的**色彩数学本体**（平台无关部分）。
//
// 契约：
//   · core/include/recon/recon_c_api.h —— `recon_frame_packet.rgb_data`
//     是 **BGRA8**（docs/00 §7.2 / docs/02 §5.5），与 iOS
//     `CVPixelBufferGetBytesPerRow` 的行填充无关（紧密排列）。
//   · docs/11_平台权限与集成.md §3.1.3（AVFoundation 路径）、§3.4（Camera2
//     路径）、§693 IOS-AR-5（Metal compute 方案与 < 0.4 ms 预算）。
//   · docs/04_RGBD管线.md §3.6 IR-1~IR-6（不重采样、不用滤波结果冒充更准数据）
//
// ── 为什么色彩数学必须下沉到 platform/common ──────────────────────────────
//
// 「YUV → BGRA」这一步在双端都要做，但**只有数学是共同的**：
//   平台相关（留在各端）                    平台无关（下沉到本文件）
//   ─────────────────────────────           ─────────────────────────────
//   像素格式判定 / 平面取址                    范围归一化（full/limited）
//   plane pixelStride / rowStride            BT.601 / BT.709 矩阵选择
//   yyCbCrMatrix attachment 读取             浮点 → 8 位量化（round + clamp）
//   NV12 / NV21 的 Cb/Cr 换序                 BGRA 字节序
//
// 若把整段逻辑在 Swift 与 Kotlin 各写一遍，**必然漂移**（铁律）。而这类漂移
// 的表现恰好是本项目最怕的形态：**不报错、只是颜色整体偏**。
//
// ── 三条最容易错的细节（都在本文件里被单测锁死）───────────────────────────
//
// ① **色度中心是 128，不是 127.5 也不是 0**。
//    标准整数式 `R = Y + 1.402·(Cr − 128)` 与归一化式 `r = y + r_from_cr·cr`
//    的等价性要求 `c_bias = 128`（推导见 `yuv_to_bgra_params` 的注释）。
//    取错会让**中性灰被染上偏色** —— 不崩溃、不报错，只是所有颜色都不对。
//
// ② **要先钳位再量化，而不是先乘 255 再钳位**。
//    `r` 在极端系数下可能略微超出 [0,1]（浮点累积）。若写成
//    `uint8(r * 255)` 再钳，则 `r = 1.0000001` 会得到 255（尚可），
//    但 `r = 1.003` 会得到 256 → 溢出 8 位（截断后变 0，**黑点**）。
//
// ③ **色度是 2×2 下采样，必须按 `(x>>1, y>>1)` 取，且不得做插值**。
//    docs/04 IR-4 禁止「把滤波结果当成更准的数据」。硬件双线性会让色度
//    被隐式平滑 —— 那不是契约的一部分。本实现一律**最近邻**（一个色度像素
//    服务 2×2 输出像素），与 GPU 侧 `texelFetch` 语义一致。
//
// ── 与 GPU 实现的关系（谁是事实来源）─────────────────────────────────────
//
// 本文件的数学是**事实来源**；`shaders/common/gpu_algo.h` 的参数块
// （`ReconYuvToBgraGpuParams`）是同一数学的**GPU 接口投影**：
//   · 归一化系数（y_bias/y_gain/c_bias/c_gain）与矩阵增益
//     （r_from_cr / b_from_cb / g_from_cr / g_from_cb）都由**本文件**算出，
//     再填进 GPU 参数块 → 「系数只有一处定义」。
//   · 着色器里因此**一个数值字面量都没有**（除 workgroup 尺寸），
//     它只做同式的 FMA。这也是两端着色器能逐字相同的前提。
//
// 本项目此前无法验证这条链路的**任何数值**（无 macOS / 无 Metal / 无 glslang）。
// 下沉后，色彩数学由 `tests/unit/test_platform_yuv.cpp` 在桌面验收（含负向
// 注入证伪），**只剩下「GPU 是否忠实执行这条式子」需要真机**。这是把一个
// 「只能真机发现」的问题缩小成「只剩 FMA 忠实性」的关键一步。

#ifndef RECON_PLATFORM_YUV_CONVERT_H_
#define RECON_PLATFORM_YUV_CONVERT_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace recon {
namespace platform {

// ---------------------------------------------------------------------------
// 平台元数据 → 归一化系数
// ---------------------------------------------------------------------------

/// 色彩矩阵族（决定 kr / kb）。
///
/// 平台层从元数据判定后选择，**不得猜测**：
///   · iOS：`CVPixelBufferGetAttachment(kCVImageBufferYCbCrMatrix)`，取值
///     `kCVImageBufferYCbCrMatrix_ITU_R_601_4` / `_ITU_R_709_2`。
///   · Android：`Image.getColorSpace()` 的 `ColorSpace.Named`。
enum class ColorMatrix {
  /// BT.601（kr=0.299, kb=0.114）。SD 视频与多数手机的预览流默认值。
  kBt601,
  /// BT.709（kr=0.2126, kb=0.0722）。HD（1080p 及以上）常用。
  kBt709,
};

/// 量化范围（决定偏置与增益）。
///
/// ⚠️ 注意本枚举的**色度偏置在两种取值下都是 128** —— 见 `YuvToBgraParams`
/// 的推导。此枚举只影响三个数：`y_bias` / `y_gain` / `c_gain`。
enum class QuantRange {
  /// full range（0–255 全长）：y_bias=0, y_gain=1/255, c_bias=128, c_gain=1/255。
  /// iOS 的 `420YpCbCr8BiPlanar**FullRange**` 属于此类。
  kFull,
  /// limited range（Video range，Y∈[16,235]、C∈[16,240]）：
  /// y_bias=16, y_gain=1/219, c_bias=128, c_gain=1/224。
  /// Android 常见；若误用 full 系数会得到**低对比 + 灰雾**（不报错）。
  kLimited,
};

/// 一组完整的 YUV→RGB 转换系数。**GPU 参数块的字段值即由此结构体填充**。
///
/// 所有系数都是「乘加」形式所需的形式，着色器里直接 `y = (Y − y_bias)·y_gain`：
///   y  ∈ [0, 1]       亮度（归一化）
///   cb/cr ∈ [−0.5, 0.5]  色度（归一化，以 128 为中心）
struct YuvToBgraParams {
  // 归一化
  float y_bias = 0.0f;
  float y_gain = 1.0f;
  float c_bias = 128.0f;
  float c_gain = 1.0f;
  // 矩阵增益（由 kr / kb 派生）
  float r_from_cr = 0.0f;
  float b_from_cb = 0.0f;
  float g_from_cr = 0.0f;
  float g_from_cb = 0.0f;

  /// 是否通过了 `Validate()` 的不变量检查（见下）。
  bool valid = false;
};

/// 由平台元数据算出转换系数。**这是系数的唯一计算处**。
///
/// 派生式（kg = 1 − kr − kb）：
///     r_from_cr = 2·(1 − kr)
///     b_from_cb = 2·(1 − kb)
///     g_from_cr = −2·kr·(1 − kr) / kg
///     g_from_cb = −2·kb·(1 − kb) / kg
///
/// 这些式子是标准 YCbCr→RGB 的等价变形（把 `(Cr−128)` 提出来后的系数），
/// 单测 `TestKnownCoefficients` 用公开参考值逐项核对（BT.601 的 1.402 /
/// 1.772 / −0.344136 / −0.714136 等）。
///
/// @return 系数集合；`valid == false` 表示入参不合法或派生结果非有限
///         （**调用方必须处理**，不得静默继续）。
YuvToBgraParams MakeYuvToBgraParams(ColorMatrix matrix, QuantRange range);

// ---------------------------------------------------------------------------
// 帧级转换
// ---------------------------------------------------------------------------

/// 一帧 BGRA8 结果。`width`/`height` 与 `data.size()` 的唯一事实来源是本结构体。
struct BgraFrame {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  /// 行优先、**紧密排列**（stride == width × 4）的 BGRA8。
  /// 字节序 B,G,R,A —— 与内核 `BgraToGray` 的取值口径一致。
  std::vector<std::uint8_t> data;
  /// 被 clamp 到边界（0 或 255）的通道数。用于诊断色彩矩阵是否失配：
  /// 若失配（例如把 limited 当 full），大量通道会顶到 255 或掉到 0。
  std::size_t clamped_channel_count = 0;

  std::size_t bytes_per_row() const {
    return static_cast<std::size_t>(width) * 4u;
  }
};

/// 转换结果包装（沿用 `DepthConvertResult` 的 `ok` 约定，双端桥接一致）。
struct YuvConvertResult {
  bool ok = false;
  BgraFrame frame;
};

/// 把「Y 全分辨率 + 交错 CbCr 半分辨率」的 4:2:0 缓冲转换为 BGRA8。
///
/// 这是 **iOS `420YpCbCr8BiPlanarFullRange` 与 Android 多数机型（NV12/NV21）**
/// 的实际布局：Cb、Cr **交错**在同一个平面（每色度像素 2 字节）。
///
/// @param y_plane        Y 平面首地址。非空。
/// @param y_stride_bytes Y 平面每行字节数（**可能带行填充**，故按字节偏移取）。
/// @param cbcr_plane     交错色度平面首地址（每色度像素 2 字节）。非空。
/// @param cbcr_stride_bytes 色度平面每行字节数。
/// @param width          输出宽（= Y 平面宽）。不重采样。
/// @param height         输出高（= Y 平面高）。不重采样。
/// @param cb_first       `true` 表示该平面是 **CbCr**（NV12 / iOS）：
///                       偶数偏移字节 = Cb、奇数偏移 = Cr；
///                       `false` 表示 **CrCb**（NV21）。
/// @param params         由 `MakeYuvToBgraParams` 算出。`valid` 必须为 true。
/// @return 结果；`ok == false` 表示参数非法（调用方必须处理）。
///
/// ## 为什么单独提供 `cb_first` 而不在内部读元数据
/// 本文件是**平台无关**的 —— 它不认识 `CVPixelBuffer` / `Image.Plane`。
/// NV12/NV21 的判定属于平台层（iOS 由 `capturedImage` 格式隐含为 NV12 序；
/// Android 由 `Plane[1]` 与 `Plane[2]` 的首地址差值判定）。传布尔量进去，
/// 使「哪个字节是 Cb」这个决定**在平台层只做一次**。
YuvConvertResult ConvertYuv420BiPlanarToBgra8(
    const std::uint8_t* y_plane, std::size_t y_stride_bytes,
    const std::uint8_t* cbcr_plane, std::size_t cbcr_stride_bytes,
    std::uint32_t width, std::uint32_t height, bool cb_first,
    const YuvToBgraParams& params);

/// 把「Y 全分辨率 + Cb、Cr **各自独立**成平面」的 4:2:0 缓冲转换为 BGRA8。
///
/// 对应真平面 I420（部分 Android 机型的 `YUV_420_888` 底层）。
/// GPU 侧**不支持**该布局（缺口 YUV-1），故这条路径是 GPU 不可用时的
/// **显式降级**（调用方须记录降级原因，不得当作正常路径）。
///
/// 参数含义同上，只是 Cb、Cr 分平面。`c_stride_bytes` 为两个色度平面共用的
/// 行字节数（I420 下二者相同；若平台给出不同值，调用方须取更保守者或在
/// 平台层先行规整 —— 本函数不接受两个不同的 stride，以避免「悄悄用错一个」）。
YuvConvertResult ConvertYuv420PlanarToBgra8(
    const std::uint8_t* y_plane, std::size_t y_stride_bytes,
    const std::uint8_t* cb_plane, const std::uint8_t* cr_plane,
    std::size_t c_stride_bytes,
    std::uint32_t width, std::uint32_t height,
    const YuvToBgraParams& params);

}  // namespace platform
}  // namespace recon

#endif  // RECON_PLATFORM_YUV_CONVERT_H_
