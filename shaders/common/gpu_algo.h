// SPDX-License-Identifier: BSD-3-Clause
//
// gpu_algo.h — GPU 算法的**唯一事实来源**（docs/12 §4.4、docs/00 §3.2）
//
// ── 本文件解决什么问题 ────────────────────────────────────────────────────
// 文档契约是「算法同源、两端分别编译」：`shaders/common/` 描述算法，
// `shaders/metal/*.metal` 与 `shaders/vulkan/*.comp` 是各自实现，
// 二者的**函数签名与数学式必须逐条对齐**，由 `tools/shader_parity.py` 校验
// （docs/14 CT-6 / MT-3）。
//
// 但「对齐」如果靠人肉比对，就必然漂移 —— 本项目已经反复吃过这个亏
// （见 MEMORY.md「同一逻辑写两遍必然漂移」）。因此这里把**算法名**提升为
// **机器可解析的单一来源**：本文件用 `RECON_GPU_ALGO(name)` 声明算法名，
// Metal 与 Vulkan 各自的入口函数必须使用**同名**，闸门从本文件解析期望集合、
// 再去两边核对。任一端多写、少写、改名 → CI 失败。
//
// ── 为什么用宏而不是一份 JSON 清单 ────────────────────────────────────────
// 一份 JSON 清单 + 本头文件 = **两处事实来源**，必然漂移（同上）。
// 用宏则「声明算法」与「注册算法」是同一个动作，结构上不可能不一致。
//
// ── 单独一份 `all()` 列表也不行 ──────────────────────────────────────────
// 早期设计曾打算在本文件末尾手写一个 `all_algo_names[]` 数组供闸门读取。
// 那是**手抄第二份** —— 与 `L10nKey.allRawValues` 手抄表是同一个坑
// （该手抄表曾漏 50 项并引用已删 case，直接导致编译失败）。
// 故此处**不提供**任何手抄汇总：闸门的判据是「扫描本文件中
// `RECON_GPU_ALGO(` 的出现」，声明即注册。
//
// ── MT-4：常量不得硬编码 ─────────────────────────────────────────────────
// 所有可调常量（`mvs_gpu_tile_size` / workgroup 尺寸 / halo 宽度等）
// **不得**在 `.metal` / `.comp` 中写成字面量，必须经 buffer（UBO / setBytes）
// 从 C++ 侧传入。这样 docs/13 的降级（如 tile 1024 → 512）才能在不重编译
// 着色器的前提下生效，也才满足 docs/14 CT-9「无静默降级」的可审计性。
//
// 本文件定义每个算法的**参数块布局**（`struct`），Shaders 侧按同一布局声明
// buffer；布局不一致由 `tools/shader_parity.py` 的字段比对兜住。

#ifndef RECON_SHADERS_GPU_ALGO_H_
#define RECON_SHADERS_GPU_ALGO_H_

#include <stdint.h>

// ---------------------------------------------------------------------------
// 算法注册宏
// ---------------------------------------------------------------------------
//
// 用法：`RECON_GPU_ALGO(recon_mvs_seed)` —— 参数即算法名（同时是
// Metal 函数名与 Vulkan 入口函数名，见 MT-3）。
//
// 在 C++ 侧它展开为一条不带语义的声明（仅用于让「算法名」成为**真实符号**，
// 而不是散落在注释里的字符串）；在沙盒侧不需要它。
//
// 注意：宏名必须保持**唯一**。同名重复声明会被闸门报为重复（否则两端
// 「函数名一致」这一判据会因为重名而互相掩盖）。
#define RECON_GPU_ALGO(function_name) \
    /* 算法：function_name —— 见本文件对应段落与 docs 契约 */ \
    struct recon_gpu_algo_##function_name##_tag {}

// ---------------------------------------------------------------------------
// workgroup 尺寸（docs/05 §5.9.1：统一 8×8 = 64 线程）
// ---------------------------------------------------------------------------
//
// 契约原文：「workgroup 统一 8×8 = 64 线程」，理由是共享内存占用
// `(8+10)² × 4 B + (8+10)² × 1 B ≈ 7.4 KB`，在 iOS 典型
// `maxTotalThreadgroupMemory` 32 KB 以内，占用率高。
//
// 这两个值是**编译期常量**（Metal 的 `threadgroup_size` 属性与 Vulkan 的
// `local_size_x/y/z` 都要求编译期字面量），因此它们**允许**在各端着色器中
// 出现字面量 —— 这是 MT-4 的**唯一例外**，因为着色器语言无法把 layout
// 限定符写成运行期变量。闸门对这两个值做**一致性检查**（两端都必须是 8/8）
// 而不是「禁止字面量」。
#define RECON_GPU_WORKGROUP_X 8
#define RECON_GPU_WORKGROUP_Y 8
#define RECON_GPU_WORKGROUP_Z 1

// halo 宽度（像素）。docs/13 §3.2.1 / docs/05 §5.9.1：「workgroup 覆盖 8×8 +
// **5 px halo**（`shaders/common` 统一）」。halo 用于把 NCC 窗口与线性采样的
// 邻域一次搬进 threadgroup/shared 内存，把复用率提到 90%。
//
// halo 是**算法参数**而非布局限定符，故按 MT-4 经 buffer 传入（见每个
// 参数块的 `halo_px` 字段）；此处常量仅供 C++ 侧做默认值与断言使用。
#define RECON_GPU_HALO_PX 5

// ---------------------------------------------------------------------------
// 精度约定（docs/05 §5.9.1 表格「精度」行）
// ---------------------------------------------------------------------------
//
//   「`half`（视差）、`float`（代价）」/「`f16`（需 `shaderFloat16`）、`float32`」
//
// 含义：**视差/深度用 fp16，代价聚合用 fp32**。
// 文档给出的理由是「视差用 fp16 足够（1/1024 像素精度）」。
//
// ⚠️ 与 docs/12 §「GPU 着色器」行的冲突（如实登记）：
//    docs/12 的体积/精度要求表里写「`mvs` 的视差精度用 `float32` 而非 `f16`
//    （05 §5.9.1 精度表需按 ABI 分支）」。而 docs/05 §5.9.1 的表格写视差用
//    `half`/`f16`。两份文档**互相矛盾**。
//
//    本项目对此的处置（对齐既往教训「两份契约矛盾时，以可验证判据裁决」）：
//      - **默认 `float32`**（取更保守、更高精度者，符合「精度优先」铁律）；
//      - 是否启用 fp16 由**运行期能力探测**决定，而非编译期分支：
//        探测 Metal `supportsFamily(.apple7)` 的 fp16 能力 /
//        Vulkan `VkPhysicalDevice16BitStorageFeatures.shaderStorageBuffer16BitAccess`。
//        探测结果经 buffer 传入（`allow_fp16` 字段），着色器按位分支。
//      - 两文档的矛盾已在 docs/14 的缺口清单中登记（见本轮日志）。
//
// 因此：着色器中**不得**硬编码 `half` / `mediump`，一律以 `float` 编写，
// `allow_fp16` 为 1 时由 C++ 侧换成 `-D` 变体编译（预留，当前未启用）。
#define RECON_GPU_ALLOW_FP16_DEFAULT 0

// ---------------------------------------------------------------------------
// 跨端共享数值常量（**两端必须逐位相同**）
// ---------------------------------------------------------------------------
//
// ── 为什么需要登记这些 ────────────────────────────────────────────────────
// 下列数值**不是算法参数**（不属于 docs/08 参数体系、不随降级变化），因此
// 按 MT-4 它们**可以**在着色器里出现；但它们又**必须两端一致** ——
// 若两端各写各的，PatchMatch 的"随机"搜索序列会分叉（哈希常量不同）、
// 一致性判定阈值会不同（eps 不同）、无效代价哨兵会不同（cost_inf 不同），
// 于是「同一输入两端应得同一结果」这一最基本的契约就悄悄破掉了。
//
// 这正是本文件反复出现的那条教训的又一个变体：**同一个量写在两处必然漂移**。
// 处置：这里登记「名字 + 值 + 用途」，两端各自的公共头用**同名同值**定义，
// 闸门 `tools/shader_parity.py` 解析三份文件的 `#define` 并**逐条比对数值**。
// 任一端改了值而不改另一端 → CI 失败。
//
// ── 命名规范 ──────────────────────────────────────────────────────────────
// 共享常量用 `RECON_GPU_<名>`（两端同名）。
// **仅某一端需要**的常量用 `RECON_VK_<名>`（Vulkan）或 `RECON_MSL_<名>`（Metal），
// 前缀即是"这是有意的单侧差异"的声明，闸门对其不做跨端比对但会打印出来，
// 便于人工复核「这个不对称是否真的必要」。
//
// 宏的第二参数是文档说明；值与名字在两端重复书写是**有意的**，因为它们
// 必须能被各自的编译器独立看到（MSL 与 GLSL 都不认识 C 头），
// 而闸门负责把"重复"变成"受控的重复"。

// 深度下界哨兵。`max(z, eps)` 与 `pc.z <= eps` 用它区分「深度为 0（无效）」
// 与「深度极小但有效」。若两端取值不同，则「哪些像素算无效」的边界会不同，
// 是最容易产生「差一个像素」类不一致的地方。
#define RECON_GPU_EPS_DEPTH 1.0e-6f

// 抛物线拟合分母的退化阈值（docs/05 §5.5）。
// 三点近乎共线时分母 → 0，δ 会爆掉；低于该阈值即放弃细化。
// 该阈值决定「什么时候算无法拟合」，两端必须一致，否则同一代价场在两端
// 会得到不同的亚像素修正。
#define RECON_GPU_EPS_DENOM 1.0e-12f

// 坐标哈希常量（`recon_hash_u32`）。PatchMatch 的随机搜索用它生成确定性
// 伪随机数（docs/12 §1.1：确定性构建要求，不用真随机数）。
// **两端必须逐位相同** —— 否则同一像素在两端会跳到不同候选，
// 结果不可比，跨端对拍（golden 回归）直接失效。
#define RECON_GPU_HASH_A 0x7feb352du
#define RECON_GPU_HASH_B 0x846ca68bu

// 哈希 → [0,1) 的归一化因子 = 2^32。
#define RECON_GPU_RAND_SCALE 4294967296.0f

// 「无支撑」的代价哨兵值。用大有限数而非 inf：inf 参与 `mix`
// 会产生 NaN 并污染整条代价链（NaN 传播是**静默**的）。
// 两端必须同值，否则「无效」与「有效但差」的边界不同。
#define RECON_GPU_COST_INF 1.0e9f

// 相对偏差 → 百分比 的换算因子（docs/05 §5.6 一致性判定用百分比表达）。
#define RECON_GPU_PERCENT_SCALE 100.0f

// 像素中心偏移。半像素是**光栅化与投影的中心约定**（`uv = pixel + 0.5`），
// 两端必须一致，否则整个反投影会系统性偏移半个像素 ——
// 这个偏差恰好与 docs/05 §6.1.2 的亚像素精度目标（0.15 px）同量级，
// 足以让"精度达标"的结论失效。
#define RECON_GPU_PIXEL_CENTER 0.5f

// ── 体素累加器的定点化尺度（CB-5 / CB-6 的整数原子方案）───────────────────
//
// 背景：`recon_mvs_fuse_voxel` 是**像素中心式**散射（docs/05 §5.7）——多个
// 深度像素可能落到同一体素，故 `voxel_sum` / `voxel_w` 的累加**必须原子**。
// 而浮点原子的可用性在两个平台上都靠不住：
//   · Metal ：`atomic_float` 仅 **Metal 3.0 / iOS 16** 起才有（MSL 规范 §2.6）；
//     本项目部署下限是 **iOS 15.0**（docs/12 §4.6），故**不可用**。
//   · Vulkan：float `atomicAdd` 需扩展 `VK_EXT_shader_atomic_float`，非核心特性。
//
// ⇒ 两端都退到**定点整数原子**（`int` 原子，Metal 1.0+ / Vulkan 核心特性）。
//   这是**同一套算法参数**，故按本段的规矩登记在 common：值与名字在两端
//   重复书写是有意的（MSL 与 GLSL 都不认识 C 头），闸门逐条比对数值把
//   "重复"管成"受控的重复"。
//
// scale = 2^12：权重量化步长 1/4096 ≈ 2.44e-4，远小于体素离散化本身的误差
// （docs/05 §6.1.2 第 7 级，h_vox/√12），故不影响 MVS 的精度验收项。
//
// 上界 2^30：32 位有符号累加的饱和阈值（在 scale=2^12 下 = 未缩放值 262144）。
//   权重累加：单样本 w ≤ 1 → 缩放后 4096 → 可容纳 262144 个满权样本；
//   加权深度：单样本 z·w，z ≤ depth_max_m（默认 10 m 量级）→ 缩放后 ≤ 40960
//             → 可容纳约 6400 个满量程样本。
//   真实体素（0.5 mm 级）被上千像素命中已属极端，故上界充裕；**但仍饱和而非
//   回绕**（回绕会把大数变负，等价于静默数据损坏）。饱和是**可观测**的
//   （`recon_mvs_reduce` 会产出异常大的值），不是静默的。
//
// ⚠️ `RECON_GPU_FIXED_SCALE_INV` 写成**字面量**而非 `(1.0 / 4096.0)`：
//    1/4096 = 2^-12 在二进制下**精确可表示**，故字面量无精度损失；
//    而写成表达式会让闸门 P7 的数值解析失败（`parse_number` 只认单 token），
//    等于把这条常量从「值必须一致」的判据里摘出去 —— 那是静默削弱判据。
#define RECON_GPU_FIXED_SCALE      4096.0f
#define RECON_GPU_FIXED_SCALE_INV  0.000244140625f   // = 1/4096 = 2^-12（二进制精确）
#define RECON_GPU_FIXED_MAX        1073741824        // 2^30

// ---------------------------------------------------------------------------
// 参数块：MVS（docs/05 §5.9.1–§5.9.3）
// ---------------------------------------------------------------------------
//
// ── 为什么所有字段都是显式宽度类型 ────────────────────────────────────────
// 这个结构体同时被 C++（构造 UBO）、Metal（`constant` buffer）、
// Vulkan（`std140` UBO）三方解释。任何一方用平台宽度类型（`int` / `long`）
// 都会造成**布局错位**，而错位在两端各自都能编译通过 —— 属静默失效。
// 故一律 `int32_t` / `uint32_t` / `float`，且闸门会比对字段集合。
//
// ── std140 对齐要求（Vulkan 侧）────────────────────────────────────────────
// 字段顺序按「4 字节 → 4 字节」排列，不混入 `double` / `vec3`，避免
// std140 的 16 字节对齐规则引入隐式填充。若将来必须加 `vec3`，需同步
// 修改 Metal 侧的填充，并由闸门强制。
struct ReconMvsGpuParams {
    // 参考帧与源视图
    int32_t  ref_width;              // 参考帧宽（像素）
    int32_t  ref_height;             // 参考帧高（像素）
    int32_t  num_views;              // 源视图数（含参考帧自身）
    int32_t  ref_view_index;         // 参考视图在视图数组中的下标

    // tile 调度（docs/05 §5.9.2 层次 2；docs/13 §2.4 可降级项）
    int32_t  tile_size_px;           // `mvs_gpu_tile_size`，默认 1024
    int32_t  tiles_x;                // 横向 tile 数
    int32_t  tiles_y;                // 纵向 tile 数
    int32_t  halo_px;                // halo 宽度，默认 RECON_GPU_HALO_PX

    // PatchMatch（docs/05 §5.4）
    int32_t  patch_size;             // NCC 窗口边长，默认 11
    int32_t  iterations;             // 外迭代轮数，默认 4
    int32_t  random_search_count;    // 随机搜索次数，默认 5
    float    rand_search_radius;     // 随机搜索初始半径（逆深度相对比例）

    float    restart_ratio;          // 随机重启比例
    float    photo_threshold;        // 1 − NCC 接受阈值
    float    depth_reg_lambda;       // 平滑正则权重
    float    geo_consistency_pct;    // 几何一致性阈值（百分比）

    // 亚像素细化（docs/05 §5.5）
    int32_t  subpixel_fit_points;    // 抛物线拟合点数
    int32_t  subpixel_iterations;    // 连续细化迭代数
    float    subpixel_window_ratio;  // 细化窗口（逆深度相对比例）
    int32_t  red_black;              // 本次 dispatch 的颜色（0=红 1=黑），红黑交替

    // 深度范围与一致性
    float    depth_min_m;            // 有效深度下界（米）
    float    depth_max_m;            // 有效深度上界（米）
    int32_t  min_view_consistency;   // 有效支撑视图数下限，默认 2
    int32_t  allow_fp16;             // 运行期能力探测结果（见上「精度约定」）

    // 运行时按 docs/13 §2.4 可降级项（**不得**降精度参数）
    int32_t  batch_size;             // `perf_gpu_batch_size`
    int32_t  memory_budget_mb;       // `perf_gpu_memory_budget_mb`
    int32_t  voxel_size_mm;          // `mvs_fusion_voxel_size`（毫米，整数传输）
    int32_t  reserved0;              // 保留：保持 16 字节对齐，显式占位
};

// ---------------------------------------------------------------------------
// 参数块：TSDF 融合（docs/04 §4.6.1–§4.6.2）
// ---------------------------------------------------------------------------
//
// 体素中心式（voxel-centric）：每个线程负责**一个体素**，做
// 变换 → 投影 → 查深度 → 早退判定 → 更新自身。
//
// 关键性质（docs/04 §4.6.2）：体素 v 在本帧只被「包含 v 的那个块」的
// 一个线程处理 → S/W/C 的更新**不需要原子操作**；**仅块分配需要原子**
// （`atomic_compare_exchange_weak` / `atomicCompareExchangeWeak`）。
//
// 文档明确**不采用**像素中心式（point-centric），因为 Metal/Vulkan 无浮点
// 原子累加。本结构体因此不含任何「累加顺序」相关字段 —— 累加顺序无关性
// 是体素中心式的**结构性收益**，不是靠约定维持的。
struct ReconTsdfGpuParams {
    // 体素网格
    int32_t  grid_dim_x;             // 网格尺寸（体素）
    int32_t  grid_dim_y;
    int32_t  grid_dim_z;
    int32_t  block_voxels;           // 分块边长（docs/13 §3.2.1）

    float    voxel_size_m;           // 体素边长（米）—— 黑名单 B-1，**不得**降级
    float    truncation_m;           // 截断距离 μ（米）
    float    min_depth_m;            // 融合深度下界
    float    max_depth_m;            // 融合深度上界

    // 相机内参（像素）
    float    fx;
    float    fy;
    float    cx;
    float    cy;

    // 网格原点（世界系，米）
    float    origin_x;
    float    origin_y;
    float    origin_z;
    int32_t  frame_index;            // 当前帧序号（诊断用）

    int32_t  block_count;            // 本次 dispatch 覆盖的块数
    int32_t  max_blocks_in_memory;   // `rgbd_max_blocks_in_memory`（可降级项）
    int32_t  weight_mode;            // 权重映射链选择（docs/04 §2.4.4）
    int32_t  reserved0;              // 保留：16 字节对齐
};

// ---------------------------------------------------------------------------
// 参数块：YUV → BGRA 色彩转换（docs/11 §3.1.3 / §3.4 / IOS-AR-5）
// ---------------------------------------------------------------------------
//
// ── 为什么这是一个「跨端算法」而不是 iOS 专属 ──────────────────────────────
// 两端的采集格式**不是** BGRA8，都必须做 YCbCr → RGB 矩阵变换：
//   · iOS（ARKit 模式）：`ARFrame.capturedImage` 是
//     `420YpCbCr8BiPlanarFullRange`（docs/11 IOS-AR-5）→ 双平面 4:2:0。
//   · Android：`ImageReader` 的 `ImageFormat.YUV_420_888`（docs/11 §3.4）
//     → 同样是双平面 4:2:0（`Image.Plane[0]` = Y，`Plane[1]` = Cb，`Plane[2]` = Cr，
//     其中的 U/V 交错与否由 `pixelStride` 决定 —— 见下方 YUV-1 缺口）。
//
// 而 `recon_frame_packet.rgb_data` 的契约是 **BGRA8**（docs/00 §7.2 /
// docs/02 §5.5）。故「YUV → BGRA」这一步是**双端共同需要**的同一件事 →
// 按铁律必须**同源**（写两遍必然漂移，见 MEMORY.md），落入本文件。
//
// ── 为什么 iOS 有两条互斥路径，而这里选 Metal compute ─────────────────────
// docs/11 §3.1.3 规定 `AVCaptureVideoDataOutput` 可设
// `kCVPixelFormatType_32BGRA` 而**零转换**；但 docs/11 §693 IOS-AR-5 规定
// **ARKit 模式**下 `capturedImage` 的格式由 ARKit 固定为
// `420YpCbCr8BiPlanarFullRange`，**不可协商**。本项目锁定 ARKit 作为首选
// 采集源（RGB 与深度同帧、时间戳天然一致，见 `CameraCapture.swift` 文件头），
// 因此**只有 YUV 路径可达**，转换必须做。IOS-AR-5 明确指定了做法与预算：
// Metal compute kernel，1080p 单帧 < 0.4 ms。本参数块即该 kernel 的接口。
//
// ── 为什么「所有系数都由 C++ 侧算出后传入」（MT-4 的极端化应用）──────────
// 直觉写法是在着色器里写 `if (matrix == BT709) { kr = 0.2126; } …`。
// 本项目**刻意不这样做**，理由有三：
//   ① MT-4：可调常量不得硬编码在着色器里 —— 色彩矩阵是**算法参数**
//      （由 `CVPixelBufferGetAttachment(kCVImageBufferYCbCrMatrix)` /
//      `Image.getColorSpace()` 在运行期决定），必须经 buffer 传。
//   ② 两端**逐位相同**：若 Metal 写一套系数、Vulkan 写另一套，跨端对拍失效。
//      把系数算在**一份 C++ 实现**（`platform/common/src/yuv_convert.cpp`）里，
//      两端只做 FMA，则「系数在哪算」只有一处。
//   ③ 可**桌面单测**：色彩数学的正确性（尤其是 limited/full range 的偏置）
//      是本项目历史上最容易错的类型（差 16/255 的偏置会变成整体灰蒙），
//      而着色器数值**本机永远测不了**（无 Metal/glslang）。把数学搬进
//      C++ 规则本体后，它就被 `test_platform_yuv.cpp` 在桌面锁定。
//
// 结果：着色器里**一个数值字面量都没有**（除 workgroup 尺寸这一 MT-4 唯一例外），
// 只有 `y + r_from_cr * cr` 这样的线性代数。这也是两端实现能逐字相同的前提。
//
// ── 字段类型与本参数块的 std140 安全性 ────────────────────────────────────
// **恰好 20 个** 4 字节标量 = 80 B，80 % 16 == 0 → std140 下**无尾部填充**，
// 三方（C++ / MSL / GLSL）字节级一致（论证见 `gpu_common.glsl` 第三节）。
// ⚠️ 增删字段会**同时**改变 std140 布局与两端声明；必须一次改齐，
//    且改完仍须满足「字段数 × 4 ≡ 0 (mod 16)」，否则会引入隐式填充。
struct ReconYuvToBgraGpuParams {
    // 输入平面几何。Y 平面为**全分辨率**，色度平面为**半分辨率**（4:2:0）。
    int32_t  y_width;                // Y 平面宽（像素）
    int32_t  y_height;               // Y 平面高（像素）
    int32_t  cbcr_width;             // 色度平面宽（= y_width / 2，向上取整）
    int32_t  cbcr_height;            // 色度平面高（= y_height / 2，向上取整）

    // 行字节数（**必须显式传**：`CVPixelBufferGetBytesPerRowOfPlane` 与
    // `Image.Plane.getRowStride()` 都**可能带行填充**，不等于 width × 字节数）。
    int32_t  y_stride_bytes;         // Y 平面每行字节数
    int32_t  cbcr_stride_bytes;      // 色度平面每行字节数

    // 输出 BGRA8 几何。通常与 Y 平面同尺寸（**不重采样**：docs/04 IR-1 精神）。
    int32_t  out_width;
    int32_t  out_height;
    int32_t  out_stride_bytes;       // 输出每行字节数（通常 = out_width × 4）

    // ── 色度平面布局（**两端真正的差异点**，故显式建模而非隐式假设）──────────
    //   chroma_pixel_stride = 2：Cb/Cr **交错**在同一平面（iOS 的
    //        `420YpCbCr8BiPlanarFullRange` 是这种；Android 上绝大多数机型的
    //        `YUV_420_888` 底层也是 NV12/NV21 半平面，`Plane[1].pixelStride==2`）。
    //        → 一个色度纹理即可同时取到 Cb 与 Cr（相邻两字节）。
    //   chroma_pixel_stride = 1：Cb/Cr 各自独立成平面（真平面 I420）。
    //        → **本算法当前不支持**，见缺口 YUV-1；调用方必须走 CPU 回退。
    //   cb_index_in_pair：交错平面内 Cb 的字节下标。0 = CbCr（NV12 / iOS），
    //        1 = CrCb（NV21）。取值由平台层按实际格式判定后传入。
    int32_t  chroma_pixel_stride;    // 见上（1 或 2）
    int32_t  cb_index_in_pair;       // 见上（0 或 1）

    // ── 范围归一化（8 位整数 → 浮点）────────────────────────────────────────
    // 计算式（两端逐字相同，**且与 CPU 规则本体逐字相同**）：
    //     y = (Y  − y_bias) × y_gain          结果 ∈ [0, 1]
    //     c = (C  − c_bias) × c_gain          结果 ∈ [−0.5, +0.5]
    //
    // ⚠️ 色度中心**恒为 128**（两种 range 相同），只有增益不同：
    //     full range（8 bit）：y_bias=0,  y_gain=1/255；c_bias=128, c_gain=1/255
    //     limited range     ：y_bias=16, y_gain=1/219；c_bias=128, c_gain=1/224
    //
    // 为什么色度中心是 128 而不是 0 或 127.5（**这里极易写错，故推导一次**）：
    //   工业标准式是**未归一化**的整数形式（BT.601 full range）：
    //       R = Y + 1.402·(Cr − 128)
    //   本算法用归一化形式 `r = y + r_from_cr · cr`。代入 `c = (Cr − c_bias)·c_gain`
    //   并乘回 255 还原到整数域：
    //       255·r = Y + 255·c_gain·r_from_cr·(Cr − c_bias)
    //   要与标准式恒等，需 `c_bias = 128` **且** `255·c_gain·r_from_cr = 1.402`。
    //   而 r_from_cr = 2(1−kr) = 1.402（BT.601）→ `c_gain = 1/255` 恰好成立。
    //   若误取 c_bias = 127.5，则 Cr=128（中性灰/无色）时会得到非零色度 →
    //   **整幅灰度图被染上偏色**，且不报错（本项目「静默失效」的典型形态）。
    //   若误取 c_bias = 0，则 c ∈ [0,1] 而非 [−0.5,0.5] → 同样整体偏色。
    //
    // ⚠️ iOS 的 `420YpCbCr8BiPlanar**FullRange**` 是 full range；但**不得**
    //    因此就把 limited range 分支砍掉 —— Android 侧（`Image.getColorSpace()`
    //    返回 `SRGB`/`BT709` limited）与本机模拟器会用到，且「按平台元数据
    //    选分支」才是契约要求（IOS-AR-5 / docs/11 §3.4）。
    float    y_bias;
    float    y_gain;
    float    c_bias;
    float    c_gain;

    // ── 色彩矩阵增益（由 kr / kb 派生，**在 C++ 规则本体里算**）─────────────
    // 定义式（BT.601 与 BT.709 同构，仅 kr/kb 不同）：
    //     R = Y + r_from_cr · Cr
    //     B = Y + b_from_cb · Cb
    //     G = Y + g_from_cr · Cr + g_from_cb · Cb
    // 其中（kg = 1 − kr − kb）：
    //     r_from_cr = 2(1 − kr)
    //     b_from_cb = 2(1 − kb)
    //     g_from_cr = −2·kr·(1 − kr) / kg
    //     g_from_cb = −2·kb·(1 − kb) / kg
    // |kr| / |kb|：BT.601 → 0.299 / 0.114；BT.709 → 0.2126 / 0.0722。
    float    r_from_cr;
    float    b_from_cb;
    float    g_from_cr;
    float    g_from_cb;

    // 保留：使字段总数为 **20**（20 × 4 B = 80 B，80 % 16 == 0）。
    // ⚠️ 这不是"随手占位"：去掉它则 19 × 4 = 76 B，76 % 16 = 12 → std140 下
    //    结构体大小被向上撑到 80 B，而 MSL 紧凑布局仍是 76 B → **两端偏移
    //    开始错位**。这类错位在无 GPU 的本机**两端都能编译通过**，只在运行期
    //    表现为字段读串（本项目历史上最贵的一类坑）。故保留位是**必需的**。
    int32_t  reserved0;
};

// ---------------------------------------------------------------------------
// 算法声明（**单一事实来源**；声明即注册）
// ---------------------------------------------------------------------------
//
// 命名规范：`recon_<模块>_<动作>`。Metal 函数名与 Vulkan 入口函数名必须与此
// **逐字相同**（MT-3）。闸门 `tools/shader_parity.py` 从本段提取期望集合。
//
// 顺序即 docs/05 §5.9.2 的「命令缓冲区 CB-1 .. CB-6」顺序，便于人工对照。

// CB-1 播种：稀疏点投影 + 中值初始化 + 空洞填补 + 初始代价。
//      docs/05 §5.3.3。被 dispatch 一次。
RECON_GPU_ALGO(recon_mvs_seed);

// CB-2 主循环：PatchMatch 传播 + 随机搜索 + 代价评估。
//      docs/05 §5.3.4 `mvs_pm_*`。红黑交替 → 每次迭代 dispatch 两次。
RECON_GPU_ALGO(recon_mvs_patchmatch);

// CB-3 亚像素细化：抛物线拟合 + 连续细化。
//      docs/05 §5.5 `mvs_subpixel_*`。被 dispatch 一次。
RECON_GPU_ALGO(recon_mvs_subpixel);

// CB-4 单视图一致性：依赖**全部源视图**的深度图，做左右一致性校验。
//      docs/05 §5.6。被 dispatch 一次。也是显存常驻量的主要来源
//      （docs/05 §5.9.3「CB-4 必须等待所有源视图的深度图完成」）。
RECON_GPU_ALGO(recon_mvs_consistency);

// CB-5 体素融合：点云散射到体素（atomicAdd 散射）。
//      docs/05 §5.7。全部 tile 完成后 dispatch 一次。
RECON_GPU_ALGO(recon_mvs_fuse_voxel);

// CB-6 融合归约 + 一致性检查：产出 D_r / C_r / 支撑视图数。
//      docs/05 §5.10 输出契约。被 dispatch 一次。
RECON_GPU_ALGO(recon_mvs_reduce);

// TSDF 体素中心式融合（docs/04 §4.6.2）。
//      每线程一个体素；S/W/C 更新无需原子，仅块分配需要。
RECON_GPU_ALGO(recon_tsdf_fuse);

// 采集阶段：YUV 4:2:0 双平面 → BGRA8（docs/11 §3.1.3 / §3.4 / IOS-AR-5）。
//      **不是 MVS/TSDF 管线的一部分**，而是它的**前置条件**：核心层的 RGB
//      入口契约为 BGRA8（docs/00 §7.2），而两端的原生采集格式都是 YUV420，
//      故没有这一步就一帧 RGB 也进不去（docs/14 §5.1.3 缺口 J）。
//      被 dispatch 一次/帧。两端都必须有 —— 见上方参数块段落。
RECON_GPU_ALGO(recon_yuv_to_bgra);

// ---------------------------------------------------------------------------
// 未实现缺口登记（**跨端一致性的第二类单一事实来源**）
// ---------------------------------------------------------------------------
//
// ── 为什么需要它 ──────────────────────────────────────────────────────────
// 本项目反复声明「两端算法必须逐条对齐」。但**比算法对齐更隐蔽的一类漂移**
// 是「实现进度漂移」：某一端悄悄把某个缺口补上了、另一端没有，于是两端
// 「同名同参」却**语义不同**。这比"函数名不一致"更危险 ——
// 函数名不一致立刻会被 P2/P3 判据抓住；进度不一致则**一切静态检查都通过**，
// 只有真机跑出不同结果时才会暴露。
//
// 因此这里把「缺口」也提升为机器可解析的登记项：`RECON_GPU_GAP(模块, 编号)`。
// 闸门 `tools/shader_parity.py` 由此得到**期望的缺口集合**，再要求
// Metal 侧（`shaders/metal/`）与 Vulkan 侧（`shaders/vulkan/`）的注释里
// **各自**出现 `SOURCE-TODO-<模块>-<编号>` 至少一次。
//
// 任一端补齐缺口时，必须：① 删掉该端的标注；② 删掉本文件的登记 ——
// 而步骤②会让另一端的标注立即变成"多余"，闸门报错 → **强制两端同步**。
// 这就是把「不许单侧悄悄完成」从口号变成 CI 判据的办法。
//
// ── 与 MT-4 / CT-9 的关系 ─────────────────────────────────────────────────
// 缺口不是"静默降级"：降级是**运行期行为**（改变参数），缺口是**编译期事实**
// （代码没写）。docs/14 CT-9 管的是前者（不得改黑名单参数），本段管的是后者
// （不得装作写完）。两者都不允许"表面满足"。
//
// ── 使用规范 ──────────────────────────────────────────────────────────────
// ① 标识符拼接：`SOURCE-TODO-MVS-1` —— 模块名大写、编号十进制、连字符分隔。
// ② 每个缺口在本文件的注释里写清「缺什么 + 为什么先不做 + 何时能补」。
// ③ 缺口的**完整语义**在每个相关着色器文件头部展开说明；本段只做登记。
#define RECON_GPU_GAP(module, index) \
    /* 缺口：module / index —— 见下方对应段落 */ \
    struct recon_gpu_gap_##module##_##index##_tag {}

#define RECON_GPU_DIFF(index) \
    /* 平台差异：index —— 见下方对应段落 */ \
    struct recon_gpu_diff_##index##_tag {}

// ── MVS 模块缺口（docs/05 §5.9 GPU 实现）─────────────────────────────────

// MVS-1  NCC 代价聚合未接入。
//        影响：`photo_threshold` / `patch_size` / `restart_ratio` 尚未真正参与
//        决策；`recon_mvs_patchmatch` 只做初值传播与随机搜索。
//        为何先不做：NCC 需要正确的视图投影链，而视图投影链依赖 MVS-3 的
//        精确往返变换；先补 NCC 会让误差来源重叠而难以归因。
//        何时能补：与 CPU 版 `core/src/mvs/patchmatch_stereo.cpp` 完成数值
//        对齐之后（该对齐尚未开始）。
RECON_GPU_GAP(MVS, 1);

// MVS-2  共享内存（`threadgroup` / `shared`）halo 缓存未启用。
//        影响：复用率未提升（docs/13 §3.2.1 的 halo 收益未兑现）。
//        为何先不做：`shared` 数组长度必须是编译期常量，而 halo 宽度是运行期
//        参数（MT-4 要求经 buffer 传）；在无真机可测的前提下启用会踩
//        `maxTotalThreadgroupMemory`（Metal）/ `maxComputeSharedMemorySize`
//        （Vulkan，移动端常见 32 KB）的坑，且无法验证。
//        何时能补：真机阶段先查询设备上限，再以编译期上界 + 运行期断言启用。
RECON_GPU_GAP(MVS, 2);

// MVS-3  左右一致性的往返变换用**参考帧位姿近似**。
//        影响：严格式 `pc_src = R_src·(R_refᵀ·(pc_ref − t_ref)) + t_src` 未被
//        使用，本实现只对反投影点再做一次 `world_to_cam(views[ref])`。
//        为何先不做：需与 CPU 版做数值对齐后才能确定「误差是算法固有的，
//        不是实现写错的」—— 否则无法判断该修实现还是修阈值。
//        何时能补：同上，与 `patchmatch_stereo.cpp` 对齐之后。
RECON_GPU_GAP(MVS, 3);

// MVS-4  `recon_mvs_fuse_voxel` 的体素线性索引用占位维度
//        （`ref_width × ref_height`），且**无上界检查**。
//        影响：真实实现须用体素网格维度，否则索引与 C++ 侧的体积扁平化
//        约定不一致（越界写风险）。
//        为何先不做：params 结构体里目前**没有**体素网格维度字段；
//        加字段会同时改变 std140 布局与两端声明，须与 C++ 侧一次改齐。
//        何时能补：接入体素网格分配器时（与 TSDF-1 的块分配同批）。
//        ⚠️ 与 `recon_tsdf_fuse` / `recon_mvs_reduce` 的索引缺口**同源**，
//        必须同时修好（半边保护等于没有保护）。
RECON_GPU_GAP(MVS, 4);

// MVS-5  源视图数组上界无运行期断言。
//        Metal 侧 `array<texture2d<...>, 8>`、Vulkan 侧 `RECON_VK_MAX_VIEWS = 8`
//        与 `mvs_num_views` 高档值 8 一致，但**没有**运行期检查
//        `num_views <= 8`。若参数体系放宽到 > 8 视图，两端都会静默读到
//        越界描述符（Metal 报错、Vulkan 未定义行为）。
//        为何先不做：`mvs_num_views` 当前值域上界就是 8（docs/08 参数体系），
//        故当前不可达；加断言需要 C++ 侧在 dispatch 前校验并走错误码路径。
//        何时能补：参数体系放宽时（或真机阶段作为防御性检查一并加）。
RECON_GPU_GAP(MVS, 5);

// ── TSDF 模块缺口（docs/04 §4.6 GPU 实现）───────────────────────────────

// TSDF-1  块分配原子（docs/04 §4.6.4）未接入：当前假设块已由 C++ 侧预分配。
//         影响：动态块生长不可用 → 只能在预分配网格内融合。
//         为何先不做：块分配是**唯一的原子需求**，接入后 `recon_tsdf_fuse`
//         的性质会从"无原子"变为"仅块分配处有原子"，这个改变需要在真机上
//         验证原子竞争的实际开销。
//         何时能补：接入体素网格分配器时（与 MVS-4 同批）。
RECON_GPU_GAP(TSDF, 1);

// TSDF-2  颜色为**单通道占位**（只累加 `c.r`）。
//         影响：TSDF 颜色不完整；当前 TSDF 只服务几何，颜色由 MVS 点云直通
//         泊松（docs/05 §5.10），故**不影响当前管线输出**。
//         为何先不做：三通道需要 `tsdf_c` 长度 ×3 或 `float3*`，属布局变更，
//         须与 C++ 侧缓冲分配一起改。
//         何时能补：RGBD 分支正式启用颜色融合时。
RECON_GPU_GAP(TSDF, 2);

// TSDF-3  体素法向未计算，视角权重用「视线与光轴夹角余弦」近似。
//         影响：掠射角处的权重不准确（真实做法用体素法向与视线夹角）。
//         为何先不做：法向需从 S 场梯度求（`SurfaceGradient`），而梯度要读
//         邻域体素 → 依赖 TSDF-4 的跨块查询。
//         何时能补：TSDF-4 完成后。
RECON_GPU_GAP(TSDF, 3);

// TSDF-4  halo 跨块邻域查询未做。
//         影响：`SurfaceGradient` 无法计算（跨块边界的梯度需要邻居块数据）。
//         为何先不做：跨块查询需要块索引表与邻块查找，属 TSDF-1 的一部分。
//         何时能补：TSDF-1 完成后。
RECON_GPU_GAP(TSDF, 4);

// ── YUV 模块缺口（docs/11 §3.1.3 / §3.4）──────────────────────────────────

// YUV-1  `recon_yuv_to_bgra` **只支持交错色度平面**（`chroma_pixel_stride == 2`，
//        即 NV12/NV21 与 iOS 的 `420YpCbCr8BiPlanarFullRange`）；
//        `chroma_pixel_stride == 1`（真平面 I420：Cb、Cr 各自独立成平面）
//        时**不产出结果**（写全 0 并置 alpha=0），由调用方走 CPU 回退。
//        影响：少数 Android 机型的 `YUV_420_888` 底层是 I420 而非 NV12/NV21
//        → 这些机器上 RGB 走 CPU 路径（`platform/common/src/yuv_convert.cpp`），
//        吞吐降低但**结果正确**（不静默降级：调用方必须显式选择并记录）。
//        为何先不做：GPU 侧一条 kernel 读两路纹理（Cb/Cr 分平面）需要额外
//        的 descriptor 与分支，而本机**无法编译/运行任何一端着色器**
//        （无 Metal/glslang）→ 加了也无法验证，只会扩大未验证面。
//        何时能补：真机阶段确认各机型实际平面布局后，若 I420 占比可观则补。
RECON_GPU_GAP(YUV, 1);

// ---------------------------------------------------------------------------
// 平台差异登记（**有意的不对称**，第三类单一事实来源）
// ---------------------------------------------------------------------------
//
// ── 为什么"不对称"也要登记 ────────────────────────────────────────────────
// 上一段（缺口）解决「两端必须一致地未完成」；本段解决**相反方向**的问题：
// 有些差异是**平台能力使然、无法消除**的（如浮点原子在 Vulkan 非核心特性）。
// 这类差异若只在某一端的注释里写，读者无法区分它究竟是
//   「有意的平台差异」 还是 「另一端漏写了」。
//
// 处置：用 `RECON_GPU_DIFF(编号)` 在**单一来源**登记，并要求**两端**的公共头
// 各引用一次该编号（`VK-DIFF-n` / `MSL-DIFF-n` 形式不限，闸门按编号匹配）。
// 这样任一端删掉说明 → 闸门报错，差异不会被静默遗忘。
//
// ── 与缺口（GAP）的区分 ───────────────────────────────────────────────────
//   GAP  ：**本该两端一致，但都还没做** → 最终会消失（补齐后删除登记）。
//   DIFF ：**平台使然，长期存在**       → 不会消失，须长期可审计。
// 两者都要求「两端同步可见」，但生命周期相反。混为一谈会导致
// 「把平台差异当欠债去还」（做不到）或「把欠债当差异留着」（不该留）。

// VK-DIFF-1  **浮点原子在两端都不可依赖 → 统一走定点整数原子**。
//
//            早先登记写的「Metal 侧 `atomic_float` 是核心特性，故本端无分支」
//            **是错的**，2026-11-01 由 CI 首次真编译证伪：`atomic_float` 是
//            **Metal 3.0 / iOS 16** 起才有的别名（MSL 规范 §2.6），而本项目
//            部署下限是 **iOS 15.0**（docs/12 §4.6）→ 在契约允许的最低平台上
//            该类型**根本不存在**。旧写法在设备 SDK 下能编过，只是因为编译
//            命令没传 `-target`、编译器取了 SDK 默认的高语言版本 —— 那是
//            「假通过」：产物语言版本高于部署目标，真机会抛
//            `Function is using language version 3.0 which is incompatible
//             with this OS`。
//
//            现状（两端**同构**）：`voxel_sum` / `voxel_w` 一律声明为
//            **32 位有符号整数**累加器，量化尺度与饱和阈值见上方
//            `RECON_GPU_FIXED_*` 段（common 登记 → 两端各自定义同名同值）。
//              · Metal ：`device atomic_int*` + `atomic_fetch_add_explicit`
//                         （`atomic_int` = Metal 1.0+，覆盖 iOS 15）。
//              · Vulkan：`atomicAdd(int)`（核心特性，无需扩展）。
//            量化差异因此**不再存在**于两端之间；它只存在于「定点累加」与
//            「理想浮点累加」之间（相对误差 ≤ 1/4096），且**两端完全相同**。
//
//            ⚠️ 该量化差异仍是真实的：定点累加的结果与"无限精度浮点累加"
//            不逐位相同，故跨端对拍与 CPU 版对拍都须以"可接受误差"而非
//            "逐位相等"为准（docs/13 的项目口径：统计等价须登记）。
RECON_GPU_DIFF(1);

// VK-DIFF-4  深度图采样：docs/05 §5.9.1 表格写 Vulkan 用
//            「`texture2D` + `sampler` + `imageSample`（线性过滤）」，
//            但 docs/04 §4.6.1 硬要求「深度图必须用**最近邻**采样」。
//            处置：**两端都不使用 sampler/过滤**，一律最近邻
//            （Metal：`access::read` + `read(uint2)`；
//             Vulkan：`texelFetch`）。以 docs/04 §4.6.1 的硬要求为准，
//            docs/05 §5.9.1 该行**需修订**（已登记为文档缺口）。
RECON_GPU_DIFF(4);

// 注：VK-DIFF-2（共享内存未启用）与 VK-DIFF-3（fp16/fp32 文档矛盾）
//     已在 MVS-2 与「精度约定」段覆盖，不重复登记为 DIFF，避免同一事实
//     有两处登记（那正是本文件反复要消灭的模式）。

#endif  // RECON_SHADERS_GPU_ALGO_H_
