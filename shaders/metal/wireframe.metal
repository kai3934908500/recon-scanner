//
// wireframe.metal — 预览线框渲染着色器（iOS 独有，非双端算法）
//
// 契约：docs/09_UI规格.md §3.7 U-R-1（只做线框，不做材质/光照/阴影/IBL）。
//
// ── 为什么**不**放进 shaders/common 的算法注册表 ─────────────────────────
// `shaders/common/gpu_algo.h` 的 `RECON_GPU_ALGO(...)` 是**双端 compute 算法**
// 的注册表 —— `tools/shader_parity.py` 的 P1 判据强制「common 声明的每个算法，
// Metal 与 Vulkan **两端都必须有同名实现**」。
//
// 而线框渲染是 **iOS 独占的渲染管线**（Android 侧渲染层尚未开工），没有 Vulkan
// 对端。若把它注册进 gpu_algo.h，P1 会（正确地）报「Metal 侧存在 common 未声明
// 的 kernel」或「Vulkan 侧缺失算法实现」。故它与 `mvs/tsdf/yuv` 那些**成对
// compute kernel 不同类**，单独放置。
//
// ── 落位 ────────────────────────────────────────────────────────────────
// 本文件位于 `shaders/metal/` 下，会被 `tools/build_metal_shaders.sh` 的
// `find shaders/metal -name '*.metal'` 一并编入 `default.metallib`。
// ⚠️ 但 P12 判据（CMake 清单 ↔ 磁盘 .metal 一致）与 P1 的 kernel 集合判据
// 边界不同：本文件必须在**两处**都被正确对待 —— 见文件末的边界说明。
//
// ── U-R-3：无动画 ───────────────────────────────────────────────────────
// 顶点变换只做「模型 → 视图 → 投影」，不含任何时间项、不含自动旋转、
// 不含惯性/阻尼。旋转完全由用户手势（拖拽）驱动，由 host 侧传矩阵。

#include <metal_stdlib>
using namespace metal;

// 与 host 侧 scaffolding（WireframeRendererView.swift）逐字段对齐。
// 常量不得硬编码进数学式（gpu_algo.h MT-4 的同一原则）。
struct WireframeUniforms {
    float4x4 mvp;          // 模型-视图-投影矩阵（U-R-3：无时间项）
    float4   color;        // 线框颜色（RGBA）
    float    pointSize;    // 点大小（当 prim 为 point 时）
    float    _pad0;
    float    _pad1;
    float    _pad2;
};

// 顶点着色器：位置已在核心侧算好（recon_get_preview_mesh），
// 这里只做矩阵变换，不做任何几何计算（docs/09 §1：几何计算不在平台层做）。
struct WireframeVertexOut {
    float4 position [[position]];
};

vertex WireframeVertexOut recon_wireframe_vertex(
        uint vid [[vertex_id]],
        const device float* positions [[buffer(0)]],
        constant WireframeUniforms& u [[buffer(1)]]) {
    WireframeVertexOut out;
    const float3 p = float3(positions[vid * 3 + 0],
                            positions[vid * 3 + 1],
                            positions[vid * 3 + 2]);
    out.position = u.mvp * float4(p, 1.0);
    return out;
}

// 片元着色器：纯色线框（U-R-1：无材质、无光照、无阴影、无 IBL）。
fragment float4 recon_wireframe_fragment(constant WireframeUniforms& u [[buffer(1)]]) {
    return u.color;
}
