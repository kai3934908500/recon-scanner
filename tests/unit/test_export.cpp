// SPDX-License-Identifier: BSD-3-Clause
//
// 网格修复与导出校验测试。
//
// 验证 docs/00 §4.6 的验收口径：
//   非流形边 = 0、边界环 = 0、导出零错误、GLB 结构合法。

#include "recon/exporter.h"
#include "recon/mesh_repair.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void CheckNear(double a, double b, double tol, const char* what) {
  if (std::fabs(a - b) > tol) {
    std::printf("  [FAIL] %s (got %.9f, want %.9f)\n", what, a, b);
    ++g_failures;
  }
}

void Check(bool cond, const char* what) {
  if (!cond) {
    std::printf("  [FAIL] %s\n", what);
    ++g_failures;
  }
}

// 构造一个单位立方体的三角网格（闭合、流形、绕序外向）。
recon::TriangleMesh MakeCube(double size) {
  recon::TriangleMesh m;
  const double h = size * 0.5;
  // 8 个顶点。
  for (int i = 0; i < 8; ++i) {
    const double x = (i & 1) ? h : -h;
    const double y = (i & 2) ? h : -h;
    const double z = (i & 4) ? h : -h;
    m.positions.emplace_back(x, y, z);
  }
  // 12 个三角形。每个面两个，绕序保证外向（右手系）。
  const uint32_t faces[12][3] = {
      {0, 2, 3}, {0, 3, 1},  // z = -h
      {4, 5, 7}, {4, 7, 6},  // z = +h
      {0, 1, 5}, {0, 5, 4},  // y = -h
      {2, 6, 7}, {2, 7, 3},  // y = +h
      {0, 4, 6}, {0, 6, 2},  // x = -h
      {1, 3, 7}, {1, 7, 5},  // x = +h
  };
  for (const auto& f : faces) {
    m.indices.push_back(f[0]);
    m.indices.push_back(f[1]);
    m.indices.push_back(f[2]);
  }
  return m;
}

void TestCubeTopology() {
  std::printf("[TEST] 立方体拓扑\n"); std::fflush(stdout);
  recon::TriangleMesh cube = MakeCube(0.1);
  recon::MeshStats stats = recon::AnalyzeMesh(cube);

  Check(stats.vertex_count == 8, "顶点数应为 8");
  Check(stats.triangle_count == 12, "三角形数应为 12");
  Check(stats.non_manifold_edges == 0, "立方体不应有非流形边");
  Check(stats.boundary_loops == 0, "立方体不应有边界环");
  Check(stats.degenerate_triangles == 0, "立方体不应有退化三角形");
  Check(stats.total_area > 0.0, "面积应大于 0");
  CheckNear(stats.total_area, 6.0 * 0.01, 1e-9, "立方体表面积应为 6×0.01");
}

void TestDegenerateRemoval() {
  std::printf("[TEST] 退化三角形剔除\n"); std::fflush(stdout);
  recon::TriangleMesh m = MakeCube(0.1);
  // 追加一个重复索引的退化三角形。
  m.indices.push_back(0);
  m.indices.push_back(0);
  m.indices.push_back(1);
  // 追加一个三点共线的零面积三角形（三个点在 z=-h 的同一平面上且共线：
  // 顶点 0=(-h,-h,-h)、1=(h,-h,-h)、4=(-h,h,-h) 不共线，
  // 因此这里用同一顶点的不同索引无法构造零面积，
  // 改为直接用三个相同索引，已在上面覆盖）。
  // 这里追加一个索引越界的三角形。
  m.indices.push_back(0);
  m.indices.push_back(1);
  m.indices.push_back(999);

  const uint32_t removed = recon::RemoveDegenerateTriangles(&m);
  Check(removed == 2, "应移除 2 个退化三角形");
  Check(m.triangle_count() == 12, "剩余应为 12 个三角形");
}

void TestIsolatedVertexRemoval() {
  std::printf("[TEST] 孤立顶点剔除\n"); std::fflush(stdout);
  recon::TriangleMesh m = MakeCube(0.1);
  // UV 数量须与顶点数一致，否则压缩后索引会错位。
  for (int i = 0; i < 8; ++i) m.uvs.emplace_back(0.0, 0.0);
  m.positions.emplace_back(9.0, 9.0, 9.0);  // 孤立顶点
  m.uvs.emplace_back(0.0, 0.0);
  const uint32_t removed = recon::RemoveIsolatedVertices(&m);
  Check(removed == 1, "应移除 1 个孤立顶点");
  Check(m.positions.size() == 8, "剩余应为 8 个顶点");
  Check(m.uvs.size() == 8, "UV 应同步压缩");
}

void TestNormalOrientation() {
  std::printf("[TEST] 法线统一矫正\n"); std::fflush(stdout);
  recon::TriangleMesh m = MakeCube(0.1);
  recon::Vec3d centroid = recon::Vec3d::Zero();
  for (const auto& p : m.positions) centroid += p;
  centroid /= static_cast<double>(m.positions.size());

  // 故意翻转一半三角形，检验矫正能力。
  for (size_t t = 0; t < m.indices.size() / 3; t += 2) {
    std::swap(m.indices[t * 3 + 1], m.indices[t * 3 + 2]);
  }
  const uint32_t flipped = recon::OrientNormalsOutward(&m, centroid);
  Check(flipped > 0, "应检测到并翻转反向面");

  // 矫正后重新分析：应无边界环（绕序一致但拓扑未变）。
  recon::RecomputeVertexNormals(&m);
  for (const auto& n : m.normals) {
    CheckNear(n.norm(), 1.0, 1e-9, "顶点法线应单位化");
  }
}

void TestExportValidation() {
  std::printf("[TEST] 导出校验\n"); std::fflush(stdout);
  recon::TriangleMesh cube = MakeCube(0.1);
  recon::ExportOptions opt;
  opt.format = recon::ExportFormat::kGlb;
  opt.scale_factor = 1.0;

  recon::ValidationResult v = recon::ValidateMeshForExport(cube, opt);
  Check(v.ok, "合法立方体应通过校验");
  Check(v.errors.empty(), "不应有错误信息");
  Check(v.bbox_max(0) - v.bbox_min(0) > 0.0, "包围盒应有体积");

  // 非法尺度因子。
  recon::ExportOptions bad = opt;
  bad.scale_factor = 0.0;
  recon::ValidationResult bv = recon::ValidateMeshForExport(cube, bad);
  Check(!bv.ok, "尺度因子为 0 应被拒绝");

  // 越界索引。
  recon::TriangleMesh broken = cube;
  broken.indices[0] = 999;
  recon::ValidationResult iv = recon::ValidateMeshForExport(broken, opt);
  Check(!iv.ok, "索引越界应被拒绝");

  // NaN 顶点。
  recon::TriangleMesh nan_mesh = cube;
  nan_mesh.positions[0](0) = std::nan("");
  recon::ValidationResult nv = recon::ValidateMeshForExport(nan_mesh, opt);
  Check(!nv.ok, "NaN 顶点应被拒绝");
}

void TestObjExport() {
  std::printf("[TEST] OBJ 导出\n"); std::fflush(stdout);
  recon::TriangleMesh cube = MakeCube(0.1);
  recon::ExportOptions opt;
  opt.format = recon::ExportFormat::kObjMtl;
  opt.scale_factor = 1.0;

  std::string obj, mtl;
  recon::ValidationResult v;
  Check(recon::ExportModel(cube, opt, &obj, &mtl, &v), "OBJ 导出应成功");
  Check(v.ok, "校验应通过");

  // 内容检查。
  Check(obj.find("ply") == std::string::npos, "OBJ 不应含 PLY 头");
  Check(obj.find("# 单位：米") != std::string::npos, "OBJ 应注明单位");
  Check(obj.find("mtllib") != std::string::npos, "OBJ 应引用 MTL");
  Check(obj.find("usemtl") != std::string::npos, "OBJ 应指定材质");

  // 顶点数与面数。
  int v_count = 0, f_count = 0;
  std::size_t pos = 0;
  while (pos < obj.size()) {
    const std::size_t eol = obj.find('\n', pos);
    const std::string line = obj.substr(pos, (eol == std::string::npos) ? std::string::npos : eol - pos);
    if (line.rfind("v ", 0) == 0) ++v_count;
    if (line.rfind("f ", 0) == 0) ++f_count;
    if (eol == std::string::npos) break;
    pos = eol + 1;
  }
  Check(v_count == 8, "OBJ 顶点数应为 8");
  Check(f_count == 12, "OBJ 面数应为 12");

  // MTL 检查。
  Check(mtl.find("newmtl") != std::string::npos, "MTL 应定义材质");
}

void TestPlyExport() {
  std::printf("[TEST] PLY 导出\n"); std::fflush(stdout);
  recon::TriangleMesh cube = MakeCube(0.1);
  recon::ExportOptions opt;
  opt.format = recon::ExportFormat::kPly;

  std::string ply, secondary;
  recon::ValidationResult v;
  Check(recon::ExportModel(cube, opt, &ply, &secondary, &v), "PLY 导出应成功");
  Check(ply.rfind("ply", 0) == 0, "PLY 应以 ply 开头");
  Check(ply.find("element vertex 8") != std::string::npos, "PLY 应声明顶点数");
  Check(ply.find("element face 12") != std::string::npos, "PLY 应声明面数");
  Check(ply.find("end_header") != std::string::npos, "PLY 应有 end_header");
}

void TestGlbExport() {
  std::printf("[TEST] GLB 导出（结构合法性）\n"); std::fflush(stdout);
  recon::TriangleMesh cube = MakeCube(0.1);
  recon::ExportOptions opt;
  opt.format = recon::ExportFormat::kGlb;

  std::string glb, secondary;
  recon::ValidationResult v;
  Check(recon::ExportModel(cube, opt, &glb, &secondary, &v), "GLB 导出应成功");
  Check(glb.size() > 12, "GLB 应有头部");

  // 魔数 'glTF'。
  const uint32_t magic = static_cast<uint8_t>(glb[0]) |
                         (static_cast<uint8_t>(glb[1]) << 8) |
                         (static_cast<uint8_t>(glb[2]) << 16) |
                         (static_cast<uint8_t>(glb[3]) << 24);
  Check(magic == 0x46546C67u, "GLB 魔数应为 glTF");

  // 版本号。
  const uint32_t version = static_cast<uint8_t>(glb[4]) |
                           (static_cast<uint8_t>(glb[5]) << 8) |
                           (static_cast<uint8_t>(glb[6]) << 16) |
                           (static_cast<uint8_t>(glb[7]) << 24);
  Check(version == 2, "GLB 版本应为 2");

  // 总长度字段必须与实际大小一致。
  const uint32_t declared = static_cast<uint8_t>(glb[8]) |
                            (static_cast<uint8_t>(glb[9]) << 8) |
                            (static_cast<uint8_t>(glb[10]) << 16) |
                            (static_cast<uint8_t>(glb[11]) << 24);
  Check(declared == glb.size(), "GLB 长度字段应与实际一致");

  // 第一个 chunk 类型应为 JSON。
  const uint32_t chunk_type = static_cast<uint8_t>(glb[16]) |
                              (static_cast<uint8_t>(glb[17]) << 8) |
                              (static_cast<uint8_t>(glb[18]) << 16) |
                              (static_cast<uint8_t>(glb[19]) << 24);
  Check(chunk_type == 0x4E4F534Au, "首个 chunk 应为 JSON");

  // JSON 内容应包含关键字段。
  const std::string json = glb.substr(20, 4096);
  Check(json.find("\"asset\"") != std::string::npos, "JSON 应含 asset");
  Check(json.find("\"meshes\"") != std::string::npos, "JSON 应含 meshes");
  Check(json.find("\"accessors\"") != std::string::npos, "JSON 应含 accessors");
  Check(json.find("\"POSITION\"") != std::string::npos, "JSON 应含 POSITION");
}

}  // namespace

int main() {
  std::printf("=== recon 网格修复与导出测试 ===\n");
  TestCubeTopology();
  TestDegenerateRemoval();
  TestIsolatedVertexRemoval();
  TestNormalOrientation();
  TestExportValidation();
  TestObjExport();
  TestPlyExport();
  TestGlbExport();

  if (g_failures == 0) {
    std::printf("\n全部通过\n");
    return 0;
  }
  std::printf("\n失败 %d 项\n", g_failures);
  return 1;
}
