// SPDX-License-Identifier: BSD-3-Clause
//
// 网格数据结构与修复接口。契约：docs/06_网格化与后处理.md §2。

#ifndef RECON_CORE_MESH_REPAIR_H_
#define RECON_CORE_MESH_REPAIR_H_

#include <cstdint>
#include <vector>

#include "recon/linalg.h"

namespace recon {
extern int g_dbg_line;

// 三角形网格。索引为 uint32，必要时由导出器降级为 uint16。
struct TriangleMesh {
  std::vector<Vec3d> positions;
  std::vector<Vec3d> normals;
  std::vector<Vec2d> uvs;
  std::vector<uint32_t> indices;

  // 逐顶点置信度。用于标注推断区域（docs/00 §4.5.4 G3 方案）。
  std::vector<float> confidences;

  void Clear() {
    positions.clear();
    normals.clear();
    uvs.clear();
    indices.clear();
    confidences.clear();
  }

  size_t triangle_count() const { return indices.size() / 3; }
};

// 网格统计。用于验收（docs/00 §4.6）。
struct MeshStats {
  uint64_t vertex_count = 0;
  uint64_t triangle_count = 0;
  uint32_t non_manifold_edges = 0;
  uint32_t boundary_loops = 0;
  // 绕序不一致的内部边数：两个相邻面以**相同方向**共用该边。
  // 一致定向的网格必须处处为 0（MC 歧义 case 会破坏它，
  // 由 OrientFacesConsistently 修复）。非 0 会连带破坏边界环的侧别判据
  // 与逐顶点法向重算。
  uint32_t inconsistent_winding_edges = 0;
  uint32_t hole_count = 0;
  uint32_t sliver_triangles = 0;
  uint32_t degenerate_triangles = 0;
  double total_area = 0.0;
  double mean_edge_length = 0.0;
  double hole_area_ratio = 0.0;
};

// 网格分析：拓扑与几何统计。
MeshStats AnalyzeMesh(const TriangleMesh& mesh);

// 移除退化三角形（重复索引 / 零面积）。返回移除数量。
uint32_t RemoveDegenerateTriangles(TriangleMesh* mesh);

// 移除孤立顶点（未被任何三角形引用）。返回移除数量。
uint32_t RemoveIsolatedVertices(TriangleMesh* mesh);

// 移除孤立面片：连通分量中只含单个三角形的面。
//
// 泊松 MC 在外扩区会产出孤立的三角形碎片 —— 它的 3 条边都只有它自己
// 一个面使用，因此看起来像一个「三角洞」。但它不是洞，而是噪声碎片。
//
// **为什么必须删而不是补**：给碎片补一个面会造出与它**完全重合**的面
// （两个面在共享边上同向 = 在边的同一侧）→ 绕序不一致 + 穿模几何。
// 实测（球面 depth=9）：不删碎片时补洞后绕序不一致边 5541 条
// （≈ 填充面总数），且补出的盖子全部穿进网格内部。
//
// 删掉碎片后它的边界环自然消失，无需再补。
//
// **不动几何**：只删三角形，不改任何顶点位置。
//
// @return 被删除的三角形数
uint32_t RemoveIsolatedFacePatches(TriangleMesh* mesh);

// 统一面绕序：保证相邻面以**相反方向**共用共享边。
//
// Bourke 标准 MC 表在 15 个歧义 case 上只固定了一种连接方式，相邻体素之间
// 可能出现绕序不一致（实测球面 depth=9：内部边 684 条中 49 条不一致）。
// 绕序不一致会破坏下游的一切局部一致性判据（边界环侧别判据、
// 逐顶点法向重算等）。
//
// 算法：按共享边做 BFS 传播，若两个面在同一条边上方向相同则翻转邻居。
// 被 ≥ 3 个面共用的非流形边不建立约束。
//
// **不动几何**：只交换索引顺序，顶点位置一个不改。
//
// @return 被翻转的三角形数
uint32_t OrientFacesConsistently(TriangleMesh* mesh);

// 移除重复三角形（同一组三个顶点被写入多次，忽略绕序）。
//
// 泊松 MC 提取在歧义 case 下会产出这类重复面。它们在边表上「看起来正常」
// （那条边仍是 2 个面），但补洞往同一条边加面时会把它变成 3~4 面共享
// → 非流形边 → 兜底 S4 拆点 → 刚闭合的洞被重新撕开。
// 必须在补洞之前清除。
uint32_t RemoveDuplicateTriangles(TriangleMesh* mesh);

/**
 * 法线统一矫正。
 *
 * 步骤 1：整体定向。比较面法向与「面心 − 质心」的点积，多数内向则整体翻转。
 * 步骤 2：局部翻转传播。BFS 沿邻接关系修正个别反向面。
 *
 * 返回翻转的三角形数。
 *
 * **注意**：步骤 1 的质心判据假设物体关于质心星形（凸），对凹形 / 空心 /
 * 多连通物体可能给出错误结论。新代码请优先用 `OrientMeshOutward`
 * （基于有符号体积，对闭合流形精确）。本函数保留为向后兼容。
 */
uint32_t OrientNormalsOutward(TriangleMesh* mesh, const Vec3d& centroid);

/**
 * 整体朝外定向（**逐连通分量**，基于「面积加权径向符号」）。
 *
 * `OrientFacesConsistently` 只保证相邻面绕序**局部一致**，不保证整体朝外。
 * 本函数在**每个 2 面共享边连通分量**内，累加面积加权的
 * `S = Σ area(t)·sign(n(t)·(center(t) − G))`（`G` = 顶点平均质心），
 * `S < 0` 的整分量翻转。
 *
 * **刻意不做 BFS 传播**（关键经验）：把「绕序传播」与「整体定向」合在一起
 * 会显著变差 —— 2 面边图会把物理上无关的面片经「恰好共用一条边」的假邻接
 * 边串成一块，沿其传播会把朝向打散。实测球面：传播 + 定向 → 朝外 67.6%；
 * **不传播** + 定向 → 87.8%。因此本函数只做逐分量翻转。
 *
 * **这一步骤是必需的**：缺失它时，整体一致朝内的网格会让 S9 纹理烘焙的
 * 入射角权重 `cos^α` 塌到 0（整片纹理变黑），也会让渲染的背面剔除把整个
 * 物体剔掉。实测泊松重建的标准球输出整体朝内（主分量 n̂·ĉ ≈ −0.86）。
 *
 * **不动几何**：只交换索引顺序。返回翻转的三角形数。
 */
uint32_t OrientMeshOutward(TriangleMesh* mesh);

/**
 * 局部绕序修复（**非传播**）：按「邻面多数表决」翻转个别绕序错误的面。
 *
 * 与 `OrientFacesConsistently` 的区别：后者用 BFS 从种子面**传播**，会把
 * 种子朝向强加给整个连通块 —— 在泊松碎片化输出上会沿假邻接边把整块打散，
 * 且**不可逆**。本函数只统计每个面的邻面中「绕序冲突」的条数，冲突占多数
 * 才翻转该面：内部一致的分量零损失，孤立翻错的面被修正。迭代至收敛。
 *
 * @param max_rounds 最大迭代轮数
 * @return 被翻转的三角形数
 */
uint32_t FixLocalWindingErrors(TriangleMesh* mesh, int max_rounds);

/**
 * 有符号体积（散度定理）：`V = (1/6) Σ (a · (b × c))`。
 *
 * 对闭合且定向一致的网格，正号 = 外法向朝外。用于验证整体朝向。
 */
double SignedVolume(const TriangleMesh& mesh);

// 重新计算顶点法向（面积加权）。
void RecomputeVertexNormals(TriangleMesh* mesh);

// ---------------------------------------------------------------------------
// S4 非流形边 / 顶点修复
// ---------------------------------------------------------------------------

// 非流形顶点分裂：把「边界度 ≥ 4」的顶点按**扇区**拆成多个顶点。
//
// 顶点处的关联面按共享边可分成若干连续扇区。合法流形曲面上每个扇区
// 贡献 0 或 2 条边界边，因此**边界度 = 2 × 扇区数**。边界度 ≥ 4 意味着
// 该顶点处曲面本身非流形（两个曲面片在该点交汇）。
//
// **必须修的原因**：两个扇区各带一条边界环，两条环在该顶点相交。
// 走环时无论用几何直行、转向量符号还是侧别判据，都无法把两条环分开
// → 环内出现重复顶点 → 非简单环 → 无法补洞。
//
// 实测（球面 depth=9）：边界度分布 度2=6152、度4=297、度6=7；
// 走环判据改进后仍残留 54 个非简单环。
//
// **不动几何**：新顶点位置 = 原顶点位置。
//
// @return 被分裂的顶点数
uint32_t SplitNonManifoldVertices(TriangleMesh* mesh);

// **顶点分裂**式修复：对非流形边关联的顶点，按「关联面的连通分支」
// 拆成多个独立顶点（位置不变）。
//
// 为什么用顶点分裂而不是规格 §2.4 优先级表里的「坍缩边」：
//   1. 坍缩边会把两个顶点合并到中点，**移动几何**。
//      对 0.1 mm 级精度的产品，移动 0.05·r（≈0.011 mm）也是真实误差。
//   2. 顶点分裂**不移动任何几何**，几何精度零损失。
//   3. 拓扑上，非流形边的成因是「多片面片在此汇合」，
//      按连通分支拆开恰好还原应有拓扑。
//
// @return 被拆分的顶点数
uint32_t RepairNonManifoldEdges(TriangleMesh* mesh);

// ---------------------------------------------------------------------------
// S5 孔洞缝合
// ---------------------------------------------------------------------------

// 孔洞级别（docs/06 §2.5）。
enum class HoleLevel : uint8_t {
  kMicro = 0,    // H0：微孔，直接扇形填充
  kSmall = 1,    // H1：小孔，扇形填充 + 边界点投影到拟合平面
  kMedium = 2,   // H2：中孔，扇形填充 + 平面投影
  kLarge = 3,    // H3：大孔，不自动填充（需局部二次泊松）
  kThrough = 4,  // H4：贯穿孔，视为真实结构，不填充
  kNonSimple = 5 // 不可填：环含重复顶点（边界度 ≥ 3 的 T 型接点），
                  // 多边形三角化没有意义，强行填充会漏边反而多出边界环
};

// 单个孔洞的信息。
struct HoleInfo {
  double area = 0.0;        // 绝对面积（m²）
  double area_ratio = 0.0;  // 面积 / 网格总面积（无量纲比值口径）
  uint32_t loop_length = 0; // 环的边数
  double plane_rms = 0.0;   // 边界点拟合平面 RMS
  Vec3d centroid = Vec3d::Zero();
  HoleLevel level = HoleLevel::kMicro;
  bool filled = false;
};

// 孔洞缝合报告。
struct HoleFillReport {
  uint32_t loop_count = 0;
  uint32_t filled_count = 0;
  uint32_t unfilled_count = 0;
  // 补洞后再次修复非流形连接所拆分的顶点数。
  // 新版填充算法复用原边界顶点，理论上不再产生 T 型接点；
  // 该字段保留为兜底计数（非 0 说明填充引入了新的非流形连接）。
  uint32_t post_fill_split = 0;
  // 填充**失败**（退化 / 自交 / 耳切失败 / 环非简单）而未能闭合的孔洞数。
  // 这些孔洞计入 unfilled_count，同时按原因分类列出以便定位。
  uint32_t fill_failed_count = 0;
  uint32_t fill_failed_degenerate = 0;  // 三角形面积过小（退化）
  uint32_t fill_failed_earclip = 0;     // 耳切失败 / 多边形自交
  uint32_t fill_failed_non_simple = 0;  // 环含重复顶点（非简单环）
  uint32_t fill_failed_diagonal = 0;    // 新增对角线与已有内部边重合
  uint32_t fill_failed_winding = 0;     // 补片与原面同向（会穿模，故不填）
  // 补洞**之后**网格上剩余的边界环数（与 AnalyzeMesh 同口径）。
  // 验收判据：remaining_loops 应等于 unfilled_count —— 相等即说明
  // 「填了的洞真的闭合了，没填的洞原样保留」。
  uint32_t remaining_loops = 0;
  // 分级统计（docs/06 §2.5 的 H0~H4）。
  uint32_t level_micro = 0;    // H0 微孔
  uint32_t level_small = 0;    // H1 小孔
  uint32_t level_medium = 0;   // H2 中孔
  uint32_t level_large = 0;    // H3 大孔（不自动填）
  uint32_t level_through = 0;  // H4 贯穿孔（不填）
  uint32_t level_unfillable = 0;  // 不可填（开放链 / 非简单环）
  double total_area = 0.0;
  std::vector<HoleInfo> holes;
};

// 孔洞缝合配置。阈值全部为**相对面积比**（尺度无关，docs/06 §2.5）。
struct HoleFillConfig {
  double resolution = 0.001;      // 网格特征边长（米），用于平面 RMS 判据
  double micro_area_ratio = 0.01; // H0 上限
  double small_area_ratio = 0.10; // H1 上限
  double large_area_ratio = 1.50; // H2 上限（超过为 H3，不填）
  double small_plane_rms_factor = 0.15;  // H1 的平面 RMS 上限（× r）
  // 补洞后是否自动再跑一次 S4（非流形边修复）。
  //
  // 单独调用 FillHoles 时保持 true（安全网）。
  // 由 RunMeshPipeline 驱动时置 false —— 流水线需要把「S4 → S5」作为
  // 整体迭代：S4 的顶点分裂会撕开已闭合的洞，必须由随后的 S5 补回，
  // 否则边界环数会比未填孔洞数多。
  bool post_repair_enabled = true;
  double max_plane_rms_factor = 0.50;   // H4 判据（× r）
};

// 孔洞缝合。
//
// **核心约定（与旧版的关键差异）**：填充面必须**复用原边界顶点**。
// 旧实现把边界顶点「投影到拟合平面」时创建了**新顶点**并用新顶点组环，
// 于是原边界边 (v_i, v_{i+1}) 依然只被 1 个面使用 →
//   ① 孔洞根本没被闭合（拓扑缺陷照旧）；
//   ② 补片自身又带一圈边界边，等于凭空多出一个边界环。
// 实测（球面，泊松 + 本流程）：每个投影洞净增 2 个边界环，
// 19 个投影洞 → 边界环比未填孔洞数多 38，与实测差值完全吻合。
//
// 现版算法：
//   n = 3  → 1 个三角形，不新增任何顶点；
//   n = 4  → 沿较短对角线剖分为 2 个三角形，不新增顶点；
//   n ≥ 5  → 平面内三角化：
//              多边形关于其重心「星形可见」 → 重心扇形（新增 1 个内部顶点，
//              内部顶点落在拟合平面上，满足 H1「消除台阶」的意图）；
//              否则 → 耳切三角化（只用原边界顶点，不新增顶点）。
//
// 分级（docs/06 §2.5）：
//   H0（微孔）：直接填充，**不移动任何已有顶点**（几何零损失）。
//   H1/H2：填充 + 内部顶点落在拟合平面（**边界顶点保持原位**，几何零损失）。
//   H3（大孔）：不自动填充，需局部二次泊松（未实现），记录待处理。
//   H4（贯穿孔）：视为真实结构，不填充，记录待用户决定。
HoleFillReport FillHoles(TriangleMesh* mesh, const HoleFillConfig& config);

// ---------------------------------------------------------------------------
// S7 三角面规整
// ---------------------------------------------------------------------------

// 三角形的最小内角（度）。退化三角形返回 0。
double MinAngleOfTriangle(const Vec3d& a, const Vec3d& b, const Vec3d& c);

// S7 子步骤：**坍缩极短边**。
//
// **为什么需要**：MC 提取出的狭长面，成因**不是**「三顶点几乎共线」，而是
// **极短边**。实测（球面 depth=7）：狭长面的最短边中位 **0.0537 mm =
// 0.062 × r**，而最长边中位 1.07 × r。这种面**边翻转救不了** ——
// 四边形同样含那条短边，翻转后依旧狭长。
//
// **精度代价（可忽略）**：坍缩把两端点合并到**中点**，最大位移 =
// 边长 / 2 ≤ 0.125 × r（默认阈值 0.25·r）。泊松的形状误差量级是 0.5 × r，
// 因此这是「用 1/4 的形状误差换掉一整类退化面」。
//
// **拓扑安全**：内部边 (a,b) 的**公共邻居数必须恰为 2**（link condition），
// 否则坍缩会让某条边被 3+ 个面共享 → 非流形边。
//
// @param max_edge_length 允许坍缩的最大边长（m），建议 0.25 × r
// @param max_rounds      最多迭代轮数
// @return 被坍缩的边数
uint32_t CollapseShortEdges(TriangleMesh* mesh, double max_edge_length,
                            int max_rounds);

// S7 子步骤：**边翻转**改善最小内角。
//
// **不移动任何顶点**，只改变顶点的连接关系 → 几何精度**零损失**；
// 只翻转恰有 2 个面的内部边 → 不改变边界环。
// 在「几何精度 > 拓扑完整性 > 纹理质量」的铁律下，这是唯一可以无条件做的
// 网格质量改善手段（切向平滑会移动顶点、边坍缩会改变形状，都需要原曲面参考）。
//
// 判据：内部边 (a,b) 的两个对面为 c、d，翻转为 (c,d) 后要求
//   ① 新边 (c,d) 尚不存在（否则造出重复面 / 非流形边）
//   ② 两个新三角形面积 > 0
//   ③ `min(新内角) > min(旧内角) + eps`（单调改善 → 不震荡，可迭代到收敛）
//
// @param min_angle_deg 目标最小内角（低于此值才尝试翻转），标准档 9°
// @param max_rounds    最多迭代轮数
// @return 被翻转的边数
uint32_t FlipEdgesForQuality(TriangleMesh* mesh, double min_angle_deg,
                              int max_rounds);

// ---------------------------------------------------------------------------
// 统一后处理流水线（docs/06 §2.0 的阶段顺序）
// ---------------------------------------------------------------------------

// 流水线配置。
struct MeshPipelineConfig {
  // 网格特征边长（米），供孔洞分级的平面 RMS 判据使用。
  double resolution = 0.001;
  // 孔洞缝合配置（其 resolution 字段会由本结构体的 resolution 覆盖）。
  HoleFillConfig hole;
  // 是否在流水线末尾重算逐顶点法向（S6 第 4 步）。
  bool recompute_normals = true;
  // 是否在流水线末尾做**整体朝外定向**（docs/06 §2.6 第 1~3 步）。
  //
  // 位置与理由：规范把 S6 法向矫正排在流水线**末端** —— 此时 S4 已把
  // 非流形边拆开、S5 已补洞、S7 已规整，网格最接近流形，逐分量的
  // 定向判据最可靠。放在 S4/S5 **之前**（泊松原始输出仍高度碎片化时）
  // 效果很差：实测泊松输出 133k 三角形里有 7381 个连通分量，
  // 提前定向后仍有一半以上的面朝内。
  //
  // 关闭仅用于诊断（对比定向前后的网格状态）。
  bool orient_outward = true;
  // S7 边翻转的目标最小内角（度）。docs/06 §2.7：快 12 / 标 9 / 高 6。
  // 设为 0 可关闭 S7。
  double min_angle_deg = 9.0;
  // S7 边翻转的最大迭代轮数。
  int s7_max_rounds = 3;
  // S7 短边坍缩的阈值（× resolution）。0 表示关闭。
  // 实测狭长面的最短边中位是 0.062×r，故 0.25 足够覆盖；
  // 再大就会开始吃掉正常边。
  double collapse_edge_factor = 0.25;
  // S7 短边坍缩的最大迭代轮数。
  int s7_collapse_rounds = 3;
};

// 流水线报告。
struct MeshPipelineReport {
  uint32_t degenerate_removed = 0;      // 净化阶段剔除的退化面数
  uint32_t duplicate_removed = 0;       // 净化阶段剔除的重复面数
  uint32_t isolated_patch_removed = 0;   // 净化阶段删除的孤立面片数
  uint32_t face_flip_fixed = 0;          // 局部绕序修复阶段翻转的三角形数
  // 整体朝外定向（`OrientMeshOutward`）翻转的三角形数。
  // 位于**流水线末端**（S6 之前）—— 早期曾在 S4/S5 之前接入，实测被
  // S4/S5/S7 的拓扑改动反复扰动，故改为末端唯一一次。
  uint32_t outward_flip_fixed = 0;
  // 保留字段（历史）：早期「两处定向」方案的末段计数。现在末端定向计入
  // `outward_flip_fixed`，此字段恒为 0，仅为兼容旧调用方保留。
  uint32_t outward_flip_final = 0;
  uint32_t vertex_split = 0;             // 非流形顶点分裂数
  uint32_t edge_collapses = 0;           // S7 短边坍缩数
  uint32_t edge_flips = 0;               // S7 边翻转数（零精度损失）
  uint32_t loops_before_s7 = 0;          // S7 前的边界环数
  uint32_t loops_after_s7 = 0;           // S7 后的边界环数
  uint32_t winding_before_s7 = 0;        // S7 前的绕序不一致边数
  uint32_t winding_after_s7 = 0;         // S7 后的绕序不一致边数
  uint32_t isolated_removed = 0;        // 净化阶段剔除的孤立顶点数
  uint32_t non_manifold_split = 0;      // S4 拆分的顶点数
  // S4 之后仍残留的非流形边数（非 0 说明 S4 未收敛 → 兜底 S4 会在补洞后
  // 再次拆点，把刚闭合的洞重新撕开）。
  uint32_t non_manifold_after_s4 = 0;
  HoleFillReport holes;                  // S5 孔洞缝合报告
  uint32_t post_fill_round = 0;         // 兜底「S4 → S5」迭代轮数
  uint32_t post_fill_refilled = 0;      // 兜底轮补上的洞数（S4 撕开后补回）
  uint32_t post_fill_unfilled = 0;      // 兜底轮仍无法闭合的洞数
  uint32_t post_degenerate_removed = 0; // 补洞后兜底剔除的退化面数
  uint32_t post_isolated_removed = 0;   // 补洞后兜底剔除的孤立顶点数
  MeshStats stats;                       // 流水线末端统计
};

// 完整后处理流水线。
//
// 顺序（docs/06 §2.0，**不可交换**）：
//   1. 净化：剔除退化面 + 孤立顶点
//   2. S4：非流形边修复（顶点分裂，不移动几何）
//   3. S5：孔洞缝合
//   4. 兜底净化：补洞若产生退化面 / 孤立顶点则清除
//   5. S6：逐顶点法向重算
//   6. 统计
//
// **为什么净化必须在 S5 之前**：退化面（零面积）被剔除时会撕开新的边界，
// 若这些边界在补洞之后才产生，就再也没人填它们 —— 实测球面用例中
// 1223 个退化面会造成上百个「补完又裂开」的洞。
MeshPipelineReport RunMeshPipeline(TriangleMesh* mesh,
                                    const MeshPipelineConfig& config);

}  // namespace recon

#endif  // RECON_CORE_MESH_REPAIR_H_
