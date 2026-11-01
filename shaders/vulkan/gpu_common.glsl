// SPDX-License-Identifier: BSD-3-Clause
//
// gpu_common.glsl — Vulkan 侧公共定义（GLSL 450 / SPIR-V）
//
// 与 `shaders/common/gpu_algo.h` 的**字段集合必须逐条一致**（字段名与顺序对齐，
// 类型按语言惯例映射：C 的 int32_t ↔ GLSL 的 int，float ↔ float）。
// 与 `shaders/metal/gpu_common.h` 亦为**同一字段集合**。
// 三方一致性由 `tools/shader_parity.py` 校验（docs/14 CT-6 / docs/12 MT-3）。
//
// ══════════════════════════════════════════════════════════════════════════
// 一、文件组织（为什么 Vulkan 侧是「一个算法一个文件」）
// ══════════════════════════════════════════════════════════════════════════
// Metal 允许**一个 .metal 文件导出多个 kernel**（本仓库 `metal/mvs.metal` 导出
// 6 个），而 GLSL 的**可执行入口只有 main()** —— 一个 `.comp` 只能产出一个
// 独立的 SPIR-V 模块。因此 Vulkan 侧的落法只能是：
//
//     shaders/vulkan/recon_mvs_seed.comp          → 函数 recon_mvs_seed() + main()
//     shaders/vulkan/recon_mvs_patchmatch.comp    → 函数 recon_mvs_patchmatch() + main()
//     shaders/vulkan/recon_mvs_subpixel.comp      → …
//     shaders/vulkan/recon_mvs_consistency.comp   → …
//     shaders/vulkan/recon_mvs_fuse_voxel.comp    → …
//     shaders/vulkan/recon_mvs_reduce.comp        → …
//     shaders/vulkan/recon_tsdf_fuse.comp         → …
//
// **文件名 = 算法名**，且文件内必有一个**同名函数**承载算法本体，`main()`
// 只做 `gl_GlobalInvocationID` 的解包与转发。这样「算法名」在 Vulkan 侧依然
// 是一个**可被正则定位的真实标识符**，而不是散落在注释里的字符串 ——
// 这正是 MT-3「两端函数名逐字一致」能被机器校验的前提。
//
// 这是**两端结构上不可避免的不对称**（Metal 多 kernel/file ↔ Vulkan 一主/file），
// 不是实现疏漏；闸门 `tools/shader_parity.py` 对两侧使用**不同的提取规则**
// （Metal 提 `kernel void <name>(`，Vulkan 提 `<ret> <name>(` + 文件名核对），
// 但比对的是**同一个期望集合**（来自 `gpu_algo.h` 的 `RECON_GPU_ALGO(...)`）。
//
// ══════════════════════════════════════════════════════════════════════════
// 二、描述符布局约定（set / binding）
// ══════════════════════════════════════════════════════════════════════════
//   set = 0
//     binding = 0  → std140 UBO：`ReconMvsGpuParams` 或 `ReconTsdfGpuParams`
//     binding = 1  → std430 SSBO：`ReconViewParams views[]`
//     binding = 2+ → 该算法的 texture / image / SSBO（逐文件见各 .comp 注释）
//
// **视图参数为何单独占一个 binding**：它与算法参数（params）生命周期不同 ——
// params 逐 dispatch 可能变（`red_black`、`rand_search_radius` 每轮都变），
// views 逐**帧**才变。分开后 C++ 侧可以只用 `vkCmdUpdateBuffer` 改 params
// 而不用重传 views（docs/13 的降级要求「不重编译着色器即可调参」）。
//
// ══════════════════════════════════════════════════════════════════════════
// 三、std140 对齐：本仓库参数块的**为什么是安全的**
// ══════════════════════════════════════════════════════════════════════════
// std140 的知名陷阱是「数组与结构的成员被 16 字节对齐撑开」。本项目的规避方式：
//   - 参数块内**全是 4 字节标量**（int / float），**不混入 vec3、double、数组**；
//     std140 下标的基对齐 = 其自身大小 = 4 B → 相邻标量**紧贴**排列（步长 4 B）。
//   - 块内**只有一个**结构体成员（`ReconMvsGpuParams p;`）。std140 规定结构体
//     成员的基对齐 = 「其成员最大基对齐」向上取 16 的倍数 = **16**；成员 p 位于
//     offset 0（已 16 对齐），结构体自身大小向上取 16 的倍数：
//         MVS  = 28 字段 × 4 B = 112 B，112 % 16 == 0 → 无尾部填充
//         TSDF = 20 字段 × 4 B =  80 B， 80 % 16 == 0 → 无尾部填充
//     故**块大小 = 结构体大小 = C++/Metal 侧结构体大小**，三方字节级一致。
//   - `ReconViewParams` = `mat4`(64) + `vec4`(16) + `vec4`(16) = 96 B，96 % 16 == 0；
//     std430（SSBO 数组）步长 = 96，std140（UBO 单元素）大小 = 96
//     → 与 Metal 的 `float4x4 + float4 + float4`（stride 96）一致。
//
// **若将来必须往参数块加 `vec3` / 数组**：std140 会引入隐式填充，而 Metal 侧
// 「紧凑」布局不会。届时**必须**同步改两端并让闸门强制 —— 现在就在此留下判据：
// 闸门会检查参数块字段类型集合 ⊆ {int, float}（禁止 vec2/3/4/mat 混入），
// 一旦有人加了向量字段，闸门先失败，避免「两端各自编译通过但运行期错位」。
//
// ══════════════════════════════════════════════════════════════════════════
// 四、MT-4：常量不得硬编码
// ══════════════════════════════════════════════════════════════════════════
// 除 workgroup 布局限定符（着色器语言要求编译期常量，见下）外，本工程
// **不定义任何算法常量**。`mvs_gpu_tile_size` / `patch_size` / `halo_px` /
// 迭代数等全部经 UBO 从 C++ 侧传入。这样 docs/13 的降级（如 tile 1024 → 512）
// 才能真正做到「不重编译着色器」。
//
// ══════════════════════════════════════════════════════════════════════════
// ⚠️ 五、与 Metal 侧的**能力差异**（如实登记，不是"已解决"）
// ══════════════════════════════════════════════════════════════════════════
// VK-DIFF-1  **浮点原子**。docs/05 §5.9.1 映射表把两端原子写成
//            「`atomic_fetch_min_explicit` ↔ `atomicMin`」，但 `recon_mvs_fuse_voxel`
//            用的是 **atomicAdd**（像素中心式散射，docs/05 §5.7），而：
//              - Metal：`atomic_float` + `atomic_fetch_add_explicit`，**核心特性**；
//              - Vulkan：`atomicAdd` 对 float **需要扩展** `VK_EXT_shader_atomic_float`
//                （GLSL 侧 `GL_EXT_shader_atomic_float_add`），不是核心特性。
//            处置（**双路径 + 显式选择**，不静默降级）：
//              - `RECON_VK_FLOAT_ATOMIC` 已定义 → 与 Metal 完全同构的 float 原子；
//              - 未定义 → **定点整数原子**回退（scale = 2^12，带饱和），
//                与 float 路径存在**量化差异**（≤ 1/4096 相对误差），
//                由 C++ 侧按运行期能力探测结果二选一，并把选择结果记入
//                `degradation_summary`（docs/14 CT-9「无静默降级」）。
//            ⚠️ 残余风险：两条路径的**数值结果不完全相同**，属真实差异；
//            已登记为缺口，待真机阶段用同一输入对比两条路径的体素和值。
//
// VK-DIFF-2  **threadgroup / shared 内存**。docs/05 §5.9.1 映射表给出
//            「`threadgroup` ↔ `shared`」。本工程**两端都尚未启用**共享内存缓存
//            （Metal 侧 SOURCE-TODO-2、此处同名缺口）：`shared` 数组长度必须是
//            编译期常量，而 halo 宽度是运行期参数（MT-4 要求经 buffer 传），
//            两者只能靠「编译期常量 + 运行期断言」调和。在无真机可测的前提下
//            启用会踩 `maxComputeSharedMemorySize`（Vulkan 保证 ≥ 16 KB，
//            移动端常见 32 KB）的坑且无法验证，故**统一留待真机阶段**，
//            两端保持对称（不引入单侧"看起来更快"的假象）。
//
// VK-DIFF-3  **精度**。docs/05 §5.9.1 写「`half`（视差）/ `f16`（需 shaderFloat16）」，
//            docs/12 写「mvs 视差用 float32 而非 f16」—— 两份文档矛盾。
//            本项目**默认一律 `float`**（取更保守、更高精度者，符合「精度优先」），
//            fp16 是否启用由运行期能力探测（`allow_fp16` 字段）决定，见
//            `shaders/common/gpu_algo.h` 的「精度约定」段。**本工程不使用
//            `mediump` / `float16_t`**，故 Vulkan 侧不需要 `shaderFloat16` 特性。
//
// VK-DIFF-4  **深度图采样方式**。docs/05 §5.9.1 表格写「`texture2D` + `sampler`
//            + `imageSample`（线性过滤）」，但 docs/04 §4.6.1 硬要求「深度图必须用
//            **最近邻**采样」。处置：本工程**不创建 sampler 对象**，输入一律用
//            `texelFetch`（= 最近邻、无过滤、绕开采样器状态），与 Metal 侧
//            `access::read` + `read(uint2)` 语义**逐条对应**。文档 §5.9.1 该行
//            需修订（已登记缺口）。
//
// 上述 VK-DIFF-1 / VK-DIFF-4 已在 `shaders/common/gpu_algo.h` 的
// `RECON_GPU_DIFF(...)` 段登记；Metal 侧（`shaders/metal/gpu_common.h`）
// 亦登记同编号，闸门要求两端都可见。
// ══════════════════════════════════════════════════════════════════════════

#ifndef RECON_VULKAN_GPU_COMMON_GLSL_
#define RECON_VULKAN_GPU_COMMON_GLSL_

// ---------------------------------------------------------------------------
// workgroup 尺寸（docs/05 §5.9.1：统一 8×8 = 64 线程）
// ---------------------------------------------------------------------------
//
// 与 `shaders/common/gpu_algo.h` 的 RECON_GPU_WORKGROUP_X/Y/Z 同值。
// 这两个值**允许**在布局限定符里出现（着色器语言要求编译期常量，是 MT-4 的
// 唯一例外）；闸门对本文件与 common 头、Metal 侧做**一致性检查**。
//
// 用宏而非直接写 8：让「workgroup 尺寸」在 Vulkan 侧也只有一个来源，
// 避免 7 个 .comp 各写一遍 8/8/1 而其中一处被改成 16×16（那会同时打破
// docs/05 §5.9.1 与两端一致性，且静态检查很难发现）。
#define RECON_VK_WORKGROUP_X 8
#define RECON_VK_WORKGROUP_Y 8
#define RECON_VK_WORKGROUP_Z 1

// 一维展开后的线程数（64）。TSDF 融合按体素线性索引分派，用一维 workgroup，
// 但**线程总数仍必须是 64** —— 与 MVS 侧统一，便于 C++ 侧只维护一套
// 线程数假设（docs/05 §5.9.1「统一 8×8 = 64 线程」）。
#define RECON_VK_WORKGROUP_1D (RECON_VK_WORKGROUP_X * RECON_VK_WORKGROUP_Y)

// ---------------------------------------------------------------------------
// 跨端共享数值常量（值必须与 shaders/common/gpu_algo.h **逐位相同**）
// ---------------------------------------------------------------------------
//
// GLSL 不认识 C 头，故这里必须重复书写；「重复」由闸门
// `tools/shader_parity.py` 逐条比对数值来管控（见 common 头对应段的说明）。
//
// ⚠️ GLSL 的浮点字面量**必须带小数点或指数**（`1.0` 合法、`1` 是整数），
//    且**没有** `f` 后缀语法 —— 与 MSL 侧（`1.0f`）写法不同。
//    两端的**数值**相同，只有字面量语法按各自语言书写；
//    闸门比对数值，故这个不对称是安全的。
#define RECON_GPU_EPS_DEPTH     1.0e-6
#define RECON_GPU_EPS_DENOM     1.0e-12
#define RECON_GPU_HASH_A        0x7feb352du
#define RECON_GPU_HASH_B        0x846ca68bu
#define RECON_GPU_RAND_SCALE    4294967296.0
#define RECON_GPU_COST_INF      1.0e9
#define RECON_GPU_PERCENT_SCALE 100.0
#define RECON_GPU_PIXEL_CENTER  0.5

// ---------------------------------------------------------------------------
// 定点原子回退的量化尺度（仅 RECON_VK_FLOAT_ATOMIC 未定义时使用）
// ---------------------------------------------------------------------------
//
// scale = 2^12：权重量化步长 1/4096 ≈ 2.44e-4，对「融合权重归一化的和」而言
// 远小于体素离散化本身的误差（docs/05 §6.1.2 第 7 级，h_vox/√12）。
//
// 溢出上界：32 位有符号累加，饱和于 2^30（在 scale=2^12 下 = 未缩放值 262144）。
//   权重累加：单样本 w ≤ 1 → 缩放后 4096 → 可容纳 262144 个满权样本；
//   加权深度：单样本 z·w，z ≤ depth_max_m（默认 10 m 量级）→ 缩放后 ≤ 40960
//             → 可容纳约 6400 个满量程样本。
// 真实体素（0.5 mm 级）被上千像素命中已属极端，故上界充裕；**但仍饱和而非
// 回绕**（回绕会把大数变成负数，等价于静默数据损坏）。饱和会在
// `recon_mvs_reduce` 处表现为异常大的值，是**可观测**的，不是静默的。
#define RECON_VK_FIXED_SCALE      4096.0
#define RECON_VK_FIXED_SCALE_INV  (1.0 / 4096.0)
#define RECON_VK_FIXED_MAX        1073741824   // 2^30

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
// 20 个 4 字节标量 = 80 B，80 % 16 == 0 → std140 下**无尾部填充**
// （结构体成员基对齐向上取 16 的倍数 = 16，大小向上取 16 的倍数 = 80 不变）。
// 见本文件第三节的完整论证。
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
// 列主序 4×4：前三列为 `R`（world→camera 的 3×3），第四列为 `t = -R·C`。
// 该约定与 docs/04 §4.6.2 的 `P_c = R_i · p_v + t_i` 一致
// （也即 MEMORY.md 里「TSDF 世界→相机必须用 R_wc·v_w + t_wc」那条结论）。
//
// 与 Metal 侧 `float4x4 Rt` 布局一致：GLSL 的 `mat4` 与 MSL 的 `float4x4`
// 同为**列主序、列对齐 16 B**，故 `Rt[3]` 在两端的语义相同（第 4 列 = t）。
struct ReconViewParams {
    mat4 Rt;
    vec4 intrinsics;   // (fx, fy, cx, cy)
    vec4 size_depth;   // (depth_w, depth_h, near, far)
};

// ---------------------------------------------------------------------------
// 通用工具（与 shaders/metal/gpu_common.h 一一对应）
// ---------------------------------------------------------------------------
//
// ⚠️ 命名与语义必须与 MSL 侧**同名同义**；闸门会比对两侧的**工具函数名集合**
// （`recon_*` 前缀的自由函数），防止「一边加了个 helper、另一边悄悄没有」
// 导致的数值漂移。

// 双线性采样（手工实现，等价于 MSL 侧用 `tex.read` 手写的那份）。
// 逐 texel fetch，**不使用** sampler 对象 —— 见 VK-DIFF-4。
//
// 当前**无调用点**（NCC 代价聚合尚未接入，见 `recon_mvs_patchmatch` 的
// SOURCE-TODO）：此处保留是为了与 MSL 侧 helper 集合对齐，避免出现在
// 「单侧多了个 helper」时闸门误判。接入 NCC 后即被使用。
vec4 recon_sample_bilinear(texture2D tex, vec2 uv, ivec2 size) {
    vec2 p = uv - vec2(RECON_GPU_PIXEL_CENTER);
    ivec2 i0 = ivec2(floor(p));
    vec2 f = p - vec2(i0);
    ivec2 c0 = clamp(i0,       ivec2(0), size - ivec2(1));
    ivec2 c1 = clamp(i0 + ivec2(1), ivec2(0), size - ivec2(1));
    float v00 = texelFetch(tex, ivec2(c0.x, c0.y), 0).r;
    float v10 = texelFetch(tex, ivec2(c1.x, c0.y), 0).r;
    float v01 = texelFetch(tex, ivec2(c0.x, c1.y), 0).r;
    float v11 = texelFetch(tex, ivec2(c1.x, c1.y), 0).r;
    float a = mix(v00, v10, f.x);
    float b = mix(v01, v11, f.x);
    return vec4(mix(a, b, f.y));
}

// 像素 → 归一化深度（逆深度线性化）。docs/05 §5.4：
// PatchMatch 在**逆深度**空间传播，因为逆深度的等距对应像素视差等距。
float recon_depth_to_invz(float z)  { return 1.0 / max(z, RECON_GPU_EPS_DEPTH); }
float recon_invz_to_depth(float iz) { return 1.0 / max(iz, RECON_GPU_EPS_DEPTH); }

// 坐标哈希（确定性：不使用随机数，避免同一像素在不同 dispatch 上跳到
// 不同候选而破坏 PatchMatch 的可复现性 —— docs/12 §1.1 确定性构建要求）。
// 位运算常量与 MSL 侧**逐位相同**，否则两端的"随机"搜索序列会分叉。
uint recon_hash_u32(uint x) {
    x ^= x >> 16; x *= RECON_GPU_HASH_A;
    x ^= x >> 15; x *= RECON_GPU_HASH_B;
    x ^= x >> 16;
    return x;
}

float recon_rand01(uint seed) {
    return float(recon_hash_u32(seed)) * (1.0 / RECON_GPU_RAND_SCALE);
}

// 投影：相机系点 → 像素。返回 false 表示在相机后方。
//
// ⚠️ 用 `out uv` + bool 返回，与 MSL 侧的 `thread float2* out_uv` 语义一致。
bool recon_project(vec3 pc, vec4 intr, out vec2 uv) {
    if (pc.z <= RECON_GPU_EPS_DEPTH) return false;
    uv.x = intr.x * pc.x / pc.z + intr.z;
    uv.y = intr.y * pc.y / pc.z + intr.w;
    return true;
}

// 反投影：像素 + 深度 → 相机系点。
vec3 recon_backproject(vec2 uv, float z, vec4 intr) {
    return vec3((uv.x - intr.z) * z / intr.x,
                (uv.y - intr.w) * z / intr.y,
                z);
}

// world → camera（docs/04 §4.6.2 步骤 ①）
//
// 数学式：`pc_i = Σ_j Rt[j][i] · pw_j + Rt[3][i]` = `(Rt * vec4(pw,1)).xyz`。
// 与 MSL 侧的 `dot(Rt[i].xyz, pw) + Rt[3][i]` **逐项等价**（见该文件注释）。
vec3 recon_world_to_cam(mat4 Rt, vec3 pw) {
    return (Rt * vec4(pw, 1.0)).xyz;
}

// camera → world（fuse_voxel 用）。与 MSL 侧写法**逐项相同**：
// 取 R 的行向量与 (pc − t) 点乘 = Rᵀ·(pc − t)。
vec3 recon_cam_to_world(mat4 Rt, vec3 pc) {
    vec3 e = pc - Rt[3].xyz;
    return vec3(dot(Rt[0].xyz, e),
                dot(Rt[1].xyz, e),
                dot(Rt[2].xyz, e));
}

// 像素/浮点 uv 是否落在图像内。抽到公共头而非各 .comp 内联 ——
// 该逻辑被 `recon_mvs_consistency` 与 TSDF 融合共同需要，写两遍必然漂移。
//
// ⚠️ 本函数一度**遗漏**在本文件中（MSL 侧有、GLSL 侧无），而各 .comp
//    已经在调用它 —— 这是一个真实的编译级缺陷，由 `tools/shader_parity.py`
//    的 helper 对称判据（P11）抓出：**闸门的价值在这里得到验证**。
//    （无 macOS/glslang 的环境下无法编译着色器，此类缺陷只能靠结构比对发现。）
bool recon_in_image(vec2 uv, vec2 size) {
    return uv.x >= 0.0 && uv.y >= 0.0 && uv.x < size.x && uv.y < size.y;
}

// 权重映射链（docs/04 §2.4.4）
//
// `weight_mode` 选择映射链分支：
//   0 = 距离权重（默认）：截断距离归一化后取余弦窗
//   1 = 距离 + 视角权重
// 具体系数由 C++ 侧决定后经 params 传入（MT-4），此处不做任何硬编码阈值。
float recon_tsdf_weight(float s_over_mu, float cos_theta, int weight_mode) {
    float w_dist = 1.0 - min(abs(s_over_mu), 1.0);
    if (weight_mode == 0) return w_dist;
    float w_angle = clamp(cos_theta, 0.0, 1.0);
    return w_dist * w_angle;
}

#endif  // RECON_VULKAN_GPU_COMMON_GLSL_
