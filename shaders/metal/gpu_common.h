// SPDX-License-Identifier: BSD-3-Clause
//
// gpu_common.h — Metal 侧公共定义（MSL）
//
// 与 `shaders/common/gpu_algo.h` 的**字段集合必须逐条一致**（字段名对齐，
// 类型按语言惯例映射：C 的 int32_t ↔ MSL 的 int，float ↔ float）。
// 一致性由 `tools/shader_parity.py` 校验（docs/14 CT-6）。
//
// ── 为什么不复用 C 头 ─────────────────────────────────────────────────────
// `shaders/common/gpu_algo.h` 是 C/C++ 头（含 `<stdint.h>`、宏、`struct`），
// MSL 编译器不认识 `int32_t` 与 C 的预处理约定。若强行 `#include` 它，
// 会引入「为两端可编译而扭曲公共头」的耦合。故按 docs/12 §4.4 的分工：
// **common 描述算法、两端各自声明布局**，对齐由闸门保证。
//
// ── MT-4：常量不得硬编码 ─────────────────────────────────────────────────
// 本文件除 workgroup 尺寸（着色器语言要求编译期字面量，见 common 头的说明）
// 外**不定义任何可调常量**。所有算法参数经 `ReconMvsGpuParams` /
// `ReconTsdfGpuParams` buffer 传入。

#ifndef RECON_METAL_GPU_COMMON_H_
#define RECON_METAL_GPU_COMMON_H_

#include <metal_stdlib>
using namespace metal;

// ---------------------------------------------------------------------------
// 跨端共享数值常量（值必须与 shaders/common/gpu_algo.h **逐位相同**）
// ---------------------------------------------------------------------------
//
// MSL 不认识 C 头，故这里必须重复书写；「重复」由闸门
// `tools/shader_parity.py` 逐条比对数值来管控（见 common 头对应段的说明）。
//
// ⚠️ MSL 与 GLSL 的浮点字面量规则**不同**：
//    GLSL（ES/450）要求浮点字面量带小数点或指数（`1.0` 合法、`1` 会被当整数）；
//    MSL（基于 C++）**可以**写 `1.0f`。
//    故本文件的共享常量一律带 `f` 后缀，Vulkan 侧（gpu_common.glsl）一律
//    不带后缀 —— 两端的**数值**相同，只是字面量语法按各自语言书写。
//    这是安全的不对称，闸门比对的是数值而非字面量文本。
#define RECON_GPU_EPS_DEPTH     1.0e-6f
#define RECON_GPU_EPS_DENOM     1.0e-12f
#define RECON_GPU_HASH_A        0x7feb352du
#define RECON_GPU_HASH_B        0x846ca68bu
#define RECON_GPU_RAND_SCALE    4294967296.0f
#define RECON_GPU_COST_INF      1.0e9f
#define RECON_GPU_PERCENT_SCALE 100.0f
#define RECON_GPU_PIXEL_CENTER  0.5f

// ---------------------------------------------------------------------------
// 平台差异（有意的、平台能力使然的不对称 —— 见 common 头的 RECON_GPU_DIFF 段）
// ---------------------------------------------------------------------------
//
// 这里列出本端**受影响的**差异编号。闸门 `tools/shader_parity.py` 要求
// 每个 `RECON_GPU_DIFF(n)` 在**两端**都能找到 `DIFF-n` 的说明 ——
// 避免某端的说明被悄悄删掉，使差异变成"看不出是漏写还是有意的"。
//
//   VK-DIFF-1 / MSL-DIFF-1  浮点原子：Metal 侧 `atomic_float` 是**核心特性**
//        （`atomic_fetch_add_explicit` 直接可用），故本端**无**该差异的代码分支；
//        Vulkan 侧需要扩展，无扩展时退为定点整数原子。
//        本端在此登记"我是差异的另一端"，以免闸门以为 Metal 侧忘了写。
//        ⚠️ 后果：两条路径的**数值**不保证逐位相同（相对误差 ≤ 1/4096）。
//        跨端对拍时须以"可接受误差"而非"逐位相等"为准。
//
//   VK-DIFF-4 / MSL-DIFF-4  深度采样用最近邻而非线性过滤（以 docs/04 §4.6.1
//        的硬要求为准）。本端用 `access::read` + `read(uint2)` 实现最近邻；
//        Vulkan 侧用 `texelFetch`。两端**行为一致**，差异只在"文档 §5.9.1
//        的映射表描述与硬要求冲突"这一点上。
//
// 注：`VK-DIFF-2`（共享内存未启用）与 `VK-DIFF-3`（fp16/fp32 文档矛盾）在两端
//     均以「缺口 MVS-2」与「精度约定段」覆盖，故此处只列 1 与 4。

// ---------------------------------------------------------------------------
// 参数块：MVS（字段与 shaders/common/gpu_algo.h 的 ReconMvsGpuParams 一致）
// ---------------------------------------------------------------------------
struct ReconMvsGpuParams {
    int   ref_width;
    int   ref_height;
    int   num_views;
    int   ref_view_index;

    int   tile_size_px;
    int   tiles_x;
    int   tiles_y;
    int   halo_px;

    int   patch_size;
    int   iterations;
    int   random_search_count;
    float rand_search_radius;

    float restart_ratio;
    float photo_threshold;
    float depth_reg_lambda;
    float geo_consistency_pct;

    int   subpixel_fit_points;
    int   subpixel_iterations;
    float subpixel_window_ratio;
    int   red_black;

    float depth_min_m;
    float depth_max_m;
    int   min_view_consistency;
    int   allow_fp16;

    int   batch_size;
    int   memory_budget_mb;
    int   voxel_size_mm;
    int   reserved0;
};

// ---------------------------------------------------------------------------
// 参数块：TSDF（字段与 ReconTsdfGpuParams 一致）
// ---------------------------------------------------------------------------
struct ReconTsdfGpuParams {
    int   grid_dim_x;
    int   grid_dim_y;
    int   grid_dim_z;
    int   block_voxels;

    float voxel_size_m;
    float truncation_m;
    float min_depth_m;
    float max_depth_m;

    float fx;
    float fy;
    float cx;
    float cy;

    float origin_x;
    float origin_y;
    float origin_z;
    int   frame_index;

    int   block_count;
    int   max_blocks_in_memory;
    int   weight_mode;
    int   reserved0;
};

// ---------------------------------------------------------------------------
// 参数块：YUV → BGRA（字段与 ReconYuvToBgraGpuParams 一致）
// ---------------------------------------------------------------------------
//
// 20 个 4 字节标量 = 80 B，80 % 16 == 0 → 无尾部填充（见 common 头说明）。
// 本算法**不使用** workgroup 共享内存、不使用 sampler，故不需要额外声明。
struct ReconYuvToBgraGpuParams {
    int   y_width;
    int   y_height;
    int   cbcr_width;
    int   cbcr_height;

    int   y_stride_bytes;
    int   cbcr_stride_bytes;

    int   out_width;
    int   out_height;
    int   out_stride_bytes;

    int   chroma_pixel_stride;
    int   cb_index_in_pair;

    float y_bias;
    float y_gain;
    float c_bias;
    float c_gain;

    float r_from_cr;
    float b_from_cb;
    float g_from_cr;
    float g_from_cb;

    int   reserved0;
};

// ---------------------------------------------------------------------------
// 视图参数：源视图的位姿与内参（每帧上传一次，长度 = num_views）
// ---------------------------------------------------------------------------
//
// 列主序 3×4：`R` 为 world→camera 的 3×3，`t = -R·C`。
// 该约定与 docs/04 §4.6.2 的 `P_c = R_i · p_v + t_i` 一致
// （也即 MEMORY.md 里「TSDF 世界→相机必须用 R_wc·v_w + t_wc」那条结论）。
struct ReconViewParams {
    float4x4 Rt;      // 前三列 = R，第四列 = t（齐次形式，w 分量恒 1）
    float4   intrinsics;  // (fx, fy, cx, cy)
    float4   size_depth;  // (depth_w, depth_h, near, far)
};

// ---------------------------------------------------------------------------
// 通用工具
// ---------------------------------------------------------------------------

// 双线性采样（Metal 的 `texture2d.sample` 需要 sampler 对象，
// 而 compute kernel 里直接手写更可控，也避免 sampler 状态与 descriptor 耦合）。
static inline float recon_sample_bilinear(texture2d<float, access::read> tex,
                                          float2 uv, int2 size) {
    float2 p = uv - RECON_GPU_PIXEL_CENTER;
    int2   i0 = int2(floor(p));
    float2 f  = p - float2(i0);
    float2 c0 = clamp(float2(i0),     float2(0.0f), float2(size - 1));
    float2 c1 = clamp(float2(i0 + 1), float2(0.0f), float2(size - 1));
    float v00 = tex.read(uint2(c0.x, c0.y)).r;
    float v10 = tex.read(uint2(c1.x, c0.y)).r;
    float v01 = tex.read(uint2(c0.x, c1.y)).r;
    float v11 = tex.read(uint2(c1.x, c1.y)).r;
    return mix(mix(v00, v10, f.x), mix(v01, v11, f.x), f.y);
}

// 像素 → 归一化深度（逆深度线性化）。docs/05 §5.4：
// PatchMatch 在**逆深度**空间传播，因为逆深度的等距对应像素视差等距。
static inline float recon_depth_to_invz(float z)  { return 1.0f / max(z, RECON_GPU_EPS_DEPTH); }
static inline float recon_invz_to_depth(float iz) { return 1.0f / max(iz, RECON_GPU_EPS_DEPTH); }

// 坐标哈希（确定性：不使用随机数，避免同一像素在不同 dispatch 上跳到
// 不同候选而破坏 PatchMatch 的可复现性 —— docs/12 §1.1 确定性构建要求）。
static inline uint recon_hash_u32(uint x) {
    x ^= x >> 16; x *= RECON_GPU_HASH_A;
    x ^= x >> 15; x *= RECON_GPU_HASH_B;
    x ^= x >> 16;
    return x;
}

static inline float recon_rand01(uint seed) {
    return float(recon_hash_u32(seed)) * (1.0f / RECON_GPU_RAND_SCALE);
}

// 投影：相机系点 → 像素。返回 false 表示在相机后方。
static inline bool recon_project(float3 pc, float4 intr, float2* out_uv) {
    if (pc.z <= RECON_GPU_EPS_DEPTH) return false;
    out_uv->x = intr.x * pc.x / pc.z + intr.z;
    out_uv->y = intr.y * pc.y / pc.z + intr.w;
    return true;
}

// 反投影：像素 + 深度 → 相机系点。
static inline float3 recon_backproject(float2 uv, float z, float4 intr) {
    return float3((uv.x - intr.z) * z / intr.x,
                  (uv.y - intr.w) * z / intr.y,
                  z);
}

// world → camera（docs/04 §4.6.2 步骤 ①）
//
// 数学式：`pc_i = Σ_j R[j][i]·pw_j + t_i`（`R` 为列向量 `Rt[0..2].xyz`，`t = Rt[3].xyz`）。
// `Rt` 列主序，`Rt[i]` 即第 i 列 → `dot(Rt[i].xyz, pw) + Rt[3][i]` 正好是第 i 行
// 与 `(pw,1)` 的点积，与 GLSL 侧的 `(Rt * vec4(pw,1)).xyz` **逐项等价**
// （见 shaders/vulkan/gpu_common.glsl 的同名函数）。
static inline float3 recon_world_to_cam(float4x4 Rt, float3 pw) {
    return float3(dot(Rt[0].xyz, pw) + Rt[3].x,
                  dot(Rt[1].xyz, pw) + Rt[3].y,
                  dot(Rt[2].xyz, pw) + Rt[3].z);
}

// camera → world（`recon_mvs_fuse_voxel` 用）
//
// `R` 是 world→camera 的旋转，故 camera→world 的旋转是 `Rᵀ`。
// `Rt` 列主序下 `Rt[j].xyz` 是 R 的第 j **列**，因此 `dot(Rt[j].xyz, e)`
// = `Σ_i R[j][i]·e_i` = 第 j 个输出分量 → 即 `Rᵀ·e`。
// 平移：`C = −Rᵀt`，故世界点 = `Rᵀ·(pc − t)`。
//
// 该 helper 此前**内联写在** `mvs.metal` 的 `recon_mvs_fuse_voxel` 里；
// 抽到公共头是为了让两端 helper 集合对称（闸门会比对两侧 `recon_*` helper 名集合），
// 也符合「同一逻辑只保留一份实现」。
static inline float3 recon_cam_to_world(float4x4 Rt, float3 pc) {
    float3 e = pc - Rt[3].xyz;
    return float3(dot(Rt[0].xyz, e),
                  dot(Rt[1].xyz, e),
                  dot(Rt[2].xyz, e));
}

// 像素/浮点 uv 是否落在图像内。抽到公共头而非各 .metal 内联 ——
// 该逻辑被 `recon_mvs_consistency` 与 TSDF 融合共同需要，写两遍必然漂移。
static inline bool recon_in_image(float2 uv, float2 size) {
    return uv.x >= 0.0f && uv.y >= 0.0f &&
           uv.x < size.x && uv.y < size.y;
}

// ---------------------------------------------------------------------------
// 权重映射链（docs/04 §2.4.4）
// ---------------------------------------------------------------------------
//
// `weight_mode` 选择映射链分支。此处实现两条：
//   0 = 距离权重（默认）：截断距离归一化后取余弦窗
//   1 = 距离 + 视角权重
// 具体系数由 C++ 侧决定后经 params 传入（MT-4），此处不做任何硬编码阈值。
static inline float recon_tsdf_weight(float s_over_mu, float cos_theta,
                                      int weight_mode) {
    // s_over_mu ∈ [-1, 1]；越接近 0 表示越贴近表面。
    float w_dist = 1.0f - min(abs(s_over_mu), 1.0f);
    if (weight_mode == 0) return w_dist;
    // 视角权重：掠射角给低权（cos_theta 小）。
    float w_angle = clamp(cos_theta, 0.0f, 1.0f);
    return w_dist * w_angle;
}

#endif  // RECON_METAL_GPU_COMMON_H_
