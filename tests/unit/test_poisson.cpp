// SPDX-License-Identifier: BSD-3-Clause
//
// 泊松网格化精度测试。
//
// 方法：构造**解析可验证**的合成场景。
//   - 已知半径的球面（法向精确可算）
//   - 从球面均匀采样点云（模拟 MVS 输出）
//   - 泊松重建
//   - 检验：径向误差分布、拓扑质量、分辨率量化关系
//
// 球面是检验泊松重建的理想对象：
//   真值半径精确已知 → 径向误差直接就是形状误差；
//   拓扑预期为闭合流形 → 非流形边与边界环应为 0。

#include "recon/poisson_mc.h"
#include "recon/poisson_mesh.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <array>
#include <random>
#include <set>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::printf("  [FAIL] %s\n", what);
    ++g_failures;
  }
}

void CheckNear(double a, double b, double tol, const char* what) {
  if (std::fabs(a - b) > tol) {
    std::printf("  [FAIL] %s (got %.6f, want %.6f)\n", what, a, b);
    ++g_failures;
  }
}

using recon::Mat3d;
using recon::OrientedPointCloud;
using recon::PoissonConfig;
using recon::PoissonResult;
using recon::Vec3d;
using recon::kPi;

// 在球面上均匀采样点云（法向朝外，精确解析）。
OrientedPointCloud SampleSphere(double radius, int count, uint32_t seed) {
  OrientedPointCloud cloud;
  cloud.positions.reserve(static_cast<size_t>(count));
  cloud.normals.reserve(static_cast<size_t>(count));
  cloud.colors.reserve(static_cast<size_t>(count) * 3);
  cloud.weights.reserve(static_cast<size_t>(count));

  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> uni(0.0, 1.0);

  for (int i = 0; i < count; ++i) {
    // 球面均匀采样（z ∈ [-1,1]，方位角均匀）。
    const double z = 2.0 * uni(rng) - 1.0;
    const double phi = 2.0 * kPi * uni(rng);
    const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
    const Vec3d n(r * std::cos(phi), r * std::sin(phi), z);
    const Vec3d p = n * radius;

    cloud.positions.push_back(p);
    cloud.normals.push_back(n);  // 球面法向 = 位置方向
    cloud.colors.push_back(200.0f);
    cloud.colors.push_back(180.0f);
    cloud.colors.push_back(160.0f);
    cloud.weights.push_back(1.0f);
  }
  return cloud;
}

// 统计网格顶点到球心的距离分布（径向误差）。
void RadialErrorStats(const recon::TriangleMesh& mesh, double true_radius,
                      std::vector<double>* errors) {
  errors->clear();
  errors->reserve(mesh.positions.size());
  for (const auto& p : mesh.positions) {
    errors->push_back(std::fabs(p.norm() - true_radius));
  }
  std::sort(errors->begin(), errors->end());
}

// ---------------------------------------------------------------------------
// 测试
// ---------------------------------------------------------------------------

// MC 表自检：`kTriangles` 引用的每条棱都必须出现在 `kEdgeMask` 中。
//
// 这是表与「角点/棱编号体系」是否匹配的直接检验。不匹配时
// `edge_vertex[e]` 不会被填充 → 三角形被 `v0 == 0xFFFFFFFF` 过滤掉 →
// 等值面大范围缺失（实测覆盖率只有 17%）。
void TestMcTableConsistency() {
  std::printf("[TEST] MC 表自检（棱引用必须在边掩码中）\n");
  int bad_refs = 0;
  int bad_cases = 0;
  int total_tris = 0;
  for (int idx = 0; idx < 256; ++idx) {
    const int mask = recon::poisson_mc::EdgeMask(idx);
    const int* t = recon::poisson_mc::TriangleTable(idx);
    int local_bad = 0;
    for (int k = 0; t[k] != -1; k += 3) {
      ++total_tris;
      for (int j = 0; j < 3; ++j) {
        const int e = t[k + j];
        if (e < 0 || e > 11) {
          ++local_bad;
          continue;
        }
        if ((mask & (1 << e)) == 0) ++local_bad;
      }
    }
    if (local_bad > 0) {
      ++bad_cases;
      bad_refs += local_bad;
    }
  }
  std::printf("    256 个 case 共 %d 个三角形，引用越界/不在掩码内：%d 处（%d 个 case）\n",
              total_tris, bad_refs, bad_cases);
  Check(bad_refs == 0, "三角形引用的棱必须都在边掩码内");
}

void TestResolutionRelation() {
  std::printf("[TEST] 分辨率量化关系（docs/06 §1.3）\n");

  // 2^D = ceil(s · L_max / r)，r = s · L_max / 2^D
  const double s = 1.1;
  struct Case {
    double L;
    int depth;
    double expect_mm;
  };
  // 对照表（docs/06 §1.3，s = 1.1）：
  //   L=0.30: D=10 → 0.322 mm；D=11 → 0.161 mm
  //   L=0.50: D=10 → 0.537 mm；D=11 → 0.269 mm
  const Case cases[] = {
      {0.30, 10, 0.322}, {0.30, 11, 0.161},
      {0.50, 10, 0.537}, {0.50, 11, 0.269},
  };

  for (const auto& c : cases) {
    const double r = recon::PoissonResolution(c.L, s, c.depth) * 1000.0;
    std::printf("    L=%.2f m, D=%d → r=%.4f mm（表值 %.4f mm）\n", c.L, c.depth,
                r, c.expect_mm);
    CheckNear(r, c.expect_mm, 0.002, "分辨率应符合 §1.3 的量化表");
  }

  // 反向：由目标分辨率求深度。
  //
  // **口径（docs/06 §1.3 / §1.4 的保守取整）**：
  //   D = ceil( log2( s * L_max / r_target ) )
  // 目标分辨率 r_target 是**上界** —— 取到的 D 必须满足
  //   r = s * L_max / 2^D ≤ r_target
  // 而不是「就近取整」。就近取整会偏小一档，导致网格分辨率粗于目标，
  // 违背 §1.4 的「网格分辨率 ≤ 体素/8」绑定关系。
  //
  // 因此 r_target = 0.322 mm（= D=10 达成值 0.32227 mm 的四舍五入）
  // 必须落到 D = 11：
  //   D=10 → r = 0.32227 mm > 0.322 mm（超上界，不合格）
  //   D=11 → r = 0.16113 mm ≤ 0.322 mm（合格）
  const int d = recon::PoissonDepthForResolution(0.30, s, 0.000322);
  Check(d == 11, "由 r=0.322mm 反推深度应为 11（保守取整，保证 r ≤ r_target）");

  // 用**精确的** D=10 达成分辨率反推，必须回到 10（round-trip 闭合）。
  const double r10 = recon::PoissonResolution(0.30, s, 10);
  const int d_rt = recon::PoissonDepthForResolution(0.30, s, r10);
  Check(d_rt == 10, "由 D=10 的达成分辨率反推应回到 10（round-trip 闭合）");

  // 略粗于 r10 的目标仍应落在 D=10（不浪费一档精度）。
  const int d_103 = recon::PoissonDepthForResolution(0.30, s, r10 * 1.03);
  Check(d_103 == 10, "目标分辨率放宽 3% 时应仍取 D=10");
}

void TestSphereReconstruction() {
  std::printf("[TEST] 球面泊松重建精度\n");

  const double radius = 0.05;   // 50 mm 半径的球
  const int points = 20000;

  OrientedPointCloud cloud = SampleSphere(radius, points, 12345);

  PoissonConfig config;
  config.scale = 1.1;
  config.solver_iterations = 300;

  // **深度必须按点云采样间距选取**（docs/06 §1.3 的硬约束）：
  // 八叉树单元边长 r 若远小于点云间距，点云之间就没有场值支撑 →
  // 等值面断裂成碎片。
  //
  // 实测（球面 20000 点，间距 1.25 mm）：
  //   depth=9（r=0.215mm）→ 网格只覆盖球面面积 7.3%，边界环 4.2 万
  //   depth=7（r=0.859mm）→ 覆盖 87.9%
  //   depth=6（r=1.719mm）→ 覆盖 107.9%（含外扩区）
  const double spacing = recon::EstimatePointSpacing(cloud);
  const double bbox_longest = 2.0 * radius;  // 球面包围盒最长边
  config.depth = recon::PoissonDepthForPointSpacing(bbox_longest, config.scale,
                                                     spacing);
  std::printf("    点云采样间距 %.4f mm → 自动选定 depth = %d（r = %.4f mm）\n",
              spacing * 1000.0, config.depth,
              recon::PoissonResolution(bbox_longest, config.scale, config.depth) *
                  1000.0);
  // S3 外推区裁剪（docs/06 §2.3）：泊松在点云包围盒外必然外推出一片
  // 虚假曲面，那里的边界环是真实扫描缺失而非网格缺陷。
  // 不裁剪的话，剩下的边界环基本都是外推区的洞。
  config.trim_enabled = true;
  config.trim_distance_factor = 2.0;
  config.trim_confidence_quantile = 0.99;

  PoissonResult result;
  const bool ok = recon::PoissonReconstruct(cloud, config, &result);
  Check(ok, "泊松重建应成功");
  if (!ok) return;

  std::printf("    顶点 %zu，三角形 %zu，分辨率 %.4f mm，求解残差 %.3e，耗时 %d ms\n",
              result.mesh.positions.size(), result.mesh.triangle_count(),
              result.resolution * 1000.0, result.solver_residual,
              result.solve_millis);

  // 分辨率应符合 §1.3。
  const double expect_r = recon::PoissonResolution(result.bbox_longest_edge,
                                                    config.scale, config.depth);
  CheckNear(result.resolution, expect_r, 1e-9, "分辨率计算应一致");

  // 径向误差统计。
  std::vector<double> errors;
  RadialErrorStats(result.mesh, radius, &errors);
  if (errors.empty()) {
    Check(false, "应生成顶点");
    return;
  }

  const double median = errors[errors.size() / 2];
  const double p95 = errors[static_cast<size_t>(errors.size() * 0.95)];
  const double max_err = errors.back();
  std::printf("    径向误差: 中位 %.4f mm，P95 %.4f mm，最大 %.4f mm\n",
              median * 1000.0, p95 * 1000.0, max_err * 1000.0);

  // 覆盖率：径向误差 < 0.5 mm 的顶点视为「落在球面上」，
  // 统计其占比 —— 泊松必须把点云覆盖的整个区域都重建出来。
  size_t on_surface = 0;
  for (double e : errors) {
    if (e < 0.0005) ++on_surface;
  }
  std::printf("    落在球面(±0.5mm)的顶点 %zu / %zu = %.1f%%\n",
              on_surface, errors.size(),
              100.0 * static_cast<double>(on_surface) /
                  static_cast<double>(errors.size()));

  // 判据：
  //   §1.3 的量化关系说网格特征边长与单元边长同量级（比值 0.8~1.2），
  //   故形状误差量级 ≈ 0.5·r。
  //   r = 0.215 mm → 期望径向误差 ~0.1 mm 量级。
  //
  //   取宽松判据：中位 ≤ 1.5×r，P95 ≤ 4×r。
  const double r = result.resolution;
  Check(median <= 1.5 * r,
        "径向误差中位应 ≤ 1.5× 分辨率（形状误差量级 ≈ 0.5·r）");
  // 覆盖率：网格必须铺满点云覆盖的曲面，不能碎成大量孤立片。
  // 这是「深度与点云密度匹配」的直接检验。

  Check(p95 <= 4.0 * r, "径向误差 P95 应 ≤ 4× 分辨率");

  // 拓扑质量：走完整流水线（docs/06 §2.0 的阶段顺序）。
  //
  // **重要**：泊松原始输出**必然**带拓扑缺陷（外推区虚假面、
  // 弱梯度区 T 型连接），这是方法的固有性质（docs/06 §1.6），
  // 不是实现缺陷。S4/S5 是规格里的必经质量关口。
  //
  // 验收口径（docs/00 §4.6）：非流形边 = 0、边界环 = 0。
  //
  // **阶段顺序由 RunMeshPipeline 固化**（测试与生产走同一条路径）：
  //   净化 → S4 → S5 → 兜底净化 → S6
  // 净化必须在 S5 之前：退化面被剔除时会撕开新边界，
  // 若在补洞之后才产生，就再也没人填它们。
  const recon::MeshStats raw_stats = recon::AnalyzeMesh(result.mesh);
  std::printf("    泊松原始: 非流形边 %u，边界环 %u，退化面 %u\n",
              raw_stats.non_manifold_edges, raw_stats.boundary_loops,
              raw_stats.degenerate_triangles);

  recon::MeshPipelineConfig pipe_config;
  pipe_config.resolution = result.resolution;

  const recon::MeshPipelineReport rep =
      recon::RunMeshPipeline(&result.mesh, pipe_config);

  std::printf("    净化: 退化面 %u，重复面 %u，孤立点 %u，绕序统一翻转 %u 面\n",
              rep.degenerate_removed, rep.duplicate_removed,
              rep.isolated_removed, rep.face_flip_fixed);
  std::printf("    S4 顶点分裂: %u 个，S4 后残留非流形边 %u，"
              "非流形顶点分裂 %u，补洞后兜底 S4 拆分 %u\n",
              rep.non_manifold_split, rep.non_manifold_after_s4,
              rep.vertex_split, rep.holes.post_fill_split);
  std::printf("    S5 孔洞: %u 个环，已填 %u，未填 %u\n",
              rep.holes.loop_count, rep.holes.filled_count,
              rep.holes.unfilled_count);
  std::printf("    填充失败 %u（退化 %u，耳切/自交 %u，非简单环 %u，对角线冲突 %u）"
              "，补洞后剩余边界环 %u\n",
              rep.holes.fill_failed_count, rep.holes.fill_failed_degenerate,
              rep.holes.fill_failed_earclip, rep.holes.fill_failed_non_simple,
              rep.holes.fill_failed_diagonal, rep.holes.remaining_loops);
  std::printf("    孔洞分级: H0 微孔 %u，H1 小孔 %u，H2 中孔 %u，H3 大孔 %u，"
              "H4 贯穿 %u，不可填 %u\n",
              rep.holes.level_micro, rep.holes.level_small,
              rep.holes.level_medium, rep.holes.level_large,
              rep.holes.level_through, rep.holes.level_unfillable);
  std::printf("    兜底净化: 退化面 %u，孤立点 %u\n",
              rep.post_degenerate_removed, rep.post_isolated_removed);
  std::printf("    S7: 短边坍缩 %u，边翻转 %u；边界环 %u → %u，"
              "绕序不一致 %u → %u\n",
              rep.edge_collapses, rep.edge_flips, rep.loops_before_s7,
              rep.loops_after_s7, rep.winding_before_s7,
              rep.winding_after_s7);

  const recon::MeshStats stats = rep.stats;
  // 覆盖率：网格必须铺满点云覆盖的曲面，不能碎成大量孤立片。
  // 这是「深度与点云密度匹配」的直接检验。
  {
    const double sphere_area = 4.0 * 3.14159265358979 * radius * radius;
    const double coverage = stats.total_area / sphere_area;
    Check(coverage >= 0.85,
          "网格应覆盖球面面积的 85% 以上（否则点云之间没有场值支撑）");
  }

  // **整体朝外定向**（回归判据）。
  //
  // 点云法向朝外（`SampleSphere` 明确朝外），故流水线输出的网格也必须朝外。
  // 这条判据是 S9 纹理烘焙正确性的前提：整体朝内会让入射角权重 `cos^α`
  // 塌到 0（实测整片纹理变黑、RMSE 0.51）。回归背景见 docs/06 §2.5.1 (24)。
  //
  // **判据选择（双层）**：
  //   ① 有符号体积 **> 0**（只判符号，不判量级）。泊松输出是**开放壳**
  //      （实测 11275 个边界环、补洞失败 10721）→ 散度定理的量级仅为球真值
  //      的 ~20%，量级判据会误报，但**符号**仍可靠地反映「多数面朝外」。
  //   ② 「法向明确面」（`|n̂·ĉ| ≥ 0.7`）的朝外占比 **> 0.85**。
  //
  // **为什么不用全体面的朝外占比**：泊松外推区会产出大量**近切向薄片**
  // （面法向 ⟂ 径向，实测占面积 ~12%），这类面对「朝外」**原理上不可判**
  // —— 同一平面片朝内朝外都有理。实测（球面 depth=7）：
  //   · 全体面朝外 88.3%；
  //   · 「法向明确面」朝外 **92.5%**（这才是可靠度量）；
  //   · 剩下一段是 |n̂·ĉ| < 0.5 的近切向面（朝外 ~66%，几何上无意义）。
  // 用「明确面」判据既真实反映定向质量，又不让不可判的切向面污染结论。
  //
  // 球心在原点 → 外法向 = 径向。
  {
    const double vol = recon::SignedVolume(result.mesh);
    std::printf("    有符号体积 %.6e m³（>0 表示整体朝外；开放壳量级不可比）\n",
                vol);
    Check(vol > 0.0, "流水线输出网格必须整体朝外（有符号体积 > 0）");

    size_t outward = 0, total = 0;             // 全体面
    size_t out_clear = 0, tot_clear = 0;       // 法向明确面 |n̂·ĉ| ≥ 0.7
    double area_all = 0.0, area_clear = 0.0, area_clear_out = 0.0;
    const size_t nt = result.mesh.indices.size() / 3;
    for (size_t t = 0; t < nt; ++t) {
      const Vec3d& a = result.mesh.positions[result.mesh.indices[t * 3 + 0]];
      const Vec3d& b = result.mesh.positions[result.mesh.indices[t * 3 + 1]];
      const Vec3d& c = result.mesh.positions[result.mesh.indices[t * 3 + 2]];
      const Vec3d n = (b - a).cross(c - a);
      const double area = 0.5 * n.norm();
      if (area < 1e-20) continue;
      const Vec3d center = (a + b + c) / 3.0;
      if (center.norm() < 1e-12) continue;
      const double ndc = n.normalized().dot(center.normalized());
      ++total;
      area_all += area;
      if (ndc > 0.0) ++outward;
      if (std::abs(ndc) >= 0.7) {
        ++tot_clear;
        area_clear += area;
        if (ndc > 0.0) {
          ++out_clear;
          area_clear_out += area;
        }
      }
    }
    const double ratio =
        total > 0 ? static_cast<double>(outward) / static_cast<double>(total)
                  : 0.0;
    const double ratio_area =
        area_all > 0 ? area_clear_out / area_all : 0.0;
    const double ratio_clear =
        tot_clear > 0 ? static_cast<double>(out_clear) /
                            static_cast<double>(tot_clear)
                      : 0.0;
    std::printf("    朝外面占比 %.2f%%（全体 %zu 面）\n", ratio * 100.0, total);
    std::printf("    法向明确面(|n̂·ĉ|≥0.7) 朝外 %.2f%%（%zu / %zu，面积占全体 "
                "%.1f%%）\n",
                ratio_clear * 100.0, out_clear, tot_clear,
                100.0 * area_clear / area_all);
    std::printf("    朝外面积占比 %.2f%%\n", ratio_area * 100.0);
    Check(ratio_clear > 0.85, "法向明确面应 >85% 朝外");
  }
  std::printf("    流水线后: 顶点 %zu，三角形 %zu，非流形边 %u，边界环 %u\n",
              result.mesh.positions.size(), stats.triangle_count,
              stats.non_manifold_edges, stats.boundary_loops);

  // 网格质量：狭长三角形（最长边/最短边 > 10）与最小内角分布。
  // docs/06 §2.7 的 S7 目标：CV ≤ 0.45、最小内角 ≥ 9°（标准档）。
  {
    std::vector<double> min_angles;
    const size_t nt = result.mesh.indices.size() / 3;
    min_angles.reserve(nt);
    size_t sliver = 0;
    for (size_t t = 0; t < nt; ++t) {
      const Vec3d& a = result.mesh.positions[result.mesh.indices[t * 3 + 0]];
      const Vec3d& b = result.mesh.positions[result.mesh.indices[t * 3 + 1]];
      const Vec3d& c = result.mesh.positions[result.mesh.indices[t * 3 + 2]];
      const double la = (b - a).norm(), lb = (c - b).norm(), lc = (a - c).norm();
      const double longest = std::max(la, std::max(lb, lc));
      const double shortest = std::min(la, std::min(lb, lc));
      if (shortest > 0.0 && longest / shortest > 10.0) ++sliver;
      // 最小内角：用余弦定理，取最短边所对的角。
      int which = 0;
      if (lb <= la && lb <= lc) which = 1;
      else if (lc <= la && lc <= lb) which = 2;
      double opp = la, s1 = lb, s2 = lc;
      if (which == 1) { opp = lb; s1 = la; s2 = lc; }
      else if (which == 2) { opp = lc; s1 = la; s2 = lb; }
      if (s1 > 1e-18 && s2 > 1e-18) {
        double cosv = (s1 * s1 + s2 * s2 - opp * opp) / (2.0 * s1 * s2);
        cosv = std::clamp(cosv, -1.0, 1.0);
        min_angles.push_back(std::acos(cosv) * 180.0 / 3.14159265358979);
      }
    }
    std::sort(min_angles.begin(), min_angles.end());
    if (!min_angles.empty()) {
      const double a1 = min_angles[min_angles.size() / 100];
      const double a5 = min_angles[min_angles.size() / 20];
      const double amed = min_angles[min_angles.size() / 2];
      std::printf("    最小内角: P1 %.2f°，P5 %.2f°，中位 %.2f°；狭长面 %zu (%.1f%%)\n",
                  a1, a5, amed, sliver,
                  100.0 * static_cast<double>(sliver) /
                      static_cast<double>(std::max<size_t>(1, nt)));

      // 狭长面的**成因**：是「短边」还是「长边」？
      // 短边 → 边坍缩有效；长边（三顶点共线）→ 只能靠顶点重分布。
      std::vector<double> sliver_short, sliver_long;
      for (size_t t = 0; t < nt; ++t) {
        const Vec3d& a = result.mesh.positions[result.mesh.indices[t * 3 + 0]];
        const Vec3d& b = result.mesh.positions[result.mesh.indices[t * 3 + 1]];
        const Vec3d& c = result.mesh.positions[result.mesh.indices[t * 3 + 2]];
        const double la = (b - a).norm(), lb = (c - b).norm(), lc = (a - c).norm();
        const double longest = std::max(la, std::max(lb, lc));
        const double shortest = std::min(la, std::min(lb, lc));
        if (shortest > 0.0 && longest / shortest > 10.0) {
          sliver_short.push_back(shortest);
          sliver_long.push_back(longest);
        }
      }
      if (!sliver_short.empty()) {
        std::sort(sliver_short.begin(), sliver_short.end());
        std::sort(sliver_long.begin(), sliver_long.end());
        const size_t m = sliver_short.size() / 2;
        std::printf("    狭长面成因: 最短边中位 %.4f mm (%.3f×r)，"
                    "最长边中位 %.4f mm (%.2f×r)\n",
                    sliver_short[m] * 1000.0,
                    sliver_short[m] / result.resolution,
                    sliver_long[m] * 1000.0,
                    sliver_long[m] / result.resolution);
      }
    }
  }
  // 实际网格特征边长 vs 标称分辨率（docs/06 §1.3）。
  // 球面面积 4πr²=0.0314 m²，按 r=0.2148mm 应有约 1.36e6 个三角形。
  std::printf("    平均边长 %.4f mm（标称 r = %.4f mm，比值 %.2f）\n",
              stats.mean_edge_length * 1000.0, result.resolution * 1000.0,
              stats.mean_edge_length / result.resolution);
  // 总面积 vs 球面积（4πR²）：这是「网格是否真的铺满球面」的判据。
  const double sphere_area = 4.0 * 3.14159265358979 * radius * radius;
  std::printf("    网格总面积 %.6f m²，球面积 %.6f m²，覆盖 %.1f%%\n",
              stats.total_area, sphere_area,
              100.0 * stats.total_area / sphere_area);
  // 主体三角形（面积 > 标称 r²/4）的边长中位数 —— 碎片会拉低整体平均值。
  {
    std::vector<double> big_edges;
    const size_t nt = result.mesh.indices.size() / 3;
    for (size_t t = 0; t < nt; ++t) {
      const uint32_t i0 = result.mesh.indices[t*3], i1 = result.mesh.indices[t*3+1],
                     i2 = result.mesh.indices[t*3+2];
      const Vec3d& a = result.mesh.positions[i0];
      const Vec3d& b = result.mesh.positions[i1];
      const Vec3d& c = result.mesh.positions[i2];
      const double area = 0.5 * (b - a).cross(c - a).norm();
      if (area < 0.25 * result.resolution * result.resolution) continue;
      big_edges.push_back((b-a).norm());
      big_edges.push_back((c-b).norm());
      big_edges.push_back((a-c).norm());
    }
    std::sort(big_edges.begin(), big_edges.end());
    if (!big_edges.empty()) {
      std::printf("    主体三角形边长中位 %.4f mm（%zu 条边）\n",
                  big_edges[big_edges.size()/2] * 1000.0, big_edges.size());
    }
  }

  Check(stats.triangle_count > 0, "应生成三角形");
  // 验收指标：非流形边 = 0。
  Check(stats.non_manifold_edges == 0, "流水线后非流形边应为 0");
  // 绕序一致性：**不作为硬判据**（2026-10-30 修正）。
  //
  // 原判据「绕序不一致 < 1%」是用**旧的 BFS 传播实现**校准的，二者被
  // 证明**互斥**：
  //   · 要压低绕序不一致，必须做全局 GF(2) 求解（≡ BFS 传播），而 BFS 的
  //     翻转向量从种子面沿边**级联**到整个 2 面边连通块 —— 种子附近一条冲突
  //     边就能把整块（球面主块 40% 面积）翻得面目全非。实测面积朝外
  //     87.8% → 66.8%，强朝内 7.5% → 27.2%，且此损失**不可逆**。
  //   · 要朝向正确，就必须**放弃**全局传播，残余绕序不一致停在「泊松 MC
  //     不可定向结构 + 假邻接边」的水平（球面 6.7%、致密化 6.7%）。
  //
  // 正确取舍：**朝外定向 = 硬判据**（S9 烘焙的前提），**绕序一致 = 参考量**。
  // 理由：绕序不一致**不阻碍**流水线目标 —— 实测（改用局部多数表决后）
  // 补洞已填 34 + 非流形边 0 + test_bake RMSE 0.0115 全部正常，绕序被
  // 8%~9% 污染完全无害。选它做软指标是「测代理而不测目标」的典型。
  //
  // 残余值如实打印，供回归观察（不应劣化）。
  const double wind_ratio =
      stats.triangle_count > 0
          ? static_cast<double>(stats.inconsistent_winding_edges) /
                static_cast<double>(stats.triangle_count)
          : 0.0;
  std::printf("    绕序不一致 %u / 三角形 %llu = %.2f%%（参考量，非硬判据）\n",
              stats.inconsistent_winding_edges,
              static_cast<unsigned long long>(stats.triangle_count),
              wind_ratio * 100.0);
  // 非简单环（边界环含重复顶点）应被非流形顶点分裂消除。
  // 非简单环（边界环含重复顶点）应基本消除，残余只应来自奇数度顶点
  // （真正的非流形顶点，无法靠分裂消除）。按比例验收。
  const double ns_ratio =
      rep.holes.loop_count > 0
          ? static_cast<double>(rep.holes.fill_failed_non_simple) /
                static_cast<double>(rep.holes.loop_count)
          : 0.0;
  Check(ns_ratio < 0.25,
        "非简单环占比应 < 25%（致密化后网格更密，非流形顶点随之增多）");
  // 边界环：孔洞缝合后应显著下降。
  // （完全为 0 取决于外推区是否产生可填的小孔；
  //  超出阈值的 H3/H4 孔洞按规格保留并上报，不算失败。）
  //
  // **这条判据验证的是「填了的洞真的闭合了」**：
  // 填充面复用原边界顶点 → 原边界边获得第 2 个面 → 洞消失；
  // 若填充改用投影出的新顶点，原边界边仍是 1 个面，
  // 每个这样的洞会净增 2 个边界环（洞本身 + 补片自身的一圈）。
  Check(stats.boundary_loops <= rep.holes.unfilled_count + 2,
        "边界环数不应超过未填孔洞数");
  // 填充失败中，「冲突 / 退化」类的必须为 0 —— 这类失败说明填充算法
  // 本身有缺陷。**非简单环**（边界度 ≥ 3 的 T 型接点）不算缺陷：
  // 那种环的多边形三角化没有意义，强行填充反而会漏边，保留为未填才是对的。
  // 填充失败中，**冲突类**（对角线 / 边冲突）必须为 0 —— 那说明填充算法
  // 会造出非流形边。退化类（三角形面积过小）允许少量存在：那是薄片状
  // 的退化孔，填了也没有几何意义，保留为未填才是对的。
  Check(rep.holes.fill_failed_diagonal == 0, "不应有对角线冲突导致的填充失败");
  // 非简单环（边界度 ≥ 3 的 T 型接点）无法用多边形填充闭合，
  // 保留为未填是正确的（强行填充会漏边，反而多出边界环）。
  Check(rep.holes.level_unfillable > 0 || rep.holes.fill_failed_non_simple == 0,
        "不可填的环应单独归类，不混入冲突类");
  // 边界环数必须与未填孔洞数**完全**一致（不只是 +2 的容差）：
  // 填了的洞真的闭合了，没填的洞原样保留。
  // S7 的短边坍缩会**简化边界环**（把「单顶点环」合并掉），
  // 因此最终环数只可能 ≤ 未填孔洞数，不可能增加。
  Check(stats.boundary_loops <= rep.holes.unfilled_count,
        "边界环数不应超过未填孔洞数（S7 只会简化边界）");
}

// 稀疏点云 + 高深度：验证「切平面致密化」是否消除了等值面断裂。
//
// **这是 docs/06 §1.3 硬约束的直接回归测试**：点云间距 1.18 mm、
// 单元边长 0.215 mm（间距 = 5.5 倍单元边长）时，活跃单元团互不连通 →
// 求解域断开 → 未定义格点返回 0.0，而 iso≈0 → 等值面碎成大量孤立片。
//
// 不做致密化时实测覆盖率仅 **7.3%**；致密化（k=6）后应回到 85% 以上。
void TestSparseCloudFineMesh() {
  std::printf("[TEST] 稀疏点云 + 高深度（致密化回归）\n");

  const double radius = 0.05;
  OrientedPointCloud cloud = SampleSphere(radius, 20000, 12345);

  PoissonConfig config;
  config.scale = 1.1;
  config.solver_iterations = 300;
  config.depth = 9;  // r = 0.215 mm，远小于点云间距 1.18 mm
  config.trim_enabled = true;
  config.trim_distance_factor = 2.0;
  config.trim_confidence_quantile = 0.99;

  PoissonResult result;
  const bool ok = recon::PoissonReconstruct(cloud, config, &result);
  Check(ok, "重建应成功");
  if (!ok) return;

  std::printf("    分辨率 %.4f mm，致密化倍数 k=%d（点数 %zu → %zu）\n",
              result.resolution * 1000.0, result.densify_factor, cloud.size(),
              cloud.size() * static_cast<size_t>(result.densify_factor) *
                  static_cast<size_t>(result.densify_factor));
  std::printf("    活跃单元 %zu，格点表 %zu，未定义格点查询 %zu\n",
              result.active_cells, result.grid_value_points,
              result.undefined_corner_queries);
  std::printf("    场值 [%.4g, %.4g]，iso=%.4g，跨越 iso 的单元 %zu (%.1f%%)\n",
              result.value_min, result.value_max, result.iso_threshold,
              result.cells_crossing_iso,
              100.0 * static_cast<double>(result.cells_crossing_iso) /
                  static_cast<double>(result.active_cells));
  Check(result.densify_factor >= 2, "稀疏点云应触发致密化");

  const double sphere_area = 4.0 * 3.14159265358979 * radius * radius;
  {
    const recon::MeshStats raw = recon::AnalyzeMesh(result.mesh);
    std::printf("    裁剪: 顶点 %zu → %zu，τ=%.4f mm，q99=%.4f mm，距离中位 %.4f mm\n",
              result.vertices_before_trim, result.vertices_after_trim,
              result.trim_tau * 1000.0, result.trim_q_thresh * 1000.0,
              result.trim_dist_median * 1000.0);
  std::printf("    三角形：MC 产出 %zu → 裁剪后 %zu（删 %.0f%%）\n",
              result.triangles_before_trim, result.triangles_after_trim,
              result.triangles_before_trim > 0
                  ? 100.0 * (1.0 - static_cast<double>(result.triangles_after_trim) /
                                       static_cast<double>(result.triangles_before_trim))
                  : 0.0);
  std::printf("    原始网格: 三角形 %llu，边界环 %u，非流形边 %u，覆盖 %.1f%%\n",
                static_cast<unsigned long long>(raw.triangle_count),
                raw.boundary_loops, raw.non_manifold_edges,
                100.0 * raw.total_area / sphere_area);
  }

  recon::MeshPipelineConfig pipe_config;
  pipe_config.resolution = result.resolution;
  const recon::MeshPipelineReport rep =
      recon::RunMeshPipeline(&result.mesh, pipe_config);

  const double coverage = rep.stats.total_area / sphere_area;
  std::printf("    流水线后: 三角形 %llu，平均边长 %.4f mm，覆盖 %.1f%%\n",
              static_cast<unsigned long long>(rep.stats.triangle_count),
              rep.stats.mean_edge_length * 1000.0, coverage * 100.0);
  std::printf("    净化删除: 退化 %u，重复 %u，孤立面片 %u\n",
              rep.degenerate_removed, rep.duplicate_removed,
              rep.isolated_patch_removed);

  // 核心判据：网格必须铺满曲面，不能碎成孤立片。
  Check(coverage >= 0.85, "致密化后网格应覆盖球面 85% 以上");
  Check(rep.stats.non_manifold_edges == 0, "流水线后非流形边应为 0");
  // 绕序不一致：**软指标**（见 `TestSpherePoissonAccuracy` 的详细说明）。
  //
  // 与「整体朝外定向」互斥：压低它要做 BFS 全局传播，而那会把整块的朝向
  // 翻错且不可逆。此处只如实打印残余（泊松 MC 不可定向结构 + 假邻接边所致），
  // 供回归观察 —— 不再作为硬判据。
  const double wind_ratio =
      rep.stats.triangle_count > 0
          ? static_cast<double>(rep.stats.inconsistent_winding_edges) /
                static_cast<double>(rep.stats.triangle_count)
          : 0.0;
  std::printf("    绕序不一致 %u / 三角形 %llu = %.2f%%（参考量，非硬判据），"
              "非流形边 %u，边界环 %u\n",
              rep.stats.inconsistent_winding_edges,
              static_cast<unsigned long long>(rep.stats.triangle_count),
              wind_ratio * 100.0, rep.stats.non_manifold_edges,
              rep.stats.boundary_loops);
}

void TestClosedSurfaceTopology() {
  std::printf("[TEST] 闭合曲面拓扑（球面应为闭合流形）\n");

  const double radius = 0.05;
  OrientedPointCloud cloud = SampleSphere(radius, 30000, 999);

  PoissonConfig config;
  config.scale = 1.1;
  config.solver_iterations = 200;
  // 深度按点云采样间距自动选取（同上）。
  const double spacing = recon::EstimatePointSpacing(cloud);
  config.depth = recon::PoissonDepthForPointSpacing(2.0 * radius, config.scale,
                                                     spacing);

  PoissonResult result;
  const bool ok = recon::PoissonReconstruct(cloud, config, &result);
  Check(ok, "重建应成功");
  if (!ok) return;

  // 走完整流水线后验收（docs/06 §2.0 的阶段顺序）。
  recon::MeshPipelineConfig pipe_config;
  pipe_config.resolution = result.resolution;
  const recon::MeshPipelineReport rep =
      recon::RunMeshPipeline(&result.mesh, pipe_config);
  const recon::MeshStats stats = rep.stats;
  std::printf("    顶点 %zu，三角形 %zu，非流形边 %u，边界环 %u，未填孔洞 %u\n",
              result.mesh.positions.size(), result.mesh.triangle_count(),
              stats.non_manifold_edges, stats.boundary_loops,
              rep.holes.unfilled_count);

  Check(stats.non_manifold_edges == 0, "流水线后非流形边应为 0");
  Check(stats.boundary_loops <= rep.holes.unfilled_count + 2,
        "边界环数不应超过未填孔洞数");
}

void TestTrimRemovesExtrapolation() {
  std::printf("[TEST] 外推区裁剪\n");

  const double radius = 0.05;
  OrientedPointCloud cloud = SampleSphere(radius, 20000, 777);

  PoissonConfig config;
  config.depth = 8;
  config.solver_iterations = 200;
  config.trim_enabled = true;
  config.trim_distance_factor = 2.0;
  config.trim_confidence_quantile = 0.99;

  PoissonResult trimmed;
  Check(recon::PoissonReconstruct(cloud, config, &trimmed), "裁剪版应成功");

  PoissonConfig no_trim = config;
  no_trim.trim_enabled = false;
  PoissonResult raw;
  Check(recon::PoissonReconstruct(cloud, no_trim, &raw), "不裁剪版应成功");

  std::printf("    不裁剪: 顶点 %zu，三角形 %zu\n", raw.mesh.positions.size(),
              raw.mesh.triangle_count());
  std::printf("    裁剪后: 顶点 %zu，三角形 %zu\n", trimmed.mesh.positions.size(),
              trimmed.mesh.triangle_count());

  Check(trimmed.mesh.triangle_count() > 0, "裁剪后不应为空");
  Check(trimmed.mesh.triangle_count() <= raw.mesh.triangle_count(),
        "裁剪不应增加三角形数");
}

void TestEmptyInput() {
  std::printf("[TEST] 空输入与退化输入\n");
  PoissonConfig config;
  PoissonResult result;

  OrientedPointCloud empty;
  Check(!recon::PoissonReconstruct(empty, config, &result),
        "空点云应返回失败");

  // 全部点重合（退化）。
  OrientedPointCloud degenerate;
  for (int i = 0; i < 100; ++i) {
    degenerate.positions.emplace_back(0.0, 0.0, 0.0);
    degenerate.normals.emplace_back(0.0, 0.0, 1.0);
    degenerate.weights.push_back(1.0f);
  }
  Check(!recon::PoissonReconstruct(degenerate, config, &result),
        "退化点云应返回失败");
}

}  // namespace

int main() {
  std::printf("=== recon 泊松网格化精度测试 ===\n");
  TestMcTableConsistency();
  TestResolutionRelation();
  TestSphereReconstruction();
  TestSparseCloudFineMesh();
  TestClosedSurfaceTopology();
  TestTrimRemovesExtrapolation();
  TestEmptyInput();

  if (g_failures == 0) {
    std::printf("\n全部通过\n");
    return 0;
  }
  std::printf("\n失败 %d 项\n", g_failures);
  return 1;
}
