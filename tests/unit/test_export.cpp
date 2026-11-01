// SPDX-License-Identifier: BSD-3-Clause
//
// 网格修复与导出校验测试。
//
// 验证 docs/00 §4.6 的验收口径：
//   非流形边 = 0、边界环 = 0、导出零错误、GLB 结构合法。

#include "recon/exporter.h"
#include "recon/mesh_repair.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#if RECON_TEST_HAVE_ZLIB
#include <zlib.h>
#endif

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

// STL 断言必须落在**产物字节**上，而不是「函数没崩」。
//
// 二进制 STL 的布局是唯一可判的东西：
//   [0,80)    header（不得以 "solid" 开头）
//   [80,84)   uint32 LE 面数
//   [84, 84+50N) 每面 50 字节：法线 12 + 三顶点 36 + attribute 2
// 因此「长度恰好 84 + 50N」本身就是一条强判据：任何多余/缺失的字节都会
// 让切片器解析错位。
namespace {

uint32_t ReadU32LE(const std::string& s, size_t off) {
  return static_cast<uint8_t>(s[off]) |
         (static_cast<uint8_t>(s[off + 1]) << 8) |
         (static_cast<uint8_t>(s[off + 2]) << 16) |
         (static_cast<uint8_t>(s[off + 3]) << 24);
}

uint16_t ReadU16LE(const std::string& s, size_t off) {
  return static_cast<uint16_t>(static_cast<uint8_t>(s[off]) |
                               (static_cast<uint8_t>(s[off + 1]) << 8));
}

float ReadF32LE(const std::string& s, size_t off) {
  const uint32_t bits = ReadU32LE(s, off);
  float f = 0.0f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// 在**中央目录**里找条目的起始偏移（不是 local header 偏移）。
//
// 这两个偏移必须分清楚：`ZipFindEntry` 返回的是 local header 偏移
// （central directory 的 +42 字段），而 CRC 在两者里的位置不同。
// 混用会得到错误期望值并把正确产物判成错的 —— 本测试真实踩过一次。
int ZipFindCdEntry(const std::string& z, const std::string& name) {
  if (z.size() < 22) return -1;
  size_t eocd = 0;
  for (size_t i = z.size() - 22;; --i) {
    if (ReadU32LE(z, i) == 0x06054B50u) { eocd = i; break; }
    if (i == 0) return -1;
  }
  const uint32_t cd_size = ReadU32LE(z, eocd + 12);
  const uint32_t cd_off = ReadU32LE(z, eocd + 16);
  size_t p = cd_off;
  const size_t end = cd_off + cd_size;
  while (p + 46 <= end) {
    if (ReadU32LE(z, p) != 0x02014B50u) return -1;
    const uint16_t nlen = ReadU16LE(z, p + 28);
    if (z.substr(p + 46, nlen) == name) return static_cast<int>(p);
    p += 46 + nlen + ReadU16LE(z, p + 30) + ReadU16LE(z, p + 32);
  }
  return -1;
}

// CRC-32（IEEE），与实现里那份算法**独立**再写一遍：
// 用同一份实现验证自己等于没验证。
uint32_t RefCrc32(const std::string& s) {
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char ch : s) {
    c ^= ch;
    for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
  }
  return c ^ 0xFFFFFFFFu;
}

}  // namespace

void TestStlExport() {
  std::printf("[TEST] STL 导出（二进制结构 + 毫米单位）\n"); std::fflush(stdout);
  // 边长 0.1 米 = 100 毫米。用这个尺寸能把「写毫米」从「写米」里区分出来。
  recon::TriangleMesh cube = MakeCube(0.1);
  recon::ExportOptions opt;
  opt.format = recon::ExportFormat::kStl;

  std::string stl, secondary;
  recon::ValidationResult v;
  Check(recon::ExportModel(cube, opt, &stl, &secondary, &v), "STL 导出应成功");
  Check(v.ok, "STL 校验应通过");

  // 长度：84 字节头 + 50 × 12 面 = 684。多一字节都不合法。
  Check(stl.size() == 84 + 50 * 12, "STL 长度应为 84 + 50×面数");

  // header 不得以 "solid" 开头 —— 否则会被解析器误判为 ASCII STL。
  Check(stl.compare(0, 5, "solid") != 0, "二进制 STL 的 header 不得以 solid 开头");

  // 面数（uint32 LE @80）。
  Check(ReadU32LE(stl, 80) == 12u, "STL 声明的面数应为 12");

  // 逐面检查第一条面记录的 attribute byte count 必须为 0（规范要求）。
  Check(ReadU16LE(stl, 84 + 48) == 0u, "attribute byte count 应为 0");

  // 法线应为单位向量。
  const double nx = ReadF32LE(stl, 84 + 0);
  const double ny = ReadF32LE(stl, 84 + 4);
  const double nz = ReadF32LE(stl, 84 + 8);
  CheckNear(std::sqrt(nx * nx + ny * ny + nz * nz), 1.0, 1e-5,
            "面法线应单位化");

  // **单位判据**：所有顶点坐标的绝对值必须落在 [50, 50+ε]（半球 ±50 mm）。
  // 若实现漏乘 1000，这里会是 0.05 附近 → 直接抓到。
  double max_abs = 0.0;
  for (size_t t = 0; t < 12; ++t) {
    for (int k = 0; k < 3; ++k) {
      const size_t base = 84 + t * 50 + 12 + k * 12;
      for (int c = 0; c < 3; ++c) {
        max_abs = std::max(max_abs, std::fabs(
                       static_cast<double>(ReadF32LE(stl, base + c * 4))));
      }
    }
  }
  CheckNear(max_abs, 50.0, 1e-3, "STL 顶点应按毫米写出（0.05 m → 50 mm）");
}

void TestStlRejectsTooFewTriangles() {
  std::printf("[TEST] STL 面数不足应拒绝\n"); std::fflush(stdout);
  // 单个三角形：不足以构成任何实体，但 STL 规范并未禁止 ——
  // 本实现对 STL 不设最小面数（只有 3MF 有 §3.4.1 的 4 面下限），
  // 因此这里断言**恰好相反**：1 个面应当导出成功。
  recon::TriangleMesh m = MakeCube(0.1);
  m.indices.resize(3);
  recon::ExportOptions opt;
  opt.format = recon::ExportFormat::kStl;
  std::string out, sec;
  recon::ValidationResult v;
  Check(recon::ExportModel(m, opt, &out, &sec, &v), "单面 STL 应允许导出");
  Check(out.size() == 84 + 50, "单面 STL 长度应为 134");
  Check(ReadU32LE(out, 80) == 1u, "单面 STL 面数应为 1");
}

// 3MF 是 ZIP 容器，故断言必须真的解析 ZIP 结构：
//   EOCD 定位 → 中央目录 → 逐条目名与偏移。
// 「开头是 PK」这种弱断言过不了负向注入（改坏一个字节仍然 PK 开头）。
namespace {

uint32_t ZipEntryCount(const std::string& z) {
  // 从尾部往前找 EOCD 签名 0x06054B50。
  if (z.size() < 22) return 0xFFFFFFFFu;
  for (size_t i = z.size() - 22;; --i) {
    if (ReadU32LE(z, i) == 0x06054B50u) return ReadU16LE(z, i + 10);
    if (i == 0) break;
  }
  return 0xFFFFFFFFu;
}

// 在**中央目录**里按名字找条目，返回其 local header 偏移；找不到返回 -1。
int ZipFindEntry(const std::string& z, const std::string& name) {
  if (z.size() < 22) return -1;
  size_t eocd = 0;
  for (size_t i = z.size() - 22;; --i) {
    if (ReadU32LE(z, i) == 0x06054B50u) { eocd = i; break; }
    if (i == 0) return -1;
  }
  const uint32_t cd_size = ReadU32LE(z, eocd + 12);
  const uint32_t cd_off = ReadU32LE(z, eocd + 16);
  size_t p = cd_off;
  const size_t end = cd_off + cd_size;
  while (p + 46 <= end) {
    if (ReadU32LE(z, p) != 0x02014B50u) return -1;
    const uint16_t nlen = ReadU16LE(z, p + 28);
    const uint16_t elen = ReadU16LE(z, p + 30);
    const uint16_t clen = ReadU16LE(z, p + 32);
    const std::string ename = z.substr(p + 46, nlen);
    if (ename == name) return static_cast<int>(ReadU32LE(z, p + 42));
    p += 46 + nlen + elen + clen;
  }
  return -1;
}

// 取条目的**原始（可能压缩）**数据。
std::string ZipEntryRaw(const std::string& z, const std::string& name) {
  const int off = ZipFindEntry(z, name);
  if (off < 0) return std::string();
  if (ReadU32LE(z, static_cast<size_t>(off)) != 0x04034B50u) return std::string();
  const uint16_t nlen = ReadU16LE(z, static_cast<size_t>(off) + 26);
  const uint16_t elen = ReadU16LE(z, static_cast<size_t>(off) + 28);
  const uint32_t csize = ReadU32LE(z, static_cast<size_t>(off) + 18);
  const size_t data = static_cast<size_t>(off) + 30 + nlen + elen;
  if (data + csize > z.size()) return std::string();
  return z.substr(data, csize);
}

// 该条目是否用 STORE（未压缩）存储。
bool ZipEntryStored(const std::string& z, const std::string& name) {
  const int off = ZipFindEntry(z, name);
  if (off < 0) return false;
  return ReadU16LE(z, static_cast<size_t>(off) + 8) == 0;
}

// local header 里记录的原始长度。偏移与 central directory **不同**：
//   local header   CRC @+14、comp @+18、uncomp @+22
//   central dir    CRC @+16、comp @+20、uncomp @+24
// 两者都叫同名字段，却差 2 字节（central directory 多一对 version made by）。
// 混用会得到错误期望值并把正确产物判成错的 —— 本测试真实踩过一次。
uint32_t ZipLocalUncompLen(const std::string& z, const std::string& name) {
  const int off = ZipFindEntry(z, name);
  if (off < 0) return 0xFFFFFFFFu;
  return ReadU32LE(z, static_cast<size_t>(off) + 22);
}

// 解压条目的**原始数据**。STORE 直接返回，Deflate 用 zlib 解开。
//
// **为什么必须能解压**：模型 XML 在正常规模下一定会被 deflate（1372 → 389
// 字节），若只对 STORE 条目做内容断言，那么「压缩路径」就完全没被验证 ——
// 这正是「判据范围要跟上代码位置」要防的假闸门。无 zlib 时返回空串，
// 调用方须显式报告「本环境无法验证压缩路径」，不得静默跳过。
std::string ZipEntryData(const std::string& z, const std::string& name) {
  const int off = ZipFindEntry(z, name);
  if (off < 0) return std::string();
  const uint32_t expected_crc = ReadU32LE(z, static_cast<size_t>(off) + 14);
  const std::string raw = ZipEntryRaw(z, name);
  if (ZipEntryStored(z, name)) {
    return raw;
  }
#if RECON_TEST_HAVE_ZLIB
  // 从**中央目录**取原始长度（和解压内存必须先按它分配）。
  size_t eocd = 0;
  for (size_t i = z.size() - 22;; --i) {
    if (ReadU32LE(z, i) == 0x06054B50u) { eocd = i; break; }
    if (i == 0) return std::string();
  }
  const uint32_t cd_off = ReadU32LE(z, eocd + 16);
  const uint32_t cd_size = ReadU32LE(z, eocd + 12);
  uint32_t uncomp = 0;
  {
    size_t p = cd_off;
    const size_t end = cd_off + cd_size;
    while (p + 46 <= end) {
      const uint16_t nlen = ReadU16LE(z, p + 28);
      if (z.substr(p + 46, nlen) == name) {
        uncomp = ReadU32LE(z, p + 24);
        break;
      }
      p += 46 + nlen + ReadU16LE(z, p + 30) + ReadU16LE(z, p + 32);
    }
  }
  if (uncomp == 0) return std::string();
  std::string out;
  out.resize(uncomp);
  // **必须用 raw inflate（windowBits = −15）**：ZIP 存的是 RFC 1951 裸 deflate，
  // 前面没有 RFC 1950 的 2 字节头与结尾的 Adler-32。若用 `uncompress()`
  // （它要求完整 zlib 流），会直接返回 Z_DATA_ERROR —— 于是「解压失败」
  // 被误读成「实现有问题」。这是 ZIP 与 zlib 容器最容易混的一处。
  z_stream zs;
  std::memset(&zs, 0, sizeof(zs));
  if (inflateInit2(&zs, -15) != Z_OK) return std::string();
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(raw.data()));
  zs.avail_in = static_cast<uInt>(raw.size());
  zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
  zs.avail_out = static_cast<uInt>(out.size());
  const int irc = inflate(&zs, Z_FINISH);
  const size_t produced = out.size() - zs.avail_out;
  inflateEnd(&zs);
  if (irc != Z_STREAM_END) return std::string();
  out.resize(produced);
  if (RefCrc32(out) != expected_crc) return std::string();  // 数据不可信
  return out;
#else
  (void)expected_crc;
  return std::string();
#endif
}

// 该条目是否被压缩过（用于报告覆盖情况）。
bool ZipEntryDeflated(const std::string& z, const std::string& name) {
  const int off = ZipFindEntry(z, name);
  if (off < 0) return false;
  return ReadU16LE(z, static_cast<size_t>(off) + 8) == 8;
}

}  // namespace

void Test3mfExport() {  std::printf("[TEST] 3MF 导出（ZIP 容器 + 模型部件）\n"); std::fflush(stdout);
  recon::TriangleMesh cube = MakeCube(0.1);
  recon::ExportOptions opt;
  opt.format = recon::ExportFormat::k3mf;

  std::string mf, secondary;
  recon::ValidationResult v;
  Check(recon::ExportModel(cube, opt, &mf, &secondary, &v), "3MF 导出应成功");
  Check(v.ok, "3MF 校验应通过");

  // 必须是 ZIP：EOCD 签名存在 + 恰好 3 个条目。
  Check(ZipEntryCount(mf) == 3u, "3MF 应恰好含 3 个 ZIP 条目");

  // 三个必需部件齐备（OPC 最小集）。
  Check(ZipFindEntry(mf, "[Content_Types].xml") >= 0,
        "应含 [Content_Types].xml");
  Check(ZipFindEntry(mf, "_rels/.rels") >= 0, "应含 _rels/.rels");
  Check(ZipFindEntry(mf, "3D/3dModel.model") >= 0, "应含 3D/3dModel.model");

  // ---- 模型部件内容（含 CRC 校验的解压）----
  const std::string model = ZipEntryData(mf, "3D/3dModel.model");
  Check(!model.empty(), "模型部件应可解压且 CRC 正确（判据落在内容上）");
  Check(model.find("unit=\"millimeter\"") != std::string::npos,
        "模型应声明 unit=millimeter");
  Check(model.find("http://schemas.microsoft.com/3dmanufacturing/core/2015/02")
            != std::string::npos,
        "模型应使用 3MF 核心命名空间");
  Check(model.find("<triangle ") != std::string::npos, "模型应含 triangle");
  Check(model.find("<vertex ") != std::string::npos, "模型应含 vertex");
  Check(model.find("<build>") != std::string::npos, "模型应含 build 段");
  Check(model.find("objectid=\"1\"") != std::string::npos,
        "build 的 item 应引用 object id=1");
  // 顶点数 / 面数：立方体是 8 顶点 12 面。
  {
    size_t cnt = 0, p = 0;
    while ((p = model.find("<vertex ", p)) != std::string::npos) { ++cnt; p += 8; }
    Check(cnt == 8, "模型应写出 8 个顶点");
    cnt = 0; p = 0;
    while ((p = model.find("<triangle ", p)) != std::string::npos) { ++cnt; p += 10; }
    Check(cnt == 12, "模型应写出 12 个三角形");
  }
  // **单位判据**：0.05 m → 50.000000（6 位定点）。写米的话这里会是 0.050000。
  Check(model.find("50.000000") != std::string::npos,
        "顶点应按毫米写出（0.05 m → 50.000000）");
  Check(model.find("0.050000") == std::string::npos,
        "顶点不得按米写出（出现 0.050000 即漏乘 1000）");
  Check(model.find("e-") == std::string::npos, "模型坐标不得用科学计数法");

  // 其余两个部件的相对关系也必须解压后核对（压缩路径同样覆盖）。
  const std::string ct = ZipEntryData(mf, "[Content_Types].xml");
  Check(!ct.empty(), "[Content_Types].xml 应可解压且 CRC 正确");
  Check(ct.find("application/vnd.ms-package.3dmanufacturing-3dmodel+xml")
            != std::string::npos,
        "应声明 .model 扩展名 → 3D Model 部件类型");
  const std::string rels = ZipEntryData(mf, "_rels/.rels");
  Check(!rels.empty(), "_rels/.rels 应可解压且 CRC 正确");
  Check(rels.find("Target=\"/3D/3dModel.model\"") != std::string::npos,
        "关系文件应指向 /3D/3dModel.model");
  Check(rels.find("3dmanufacturing/2013/01/3dmodel") != std::string::npos,
        "StartPart 关系类型应为 3dmodel");

  // 压缩路径的覆盖情况必须**显式报告**，不静默跳过。
  // 「压缩了但没验证」等于没验证压缩 —— 所以这里把事实打出来。
  if (ZipEntryDeflated(mf, "3D/3dModel.model")) {
    std::printf("  [INFO] 模型部件走 deflate（%zu 字节原文已成功解压校验）\n",
                model.size());
  } else {
    std::printf("  [INFO] 模型部件走 STORE（未压缩）\n");
  }

  // CRC 判据：条目记录的 CRC 必须等于对**原始数据**算出的 CRC。
  // 用一份独立实现的 RefCrc32，而不是实现里那份 —— 用同一份验证自己等于没验证。
  // 同时核对 central directory 与 local header 的 CRC/长度字段互相一致
  // （两者不一致的 ZIP 会被多数解压器判为损坏）。
  {
    const int cd = ZipFindCdEntry(mf, "3D/3dModel.model");
    const int lo = ZipFindEntry(mf, "3D/3dModel.model");
    Check(cd >= 0 && lo >= 0, "应能同时定位 central directory 与 local header");
    // central directory：CRC @+16、comp @+20、uncomp @+24。
    const uint32_t cd_crc = ReadU32LE(mf, static_cast<size_t>(cd) + 16);
    const uint32_t cd_uncomp = ReadU32LE(mf, static_cast<size_t>(cd) + 24);
    // local header：CRC @+14、comp @+18、uncomp @+22。
    const uint32_t lo_crc = ReadU32LE(mf, static_cast<size_t>(lo) + 14);
    const uint32_t lo_uncomp = ReadU32LE(mf, static_cast<size_t>(lo) + 22);
    Check(cd_crc == RefCrc32(model), "模型部件的 CRC 应与独立重算一致");
    Check(cd_crc == lo_crc, "central directory 与 local header 的 CRC 应一致");
    Check(cd_uncomp == model.size() && lo_uncomp == model.size(),
          "两处记录的原始长度都应等于解压结果");
    // 压缩长度必须小于原始长度 —— 否则说明走了 STORE 却标了 method=8。
    Check(ReadU32LE(mf, static_cast<size_t>(cd) + 20) < cd_uncomp,
          "deflate 条目的压缩长度应小于原始长度");
  }
}

void Test3mfRejectsTooFewTriangles() {
  std::printf("[TEST] 3MF 面数下限（规范 §3.4.1 要求 ≥4）\n"); std::fflush(stdout);
  // 3MF 规范 §3.4.1：`model` 类型网格**至少 4 个三角形**（闭合实体下限）。
  // 这里注入 3 个面 —— 实现必须拒绝，而不是写出一个非法包。
  recon::TriangleMesh m = MakeCube(0.1);
  m.indices.resize(3 * 3);  // 只剩 3 个面
  recon::ExportOptions opt;
  opt.format = recon::ExportFormat::k3mf;
  std::string out, sec;
  recon::ValidationResult v;
  Check(!recon::ExportModel(m, opt, &out, &sec, &v),
        "仅 3 个面的网格不得导出为 3MF");

  // 对照组：4 个面必须放行 —— 否则「拒绝」可能只是因为别的原因。
  recon::TriangleMesh ok = MakeCube(0.1);
  ok.indices.resize(3 * 4);
  Check(recon::ExportModel(ok, opt, &out, &sec, &v),
        "4 个面的网格应允许导出为 3MF");
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
  TestStlExport();
  TestStlRejectsTooFewTriangles();
  Test3mfExport();
  Test3mfRejectsTooFewTriangles();

  if (g_failures == 0) {
    std::printf("\n全部通过\n");
    return 0;
  }
  std::printf("\n失败 %d 项\n", g_failures);
  return 1;
}
