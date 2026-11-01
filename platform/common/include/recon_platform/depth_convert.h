// SPDX-License-Identifier: BSD-3-Clause
//
// depth_convert.h — 平台深度缓冲 → C ABI 要求格式的换算（平台无关部分）。
//
// 契约：
//   · core/include/recon/recon_c_api.h —— `recon_frame_packet.depth_data`
//     是「uint16 毫米」，`depth_stride` 为每行字节数。
//   · docs/02_采集与同步.md §5.5 F-1（深度单位固定为毫米、沿光轴 z）
//   · docs/04_RGBD管线.md §3.6 IR-1~IR-6（保留原始深度副本铁律）
//   · docs/11_平台权限与集成.md §3.7（平台 → 核心层字段对接）
//
// ── 为什么放在 platform/common ─────────────────────────────────────────────
//
// 铁律：**同一逻辑写两遍必然漂移**。iOS 的 `ARDepthData.depthMap`
// 是 `kCVPixelFormatType_DepthFloat32`（米），Android 的 Camera2 `DEPTH16`
// 是毫米、`ARCore Depth API` 又有各自的单位口径 —— 但**「逐像素把浮点米
// 换算成 uint16 毫米」这一步的规则是同一件事**，且它直接进入 TSDF 积分，
// 任何一侧的取整方式漂移（`(uint16)mm` 的向零截断 versus 四舍五入）都会带来
// **系统性**米级偏置以外的小偏置，属「不报错但结果偏」。
//
// 故单位换算的**数学**以一份 C++17 实现承载；各端只负责把「自己格式的缓冲」
// 喂进来（像素格式判定、平面取址、YCbCr→RGB 等**真平台相关**的活留在各端）。
//
// ── 与 docs/04 IR-1 的关系 ────────────────────────────────────────────────
//
// IR-1 是**铁律级**规定：「原始深度帧按原生分辨率、原生单位（uint16 毫米）
// 逐帧落盘……不得重采样、不得转码为 float32、不得丢帧」。IR-4 进一步规定：
// 任何滤波结果都**不得**被当作「更准的深度」。因此本函数**只做单位换算**：
// 一个像素都不合并、一个像素都不平滑、分辨率严格不变。

#ifndef RECON_PLATFORM_DEPTH_CONVERT_H_
#define RECON_PLATFORM_DEPTH_CONVERT_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace recon {
namespace platform {

/// 深度换算的像素上限（毫米）。65535 mm ≈ 65.5 m。
///
/// 远超 `scan_viewpoint_distance_m` 的值域 `[0.25, 1.20]`（docs/08 G12），
/// 故触顶者必为噪声/远景，夹取而非丢弃，以保住「分辨率不变」这一铁律。
constexpr std::uint16_t kDepthMaxMM = 65535;

/// 一帧已换算为毫米的深度数据。
///
/// `width` / `height` 与 `millimeters.size()` 的**唯一事实来源是本结构体**
/// —— 调用方不得自行从平台缓冲再读一次尺寸（第二事实来源必然漂移）。
struct DepthFrameMM {
  /// 深度图宽度（像素）。**等于平台原生分辨率**（IR-1：不重采样）。
  std::uint32_t width = 0;
  /// 深度图高度（像素）。
  std::uint32_t height = 0;
  /// 行优先、紧密排列（stride == width）的毫米深度。0 = 无效。
  std::vector<std::uint16_t> millimeters;
  /// 有效像素数（**深度非零**，含被上限夹取的触顶像素）。
  ///
  /// 口径依据：core `depth_preprocess.h` 的 `valid_input` = 「深度 ≠ 0 且过置信
  /// 硬门槛」（RM-4）。触顶像素的深度是 65535（非零），故**计入有效**；其"是否
  /// 可信"由后续的飞行点剔除与 TSDF 截断负责，不在本层裁决。
  ///
  /// ⚠️ 本注释曾误写为「满足 `0 < d < 上限`」，与代码及 core 口径矛盾
  /// （`0 < d < 上限` 会排除触顶者）。由 `test_platform_capture.cpp` 的
  /// `TestDepthClampAtMax` 抓出并锁死为「含触顶」。
  std::size_t valid_pixel_count = 0;
  /// 被上限截断的像素数（原始深度 ≥ 65.535 m 后被夹到 65535）。
  ///
  /// **是 `valid_pixel_count` 的子集**（触顶像素仍然有效）。两者不是互补关系
  /// —— 「有效 vs 无效」的判据是「深度是否为零」，「是否触顶」是另一个维度。
  ///
  /// 该值**不应**被静默忽略：持续非零意味着深度图里混入了远超扫描距离的
  /// 数值（噪声或远景），属可诊断信号，故显式带回供上层上报。
  std::size_t truncated_pixel_count = 0;

  /// 有效像素占比 [0,1]。供扫描自检与 U-03 的深度质量提示使用。
  double valid_ratio() const {
    const std::size_t total = static_cast<std::size_t>(width) * height;
    return total > 0 ? static_cast<double>(valid_pixel_count) /
                           static_cast<double>(total)
                     : 0.0;
  }
};

/// 把「单平面 Float32、单位米、沿光轴 z」的深度缓冲换算为 uint16 毫米。
///
/// @param src          源缓冲首地址（Float32）。非空。
/// @param width        深度图宽度（像素，>0）。
/// @param height       深度图高度（像素，>0）。
/// @param src_stride_bytes 源缓冲每行字节数（可能带行填充，故按字节偏移取值）。
/// @return 换算结果；`ok == false` 表示参数非法（**调用方必须处理**，不得
///         静默继续）。
///
/// ## 换算规则（逐像素、无跨像素依赖 ⇒ 分辨率严格不变）
///
/// | 输入 | 输出 | 理由 |
/// |---|---|---|
/// | `NaN` / `±Inf` | `0` | 无效；`is_finite` 是唯一可靠判据 |
/// | `≤ 0` | `0` | 平台用 0 表示无回波；负值物理上不可能 |
/// | `0 < m < 65.535` | `round(m × 1000)` | 米 → 毫米 |
/// | `≥ 65.535` | `65535` | 夹取（见 `kDepthMaxMM`） |
///
/// ## 为什么取整用四舍五入而非向零截断
///
/// `UInt16(mm)` 式的向零截断会带来系统性 `-0.5 mm` 偏置（在 0.35 m 处约
/// 1.4e-3 相对误差），虽小但会进入 TSDF 的系统误差。故显式 `+0.5` 后取整。
///
/// ## 为什么不提供输出 stride 参数
///
/// 内核内部按 `width * 4`（RGB）／`width * 2`（深度）紧排消费；平台层若需要
/// 行填充，应在**投递时**按 C ABI 的 `depth_stride` 字段声明，而不是在这里
/// 产出带填充的缓冲 —— 那会让「本结构体的尺寸即唯一事实来源」失效。
struct DepthConvertResult {
  bool ok = false;
  /// `ok == true` 时有效。
  DepthFrameMM frame;
};

DepthConvertResult ConvertDepthFloat32MetersToMM16(
    const float* src, std::uint32_t width, std::uint32_t height,
    std::size_t src_stride_bytes);

}  // namespace platform
}  // namespace recon

#endif  // RECON_PLATFORM_DEPTH_CONVERT_H_
