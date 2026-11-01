// SPDX-License-Identifier: BSD-3-Clause
//
// yuv.metal — 采集前置：YUV 4:2:0 双平面 → BGRA8（Metal 实现）
//
// 契约：docs/11_平台权限与集成.md §3.1.3（AVFoundation 路径）、
//       §3.4（Android Camera2 路径）、§693 IOS-AR-5（**本算法被指定的做法**）：
//
//     「ARKit 模式下的 RGB 缓冲为 YUV（kCVPixelFormatType_420YpCbCr8BiPlanar
//       FullRange）。为保持 recon_frame_packet.rgb_data = BGRA8 契约不变，
//       转换在平台层的 Metal compute kernel 中完成
//       （platform/ios/MetalComputeBackend.mm 的 yuv_to_bgra），
//       1080p 单帧 GPU 转换 < 0.4 ms」
//
// 函数名 `recon_yuv_to_bgra` 必须与 `shaders/common/gpu_algo.h` 的
// `RECON_GPU_ALGO(recon_yuv_to_bgra)` 及 `shaders/vulkan/recon_yuv_to_bgra.comp`
// 的入口**逐字相同**（docs/12 MT-3），由 `tools/shader_parity.py` 校验。
//
// ── 为什么在 GPU 上做、而不是 CPU ─────────────────────────────────────────
// 1080p 一帧 = 2 073 600 像素 × (3 次乘加 + 量化 + 4 次写出)。CPU 单线程约
// 3~6 ms，在 30 fps 下占掉 `τ_pipeline` 预算的一大块（docs/02 §3.7）；
// 而 ARKit 帧回调**就在**采集串行队列上，CPU 转换会直接反压相机
// （docs/11 §3.1.3 的背压与本算法在此处是同一件事的两面）。
//
// ── 数学式（与 Vulkan 侧逐条对应，一字不差）──────────────────────────────
//
//   1. 归一化（系数全部由 C++ 侧按平台元数据算出后经 params 传入 —— MT-4）：
//          y  = (Y  − y_bias) × y_gain            // Y ∈ [0,255]
//          cb = (Cb − c_bias) × c_gain            // Cb/Cr ∈ [0,255]
//          cr = (Cr − c_bias) × c_gain
//
//   2. 矩阵（r_from_cr / b_from_cb / g_from_cr / g_from_cb 亦由 C++ 侧传入，
//      见 gpu_algo.h 参数块段落的派生式）：
//          r = y + r_from_cr × cr
//          b = y + b_from_cb × cb
//          g = y + g_from_cr × cr + g_from_cb × cb
//
//   3. 量化（**四舍五入 + 钳位**，与 CPU 规则本体逐字相同）：
//          R8 = clamp(round(r × 255), 0, 255)     // r 已 ∈ [0,1]
//          …
//
//   4. 写出 BGRA8（**内存序是 B,G,R,A** —— 见下方「为什么是 BGRA 而不是 RGBA」）
//
// ── 为什么是 BGRA 而不是 RGBA ─────────────────────────────────────────────
// 内核的 `BgraToGray`（core/src/orchestrator/orchestrator.cpp 第 53 行）按
// `row[x*4 + {0,1,2}] = {B,G,R}` 取值 → 字节序 **B 在前**。这是
// `recon_frame_packet.rgb_data` 的既有契约（docs/00 §7.2 / docs/02 §5.5），
// 本算法**必须**服从它。写错顺序不会崩溃，只会让 R/B 互换 —— 而灰度化用
// BT.601 的 (77,150,29) 权重，R/B 互换会让**所有颜色都偏**且不报错。
//
// ── 调度 ──────────────────────────────────────────────────────────────────
//     dispatchThreads(MTLSize(out_width, out_height, 1), …)
// 用 `dispatchThreads`（**非** `dispatchThreadgroups`）：本算法逐像素且尺寸
// 非 8 的整数倍时，`dispatchThreads` 由驱动处理尾部；kernel 内的 in-bounds
// 检查（`gid.x >= out_width`）是**必须保留的**防御（Metal 允许越界线程存在）。
//
// ══════════════════════════════════════════════════════════════════════════
// ⚠️ 实现状态（必须如实阅读）
// ══════════════════════════════════════════════════════════════════════════
// 本文件完成的是**结构性契约与数学**：算法名、参数块布局、色彩矩阵、量化、
// 越界保护。与 Vulkan 侧由 `tools/shader_parity.py` 强制逐条对齐。
//
// **尚未验证**（逐条列出，避免"看起来像完成品"的误导）：
//   SOURCE-TODO-YUV-1  只支持**交错色度平面**（`chroma_pixel_stride == 2`）。
//                      真平面 I420（stride == 1）时本 kernel 写全 0（alpha=0）
//                      表示"未产出"，由调用方走 CPU 回退路径。
//                      理由见 `shaders/common/gpu_algo.h` 的 YUV-1 登记。
//
// 判据说明：本文件**不能**声称"YUV→BGRA 已在真机跑通"。它的价值是：
//   ① 让「两端算法同源」这一契约**可机器验证**；
//   ② 把**色彩数学**的正确性交给 `platform/common` 的 CPU 规则本体
//      （`test_platform_yuv.cpp` 在桌面验收含负向注入），而 GPU 侧只做
//      同式的 FMA —— 于是"数值对不对"不再是只能真机回答的问题。
// 真机数值验收（docs/14 §5.3）需要 macOS + 一台 iPhone，**本轮从未执行**。
// ══════════════════════════════════════════════════════════════════════════

#include "gpu_common.h"

// ---------------------------------------------------------------------------
// 采集前置：YUV 4:2:0 双平面 → BGRA8
// ---------------------------------------------------------------------------
//
// 输入：
//   tex_y     —— Y 平面（r8，全分辨率）。用 `access::read`（最近邻，
//                与 VK-DIFF-4 的纪律一致：**不用** sampler/过滤 —— 采样
//                位置的整像素语义必须由我们自己控制，而不是交给硬件）
//   tex_cbcr  —— 交错色度平面（rg8，半分辨率）：.r = Cb、.g = Cr
//                （由 C++ 侧按 `cb_index_in_pair` 决定顺序后绑定；
//                若平台是 NV21（CrCb），C++ 侧在建纹理视图时**换序**，
//                 而不是让 kernel 分支 —— 这与 CPU 规则本体的做法一致）
// 输出：
//   out_bgra  —— BGRA8（rgba8 纹理，字节序 B,G,R,A）
kernel void recon_yuv_to_bgra(texture2d<float, access::read>  tex_y    [[texture(0)]],
                              texture2d<float, access::read>  tex_cbcr [[texture(1)]],
                              texture2d<float, access::write> out_bgra [[texture(2)]],
                              constant ReconYuvToBgraGpuParams& p      [[buffer(0)]],
                              uint2 gid [[thread_position_in_grid]])
{
    // 越界保护：`dispatchThreads` 允许存在越界线程（尺寸非 8 倍数时）。
    if (gid.x >= uint(p.out_width) || gid.y >= uint(p.out_height)) return;

    // ── 前置：本 kernel 只支持交错色度平面（YUV-1）────────────────────────
    // 真平面 I420 时**不产出**（alpha=0 即"未写出"哨兵），交由调用方
    // 走 CPU 回退。**不做静默的错误结果**：宁可空转也不产出乱色。
    if (p.chroma_pixel_stride != 2) {
        out_bgra.write(float4(0.0f, 0.0f, 0.0f, 0.0f), gid);
        return;
    }

    // ── Y：全分辨率直接按像素取 ────────────────────────────────────────────
    // 纹理的值域：C++ 侧把 r8 纹理绑定为 **normalized**（读到 [0,1]），
    // 故此处按 255 还原回整数域，再做与 CPU 规则本体**完全相同**的
    // `(Y − y_bias) × y_gain`。这样"归一化 + 量化"两个环节的取舍
    // 与 CPU 侧一字不差（否则会出现 ±1 的量化差，跨端对拍即失败）。
    const float y_raw = round(tex_y.read(uint2(gid.x, gid.y)).r * 255.0f);

    // ── Cb/Cr：半分辨率，右移一位取色度像素 ───────────────────────────────
    // 4:2:0 的色度是 2×2 下采样：输出像素 (x,y) 用色度像素 (x/2, y/2)。
    // `>> 1` 而非 `/2`：前者是整数语义，对越界前的 clamp 更可预测。
    const uint2 cgid = uint2(gid.x >> 1, gid.y >> 1);
    const float2 cbcr_raw = round(tex_cbcr.read(cgid).rg * 255.0f);

    // ── 归一化（系数由 C++ 侧经 params 传入）──────────────────────────────
    const float y  = (y_raw  - p.y_bias) * p.y_gain;
    const float cb = (cbcr_raw.x - p.c_bias) * p.c_gain;
    const float cr = (cbcr_raw.y - p.c_bias) * p.c_gain;

    // ── 色彩矩阵（BT.601 / BT.709 同构，系数由 C++ 侧派生后传入）──────────
    const float r = y + p.r_from_cr * cr;
    const float b = y + p.b_from_cb * cb;
    const float g = y + p.g_from_cr * cr + p.g_from_cb * cb;

    // ── 量化：四舍五入 + 钳位 [0,1] ────────────────────────────────────────
    // `clamp` 在量化**之前**：负数或 >1 的值若先乘 255 再钳，会因浮点舍入
    // 在边界产生 256（溢出 8 位）。先钳后乘是 CPU 规则本体的口径，必须一致。
    const float3 rgb = clamp(float3(r, g, b), 0.0f, 1.0f);

    // ── 写出 BGRA8：**内存字节序** B,G,R,A 见文件头说明 ────────────────────
    // 注意这是 rgba8 纹理的 `.bgra` 分量排列表达式 ——
    // `float4(b, g, r, 1)` 写入 rgba8 纹理后，内存里依次是 B,G,R,A。
    out_bgra.write(float4(rgb.z, rgb.y, rgb.x, 1.0f), gid);
}
