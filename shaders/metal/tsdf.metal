// SPDX-License-Identifier: BSD-3-Clause
//
// tsdf.metal — TSDF 体素中心式融合（Metal 实现）
//
// 契约：docs/04_RGBD管线.md §4.6（GPU 实现）。
//
// ── 为什么是体素中心式（voxel-centric）─────────────────────────────────────
// docs/04 §4.6.2 明文选定体素中心式，理由是**原子操作需求**：
//
//   体素中心式：每线程处理**一个体素** → 该体素本帧只被这一个线程写
//               → S/W/C 的更新**不需要原子**；仅块分配需要原子。
//   像素中心式：每线程处理一个像素 → 沿射线遍历体素、逐体素累加
//               → S/W/C **全部需要浮点原子累加**，而 Metal/Vulkan **无**
//                  浮点原子（Metal 有 `atomic_float` 但 Vulkan 的
//                  `atomicAdd` 只对整型可用）→ 方案不可行。
//
// 因此「用体素中心式」不是风格选择，而是**被后端能力约束的唯一可行解**。
// 这一点的工程含义是：**S/W/C 的写入顺序不影响结果**（无需依赖累加对称性）。
//
// ── 与 docs/04 §4.6.2 的五步流程逐条对应 ─────────────────────────────────
//   ① P_c = R_i · p_v + t_i           → recon_world_to_cam
//   ② (u,v) = π_K(P_c)                → recon_project，P_c.z ≤ 0 或越界则跳过
//   ③ d = D_i(u,v)  最近邻采样          → src_depth.read(nearest)，无效则跳过
//   ④ 早退：|d − P_c.z| > μ → 跳过     ★ 把 99% 体素在第 ④ 步剔除
//   ⑤ s = d − P_c.z；t = clamp(s/μ,−1,1)
//   ⑥ 算权重 w（§2.4.4 映射链）
//   ⑦ S += w·t ; W += w ; C += w·c
//
// ── 采样方式（docs/04 §4.6.1 表格「采样」行）──────────────────────────────
//   「`texture2d<float, access::read>`」/「深度图必须用**最近邻**采样」
// 本文件用 `access::read` + `read(uint2)`，**不做**插值 —— 这正是最近邻。
//
// ══════════════════════════════════════════════════════════════════════════
// ⚠️ 实现状态（如实阅读）
// ══════════════════════════════════════════════════════════════════════════
// 已实现：docs/04 §4.6.2 的七步流程、最近邻采样、早退、截断、权重映射链、
//         体素线性索引解码、**无原子写入**（体素中心式的结构性结果）。
//
// 未实现：
//   SOURCE-TODO-TSDF-1  块分配原子（docs/04 §4.6.4）。当前假设块已预分配，
//                  故本文件无原子操作。接入块分配器后需新增
//                  `atomic_compare_exchange_weak`。
//   SOURCE-TODO-TSDF-2  颜色为**单通道**占位。真实实现按 3 通道存
//                  （`tsdf_c` 需是 `device float*` 的 3 倍长度或 `float3*`）。
//                  此处单通道是**明确缩写**，不是"忘了第三通道"。
//   SOURCE-TODO-TSDF-3  体素法向未计算，视角权重用「视线与光轴夹角」近似。
//   SOURCE-TODO-TSDF-4  未做 halo 跨块邻域查询（`SurfaceGradient` 需要，
//                  见 core/include/recon/tsdf_fusion.h 的「实现口径」说明）。
// ══════════════════════════════════════════════════════════════════════════

#include "gpu_common.h"

// ---------------------------------------------------------------------------
// TSDF 体素中心式融合
// 契约：docs/04 §4.6.2；算法名对应 shaders/common/gpu_algo.h 的
//       RECON_GPU_ALGO(recon_tsdf_fuse)（MT-3）。
// ---------------------------------------------------------------------------
//
// 分派：`dispatchThreads(grid_dim)`，一维展开，每线程解出一个体素索引。
// 用一维而非三维分派：iOS 各机型的 `maxThreadsPerThreadgroup` 对三维
// 分派有更紧的限制（尤其 z 维），一维展开后由线程内自行解码更稳。
//
// ── 块分配（docs/04 §4.6.4：仅此处需要原子）──────────────────────────────
// 真实实现需在遇到「块尚未分配」时用 `atomic_compare_exchange_weak` 抢块。
// 本实现假设块已在 C++ 侧预分配（`block_count` 由 params 给出），因此
// **本文件没有原子操作** —— 这是如实状态，不是遗漏；
// 待接入块分配器后此处会新增一处原子，届时闸门与文档同步更新。
kernel void recon_tsdf_fuse(texture2d<float, access::read> src_depth [[texture(0)]],
                            texture2d<float, access::read> src_rgb   [[texture(1)]],
                            device   float*                 tsdf_s   [[buffer(0)]],
                            device   float*                 tsdf_w   [[buffer(1)]],
                            device   float*                 tsdf_c   [[buffer(2)]],
                            constant ReconTsdfGpuParams&    p        [[buffer(3)]],
                            constant ReconViewParams&       view     [[buffer(4)]],
                            uint gid [[thread_position_in_grid]])
{
    // --- 体素线性索引 → 三维坐标 ------------------------------------------
    const int dimx = p.grid_dim_x;
    const int dimy = p.grid_dim_y;
    const int dimz = p.grid_dim_z;
    const int total = dimx * dimy * dimz;
    if (int(gid) >= total) return;

    const int vz = int(gid) / (dimx * dimy);
    const int rest = int(gid) - vz * dimx * dimy;
    const int vy = rest / dimx;
    const int vx = rest - vy * dimx;

    // --- ① 体素中心的世界坐标 → 相机系 ------------------------------------
    float3 pw = float3(p.origin_x + (float(vx) + 0.5f) * p.voxel_size_m,
                       p.origin_y + (float(vy) + 0.5f) * p.voxel_size_m,
                       p.origin_z + (float(vz) + 0.5f) * p.voxel_size_m);
    float3 pc = recon_world_to_cam(view.Rt, pw);

    // --- ② 投影；P_c.z ≤ 0 或越界 → 跳过 ---------------------------------
    float2 uv;
    if (!recon_project(pc, view.intrinsics, &uv)) return;
    const float2 dsize = view.size_depth.xy;
    if (!recon_in_image(uv, dsize)) return;

    // 深度范围门限（docs/04 §4 的 min_depth_m / max_depth_m）。
    if (pc.z < p.min_depth_m || pc.z > p.max_depth_m) return;

    // --- ③ 最近邻查深度；无效则跳过 ---------------------------------------
    // `access::read` + `read(uint2)` = 最近邻（**不**插值），
    // 这是 docs/04 §4.6.1 的硬要求：深度图不得做插值。
    const uint2 pix = uint2(uv.x, uv.y);
    const float d = src_depth.read(pix).r;
    if (d <= p.min_depth_m || d >= p.max_depth_m) return;

    // --- ④ 早退：|d − P_c.z| > μ → 跳过（剔除 99% 体素）-------------------
    const float mu = p.truncation_m;
    const float s  = d - pc.z;
    if (abs(s) > mu) return;

    // --- ⑤ 截断与归一化 ---------------------------------------------------
    const float t = clamp(s / mu, -1.0f, 1.0f);

    // --- ⑥ 权重（docs/04 §2.4.4 映射链）-----------------------------------
    // 视角权重：体素法向未计算（§4.6 未要求），用视线与光轴的夹角余弦近似。
    const float cos_theta = clamp(pc.z / max(length(pc), RECON_GPU_EPS_DEPTH), 0.0f, 1.0f);
    const float w = recon_tsdf_weight(t, cos_theta, p.weight_mode);
    if (w <= 0.0f) return;

    // --- ⑦ 更新自身（**体素中心式**：本帧只有本线程写这三个量，无需原子）---
    const int idx = int(gid);
    // 颜色同样用最近邻像素（TSDF 颜色只是低频底色，docs/04 §4.5 边界说明）。
    const float3 c = src_rgb.read(pix).rgb;

    tsdf_s[idx] += w * t;
    tsdf_w[idx] += w;
    tsdf_c[idx] += w * c.r;   // 单通道占位；真实实现按 3 通道存（见 SOURCE-TODO-TSDF-2）
}
