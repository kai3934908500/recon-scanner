// SPDX-License-Identifier: BSD-3-Clause
//
// S8 UV 展开测试。契约：docs/06_网格化与后处理.md §2.8、§4.1。
//
// 用**解析构造**的网格做验证：
//   · 经纬球面（法向连续变化，chart 分割受考验）
//   · 立方体（六面，理想情况应恰好切成 6 个 chart）
//   · 平面（单 chart，拉伸应接近 1）

#include "recon/uv_unwrap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "recon/mesh_repair.h"

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::printf("  [FAIL] %s\n", what);
    ++g_failures;
  }
}

using recon::TriangleMesh;
using recon::UvAtlas;
using recon::UvConfig;
using recon::Vec3d;
using recon::kPi;

// 标准经纬球（**极点只用单个顶点**）。
//
// 若像常见写法那样让 i=0 的 `lon+1` 个点全部落在极点（位置重合但索引不同），
// 网格自身就带 96 个退化三角形 —— 那不是 UV 展开的问题，而是输入不合法。
// 真流形球面必须共用极点顶点。
TriangleMesh MakeUvSphere(double radius, int lat, int lon) {
  TriangleMesh m;
  // 北极。
  const uint32_t north = 0;
  m.positions.emplace_back(0.0, 0.0, radius);
  // 中间 lat-1 圈。
  for (int i = 1; i < lat; ++i) {
    const double theta = kPi * static_cast<double>(i) / static_cast<double>(lat);
    for (int j = 0; j < lon; ++j) {
      const double phi =
          2.0 * kPi * static_cast<double>(j) / static_cast<double>(lon);
      m.positions.emplace_back(radius * std::sin(theta) * std::cos(phi),
                               radius * std::sin(theta) * std::sin(phi),
                               radius * std::cos(theta));
    }
  }
  // 南极。
  const uint32_t south = static_cast<uint32_t>(m.positions.size());
  m.positions.emplace_back(0.0, 0.0, -radius);

  const auto ring = [&](int i, int j) -> uint32_t {
    return 1 + static_cast<uint32_t>((i - 1) * lon + (j % lon));
  };

  // 北极扇。
  for (int j = 0; j < lon; ++j) {
    m.indices.push_back(north);
    m.indices.push_back(ring(1, j + 1));
    m.indices.push_back(ring(1, j));
  }
  // 中间带（四边形拆 2 三角）。
  for (int i = 1; i < lat - 1; ++i) {
    for (int j = 0; j < lon; ++j) {
      const uint32_t a = ring(i, j);
      const uint32_t b = ring(i, j + 1);
      const uint32_t c = ring(i + 1, j + 1);
      const uint32_t d = ring(i + 1, j);
      m.indices.push_back(a);
      m.indices.push_back(b);
      m.indices.push_back(c);
      m.indices.push_back(a);
      m.indices.push_back(c);
      m.indices.push_back(d);
    }
  }
  // 南极扇。
  for (int j = 0; j < lon; ++j) {
    m.indices.push_back(south);
    m.indices.push_back(ring(lat - 1, j));
    m.indices.push_back(ring(lat - 1, j + 1));
  }
  return m;
}

// 立方体（12 个三角形，法向只有 6 个方向）。
TriangleMesh MakeCube(double half) {
  TriangleMesh m;
  const double h = half;
  m.positions = {{-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h},
                 {-h, -h, h},  {h, -h, h},  {h, h, h},  {-h, h, h}};
  const uint32_t quads[6][4] = {
      {0, 3, 2, 1},  // -z
      {4, 5, 6, 7},  // +z
      {0, 1, 5, 4},  // -y
      {3, 7, 6, 2},  // +y
      {0, 4, 7, 3},  // -x
      {1, 2, 6, 5},  // +x
  };
  for (const auto& q : quads) {
    m.indices.push_back(q[0]);
    m.indices.push_back(q[1]);
    m.indices.push_back(q[2]);
    m.indices.push_back(q[0]);
    m.indices.push_back(q[2]);
    m.indices.push_back(q[3]);
  }
  return m;
}

// 平面网格（单 chart）。
TriangleMesh MakePlane(int n) {
  TriangleMesh m;
  for (int i = 0; i <= n; ++i) {
    for (int j = 0; j <= n; ++j) {
      m.positions.emplace_back(static_cast<double>(i) / n,
                               static_cast<double>(j) / n, 0.0);
    }
  }
  const int stride = n + 1;
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      const uint32_t a = static_cast<uint32_t>(i * stride + j);
      const uint32_t b = static_cast<uint32_t>(i * stride + j + 1);
      const uint32_t c = static_cast<uint32_t>((i + 1) * stride + j + 1);
      const uint32_t d = static_cast<uint32_t>((i + 1) * stride + j);
      m.indices.push_back(a);
      m.indices.push_back(b);
      m.indices.push_back(c);
      m.indices.push_back(a);
      m.indices.push_back(c);
      m.indices.push_back(d);
    }
  }
  return m;
}

// 通用校验：UV 合法、无 NaN、在 [0,1]、索引一致。
void ValidateAtlas(const UvAtlas& atlas, const char* tag) {
  Check(atlas.ok, tag);
  if (!atlas.ok) return;
  Check(atlas.uvs.size() == atlas.source_vertex.size(),
        "uvs 与 source_vertex 长度应一致");
  Check(!atlas.indices.empty(), "索引不应为空");

  bool all_finite = true;
  bool all_in_range = true;
  for (const auto& uv : atlas.uvs) {
    if (!std::isfinite(uv(0)) || !std::isfinite(uv(1))) all_finite = false;
    if (uv(0) < -1e-9 || uv(0) > 1.0 + 1e-9 || uv(1) < -1e-9 ||
        uv(1) > 1.0 + 1e-9) {
      all_in_range = false;
    }
  }
  Check(all_finite, "UV 不应含 NaN / Inf");
  Check(all_in_range, "UV 应全部落在 [0,1]");

  bool idx_ok = true;
  for (uint32_t i : atlas.indices) {
    if (i >= atlas.uvs.size()) idx_ok = false;
  }
  Check(idx_ok, "索引不应越界");
  // 零面积 UV 三角形：平面投影在「法向与投影方向平行」处（球面极点）
  // 必然产生，属固有缺陷。规格 §2.8 R4 要求「导出前剔除并记录」，
  // 因此这里只要求比例极低，不要求为 0。
  const size_t tri_n = atlas.indices.size() / 3;
  const double zero_ratio =
      tri_n > 0 ? static_cast<double>(atlas.zero_area_uv_triangles) /
                      static_cast<double>(tri_n)
                : 0.0;
  Check(zero_ratio < 0.01, "零面积 UV 三角形占比应 < 1%（极点固有）");
}

// ---------------------------------------------------------------------------

void TestPlane() {
  std::printf("[TEST] 平面单 chart（拉伸应接近 1）\n");
  const TriangleMesh m = MakePlane(20);
  UvConfig cfg;
  cfg.atlas_size = 2048;
  cfg.padding = 4;
  cfg.gutter = 8;
  const UvAtlas atlas = recon::UnwrapMesh(m, cfg);
  ValidateAtlas(atlas, "平面展开应成功");
  if (!atlas.ok) return;

  std::printf("    chart %d，接缝新增顶点 %d，利用率 %.1f%%，"
              "拉伸 均值 %.3f / 最大 %.3f\n",
              atlas.chart_count, atlas.seam_vertex_count,
              atlas.used_area_ratio * 100.0, atlas.mean_stretch,
              atlas.max_stretch);

  // 平面是平面投影的理想情况：应切成 1 个 chart，拉伸处处为 1。
  Check(atlas.chart_count == 1, "平面应只切成 1 个 chart");
  Check(atlas.mean_stretch > 0.95 && atlas.mean_stretch < 1.05,
        "平面展开的平均面积拉伸应接近 1");
  Check(atlas.max_stretch < 1.5, "平面展开的最大拉伸应 < 1.5");
  // 单 chart 无接缝 → 顶点数不增加。
  Check(atlas.uvs.size() == m.positions.size(), "单 chart 不应分裂顶点");
}

void TestCube() {
  std::printf("[TEST] 立方体（六面应切成 6 个 chart）\n");
  const TriangleMesh m = MakeCube(0.5);
  UvConfig cfg;
  cfg.atlas_size = 1024;
  cfg.join_angle_deg = 25.0;
  const UvAtlas atlas = recon::UnwrapMesh(m, cfg);
  ValidateAtlas(atlas, "立方体展开应成功");
  if (!atlas.ok) return;

  std::printf("    chart %d，接缝新增顶点 %d，利用率 %.1f%%，"
              "拉伸 均值 %.3f / 最大 %.3f\n",
              atlas.chart_count, atlas.seam_vertex_count,
              atlas.used_area_ratio * 100.0, atlas.mean_stretch,
              atlas.max_stretch);

  // 六个面的法向互差 90°，远超 25° 阈值 → 必须切成 6 个 chart。
  Check(atlas.chart_count == 6, "立方体应切成恰好 6 个 chart");
  Check(atlas.zero_area_uv_triangles == 0, "立方体不应有零面积 UV");
  // 每个面都要分裂出独立的 4 个角点 → 6×4 = 24 个 UV 顶点。
  Check(atlas.uvs.size() == 24, "立方体应展开成 24 个 UV 顶点");
}

void TestSphere() {
  std::printf("[TEST] 经纬球面（法向连续变化）\n");
  const TriangleMesh m = MakeUvSphere(0.05, 24, 48);
  std::printf("    输入: 顶点 %zu，三角形 %zu\n", m.positions.size(),
              m.triangle_count());
  UvConfig cfg;
  cfg.atlas_size = 4096;
  cfg.join_angle_deg = 25.0;
  const UvAtlas atlas = recon::UnwrapMesh(m, cfg);
  ValidateAtlas(atlas, "球面展开应成功");
  if (!atlas.ok) return;

  std::printf("    chart %d，接缝新增顶点 %d，利用率 %.1f%%，"
              "拉伸 均值 %.3f / 最大 %.3f\n",
              atlas.chart_count, atlas.seam_vertex_count,
              atlas.used_area_ratio * 100.0, atlas.mean_stretch,
              atlas.max_stretch);

  // 球面法向覆盖全空间，必然切成多个 chart。
  Check(atlas.chart_count >= 4, "球面应切成多个 chart");
  Check(atlas.chart_count <= 200, "chart 数不应失控");
  // 面积守恒：总 UV 面积应 ≈ 1（各 chart 按 3D 面积分配）。
  Check(atlas.mean_stretch > 0.7 && atlas.mean_stretch < 1.4,
        "球面的平均面积拉伸应在 [0.7, 1.4]");
  // 图集利用率：shelf 打包通常 60~85%，低于 30% 说明打包算法有问题。
  Check(atlas.used_area_ratio > 0.30, "图集利用率应 > 30%");
}

void TestEmpty() {
  std::printf("[TEST] 空输入\n");
  TriangleMesh empty;
  UvConfig cfg;
  const UvAtlas atlas = recon::UnwrapMesh(empty, cfg);
  Check(!atlas.ok, "空网格应返回失败");
}

}  // namespace

int main() {
  std::printf("=== recon UV 展开测试 ===\n");
  TestPlane();
  TestCube();
  TestSphere();
  TestEmpty();

  if (g_failures == 0) {
    std::printf("\n全部通过\n");
    return 0;
  }
  std::printf("\n失败 %d 项\n", g_failures);
  return 1;
}
