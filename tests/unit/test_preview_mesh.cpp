// SPDX-License-Identifier: BSD-3-Clause
//
// 预览网格构建单元测试（docs/09 §3.7 U-R-1..U-R-4）。
//
// 判据落点：
//   · U-R-2 抽稀是**确定性**的（同输入同输出），且只影响预览、不改源网格；
//   · 只保留被选中面引用的顶点（索引重映射正确、无悬挂顶点）；
//   · 空网格 → 空结果但 ok（「没有几何」≠「数据损坏」）；
//   · 索引越界 → ok=false（数据损坏必须显式失败，不得静默产出畸形缓冲）；
//   · decimation_step 如实回报（1 = 未抽稀）。

#include "recon/preview_mesh.h"

#include <cmath>
#include <cstdio>
#include <set>

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::printf("  [FAIL] %s\n", what);
    ++g_failures;
  }
}

// 构造一个 n 个三角形的条带网格：每个三角形用 3 个独立顶点，便于精确计数。
recon::TriangleMesh MakeStrip(size_t n_tris) {
  recon::TriangleMesh m;
  for (size_t t = 0; t < n_tris; ++t) {
    const double x = static_cast<double>(t);
    m.positions.emplace_back(x, 0.0, 0.0);
    m.positions.emplace_back(x + 1.0, 0.0, 0.0);
    m.positions.emplace_back(x, 1.0, 0.0);
    const uint32_t base = static_cast<uint32_t>(t * 3);
    m.indices.push_back(base);
    m.indices.push_back(base + 1);
    m.indices.push_back(base + 2);
  }
  return m;
}

// 构造一个三角形，但共享顶点（检验重映射去重）。
recon::TriangleMesh MakeSharedVertexTris() {
  recon::TriangleMesh m;
  m.positions.emplace_back(0.0, 0.0, 0.0);   // 0
  m.positions.emplace_back(1.0, 0.0, 0.0);   // 1
  m.positions.emplace_back(0.0, 1.0, 0.0);   // 2
  m.positions.emplace_back(1.0, 1.0, 0.0);   // 3
  // 两个三角形共享边 (1,2)
  m.indices = {0, 1, 2, 1, 3, 2};
  return m;
}

// ---------------------------------------------------------------------------
// 1. 不触发抽稀：面数 ≤ 上限时全部保留，step == 1
// ---------------------------------------------------------------------------
void TestNoDecimation() {
  std::printf("[TEST] 未触发抽稀（面数 ≤ 上限）\n");
  recon::TriangleMesh src = MakeStrip(10);
  recon::PreviewBuildResult r = recon::BuildPreviewMesh(src, 100);

  Check(r.ok, "构建应成功");
  Check(r.mesh.source_triangle_count == 10, "源面数应如实回报 10");
  Check(r.mesh.triangle_count() == 10, "未抽稀时面数应为 10");
  Check(r.mesh.decimation_step == 1, "未抽稀时 step 应为 1");
  Check(r.mesh.vertex_count() == 30, "未抽稀时顶点应为 30");
  Check(r.mesh.positions.size() == 90, "位置数组长度应为 3×30");

  // 源网格**不得**被修改（U-R-2：预览抽稀不写回网格）。
  Check(src.triangle_count() == 10, "源网格面数不应被改动");
  Check(src.positions.size() == 30, "源网格顶点不应被改动");
}

// ---------------------------------------------------------------------------
// 2. 触发抽稀：step = ceil(src/limit)，面数应为 ceil(src/step)
// ---------------------------------------------------------------------------
void TestDecimation() {
  std::printf("[TEST] 抽稀步长与面数\n");
  recon::TriangleMesh src = MakeStrip(100);

  // limit=10 → step = ceil(100/10) = 10 → 保留 t=0,10,...,90 共 10 个面
  recon::PreviewBuildResult r = recon::BuildPreviewMesh(src, 10);
  Check(r.ok, "构建应成功");
  Check(r.mesh.decimation_step == 10, "step 应为 10");
  Check(r.mesh.triangle_count() == 10, "抽稀后面数应为 10");
  Check(r.mesh.source_triangle_count == 100, "源面数应如实回报 100");
  Check(r.mesh.vertex_count() == 30, "只保留被选中面的顶点 → 30");

  // limit=0（默认上限 200 万）→ 100 面不触发抽稀
  recon::PreviewBuildResult r0 = recon::BuildPreviewMesh(src, 0);
  Check(r0.mesh.decimation_step == 1, "limit=0 时应使用默认上限、step=1");
  Check(r0.mesh.triangle_count() == 100, "limit=0 时应全保留");

  // 非整除：src=101, limit=10 → step = ceil(101/10) = 11 → 保留 t=0,11,...,99 共 10 个
  recon::TriangleMesh odd = MakeStrip(101);
  recon::PreviewBuildResult ro = recon::BuildPreviewMesh(odd, 10);
  Check(ro.mesh.decimation_step == 11, "非整除时 step 应为 ceil(101/10)=11");
  Check(ro.mesh.triangle_count() == 10, "非整除时保留面数应为 10");
}

// ---------------------------------------------------------------------------
// 3. 确定性：同输入 → 逐字节同输出（两次构建必须完全一致）
// ---------------------------------------------------------------------------
void TestDeterminism() {
  std::printf("[TEST] 抽稀确定性（同输入同输出）\n");
  recon::TriangleMesh src = MakeStrip(37);
  recon::PreviewBuildResult a = recon::BuildPreviewMesh(src, 7);
  recon::PreviewBuildResult b = recon::BuildPreviewMesh(src, 7);

  Check(a.mesh.indices == b.mesh.indices, "两次构建的索引应逐元素相同");
  Check(a.mesh.positions == b.mesh.positions, "两次构建的位置应逐元素相同");
  Check(a.mesh.decimation_step == b.mesh.decimation_step, "两次 step 应相同");
}

// ---------------------------------------------------------------------------
// 4. 索引重映射：共享顶点应去重，且索引全部落在新顶点范围内
// ---------------------------------------------------------------------------
void TestIndexRemap() {
  std::printf("[TEST] 索引重映射与去重\n");
  recon::TriangleMesh src = MakeSharedVertexTris();
  recon::PreviewBuildResult r = recon::BuildPreviewMesh(src, 100);

  Check(r.ok, "构建应成功");
  Check(r.mesh.triangle_count() == 2, "应保留 2 个面");
  // 4 个源顶点全部被引用 → 保留 4 个
  Check(r.mesh.vertex_count() == 4, "共享顶点应去重为 4");

  // 每个索引都必须 < vertex_count（不得越界）
  bool all_in_range = true;
  for (uint32_t i : r.mesh.indices) {
    if (i >= r.mesh.vertex_count()) all_in_range = false;
  }
  Check(all_in_range, "所有索引必须落在 [0, vertex_count)");

  // 共用的顶点在重映射后必须**仍是同一个**（去重正确性）
  // 源 indices = {0,1,2, 1,3,2} → 面1=(0,1,2)，面2=(1,3,2)
  // 于是 index[1] 与 index[3] 都对应源顶点 1，应相等；index[2] 与 index[5] 同理。
  Check(r.mesh.indices[1] == r.mesh.indices[3], "源顶点 1 在新索引中应一致");
  Check(r.mesh.indices[2] == r.mesh.indices[5], "源顶点 2 在新索引中应一致");

  // 位置必须是 float32 精确可表达的整数值（本用例坐标皆为整数）
  Check(std::fabs(r.mesh.positions[0] - 0.0f) < 1e-9f, "顶点 0 的 x 应为 0");
  Check(std::fabs(r.mesh.positions[3] - 1.0f) < 1e-9f, "顶点 1 的 x 应为 1");
}

// ---------------------------------------------------------------------------
// 5. 空网格：返回空结果但 ok=true（调用方据此转「无几何」错误码）
// ---------------------------------------------------------------------------
void TestEmptyMesh() {
  std::printf("[TEST] 空网格\n");
  recon::TriangleMesh empty;
  recon::PreviewBuildResult r = recon::BuildPreviewMesh(empty, 100);

  Check(r.ok, "空网格不算数据损坏，ok 应为 true");
  Check(r.mesh.triangle_count() == 0, "空网格面数应为 0");
  Check(r.mesh.vertex_count() == 0, "空网格顶点数应为 0");
  Check(r.mesh.positions.empty(), "位置数组应为空");
  Check(r.mesh.indices.empty(), "索引数组应为空");
}

// ---------------------------------------------------------------------------
// 6. 索引越界：ok=false（负向注入 —— 数据损坏必须显式失败）
// ---------------------------------------------------------------------------
void TestOutOfRangeIndex() {
  std::printf("[TEST] 索引越界（负向注入）\n");
  recon::TriangleMesh bad = MakeStrip(2);
  bad.indices[3] = 9999;  // 第二个面的首个索引指向不存在的顶点

  recon::PreviewBuildResult r = recon::BuildPreviewMesh(bad, 100);
  Check(!r.ok, "索引越界时 ok 必须为 false（不得静默产出畸形缓冲）");
  Check(r.mesh.positions.empty(), "失败时位置数组应为空");
  Check(r.mesh.indices.empty(), "失败时索引数组应为空");
}

// ---------------------------------------------------------------------------
// 7. 退化的面小于 1 三角（indices 不足 3 个）
// ---------------------------------------------------------------------------
void TestPartialTriangle() {
  std::printf("[TEST] 不足一个三角面\n");
  recon::TriangleMesh m;
  m.positions.emplace_back(0.0, 0.0, 0.0);
  m.positions.emplace_back(1.0, 0.0, 0.0);
  m.indices = {0, 1};  // 只有 2 个索引 → 0 个完整面

  recon::PreviewBuildResult r = recon::BuildPreviewMesh(m, 100);
  Check(r.mesh.triangle_count() == 0, "不足 3 个索引应产出 0 个面");
  Check(r.mesh.source_triangle_count == 0, "源面数应为 0");
}

}  // namespace

int main() {
  std::printf("=== recon 预览网格测试（docs/09 U-R-1..U-R-4）===\n");
  TestNoDecimation();
  TestDecimation();
  TestDeterminism();
  TestIndexRemap();
  TestEmptyMesh();
  TestOutOfRangeIndex();
  TestPartialTriangle();

  if (g_failures == 0) {
    std::printf("全部通过。\n");
    return 0;
  }
  std::printf("失败 %d 项。\n", g_failures);
  return 1;
}
