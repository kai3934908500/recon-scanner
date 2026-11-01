// SPDX-License-Identifier: BSD-3-Clause
//
// mvs.metal — MVS 六个计算核（Metal 实现）
//
// 契约：docs/05_单目管线.md §5.9（GPU 实现）、docs/12 §4.4（编译与嵌入）。
//
// ── 函数名与 common 的对应（MT-3）─────────────────────────────────────────
// 本文件导出的每个 `kernel` 函数名，必须与 `shaders/common/gpu_algo.h` 中
// `RECON_GPU_ALGO(...)` 声明的**同名**：
//
//     common 声明                       本文件实现
//     recon_mvs_seed            ↔      kernel void recon_mvs_seed(...)
//     recon_mvs_patchmatch      ↔      kernel void recon_mvs_patchmatch(...)
//     recon_mvs_subpixel        ↔      kernel void recon_mvs_subpixel(...)
//     recon_mvs_consistency     ↔      kernel void recon_mvs_consistency(...)
//     recon_mvs_fuse_voxel      ↔      kernel void recon_mvs_fuse_voxel(...)
//     recon_mvs_reduce          ↔      kernel void recon_mvs_reduce(...)
//
// 由 `tools/shader_parity.py` 校验（docs/14 CT-6）。
//
// ── MT-4：常量不得硬编码 ──────────────────────────────────────────────────
// 本文件**没有**任何算法常量字面量（如 1024 / 11 / 0.15）。全部经
// `ReconMvsGpuParams` 传入。唯一的字面量是 `threads_per_threadgroup(8, 8)`
// —— 那是着色器语言要求的编译期布局限定符，且与 Vulkan 侧一致性由闸门校验。
//
// ── 精度（docs/05 §5.9.1 与 docs/12 的冲突处置）───────────────────────────
// 全部用 `float`。`allow_fp16` 为 1 时由构建侧换成 fp16 变体（当前未启用）。
// 理由见 `shaders/common/gpu_algo.h` 的「精度约定」段。
//
// ══════════════════════════════════════════════════════════════════════════
// ⚠️ 实现状态（必须如实阅读，这不是"已完成"）
// ══════════════════════════════════════════════════════════════════════════
// 本文件完成的是**结构性契约**：六个核的名称、参数块布局、调度维度、
// 越界保护、红黑交替的读写分离、边界退化处理。这些是**可被静态闸门
// 校验**的部分（docs/14 CT-6），也是两端必须逐字一致的部分（MT-3）。
//
// **尚未实现**（逐条列出，避免"看起来像完成品"的误导）：
//   SOURCE-TODO-MVS-1  NCC 代价聚合。`recon_mvs_patchmatch` 目前只做初值传播与
//                  随机搜索，**没有**计算 patch 的 NCC 代价，因此
//                  `photo_threshold` / `patch_size` 尚未真正参与决策。
//   SOURCE-TODO-MVS-2  threadgroup 共享内存。halo 复用（docs/13 §3.2.1）未启用，
//                  理由见 `recon_mvs_patchmatch` 内注释（需真机确认
//                  `maxTotalThreadgroupMemory`，docs/12 MT-5）。
//   SOURCE-TODO-MVS-3  `recon_mvs_consistency` 的往返变换用参考帧位姿近似，
//                  精确实现需 views[ref] 的逆变换；须与 CPU 版
//                  `core/src/mvs/patchmatch_stereo.cpp` 做数值对齐。
//   SOURCE-TODO-MVS-4  `recon_mvs_fuse_voxel` 的体素线性索引用了占位维度，
//                  真实实现须用体素网格维度（与 C++ 侧扁平化约定一致）。
//   SOURCE-TODO-MVS-5  视图数组上界硬编码为 8（`array<...,8>`），与
//                  `mvs_num_views` 高档 8 一致，但未做运行期断言。
//
// 判据说明：本文件**不能**声称"GPU MVS 可用"。它的价值是让「两端算法同源」
// 这一契约**可机器验证**，而不是提供可运行的 GPU 加速。真机阶段的验收项
// （docs/14 §5.3 GPU 行）需要显存实测，本轮**从未执行过**。
// ══════════════════════════════════════════════════════════════════════════

#include "gpu_common.h"

// 越界检查 `recon_in_image` 已上移到 `gpu_common.h`（TSDF 融合也要用，
// 写两遍必然漂移）。

// ---------------------------------------------------------------------------
// CB-1 播种：稀疏点投影 + 中值初始化 + 空洞填补 + 初始代价
// docs/05 §5.3.3
// ---------------------------------------------------------------------------
//
// 输入：
//   sparse_depth  —— 来自 SfM/VIO 的稀疏深度（无效处为 0）
//   rgb_ref       —— 参考帧灰度（已由 C++ 侧转灰度，避免在 GPU 里做色彩转换）
// 输出：
//   out_invz      —— 逆深度初值（0 表示无效）
//   out_cost      —— 初始代价
//
// 逐线程处理一个像素（不是 tile）：播种阶段计算量小，无需 halo 复用。
kernel void recon_mvs_seed(texture2d<float, access::read>  sparse_depth [[texture(0)]],
                           texture2d<float, access::read>  rgb_ref      [[texture(1)]],
                           texture2d<float, access::write> out_invz     [[texture(2)]],
                           texture2d<float, access::write> out_cost     [[texture(3)]],
                           constant ReconMvsGpuParams&     p            [[buffer(0)]],
                           uint2 gid [[thread_position_in_grid]])
{
    const int2 size = int2(p.ref_width, p.ref_height);
    if (gid.x >= uint(size.x) || gid.y >= uint(size.y)) return;

    float d = sparse_depth.read(gid).r;
    // 播种：有稀疏深度就用它的逆深度；否则置 0（无效），
    // 由后续的红黑传播从邻居扩散填洞。
    if (d > p.depth_min_m && d < p.depth_max_m) {
        out_invz.write(float4(recon_depth_to_invz(d), 0, 0, 1), gid);
    } else {
        out_invz.write(float4(0, 0, 0, 1), gid);
    }
    // 初始代价：无支撑视图时置极大（正无穷会污染后续 mix，用大常数）。
    out_cost.write(float4(RECON_GPU_COST_INF, 0, 0, 1), gid);
}

// ---------------------------------------------------------------------------
// CB-2 主循环：PatchMatch 传播 + 随机搜索 + 代价评估
// docs/05 §5.3.4（`mvs_pm_*`）
// ---------------------------------------------------------------------------
//
// 红黑交替：`p.red_black` 决定本次处理红格还是黑格。
// 红黑的意义是**消除传播顺序依赖** —— 若按行序传播，同一 dispatch 内
// 相邻线程读到的可能是上一轮的值，导致结果随线程调度变化（不可复现）。
// 红黑把同行/同列的邻居隔开，使一轮内所有读写互不依赖。
//
// halo 的用法（docs/13 §3.2.1「workgroup 覆盖 8×8 + 5 px halo」）：
// 真实实现会把 `8+2*5 = 18` 宽的窗口灰度一次性载入 threadgroup 内存，
// 使 NCC 窗口内的重复采样命中共享内存。此处**未启用** threadgroup 缓存，
// 直接走 texture.read —— 这是**如实的当前状态**，不是"已优化"的假象。
// 启用共享内存需要先确认 iOS 各机型的 `maxTotalThreadgroupMemory`
// （docs/12 MT-5 要求启动时查询），在无真机可测的前提下贸然启用会踩
// 「32 KB 限额」的坑而无法验证，故留待真机阶段。
//
// ── ⚠️ 纹理槽位排布（MSL 与 Vulkan 的**真实差异**，2026-11-01 CI 首编暴露）───
// MSL 的 `array<texture2d<...>, N> x [[texture(k)]]` **占用连续的 N 个槽位**
// （k .. k+N−1），而 Vulkan 的 `texture2D src_rgb[N]` 只占**一个** `binding`
// （描述符数组）—— 两端在这一点的语义**不同**。
// 因此 MSL 侧凡是带纹理数组的 kernel，都必须把数组声明**放在所有标量纹理
// 之后**（或至少留出足够空档），否则后续 `[[texture(k+1)]]` 会与数组占用的
// 槽位冲突，报 `cannot reserve 'texture' resource location at index N`。
// 此前漏排（数组在中间）在 Windows 上**根本无法暴露**（本机无 Metal 编译器），
// 只有 CI 首次真正编译才现形。见 LESSONS.md。
//
// ── ⚠️ 线程位置属性必须**维度一致**（CI 首编暴露的第 4 类错误）──────────────
// MSL 要求同一 kernel 内的「位置」类内建（`thread_position_in_grid` /
// `thread_position_in_threadgroup` / `threadgroup_position_in_grid` / …）
// **要么全为标量、要么全为同元素数的向量**。本 kernel 的 `gid` 必须是
// `uint2`（二维索引），故**不得再并存**标量形态的
// `thread_position_in_threadgroup`，否则报
// `expecting input declarations with either all scalar types or all vector
// types with the same number of elements`。
//
// 原先声明的 `tid` 在本实现中**从未被读取**（`(void)tid;`，共享内存未启用，
// 见 SOURCE-TODO-MVS-2），故直接移除 —— 零语义损失。
// ⚠️ 将来启用共享内存时**不要**直接把它加回来：必须同时改掉 `gid` 的形态
//    （或改用非「位置」类的索引内建，如 `thread_index_in_threadgroup`，
//    其是否受同一维度约束需**在 macOS 上实测确认**，本机无法验证故不预设）。
kernel void recon_mvs_patchmatch(texture2d<float, access::read>  rgb_ref   [[texture(0)]],
                                 texture2d<float, access::read>  in_invz   [[texture(1)]],
                                 texture2d<float, access::read>  in_cost   [[texture(2)]],
                                 texture2d<float, access::write> out_invz  [[texture(3)]],
                                 texture2d<float, access::write> out_cost  [[texture(4)]],
                                 array<texture2d<float, access::read>, 8> src_rgb [[texture(5)]],
                                 constant ReconMvsGpuParams&     p         [[buffer(0)]],
                                 constant ReconViewParams*       views     [[buffer(1)]],
                                 uint2 gid [[thread_position_in_grid]])
{
    const int2 size = int2(p.ref_width, p.ref_height);
    if (gid.x >= uint(size.x) || gid.y >= uint(size.y)) return;

    // 红黑：用 (x + y) 的奇偶判定本格颜色。
    const bool is_red = (((gid.x + gid.y) & 1u) == 0u);
    if (is_red != (p.red_black == 0)) return;

    float best_iz   = in_invz.read(gid).r;
    float best_cost = in_cost.read(gid).r;

    // 以下两个参数在**当前简化实现**中未被读取，显式标注以说明不是遗漏：
    //   src_rgb —— NCC 代价需要，但 NCC 聚合尚未接入（见下方 SOURCE-TODO）
    //   views   —— 候选视图投影需要，同上
    // （原第三个 `tid` 已随签名一并移除 —— MSL 不允许标量与向量位置内建混用，
    //   而 2D 的 `gid` 必须保留；详见 kernel 签名上方的说明。）
    (void)src_rgb;
    (void)views;

    // 传播候选：若本格无效（best_iz == 0），从**上一轮**的邻居借初值。
    // 邻居取自 `in_*`（只读）而非 `out_*`，保证一轮内读写分离、结果可复现。
    if (best_iz <= 0.0f) {
        const int2 nb[2] = { int2(gid) + int2(-1, 0), int2(gid) + int2(0, -1) };
        for (int k = 0; k < 2; ++k) {
            if (nb[k].x < 0 || nb[k].y < 0) continue;
            float iz = in_invz.read(uint2(nb[k])).r;
            if (iz > 0.0f) { best_iz = iz; break; }
        }
    }

    // 随机搜索：半径按迭代收缩（`rand_search_radius` 由 C++ 侧按当前迭代
    // 算好传入 —— 不在此处用 `iteration` 自算，避免两端公式不一致）。
    if (best_iz > 0.0f) {
        float r = recon_rand01((gid.x << 16) ^ gid.y ^ uint(p.red_black));
        float lo = best_iz * (1.0f - p.rand_search_radius);
        float hi = best_iz * (1.0f + p.rand_search_radius);
        float cand = mix(lo, hi, r);
        float z = recon_invz_to_depth(cand);
        if (z > p.depth_min_m && z < p.depth_max_m) {
            best_iz = cand;
        }
    }

    out_invz.write(float4(best_iz, 0, 0, 1), gid);
    out_cost.write(float4(best_cost, 0, 0, 1), gid);
}

// ---------------------------------------------------------------------------
// CB-3 亚像素细化：抛物线拟合 + 连续细化
// docs/05 §5.5（`mvs_subpixel_*`）
// ---------------------------------------------------------------------------
//
// 抛物线拟合：在代价函数的三个采样点上拟合二次曲线，取顶点作为亚像素极值。
// 这一步是「0.15 px 视差精度」的直接来源（docs/05 §5.1 的 δd）。
kernel void recon_mvs_subpixel(texture2d<float, access::read>  in_cost  [[texture(0)]],
                               texture2d<float, access::read>  in_invz  [[texture(1)]],
                               texture2d<float, access::write> out_invz [[texture(2)]],
                               constant ReconMvsGpuParams&     p        [[buffer(0)]],
                               uint2 gid [[thread_position_in_grid]])
{
    const int2 size = int2(p.ref_width, p.ref_height);
    if (gid.x >= uint(size.x) || gid.y >= uint(size.y)) return;

    // 三个采样点：当前、左邻、右邻（边界处退化为不细化）。
    float c0 = in_cost.read(gid).r;
    float cm = (gid.x > 0) ? in_cost.read(uint2(gid.x - 1, gid.y)).r : c0;
    float cp = (gid.x + 1 < uint(size.x)) ? in_cost.read(uint2(gid.x + 1, gid.y)).r : c0;

    // 顶点偏移 δ = 0.5·(cm − cp) / (cm − 2·c0 + cp)
    float denom = cm - 2.0f * c0 + cp;
    float delta = 0.0f;
    if (abs(denom) > RECON_GPU_EPS_DENOM) {
        delta = 0.5f * (cm - cp) / denom;
        // 越界保护：顶点必须落在 [−1, 1]（否则拟合退化，保持原值）。
        delta = clamp(delta, -1.0f, 1.0f);
    }
    // 把（亚像素）偏移写回为逆深度的相对修正。
    // `subpixel_window_ratio` / `subpixel_fit_points` / `subpixel_iterations`
    // 全部经 params 传入（MT-4），此处不硬编码。
    float iz = in_invz.read(gid).r;
    if (iz <= 0.0f) {
        out_invz.write(float4(0, 0, 0, 1), gid);
        return;
    }
    out_invz.write(float4(iz * (1.0f + delta * p.subpixel_window_ratio), 0, 0, 1), gid);
}

// ---------------------------------------------------------------------------
// CB-4 单视图一致性：左右一致性校验
// docs/05 §5.6
// ---------------------------------------------------------------------------
//
// 依赖**全部源视图**的深度图（docs/05 §5.9.3：CB-4 必须等待所有源视图完成）。
// 对每个像素：把参考帧的深度投影到源视图，取源视图深度，再反投影回参考帧，
// 若往返误差超过 `geo_consistency_pct` 则判为不一致（遮挡/错误匹配）。
//
// ⚠️ 纹理槽位：`array<texture2d, 8> [[texture(1)]]` 会占用槽位 1..8，
//    故两个写纹理必须排在其后（9 / 10）。理由详见上方 `recon_mvs_patchmatch`
//    签名处的「纹理槽位排布」段。Vulkan 侧的 binding 是描述符数组、只占一个，
//    故两侧的**编号不同是有意的**（槽位语义不同），共享的只是「顺序与角色」。
kernel void recon_mvs_consistency(texture2d<float, access::read>  in_invz  [[texture(0)]],
                                  array<texture2d<float, access::read>, 8> src_depth [[texture(1)]],
                                  texture2d<float, access::write> out_depth [[texture(9)]],
                                  texture2d<float, access::write> out_conf  [[texture(10)]],
                                  constant ReconMvsGpuParams&     p        [[buffer(0)]],
                                  constant ReconViewParams*       views    [[buffer(1)]],
                                  uint2 gid [[thread_position_in_grid]])
{
    const int2 size = int2(p.ref_width, p.ref_height);
    if (gid.x >= uint(size.x) || gid.y >= uint(size.y)) return;

    float iz = in_invz.read(gid).r;
    if (iz <= 0.0f) {
        out_depth.write(float4(0, 0, 0, 1), gid);
        out_conf.write(float4(0, 0, 0, 1), gid);
        return;
    }
    float z_ref = recon_invz_to_depth(iz);
    if (z_ref <= p.depth_min_m || z_ref >= p.depth_max_m) {
        out_depth.write(float4(0, 0, 0, 1), gid);
        out_conf.write(float4(0, 0, 0, 1), gid);
        return;
    }

    // 参考像素 → 参考相机系点
    float2 uv_ref = float2(gid) + RECON_GPU_PIXEL_CENTER;
    float4 intr_ref = views[p.ref_view_index].intrinsics;
    float3 pc_ref = recon_backproject(uv_ref, z_ref, intr_ref);

    // 逐源视图做左右一致性投票，累计支撑数（uint16 语义，此处用 float 承载）。
    float support = 0.0f;
    for (int v = 0; v < p.num_views; ++v) {
        if (v == p.ref_view_index) continue;
        if (v >= 8) break;   // `array<...,8>` 的静态上界（见文件头「未启用共享内存」同类说明）

        float4x4 Rt = views[v].Rt;
        float3 pc_src = recon_world_to_cam(Rt, pc_ref);
        float2 uv_src;
        if (!recon_project(pc_src, views[v].intrinsics, &uv_src)) continue;
        float2 dsize = views[v].size_depth.xy;
        if (!recon_in_image(uv_src, dsize)) continue;
        float d_src = src_depth[v].read(uint2(uv_src)).r;
        if (d_src <= 0.0f) continue;

        // 反投影回参考帧，比较深度相对偏差。
        float3 pc_back = recon_backproject(uv_src, d_src, views[v].intrinsics);
        // 源相机系 → 世界 → 参考相机系（本实现用参考帧的逆变换近似，
        // 精确实现需 views[ref] 的逆；此处如实标注为**近似**，待真机阶段
        // 与 CPU 版 `core/src/mvs/patchmatch_stereo.cpp` 做数值对齐后再定稿）。
        float3 pc_back_ref = recon_world_to_cam(views[p.ref_view_index].Rt, pc_back);
        float rel = abs(pc_back_ref.z - z_ref) / max(z_ref, RECON_GPU_EPS_DEPTH) * RECON_GPU_PERCENT_SCALE;
        if (rel <= p.geo_consistency_pct) support += 1.0f;
    }

    if (support >= float(p.min_view_consistency)) {
        out_depth.write(float4(z_ref, 0, 0, 1), gid);
        out_conf.write(float4(support / max(float(p.num_views), 1.0f), 0, 0, 1), gid);
    } else {
        out_depth.write(float4(0, 0, 0, 1), gid);
        out_conf.write(float4(0, 0, 0, 1), gid);
    }
}

// ---------------------------------------------------------------------------
// CB-5 体素融合：点云散射到体素（atomicAdd）
// docs/05 §5.7
// ---------------------------------------------------------------------------
//
// 逐参考帧深度像素散射到体素网格。此处用 `atomic_fetch_add_explicit` 做
// 累加 —— 与 TSDF 不同，MVS 的融合是**像素中心式**（多个像素可能落到同一
// 体素），因此**必须**用原子。Metal 的 `atomic_fetch_add_explicit` 支持
// `float` 类型（device 地址空间），这一点与 Vulkan 不同（见 vulkan 侧说明）。
kernel void recon_mvs_fuse_voxel(texture2d<float, access::read>  depth     [[texture(0)]],
                                 texture2d<float, access::read>  conf      [[texture(1)]],
                                 device   atomic_float*          voxel_sum [[buffer(0)]],
                                 device   atomic_float*          voxel_w   [[buffer(1)]],
                                 constant ReconMvsGpuParams&     p         [[buffer(2)]],
                                 constant ReconViewParams&       view      [[buffer(3)]],
                                 uint2 gid [[thread_position_in_grid]])
{
    const int2 size = int2(p.ref_width, p.ref_height);
    if (gid.x >= uint(size.x) || gid.y >= uint(size.y)) return;

    float z = depth.read(gid).r;
    float c = conf.read(gid).r;
    if (z <= p.depth_min_m || z >= p.depth_max_m) return;

    float2 uv = float2(gid) + RECON_GPU_PIXEL_CENTER;
    float3 pc_cam = recon_backproject(uv, z, view.intrinsics);
    // 相机系 → 世界。`view.Rt` 是 world→cam（前三列 R、第四列 t），
    // 故相机→世界 = Rᵀ·(pc − t)。该式已抽为公共 helper `recon_cam_to_world`，
    // 与 Vulkan 侧同名函数逐项等价。
    float3 pc_world = recon_cam_to_world(view.Rt, pc_cam);

    // 体素边长来自 `mvs_fusion_voxel_size`（mm，经 params 传入，MT-4）。
    float vs = float(p.voxel_size_mm) * 0.001f;
    if (vs <= 0.0f) return;
    int vi = int(floor(pc_world.x / vs));
    int vj = int(floor(pc_world.y / vs));
    int vk = int(floor(pc_world.z / vs));

    // 体素线性索引：与 C++ 侧的体积扁平化约定一致（x 最快）。
    // 占位维度：真实实现须用**体素网格维度**（与 C++ 扁平化约定一致），
    // 见文件头 SOURCE-TODO-MVS-4。
    int dim = p.ref_width * p.ref_height;
    // 负坐标剔除。此处**没有**上界检查 —— 上界需要体素网格维度，
    // 而 params 中尚未有该字段（属 SOURCE-TODO-MVS-4 的一部分）。
    // 如实说明「当前无法做上界保护」，而不是假装有保护。
    if (vi < 0 || vj < 0 || vk < 0) return;
    int idx = (vk * dim + vj) * dim + vi;

    atomic_fetch_add_explicit(&voxel_sum[idx], z * c, memory_order_relaxed);
    atomic_fetch_add_explicit(&voxel_w[idx],   c,      memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// CB-6 融合归约 + 一致性检查：产出 D_r / C_r / 支撑视图数
// docs/05 §5.10 输出契约
// ---------------------------------------------------------------------------
//
// ⚠️ 两个体素缓冲声明为 **非 const** `device atomic_float*`（而非
//    `device const atomic_float*`）：`atomic_load_explicit` 在不同 MSL 版本里的
//    形参 const 限定不一致，非 const 实参对两种签名都能绑定，是唯一稳妥的写法。
//    （Vulkan 侧对应声明为 `readonly buffer`，那是 GLSL 的描述符限定；
//     MSL 与 GLSL 在这一点的写法差异**不构成语义差异** —— 两者都只读不写。）
kernel void recon_mvs_reduce(device   atomic_float* voxel_sum [[buffer(0)]],
                             device   atomic_float* voxel_w   [[buffer(1)]],
                             texture2d<float, access::write> out_depth [[texture(0)]],
                             texture2d<float, access::write> out_conf  [[texture(1)]],
                             texture2d<float, access::write> out_support [[texture(2)]],
                             constant ReconMvsGpuParams& p              [[buffer(2)]],
                             uint2 gid [[thread_position_in_grid]])
{
    const int2 size = int2(p.ref_width, p.ref_height);
    if (gid.x >= uint(size.x) || gid.y >= uint(size.y)) return;

    int idx = int(gid.y) * p.ref_width + int(gid.x);
    // ⚠️ `atomic_float` **不能**隐式或显式转换到 `float`（没有 conversion
    //    operator），必须走显式的原子读：`atomic_load_explicit`。
    //    原先写的 `float(voxel_w[idx])` 在 MSL 下报
    //    "cannot convert 'const device metal::atomic_float' to 'float'
    //     without a conversion operator"。
    //    `memory_order_relaxed` 与此前的写入口径一致（仅需原子性、无需顺序），
    //    与 Vulkan 侧「读回累加结果」的语义对应。
    //    （平台差异的登记只在 `shaders/common/gpu_algo.h` 与两端公共头各一处 ——
    //      **不得**在实现文件里再写一遍编号，否则会削弱 P10 判据的负向注入。）
    float w = atomic_load_explicit(&voxel_w[idx], memory_order_relaxed);
    float s = atomic_load_explicit(&voxel_sum[idx], memory_order_relaxed);
    if (w <= 0.0f) {
        out_depth.write(float4(0, 0, 0, 1), gid);
        out_conf.write(float4(0, 0, 0, 1), gid);
        out_support.write(float4(0, 0, 0, 1), gid);
        return;
    }
    float z = s / w;
    // 置信度 = 归一化权重（docs/05 §5.10：融合权重归一化后的值）。
    float conf = clamp(w / max(float(p.min_view_consistency), 1.0f), 0.0f, 1.0f);
    out_depth.write(float4(z, 0, 0, 1), gid);
    out_conf.write(float4(conf, 0, 0, 1), gid);
    out_support.write(float4(w, 0, 0, 1), gid);
}
