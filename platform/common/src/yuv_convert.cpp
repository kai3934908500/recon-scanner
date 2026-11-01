// SPDX-License-Identifier: BSD-3-Clause
//
// yuv_convert.cpp — YUV 4:2:0 → BGRA8 的色彩数学本体。
//
// 规则表、推导与三条易错点见同目录 yuv_convert.h。

#include "recon_platform/yuv_convert.h"

#include <cmath>

namespace recon {
namespace platform {
namespace {

// ---------------------------------------------------------------------------
// 色彩矩阵族的 kr / kb
// ---------------------------------------------------------------------------
// BT.601（SD）：kr=0.299, kb=0.114
// BT.709（HD）：kr=0.2126, kb=0.0722
// 二者之和 < 1（kg = 1 − kr − kb > 0），这是派生式分母有意义的必要条件。
struct KrKb {
  double kr;
  double kb;
};

KrKb KrKbFor(ColorMatrix matrix) {
  switch (matrix) {
    case ColorMatrix::kBt709:
      return {0.2126, 0.0722};
    case ColorMatrix::kBt601:
    default:
      return {0.299, 0.114};
  }
}

// 把 double 收窄为 float 并检查有限性。
bool Finite(double v) { return std::isfinite(v); }

// ---------------------------------------------------------------------------
// 逐像素核心（两种布局共用的同一份数学）
// ---------------------------------------------------------------------------
// 三种布局（交错 CbCr / 交错 CrCb / 平面）**共用**这一段：
// 传进来的已是「整数域的 Y / Cb / Cr」（0–255），本函数只管归一化 + 矩阵 +
// 量化 + 写 BGRA。把「取哪个字节」与「怎么算」彻底分开，是为了让
// **数学只有一份** —— 若为每种布局各写一份转换，就又回到了
// 「同一逻辑写两遍必然漂移」。
inline void ConvertPixel(std::uint8_t y_int, std::uint8_t cb_int,
                         std::uint8_t cr_int, const YuvToBgraParams& p,
                         std::uint8_t* dst_bgra,
                         std::size_t* clamped_channels) {
  // ① 归一化。式与 GPU 着色器**逐字相同**：
  //        y  = (Y  − y_bias) × y_gain
  //        cb = (Cb − c_bias) × c_gain
  //        cr = (Cr − c_bias) × c_gain
  const float y = (static_cast<float>(y_int) - p.y_bias) * p.y_gain;
  const float cb = (static_cast<float>(cb_int) - p.c_bias) * p.c_gain;
  const float cr = (static_cast<float>(cr_int) - p.c_bias) * p.c_gain;

  // ② 矩阵。式同着色器：r = y + r_from_cr·cr；b = y + b_from_cb·cb；
  //    g = y + g_from_cr·cr + g_from_cb·cb。
  const float r = y + p.r_from_cr * cr;
  const float b = y + p.b_from_cb * cb;
  const float g = y + p.g_from_cr * cr + p.g_from_cb * cb;

  // ③ 量化：**先钳后乘**（见头文件易错点 ②）。
  //    用 +0.5 后取整实现四舍五入（与 depth_convert 的口径一致）。
  //    钳位到 [0,1] 做在乘以 255 **之前**，避免 1.0004 → 256 → 截断成 0。
  const float rq = (r < 0.0f) ? 0.0f : (r > 1.0f ? 1.0f : r);
  const float gq = (g < 0.0f) ? 0.0f : (g > 1.0f ? 1.0f : g);
  const float bq = (b < 0.0f) ? 0.0f : (b > 1.0f ? 1.0f : b);

  // 诊断计数：钳位发生过吗？（r/g/b 的**原始**值越界才算）
  if (r < 0.0f || r > 1.0f) ++(*clamped_channels);
  if (g < 0.0f || g > 1.0f) ++(*clamped_channels);
  if (b < 0.0f || b > 1.0f) ++(*clamped_channels);

  // ④ 写出 BGRA8（**内存序 B,G,R,A** —— 与内核 BgraToGray 的取值口径一致）。
  dst_bgra[0] = static_cast<std::uint8_t>(bq * 255.0f + 0.5f);
  dst_bgra[1] = static_cast<std::uint8_t>(gq * 255.0f + 0.5f);
  dst_bgra[2] = static_cast<std::uint8_t>(rq * 255.0f + 0.5f);
  dst_bgra[3] = 255;
}

// 逐帧循环的公共骨架。`fetch` 负责「按输出像素 (x,y) 取出 Y/Cb/Cr 三个整数」，
// 使两种布局只差这一个 lambda，而循环体、量化、写出全部共用。
//
// ⚠️ `fetch` **每像素只调用一次**。早期版本写成
// `std::get<0>(fetch_cc(x,y))` / `std::get<1>(...)` / `std::get<2>(...)` 三次，
// 等于同一像素的取址与解引用做了三遍 —— 1080p 下多出约 1200 万次冗余调用。
// 更重要的是它把「一次取值」误表达为「三次独立取值」，与 GPU 侧
// （一次 `texelFetch` + 一次 `.rg` 读取）**结构上不再对应**，
// 使「两端同构」这一可读性契约被悄悄破坏。故显式取一次再解包。
struct YccSample {
  std::uint8_t y;
  std::uint8_t cb;
  std::uint8_t cr;
};

template <typename FetchChroma>
YuvConvertResult ConvertImpl(std::uint32_t width, std::uint32_t height,
                             const YuvToBgraParams& params,
                             FetchChroma&& fetch) {
  YuvConvertResult out;
  if (width == 0 || height == 0 || !params.valid) {
    return out;  // ok == false
  }

  BgraFrame frame;
  frame.width = width;
  frame.height = height;
  const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
  frame.data.assign(row_bytes * height, 0);

  for (std::uint32_t y = 0; y < height; ++y) {
    std::uint8_t* dst_row = frame.data.data() + static_cast<std::size_t>(y) * row_bytes;
    for (std::uint32_t x = 0; x < width; ++x) {
      const YccSample s = fetch(x, y);          // ← 每像素**一次**
      ConvertPixel(s.y, s.cb, s.cr, params, dst_row + static_cast<std::size_t>(x) * 4u,
                   &frame.clamped_channel_count);
    }
  }

  out.ok = true;
  out.frame = std::move(frame);
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// 系数派生
// ---------------------------------------------------------------------------
YuvToBgraParams MakeYuvToBgraParams(ColorMatrix matrix, QuantRange range) {
  YuvToBgraParams p;

  const KrKb k = KrKbFor(matrix);
  const double kr = k.kr;
  const double kb = k.kb;
  const double kg = 1.0 - kr - kb;

  // kg 是 g_from_cr / g_from_cb 的分母。标准取值下恒 > 0
  // （BT.601: 0.587；BT.709: 0.7152）。为 0 意味着矩阵定义不成立 ——
  // 此时**拒绝**而不是除以 0 得到 inf 污染整条链路（inf → 量化 → 静默黑屏）。
  if (!Finite(kg) || kg <= 0.0) {
    return p;  // valid == false
  }

  // 派生式（见头文件）：
  const double r_from_cr = 2.0 * (1.0 - kr);
  const double b_from_cb = 2.0 * (1.0 - kb);
  const double g_from_cr = -2.0 * kr * (1.0 - kr) / kg;
  const double g_from_cb = -2.0 * kb * (1.0 - kb) / kg;

  // 归一化系数。**c_bias 恒为 128**（两种 range 都是），只有 c_gain 不同 ——
  // 见头文件易错点 ①。这里刻意把 c_bias 写在两个分支里都赋 128 而不是
  // 用常量一次赋值：将来若出现「色度中心非 128」的新范围（如 10-bit 的
  // 512），两处都会提醒到，而共用常量会漏掉一处。
  if (range == QuantRange::kLimited) {
    p.y_bias = 16.0f;
    p.y_gain = 1.0f / 219.0f;
    p.c_bias = 128.0f;
    p.c_gain = 1.0f / 224.0f;
  } else {
    p.y_bias = 0.0f;
    p.y_gain = 1.0f / 255.0f;
    p.c_bias = 128.0f;
    p.c_gain = 1.0f / 255.0f;
  }

  if (!Finite(r_from_cr) || !Finite(b_from_cb) || !Finite(g_from_cr) ||
      !Finite(g_from_cb)) {
    return p;  // valid == false
  }

  p.r_from_cr = static_cast<float>(r_from_cr);
  p.b_from_cb = static_cast<float>(b_from_cb);
  p.g_from_cr = static_cast<float>(g_from_cr);
  p.g_from_cb = static_cast<float>(g_from_cb);
  p.valid = true;
  return p;
}

// ---------------------------------------------------------------------------
// 交错（半）平面：CbCr（NV12 / iOS）或 CrCb（NV21）
// ---------------------------------------------------------------------------
YuvConvertResult ConvertYuv420BiPlanarToBgra8(
    const std::uint8_t* y_plane, std::size_t y_stride_bytes,
    const std::uint8_t* cbcr_plane, std::size_t cbcr_stride_bytes,
    std::uint32_t width, std::uint32_t height, bool cb_first,
    const YuvToBgraParams& params) {
  YuvConvertResult out;
  if (y_plane == nullptr || cbcr_plane == nullptr) {
    return out;
  }
  // stride 必须至少容纳一整行。**声明非法时拒绝而非猜测**
  // （猜测会让「行填充」变成静默的整幅错位）。
  if (y_stride_bytes < static_cast<std::size_t>(width)) {
    return out;
  }
  // 色度平面一行需要 ceil(width/2) 个色度像素 × 2 字节。
  const std::size_t cbcr_row_bytes =
      static_cast<std::size_t>((width + 1u) / 2u) * 2u;
  if (cbcr_stride_bytes < cbcr_row_bytes) {
    return out;
  }

  const std::size_t cb_off = cb_first ? 0u : 1u;
  const std::size_t cr_off = cb_first ? 1u : 0u;

  return ConvertImpl(width, height, params,
                     [&](std::uint32_t x, std::uint32_t y) {
                       const std::uint8_t yy =
                           y_plane[static_cast<std::size_t>(y) * y_stride_bytes + x];
                       // 4:2:0：色度 2×2 下采样 → 右移一位（最近邻，不插值）。
                       const std::uint8_t* c =
                           cbcr_plane + static_cast<std::size_t>(y >> 1) * cbcr_stride_bytes +
                           static_cast<std::size_t>(x >> 1) * 2u;
                       return YccSample{yy, c[cb_off], c[cr_off]};
                     });
}

// ---------------------------------------------------------------------------
// 真平面 I420（部分 Android 机型）—— GPU 不可用时的显式降级路径
// ---------------------------------------------------------------------------
YuvConvertResult ConvertYuv420PlanarToBgra8(
    const std::uint8_t* y_plane, std::size_t y_stride_bytes,
    const std::uint8_t* cb_plane, const std::uint8_t* cr_plane,
    std::size_t c_stride_bytes,
    std::uint32_t width, std::uint32_t height,
    const YuvToBgraParams& params) {
  YuvConvertResult out;
  if (y_plane == nullptr || cb_plane == nullptr || cr_plane == nullptr) {
    return out;
  }
  if (y_stride_bytes < static_cast<std::size_t>(width)) {
    return out;
  }
  if (c_stride_bytes < static_cast<std::size_t>((width + 1u) / 2u)) {
    return out;
  }

  return ConvertImpl(width, height, params,
                     [&](std::uint32_t x, std::uint32_t y) {
                       const std::uint8_t yy =
                           y_plane[static_cast<std::size_t>(y) * y_stride_bytes + x];
                       // 每色度像素 1 字节（真平面），故按 (x>>1) 索引。
                       const std::size_t ci =
                           static_cast<std::size_t>(y >> 1) * c_stride_bytes + (x >> 1);
                       return YccSample{yy, cb_plane[ci], cr_plane[ci]};
                     });
}

}  // namespace platform
}  // namespace recon
