// SPDX-License-Identifier: BSD-3-Clause
//
// YUV → BGRA 色彩数学的单元测试（docs/14 §5.1 平台适配层）。
//
// 覆盖 `platform/common/src/yuv_convert.cpp` —— 从 iOS / Android **都**需要的
// 「YUV 4:2:0 → BGRA8」这一步里抽出来的**平台无关数学**。
//
// ── 为什么这份测试是本轮最有价值的东西 ────────────────────────────────────
//
// 此前 `FrameConverter.bgra8` 是空壳（恒返回 nil）→ RGB 一帧都进不去
// （docs/14 §5.1.3 缺口 J）。而实现它所需的 YCbCr→RGB 数学**恰好是本项目
// 最难验证的一类代码**：
//   · 它只存在于平台层（Swift / Kotlin），本机**不能编译**；
//   · 它的错误表现是「颜色整体偏」而不是崩溃 —— 不报错、不抛异常；
//   · GPU 版（Metal / Vulkan 着色器）本机**连语法都验证不了**（无 xcrun/glslang）。
//
// 把数学下沉 `platform/common` 后，它变成**纯 C++、可在桌面跑**的函数 →
// 这份测试就能把「系数算错」「色度中心取错」「先乘后钳溢出」这些坑
// **在提交前当场抓住**，而不是等一台 iPhone。
//
// ── 与 GPU 侧的关系 ───────────────────────────────────────────────────────
//
// 本测试验证的数学与 `shaders/metal/yuv.metal` /
// `shaders/vulkan/recon_yuv_to_bgra.comp` 里那几行 FMA **逐字同式**
// （两端着色器刻意不写任何数值字面量，全部经 `ReconYuvToBgraGpuParams` 传入，
// 而该参数块的字段值正由 `MakeYuvToBgraParams` 填充）。
// 故：本测试绿 ⇒ 数学对；真机只剩「GPU 是否忠实执行这几行 FMA」待验。
//
// 每条断言都对应一个**具体的错误实现**（把实现改回错误写法，本测试必失败）。

#include "recon_platform/yuv_convert.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using recon::platform::BgraFrame;
using recon::platform::ColorMatrix;
using recon::platform::ConvertYuv420BiPlanarToBgra8;
using recon::platform::ConvertYuv420PlanarToBgra8;
using recon::platform::MakeYuvToBgraParams;
using recon::platform::QuantRange;
using recon::platform::YuvToBgraParams;

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::printf("  [FAIL] %s\n", what);
    ++g_failures;
  }
}

// 浮点近似比较（系数核对用）。
bool Near(float a, float b, float eps = 1e-5f) {
  return std::fabs(a - b) <= eps;
}

// 取某个输出像素的 BGRA 四元组。
struct Rgba {
  int b, g, r, a;
};
Rgba PixelAt(const BgraFrame& f, std::uint32_t x, std::uint32_t y) {
  const std::size_t off = static_cast<std::size_t>(y) * f.bytes_per_row() +
                          static_cast<std::size_t>(x) * 4u;
  return Rgba{static_cast<int>(f.data[off + 0]), static_cast<int>(f.data[off + 1]),
              static_cast<int>(f.data[off + 2]), static_cast<int>(f.data[off + 3])};
}

// ------------------------------------------------------------------------- //
// ① 系数派生：与公开参考值逐项核对
// ------------------------------------------------------------------------- //
//
// 参考值（标准 BT.601 / BT.709 的 YCbCr→RGB 系数，取 4~6 位有效数字）：
//
//   BT.601: r_from_cr=1.402    b_from_cb=1.772
//           g_from_cr=-0.714136  g_from_cb=-0.344136
//   BT.709: r_from_cr=1.5748   b_from_cb=1.8556
//           g_from_cr=-0.468124  g_from_cb=-0.187324
//
// ⚠️ 上组期望值**不是**凭印象写的，而是由标准整数式独立推导后填入：
//        R = Y + 1.402·(Cr−128)
//        G = Y − 0.344136·(Cb−128) − 0.714136·(Cr−128)
//        B = Y + 1.772·(Cb−128)
//   注意 G 里 **Cr 的系数（−0.714136）绝对值更大** —— 因为 kg=0.587 小于 kr。
//   本测试首版正是把 `g_from_cr` / `g_from_cb` 的期望值**写反了**（互相调换），
//   被实现当场判失败。这恰好演示了这份测试的价值：它对**两侧**都设防 ——
//   既抓实现写错，也抓**期望值本身写错**（否则「改成让测试过」就会变成
//   把实现改坏）。核对方式不是"看哪个能过"，而是回到上面的标准式重推。
//
// 若把 kr/kb 写反（0.299↔0.114）、或把 g_from_* 的公式写成无 kg 归一化的
// 简化式，本测试立刻失败。
void TestKnownCoefficients() {
  std::printf("[TEST] 系数派生：BT.601 / BT.709 与标准式独立推导值一致\n");

  const auto p601 = MakeYuvToBgraParams(ColorMatrix::kBt601, QuantRange::kFull);
  Check(p601.valid, "BT.601 系数 valid");
  Check(Near(p601.r_from_cr, 1.402f), "BT.601 r_from_cr = 1.402");
  Check(Near(p601.b_from_cb, 1.772f), "BT.601 b_from_cb = 1.772");
  Check(Near(p601.g_from_cr, -0.714136f, 1e-5f), "BT.601 g_from_cr = -0.714136");
  Check(Near(p601.g_from_cb, -0.344136f, 1e-5f), "BT.601 g_from_cb = -0.344136");

  const auto p709 = MakeYuvToBgraParams(ColorMatrix::kBt709, QuantRange::kFull);
  Check(p709.valid, "BT.709 系数 valid");
  Check(Near(p709.r_from_cr, 1.5748f), "BT.709 r_from_cr = 1.5748");
  Check(Near(p709.b_from_cb, 1.8556f), "BT.709 b_from_cb = 1.8556");
  Check(Near(p709.g_from_cr, -0.468124f, 1e-5f), "BT.709 g_from_cr = -0.468124");
  Check(Near(p709.g_from_cb, -0.187324f, 1e-5f), "BT.709 g_from_cb = -0.187324");

  // 两个矩阵族必须**确实不同** —— 否则「选择」这件事没有意义
  // （防止有人把两个分支写成同一组常数而测试仍过）。
  Check(!Near(p601.r_from_cr, p709.r_from_cr, 1e-3f),
        "BT.601 与 BT.709 的 r_from_cr 不同（选择生效）");

  // 结构性不变量：两个 g 系数都应为负（色差对 G 是**减**关系）。
  // 若有人把负号漏了，G 会随色度增大而增大 → 整体色彩完全错乱。
  Check(p601.g_from_cr < 0.0f && p601.g_from_cb < 0.0f,
        "两个 g 系数均为负（色差对 G 是减关系）");
}

// ------------------------------------------------------------------------- //
// ② 归一化系数：色度中心恒为 128（易错点 ①）
// ------------------------------------------------------------------------- //
void TestNormalizationCoefficients() {
  std::printf("[TEST] 归一化系数：色度中心 128，full/limited 的增益差异\n");

  const auto full = MakeYuvToBgraParams(ColorMatrix::kBt601, QuantRange::kFull);
  Check(full.y_bias == 0.0f, "full: y_bias = 0");
  Check(Near(full.y_gain, 1.0f / 255.0f), "full: y_gain = 1/255");
  Check(full.c_bias == 128.0f, "full: c_bias = 128（**不是 0、也不是 127.5**）");
  Check(Near(full.c_gain, 1.0f / 255.0f), "full: c_gain = 1/255");

  const auto lim = MakeYuvToBgraParams(ColorMatrix::kBt601, QuantRange::kLimited);
  Check(lim.y_bias == 16.0f, "limited: y_bias = 16");
  Check(Near(lim.y_gain, 1.0f / 219.0f), "limited: y_gain = 1/219");
  Check(lim.c_bias == 128.0f, "limited: c_bias = **128**（与 full 相同）");
  Check(Near(lim.c_gain, 1.0f / 224.0f), "limited: c_gain = 1/224");

  // 两种 range 的色度中心必须相等 —— 这是易错点①的形式化断言。
  Check(full.c_bias == lim.c_bias, "两种 range 的 c_bias 相等（均 128）");
  // 而增益必须不同（否则 limited 分支形同虚设）。
  Check(!Near(full.c_gain, lim.c_gain), "两种 range 的 c_gain 不同");
}

// ------------------------------------------------------------------------- //
// ③ 中性灰：Y=任意、Cb=Cr=128 → R=G=B=Y（**整幅偏色的哨兵测试**）
// ------------------------------------------------------------------------- //
//
// 这是**最直接的偏色检测**：128 是色度的「零点」（无色）。
//   · 若 c_bias 误取 0   → cb = 128/255 ≈ 0.502，R/G/B 都被推偏 → 失败
//   · 若 c_bias 误取 127.5 → cb ≈ 0.00196 → 有轻微但**系统性**的偏色 → 失败
//   · 若 c_gain 误取 1/219（把 Y 的增益套到色度上）→ 偏色 → 失败
void TestNeutralGrayHasNoColorCast() {
  std::printf("[TEST] 中性灰（Cb=Cr=128）不产生偏色\n");

  const auto p = MakeYuvToBgraParams(ColorMatrix::kBt601, QuantRange::kFull);
  // 1×1 交错平面：Y=100, Cb=128, Cr=128
  const std::uint8_t y_plane[1] = {100};
  const std::uint8_t cbcr[2] = {128, 128};

  const auto r = ConvertYuv420BiPlanarToBgra8(y_plane, 1, cbcr, 2, 1, 1, true, p);
  Check(r.ok, "转换 ok");
  const Rgba px = PixelAt(r.frame, 0, 0);
  Check(px.r == 100 && px.g == 100 && px.b == 100,
        "中性灰 → R=G=B=100（任一不等即偏色）");
  Check(px.a == 255, "alpha = 255");
  Check(r.frame.clamped_channel_count == 0, "中性灰无通道被钳位");
}

// ------------------------------------------------------------------------- //
// ④ 纯色：已知 (Y,Cb,Cr) → 已知 RGB
// ------------------------------------------------------------------------- //
//
// 用 BT.601 full range 的标准算式**手算**期望值，独立于实现：
//   红：Y=76,  Cb=84,  Cr=255
//     r = (76/255) + 1.402·((255−128)/255) = 0.29804 + 1.402·0.49804 ≈ 0.99629 → 254
//     g = 0.29804 − 0.714136·((255−128)/255) − 0.344136·((84−128)/255)
//       = 0.29804 − 0.35566 + 0.05939 ≈ 0.00177 → 1~2
//     b = 0.29804 + 1.772·((84−128)/255) = 0.29804 − 0.30575 ≈ −0.00771 → 钳 0
void TestKnownColorRed() {
  std::printf("[TEST] 已知色：BT.601 full range 的「红」\n");

  const auto p = MakeYuvToBgraParams(ColorMatrix::kBt601, QuantRange::kFull);
  const std::uint8_t y_plane[1] = {76};
  const std::uint8_t cbcr[2] = {84, 255};  // Cb=84, Cr=255（NV12 序）

  const auto r = ConvertYuv420BiPlanarToBgra8(y_plane, 1, cbcr, 2, 1, 1, true, p);
  Check(r.ok, "ok");
  const Rgba px = PixelAt(r.frame, 0, 0);
  // 容差 ±2：手算的舍入路径与实现略有差异（实现先钳后乘）。
  Check(std::abs(px.r - 254) <= 2, "R ≈ 254");
  // G ≈ 1.8：**这同时验证了两个 g 系数都参与了运算**（只用一个也可得 0~5，
  // 但把两者调换会得 ~68 → 失败）。严格的逐系数判据在 TestKnownCoefficients。
  Check(px.g <= 5, "G ≈ 2（两个 g 系数共同作用的结果）");
  Check(px.b <= 2, "B ≈ 0（负值被钳位）");
}

// 已知色「蓝」：独立验证 b_from_cb 与 g_from_cb 的**联合**取值。
//   蓝：Y=29, Cb=255, Cr=107
//     r = (29/255) + 1.402·((107−128)/255) = 0.11373 − 0.11546 ≈ −0.00173 → 0
//     b = 0.11373 + 1.772·((255−128)/255) = 0.11373 + 0.88259 ≈ 0.99632 → 254
//     g = 0.11373 − 0.714136·(−0.08235) − 0.344136·0.49804
//       = 0.11373 + 0.05881 − 0.17140 ≈ 0.00114 → 0~1
void TestKnownColorBlue() {
  std::printf("[TEST] 已知色：BT.601 full range 的「蓝」\n");

  const auto p = MakeYuvToBgraParams(ColorMatrix::kBt601, QuantRange::kFull);
  const std::uint8_t y_plane[1] = {29};
  const std::uint8_t cbcr[2] = {255, 107};

  const auto r = ConvertYuv420BiPlanarToBgra8(y_plane, 1, cbcr, 2, 1, 1, true, p);
  Check(r.ok, "ok");
  const Rgba px = PixelAt(r.frame, 0, 0);
  Check(px.b >= 252, "B ≈ 254");
  Check(px.r <= 2, "R ≈ 0（负值被钳位）");
  Check(px.g <= 5, "G ≈ 1");
}

// ------------------------------------------------------------------------- //
// ⑤ NV21（CrCb 序）与 NV12（CbCr 序）必须得到**不同**结果且彼此互换
// ------------------------------------------------------------------------- //
//
// 这是「cb_first 传错」的哨兵：若实现忽略 cb_first（把 byte[size_t(0)] 硬当 Cb），
// 则同一块缓冲在两种声明下产出**相同**结果 —— 而正确实现必须产出不同值。
void TestCbCrOrderMatters() {
  std::printf("[TEST] 交错顺序：NV12(CbCr) vs NV21(CrCb)\n");

  const auto p = MakeYuvToBgraParams(ColorMatrix::kBt601, QuantRange::kFull);
  const std::uint8_t y_plane[1] = {100};
  // 平面里两个字节故意取**差异大**的值，使顺序影响明显：byte0=200, byte1=50
  const std::uint8_t cbcr[2] = {200, 50};

  const auto nv12 = ConvertYuv420BiPlanarToBgra8(y_plane, 1, cbcr, 2, 1, 1, true, p);
  const auto nv21 = ConvertYuv420BiPlanarToBgra8(y_plane, 1, cbcr, 2, 1, 1, false, p);
  Check(nv12.ok && nv21.ok, "两种顺序都 ok");

  const Rgba a = PixelAt(nv12.frame, 0, 0);
  const Rgba b = PixelAt(nv21.frame, 0, 0);
  // NV12：Cb=200, Cr=50 → R 低 B 高；NV21：Cb=50, Cr=200 → R 高 B 低。
  Check(a.b != b.b || a.r != b.r, "两种顺序的结果不同（cb_first 生效）");
  Check(a.r < b.r, "NV12 的 R 低于 NV21（因 Cr 分别为 50 / 200）");
  Check(a.b > b.b, "NV12 的 B 高于 NV21（因 Cb 分别为 200 / 50）");
}

// ------------------------------------------------------------------------- //
// ⑥ 4:2:0 下采样：2×2 输出像素共享**同一**色度像素（最近邻，不插值）
// ------------------------------------------------------------------------- //
void TestChromaSubsamplingNearest() {
  std::printf("[TEST] 4:2:0：2×2 像素共享同一色度值（最近邻）\n");

  const auto p = MakeYuvToBgraParams(ColorMatrix::kBt601, QuantRange::kFull);
  // 2×2 亮度全相同，色度平面只有 1 个色度像素（Cb=84,Cr=255 红）。
  const std::uint8_t y_plane[4] = {76, 76, 76, 76};
  const std::uint8_t cbcr[2] = {84, 255};

  const auto r = ConvertYuv420BiPlanarToBgra8(y_plane, 2, cbcr, 2, 2, 2, true, p);
  Check(r.ok, "ok");
  Check(r.frame.data.size() == 2u * 2u * 4u, "输出 2×2 BGRA8");

  const Rgba p00 = PixelAt(r.frame, 0, 0);
  const Rgba p10 = PixelAt(r.frame, 1, 0);
  const Rgba p01 = PixelAt(r.frame, 0, 1);
  const Rgba p11 = PixelAt(r.frame, 1, 1);
  // 四个像素必须**完全相同** —— 只有一个色度源、不插值。
  Check(p00.r == p10.r && p00.r == p01.r && p00.r == p11.r,
        "2×2 的 R 全相同（无插值）");
  Check(p00.g == p11.g && p00.b == p11.b, "2×2 的 G/B 全相同");
  Check(p00.r > 200, "呈红色（R 高）");
}

// ------------------------------------------------------------------------- //
// ⑦ 行填充（stride > width）必须被正确跨过
// ------------------------------------------------------------------------- //
//
// 平台缓冲常带行填充（`CVPixelBufferGetBytesPerRowOfPlane` 返回值可能
// 大于 width）。若实现忽略 stride 用 compact 索引，第 2 行起全错 —— 静默。
void TestRowPaddingHandled() {
  std::printf("[TEST] 行填充：stride > width 时跨过填充字节\n");

  const auto p = MakeYuvToBgraParams(ColorMatrix::kBt601, QuantRange::kFull);
  // Y stride = 4（width=2 + 2 填充）；CbCr stride = 4（1 色度像素需 2 B + 2 填充）
  const std::uint8_t y_plane[8] = {
      10, 20, 0xEE, 0xEE,   // 行 0：Y=10,20，后两字节是填充（毒值 0xEE）
      30, 40, 0xEE, 0xEE};  // 行 1
  const std::uint8_t cbcr[8] = {
      128, 128, 0xEE, 0xEE,  // 色度行 0
      128, 128, 0xEE, 0xEE}; // 色度行 1

  const auto r = ConvertYuv420BiPlanarToBgra8(y_plane, 4, cbcr, 4, 2, 2, true, p);
  Check(r.ok, "ok");
  // 若 stride 被忽略，行 1 会读到毒值 0xEE → 亮度 238；正确实现应为 30/40。
  Check(PixelAt(r.frame, 0, 0).r == 10, "(0,0) Y=10");
  Check(PixelAt(r.frame, 1, 0).r == 20, "(1,0) Y=20");
  Check(PixelAt(r.frame, 0, 1).r == 30, "(0,1) Y=30（**跨过了行填充**）");
  Check(PixelAt(r.frame, 1, 1).r == 40, "(1,1) Y=40");
}

// ------------------------------------------------------------------------- //
// ⑧ 量化：负值/超界被钳位，且**不溢出**（易错点 ②）
// ------------------------------------------------------------------------- //
//
// 构造使 r 原始值 > 1 的输入（Y 大 + Cr 极大），验证：
//   · 输出 ≤ 255（**不是** 256 截断成 0）
//   · clusters 计数被记上（诊断信号）
void TestClampBeforeScale() {
  std::printf("[TEST] 量化：先钳后乘，不产生 256 溢出\n");

  const auto p = MakeYuvToBgraParams(ColorMatrix::kBt601, QuantRange::kFull);
  // Y=255, Cr=255 → r = 1.0 + 1.402·0.49804 ≈ 1.698 → 必须钳到 255。
  const std::uint8_t y_plane[1] = {255};
  const std::uint8_t cbcr[2] = {128, 255};

  const auto r = ConvertYuv420BiPlanarToBgra8(y_plane, 1, cbcr, 2, 1, 1, true, p);
  Check(r.ok, "ok");
  const Rgba px = PixelAt(r.frame, 0, 0);
  Check(px.r == 255, "超界 R 被钳到 255（若实现先乘后钳会得到 0）");
  Check(r.frame.clamped_channel_count > 0, "钳位次数被记上（诊断信号）");
}

// ------------------------------------------------------------------------- //
// ⑨ 真平面 I420 路径（GPU 缺口 YUV-1 的 CPU 回退）
// ------------------------------------------------------------------------- //
void TestPlanarPath() {
  std::printf("[TEST] 真平面 I420（CPU 回退路径）\n");

  const auto p = MakeYuvToBgraParams(ColorMatrix::kBt601, QuantRange::kFull);
  // 2×2：Y 全 100；1 个色度像素 Cb=128, Cr=128（中性灰）。
  const std::uint8_t y_plane[4] = {100, 100, 100, 100};
  const std::uint8_t cb[1] = {128};
  const std::uint8_t cr[1] = {128};

  const auto r = ConvertYuv420PlanarToBgra8(y_plane, 2, cb, cr, 1, 2, 2, p);
  Check(r.ok, "ok");
  Check(PixelAt(r.frame, 0, 0).r == 100, "平面路径中性灰 R=100");
  Check(PixelAt(r.frame, 1, 1).g == 100, "平面路径中性灰 G=100");

  // 与交错路径的**中性灰**结果必须一致（两条路径的数学同一份）。
  const std::uint8_t cbcr[2] = {128, 128};
  const auto bi = ConvertYuv420BiPlanarToBgra8(y_plane, 2, cbcr, 2, 2, 2, true, p);
  Check(bi.ok && bi.frame.data == r.frame.data,
        "交错 vs 平面在等价输入下产出逐字节相同");
}

// ------------------------------------------------------------------------- //
// ⑩ 有限性：NaN / Inf 不得进入输出
// ------------------------------------------------------------------------- //
//
// 即使系数合法，极端输入（若将来平台给出 16-bit 值）也可能溢出。
// 这里用「构造非法系数」验证防线：valid=false 的系数必须被拒绝。
void TestInvalidParamsRejected() {
  std::printf("[TEST] 非法输入：拒绝而非尽力而为\n");

  const std::uint8_t y_plane[1] = {100};
  const std::uint8_t cbcr[2] = {128, 128};

  // 默认构造的 YuvToBgraParams.valid == false → 必须拒绝。
  const YuvToBgraParams bad;
  Check(!bad.valid, "默认构造的系数 valid == false");
  const auto r = ConvertYuv420BiPlanarToBgra8(y_plane, 1, cbcr, 2, 1, 1, true, bad);
  Check(!r.ok, "valid==false 的系数 → ok == false（不产出）");

  // 空指针 / 零尺寸 / stride 不足 → 一律拒绝（不得越界读）。
  const auto good = MakeYuvToBgraParams(ColorMatrix::kBt601, QuantRange::kFull);
  Check(!ConvertYuv420BiPlanarToBgra8(nullptr, 1, cbcr, 2, 1, 1, true, good).ok,
        "Y 平面为空 → ok == false");
  Check(!ConvertYuv420BiPlanarToBgra8(y_plane, 1, nullptr, 2, 1, 1, true, good).ok,
        "色度平面为空 → ok == false");
  Check(!ConvertYuv420BiPlanarToBgra8(y_plane, 1, cbcr, 2, 0, 1, true, good).ok,
        "width 为 0 → ok == false");
  Check(!ConvertYuv420BiPlanarToBgra8(y_plane, 0, cbcr, 2, 1, 1, true, good).ok,
        "Y stride 不足一行 → ok == false（不越界读）");
}

}  // namespace

int main() {
  std::printf("=== YUV → BGRA 色彩数学单元测试（platform/common/yuv_convert）===\n");
  TestKnownCoefficients();
  TestNormalizationCoefficients();
  TestNeutralGrayHasNoColorCast();
  TestKnownColorRed();
  TestKnownColorBlue();
  TestCbCrOrderMatters();
  TestChromaSubsamplingNearest();
  TestRowPaddingHandled();
  TestClampBeforeScale();
  TestPlanarPath();
  TestInvalidParamsRejected();

  if (g_failures == 0) {
    std::printf("全部通过。\n");
    return 0;
  }
  std::printf("失败 %d 项。\n", g_failures);
  return 1;
}
