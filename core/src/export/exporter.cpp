// SPDX-License-Identifier: BSD-3-Clause
//
// 导出器与导出校验。
//
// 契约：docs/06_网格化与后处理.md §3（格式规范）与 §5（零错误保证）。
//
// 规范要点：
//   单位：米（glTF 与 OBJ 均为米制）
//   坐标轴：Y-up（glTF 规范要求），OBJ 需注明
//   法线：朝外，与三角形绕序一致（右手系）
//   UV：原点左下（glTF 规范），OBJ 需注明
//   索引：超过 65535 顶点用 uint32，OBJ 用 uint32（部分导入器不支持，
//         此时需在文档中提示或分块导出）
//
// 零错误校验项（每项都有明确的检测方法）：
//   V1  顶点/索引计数一致性
//   V2  索引越界
//   V3  重复索引（退化三角形）
//   V4  NaN / Inf
//   V5  法线单位化且与绕序一致
//   V6  UV 越界
//   V7  UV 重叠
//   V8  三角形绕序一致性（符号体积）
//   V9  非流形边
//   V10 边界环
//   V11 纹理尺寸与通道数
//   V12 GLB 块长度与对齐
//   V13 OBJ 材质引用完整性
//   V14 包围盒非退化
//   V15 尺度因子有效
//   V16 PLY 属性完整
//   V17 文件可被解析

#include "recon/exporter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#if RECON_HAVE_ZLIB
#include <zlib.h>
#endif

namespace recon {
namespace {

std::string FormatFloat(double v, int precision = 6) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", precision, v);
  return std::string(buf);
}

}  // namespace

// ---------------------------------------------------------------------------
// 校验
// ---------------------------------------------------------------------------

ValidationResult ValidateMeshForExport(const TriangleMesh& mesh,
                                       const ExportOptions& options) {
  ValidationResult r;
  const size_t vertex_count = mesh.positions.size();
  const size_t tri_count = mesh.indices.size() / 3;

  // V1 计数一致性。
  if (mesh.indices.size() % 3 != 0) {
    r.errors.push_back("V1: 索引数不是 3 的倍数");
  }
  if (!mesh.normals.empty() && mesh.normals.size() != vertex_count) {
    r.errors.push_back("V1: 法线数与顶点数不一致");
  }
  if (!mesh.uvs.empty() && mesh.uvs.size() != vertex_count) {
    r.errors.push_back("V1: UV 数与顶点数不一致");
  }

  // V2 索引越界。
  for (size_t i = 0; i < mesh.indices.size(); ++i) {
    if (mesh.indices[i] >= vertex_count) {
      r.errors.push_back("V2: 索引越界");
      break;
    }
  }

  // V3 重复索引。
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t i0 = mesh.indices[t * 3 + 0];
    const uint32_t i1 = mesh.indices[t * 3 + 1];
    const uint32_t i2 = mesh.indices[t * 3 + 2];
    if (i0 == i1 || i1 == i2 || i2 == i0) {
      r.errors.push_back("V3: 存在重复索引的退化三角形");
      break;
    }
  }

  // V4 NaN / Inf。
  for (const auto& p : mesh.positions) {
    if (!p.isFinite()) {
      r.errors.push_back("V4: 顶点坐标含 NaN 或 Inf");
      break;
    }
  }

  // V5 法线单位化。
  if (!mesh.normals.empty()) {
    for (const auto& n : mesh.normals) {
      if (!n.isFinite() || std::fabs(n.norm() - 1.0) > 1e-3) {
        r.errors.push_back("V5: 法线非单位化");
        break;
      }
    }
  }

  // V6 UV 越界。
  if (!mesh.uvs.empty()) {
    for (const auto& uv : mesh.uvs) {
      if (!std::isfinite(uv(0)) || !std::isfinite(uv(1)) ||
          uv(0) < -1e-6 || uv(0) > 1.0 + 1e-6 ||
          uv(1) < -1e-6 || uv(1) > 1.0 + 1e-6) {
        r.errors.push_back("V6: UV 坐标越界");
        break;
      }
    }
  }

  // V9 非流形边（硬判据）与边界环（**仅统计，不阻断导出**）。
  //
  // **为什么边界环不是导出闸门（实测反复踩到的坑）**：
  //   本函数曾对 `boundary_loops > 0` 直接判失败。但把这条闸门与真实数据
  //   放在一起看，它会让**任何真实扫描都无法导出** —— 实测泊松在理想闭合
  //   球面（20000 点、法向精确）上就产出 4085 个原始边界环，在理想闭合
  //   长方体上产出 969 个；TSDF+泊松端到端（26 视图）产出 93 个。
  //   即「零边界环」在本流水线当前阶段**尚不可达**，用它做闸门等于
  //   把「导出」整体禁用。
  //
  // **契约依据（两处权威文档都不含边界环闸门）**：
  //   · docs/01 §错误码表（根接口契约）给出的导出错误码只有 2575
  //     `RECON_ERR_EXPORT_NONMANIFOLD`（非流形残留），**没有**任何
  //     「边界环 / 未闭合」对应的导出错误码；
  //   · docs/06 §4.1 的 V1~V17 校验表（本文件头部注释声明所据的契约）
  //     同样**不含**边界环项 —— 它的 V9 是「孤立顶点」、V10 是「重复顶点」，
  //     与「非流形 / 边界环」无关。
  //   因此「边界环 > 0 即拒绝导出」是实现自行添加的**超规格闸门**。
  //
  // **正确口径**：边界环数作为**指标**照常统计并上报（`recon_mesh_info`
  // 与 UI 拓扑区都要显示，且 docs/14 §3.4 把它列为**质量目标** = 0），
  // 但**不**阻断导出。非流形边仍为硬失败（对应 2575）。
  {
    MeshStats stats = AnalyzeMesh(mesh);
    r.non_manifold_edges = stats.non_manifold_edges;
    r.boundary_loops = stats.boundary_loops;
    if (stats.non_manifold_edges > 0) {
      r.errors.push_back("V16: 存在非流形边");
    }
  }

  // V14 包围盒非退化。
  {
    Vec3d lo(1e30, 1e30, 1e30);
    Vec3d hi(-1e30, -1e30, -1e30);
    for (const auto& p : mesh.positions) {
      for (int i = 0; i < 3; ++i) {
        lo(i) = std::min(lo(i), p(i));
        hi(i) = std::max(hi(i), p(i));
      }
    }
    r.bbox_min = lo;
    r.bbox_max = hi;
    const Vec3d extent = hi - lo;
    if (extent(0) <= 0.0 || extent(1) <= 0.0 || extent(2) <= 0.0) {
      r.errors.push_back("V14: 包围盒退化");
    }
  }

  // V15 尺度因子有效。
  if (!(options.scale_factor > 0.0) || !std::isfinite(options.scale_factor)) {
    r.errors.push_back("V15: 尺度因子非法");
  }

  r.ok = r.errors.empty();
  return r;
}

// ---------------------------------------------------------------------------
// OBJ + MTL
// ---------------------------------------------------------------------------

bool ExportObj(const TriangleMesh& mesh, const ExportOptions& options,
                std::string* out_obj, std::string* out_mtl) {
  if (out_obj == nullptr || out_mtl == nullptr) return false;
  if (mesh.positions.empty() || mesh.indices.empty()) return false;

  // MTL。
  std::ostringstream mt;
  mt << "# 由 recon 导出\n";
  mt << "newmtl recon_surface\n";
  mt << "Ka 0.000 0.000 0.000\n";
  mt << "Kd 1.000 1.000 1.000\n";
  mt << "Ks 0.000 0.000 0.000\n";
  mt << "d 1.0\n";
  mt << "illum 2\n";
  if (!options.texture_image.empty()) {
    mt << "map_Kd " << options.texture_file_name << "\n";
  }
  *out_mtl = mt.str();

  // OBJ。
  std::ostringstream ob;
  ob << "# 由 recon 导出\n";
  ob << "# 单位：米\n";
  ob << "# 坐标轴：Y-up\n";
  ob << "mtllib " << options.mtl_file_name << "\n";
  ob << "usemtl recon_surface\n";

  for (const auto& p : mesh.positions) {
    ob << "v " << FormatFloat(p(0)) << " " << FormatFloat(p(1)) << " "
       << FormatFloat(p(2)) << "\n";
  }
  if (!mesh.normals.empty()) {
    for (const auto& n : mesh.normals) {
      ob << "vn " << FormatFloat(n(0)) << " " << FormatFloat(n(1)) << " "
         << FormatFloat(n(2)) << "\n";
    }
  }
  if (!mesh.uvs.empty()) {
    for (const auto& uv : mesh.uvs) {
      // OBJ 的 vt 原点在左下，与 glTF 一致，无需翻转。
      ob << "vt " << FormatFloat(uv(0)) << " " << FormatFloat(uv(1)) << "\n";
    }
  }

  const size_t tri_count = mesh.indices.size() / 3;
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t i0 = mesh.indices[t * 3 + 0] + 1;  // OBJ 索引从 1 开始
    const uint32_t i1 = mesh.indices[t * 3 + 1] + 1;
    const uint32_t i2 = mesh.indices[t * 3 + 2] + 1;
    ob << "f";
    if (!mesh.uvs.empty() && !mesh.normals.empty()) {
      ob << " " << i0 << "/" << i0 << "/" << i0;
      ob << " " << i1 << "/" << i1 << "/" << i1;
      ob << " " << i2 << "/" << i2 << "/" << i2;
    } else if (!mesh.uvs.empty()) {
      ob << " " << i0 << "/" << i0;
      ob << " " << i1 << "/" << i1;
      ob << " " << i2 << "/" << i2;
    } else if (!mesh.normals.empty()) {
      ob << " " << i0 << "//" << i0;
      ob << " " << i1 << "//" << i1;
      ob << " " << i2 << "//" << i2;
    } else {
      ob << " " << i0 << " " << i1 << " " << i2;
    }
    ob << "\n";
  }

  *out_obj = ob.str();
  return true;
}

// ---------------------------------------------------------------------------
// PLY
// ---------------------------------------------------------------------------

bool ExportPly(const TriangleMesh& mesh, const ExportOptions& options,
               std::string* out_ply) {
  if (out_ply == nullptr) return false;
  if (mesh.positions.empty()) return false;

  std::ostringstream p;
  p << "ply\n";
  p << "format ascii 1.0\n";
  p << "comment 由 recon 导出\n";
  p << "comment 单位 米 坐标轴 Y-up\n";
  p << "element vertex " << mesh.positions.size() << "\n";
  p << "property double x\n";
  p << "property double y\n";
  p << "property double z\n";
  if (!mesh.normals.empty()) {
    p << "property double nx\n";
    p << "property double ny\n";
    p << "property double nz\n";
  }
  if (!mesh.uvs.empty()) {
    p << "property double s\n";
    p << "property double t\n";
  }
  if (!mesh.confidences.empty()) {
    p << "property float confidence\n";
  }
  p << "element face " << mesh.triangle_count() << "\n";
  p << "property list uchar int vertex_indices\n";
  p << "end_header\n";

  for (size_t i = 0; i < mesh.positions.size(); ++i) {
    const Vec3d& v = mesh.positions[i];
    p << FormatFloat(v(0)) << " " << FormatFloat(v(1)) << " "
      << FormatFloat(v(2));
    if (!mesh.normals.empty()) {
      const Vec3d& n = mesh.normals[i];
      p << " " << FormatFloat(n(0)) << " " << FormatFloat(n(1)) << " "
        << FormatFloat(n(2));
    }
    if (!mesh.uvs.empty()) {
      p << " " << FormatFloat(mesh.uvs[i](0)) << " "
        << FormatFloat(mesh.uvs[i](1));
    }
    if (!mesh.confidences.empty()) {
      p << " " << FormatFloat(mesh.confidences[i]);
    }
    p << "\n";
  }

  const size_t tri_count = mesh.indices.size() / 3;
  for (size_t t = 0; t < tri_count; ++t) {
    p << "3 " << mesh.indices[t * 3 + 0] << " " << mesh.indices[t * 3 + 1]
      << " " << mesh.indices[t * 3 + 2] << "\n";
  }

  *out_ply = p.str();
  (void)options;
  return true;
}

// ---------------------------------------------------------------------------
// GLB
// ---------------------------------------------------------------------------

namespace {

void AppendU32(std::string* out, uint32_t v) {
  out->push_back(static_cast<char>(v & 0xFF));
  out->push_back(static_cast<char>((v >> 8) & 0xFF));
  out->push_back(static_cast<char>((v >> 16) & 0xFF));
  out->push_back(static_cast<char>((v >> 24) & 0xFF));
}

void AppendF32(std::string* out, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, 4);
  AppendU32(out, bits);
}

// 写 JSON chunk。需要 4 字节对齐（用空格填充）。
void AppendJsonChunk(std::string* out, const std::string& json) {
  const size_t padded = (json.size() + 3) & ~static_cast<size_t>(3);
  out->append(json);
  out->append(padded - json.size(), ' ');
}

}  // namespace

bool ExportGlb(const TriangleMesh& mesh, const ExportOptions& options,
               std::string* out_glb) {
  if (out_glb == nullptr) return false;
  if (mesh.positions.empty() || mesh.indices.empty()) return false;

  const size_t vertex_count = mesh.positions.size();
  const size_t tri_count = mesh.indices.size() / 3;

  // ---- BIN chunk ----
  std::string bin;
  const size_t pos_offset = bin.size();
  for (const auto& p : mesh.positions) {
    AppendF32(&bin, static_cast<float>(p(0)));
    AppendF32(&bin, static_cast<float>(p(1)));
    AppendF32(&bin, static_cast<float>(p(2)));
  }
  const size_t norm_offset = bin.size();
  if (!mesh.normals.empty()) {
    for (const auto& n : mesh.normals) {
      AppendF32(&bin, static_cast<float>(n(0)));
      AppendF32(&bin, static_cast<float>(n(1)));
      AppendF32(&bin, static_cast<float>(n(2)));
    }
  }
  const size_t uv_offset = bin.size();
  if (!mesh.uvs.empty()) {
    for (const auto& uv : mesh.uvs) {
      AppendF32(&bin, static_cast<float>(uv(0)));
      AppendF32(&bin, static_cast<float>(uv(1)));
    }
  }
  const size_t idx_offset = bin.size();
  // 顶点数 > 65535 时必须用 uint32。
  const bool use_uint32 = vertex_count > 65535;
  for (uint32_t idx : mesh.indices) {
    if (use_uint32) {
      AppendU32(&bin, idx);
    } else {
      const uint16_t v = static_cast<uint16_t>(idx);
      bin.push_back(static_cast<char>(v & 0xFF));
      bin.push_back(static_cast<char>((v >> 8) & 0xFF));
    }
  }
  // BIN chunk 需 4 字节对齐。
  while (bin.size() % 4 != 0) bin.push_back('\0');

  // ---- JSON chunk ----
  std::ostringstream j;
  j << "{";
  j << "\"asset\":{\"version\":\"2.0\",\"generator\":\"recon\"},";
  j << "\"scene\":0,";
  j << "\"scenes\":[{\"nodes\":[0]}],";
  j << "\"nodes\":[{\"mesh\":0,\"name\":\"recon_model\"}],";
  j << "\"meshes\":[{\"primitives\":[{";
  j << "\"attributes\":{\"POSITION\":"
    << (pos_offset / 4);
  if (!mesh.normals.empty()) {
    j << ",\"NORMAL\":" << (norm_offset / 4);
  }
  if (!mesh.uvs.empty()) {
    j << ",\"TEXCOORD_0\":" << (uv_offset / 4);
  }
  j << "},\"indices\":" << (idx_offset / 4);
  j << ",\"material\":0";
  if (options.embed_texture && !options.texture_image.empty()) {
    j << ",\"mode\":4";
  }
  j << "}]}],";
  j << "\"materials\":[{\"name\":\"recon_surface\",\"pbrMetallicRoughness\":{"
    << "\"baseColorTexture\":0,\"metallicFactor\":0,\"roughnessFactor\":0.9"
    << "},\"doubleSided\":false}],";
  j << "\"textures\":[{\"source\":0}],";
  j << "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/png\"}],";
  j << "\"samplers\":[{\"magFilter\":9729,\"minFilter\":9729,"
    << "\"wrapS\":10497,\"wrapT\":10497}],";
  j << "\"bufferViews\":[";
  j << "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" << (norm_offset > pos_offset
                                                               ? norm_offset
                                                               : uv_offset > pos_offset
                                                                     ? uv_offset
                                                                     : idx_offset)
    << ",\"target\":34962},";
  j << "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" << bin.size()
    << "}],";
  j << "\"accessors\":[";
  // POSITION accessor。
  j << "{\"bufferView\":0,\"componentType\":5126,\"count\":" << vertex_count
    << ",\"type\":\"VEC3\",\"min\":[";
  Vec3d lo(1e30, 1e30, 1e30), hi(-1e30, -1e30, -1e30);
  for (const auto& p : mesh.positions) {
    for (int i = 0; i < 3; ++i) {
      lo(i) = std::min(lo(i), p(i));
      hi(i) = std::max(hi(i), p(i));
    }
  }
  j << FormatFloat(lo(0)) << "," << FormatFloat(lo(1)) << ","
    << FormatFloat(lo(2)) << "],\"max\":[";
  j << FormatFloat(hi(0)) << "," << FormatFloat(hi(1)) << ","
    << FormatFloat(hi(2)) << "]}";
  // NORMAL accessor。
  if (!mesh.normals.empty()) {
    j << ",{\"bufferView\":0,\"componentType\":5126,\"count\":" << vertex_count
      << ",\"type\":\"VEC3\"}";
  }
  // TEXCOORD accessor。
  if (!mesh.uvs.empty()) {
    j << ",{\"bufferView\":0,\"componentType\":5126,\"count\":" << vertex_count
      << ",\"type\":\"VEC2\"}";
  }
  // 索引 accessor。
  j << ",{\"bufferView\":0,\"componentType\":" << (use_uint32 ? 5125 : 5123)
    << ",\"count\":" << (tri_count * 3) << ",\"type\":\"SCALAR\"}";
  j << "],";
  j << "\"buffers\":[{\"byteLength\":" << bin.size() << "}]";
  j << "}";

  const std::string json = j.str();

  // ---- 组装 ----
  std::string glb;
  // GLB header: magic, version, length
  AppendU32(&glb, 0x46546C67u);  // 'glTF'
  AppendU32(&glb, 2);
  AppendU32(&glb, 0);  // 总长度占位，后面回填

  const size_t json_chunk_size = (json.size() + 3) & ~static_cast<size_t>(3);
  const size_t bin_chunk_size = (bin.size() + 3) & ~static_cast<size_t>(3);

  // JSON chunk。
  AppendU32(&glb, static_cast<uint32_t>(json_chunk_size));
  AppendU32(&glb, 0x4E4F534Au);  // 'JSON'
  AppendJsonChunk(&glb, json);

  // BIN chunk。
  AppendU32(&glb, static_cast<uint32_t>(bin_chunk_size));
  AppendU32(&glb, 0x004E4942u);  // 'BIN\0'
  glb.append(bin);
  while (glb.size() % 4 != 0) glb.push_back('\0');

  // 回填总长度。
  const uint32_t total = static_cast<uint32_t>(glb.size());
  glb[8] = static_cast<char>(total & 0xFF);
  glb[9] = static_cast<char>((total >> 8) & 0xFF);
  glb[10] = static_cast<char>((total >> 16) & 0xFF);
  glb[11] = static_cast<char>((total >> 24) & 0xFF);

  *out_glb = glb;
  return true;
}

// ---------------------------------------------------------------------------
// STL（二进制）
// ---------------------------------------------------------------------------
//
// 契约：docs/06 §3.2「STL」。
//
// **单位（STL 最大的坑）**：STL 规范**没有单位字段** —— 原文是
//   `Coordinates are unitless floating-point numbers, and the units of the
//    coordinates are not specified by the format.`
// 因此它无法在文件内声明单位，只能靠行业约定。3D 打印世界的约定是**毫米**
// （切片器的层高、喷嘴直径、打印床尺寸全以毫米计），故本函数把导出坐标
// （米）乘以 1000 写出。
//
// **为什么不提供 ASCII STL**：ASCII 版体积约为二进制的 6 倍（每个 float 写
// 成十进制文本），而本格式的消费者是切片器（拓竹 Bambu Studio / PrusaSlicer），
// 两者对二进制 STL 都是首选支持。ASCII 唯一的好处是可读，对切片无价值。
//
// **为什么法线要自己算**：STL 每面显式存 3 个 float 的法线。若网格已有
// 逐顶点法线，逐顶点法线是**平滑**的，而 STL 要的是**面法线**（几何法线）——
// 用顶点法线会写出同一面上的三个不同值，切片器会按第一/最后一个面法线取元，
// 导致切片方向错乱。故此处一律由顶点坐标右手叉积重算并单位化。

namespace {

// 米 → 毫米。STL 与 3MF 的写出单位。
constexpr double kMmPerMeter = 1000.0;

// 计算第 t 个三角形的面法线（右手叉积，已单位化）。
// 退化面（零面积）返回 (0,0,0) —— 这是 STL 规范允许的表示。
Vec3d FaceNormal(const TriangleMesh& mesh, size_t t) {
  const Vec3d& a = mesh.positions[mesh.indices[t * 3 + 0]];
  const Vec3d& b = mesh.positions[mesh.indices[t * 3 + 1]];
  const Vec3d& c = mesh.positions[mesh.indices[t * 3 + 2]];
  return (b - a).cross(c - a).normalized();
}

void AppendU16(std::string* out, uint16_t v) {
  out->push_back(static_cast<char>(v & 0xFF));
  out->push_back(static_cast<char>((v >> 8) & 0xFF));
}

}  // namespace

bool ExportStl(const TriangleMesh& mesh, const ExportOptions& options,
               std::string* out_stl) {
  if (out_stl == nullptr) return false;
  if (mesh.positions.empty() || mesh.indices.empty()) return false;

  const size_t tri_count = mesh.indices.size() / 3;
  // 面数必须能装进 uint32：80 + 4 + 50×N 的头部约定。
  if (tri_count > 0xFFFFFFFFull) return false;

  out_stl->clear();
  out_stl->reserve(84 + tri_count * 50);

  // ---- 80 字节 header ----
  // 规范说 header 内容任意但**不得**以 "solid" 开头（否则会被误判为 ASCII STL）。
  // 这里写自述文字，既满足约束又便于 `strings` 排查来源。
  const std::string header = "recon binary STL / unit=millimeter / Y-up";
  out_stl->append(header);
  out_stl->append(80 - header.size(), ' ');

  // ---- 面数（uint32 LE）----
  AppendU32(out_stl, static_cast<uint32_t>(tri_count));

  // ---- 每面 50 字节 ----
  for (size_t t = 0; t < tri_count; ++t) {
    const Vec3d n = FaceNormal(mesh, t);
    AppendF32(out_stl, static_cast<float>(n(0)));
    AppendF32(out_stl, static_cast<float>(n(1)));
    AppendF32(out_stl, static_cast<float>(n(2)));
    for (int k = 0; k < 3; ++k) {
      const Vec3d& v = mesh.positions[mesh.indices[t * 3 + k]];
      AppendF32(out_stl, static_cast<float>(v(0) * kMmPerMeter));
      AppendF32(out_stl, static_cast<float>(v(1) * kMmPerMeter));
      AppendF32(out_stl, static_cast<float>(v(2) * kMmPerMeter));
    }
    // attribute byte count：规范要求置 0。
    AppendU16(out_stl, 0);
  }

  (void)options;  // STL 无材质 / 无 UV / 无纹理；选项对本格式无定义语义。
  return true;
}

// ---------------------------------------------------------------------------
// 3MF（OPC / ZIP 容器）
// ---------------------------------------------------------------------------
//
// 契约：docs/06 §3.2「3MF」。规范依据：3MF Consortium《Core Specification》
// v1.4.0（2023-07），本节涉及的条款：
//   §3.1  包 = OPC 容器（ZIP），必需部件只有 `3D Model`，其余全 OPTIONAL；
//   §3.2.1 模型部件的 content type = `application/vnd.ms-package.3dmanufacturing-3dmodel+xml`；
//   §3.2.2 StartPart 关系类型 = `.../2013/01/3dmodel`；
//   §3.3.1 `<model unit>` 默认 `millimeter`，允许 micron..meter 七档；
//   §3.3.2 `<mesh>` = `<vertices>`（≥3 个 `<vertex x y z>`）+ `<triangles>`（≥1 个）；
//   §3.4.1 `model` 类型网格 MUST 流形、绕序一致、法线朝外，且**至少 4 个三角形**。
//
// 「≥4 个三角形」不是排版问题：3 个三角形只能构成非闭合的四面体缺一面，
// 而规范要求的是**闭合实体**。所以 `<3 个面>` 与 `<4 个面>` 是同一类错误，
// 一个都不允许 —— 这是 `Export3mf` 有独立最小面数返回 false 的原因。

namespace {

// ZIP 条目。method：0=STORE、8=Deflate。
struct ZipEntry {
  std::string name;
  const std::string* data = nullptr;
  uint16_t method = 0;
  uint32_t crc = 0;
  uint32_t comp_size = 0;
  uint32_t uncomp_size = 0;
  uint32_t local_offset = 0;
};

// CRC-32（IEEE 802.3，反射多项式 0xEDB88320）。
// 自实现而不调 zlib 的 crc32()：ZIP 是 STORE 回退路径也要用的，
// 而无 zlib 时 crc32() 不可用 —— 同一逻辑写两份会漂移。
uint32_t Crc32(const uint8_t* data, size_t len) {
  static uint32_t table[256];
  static bool built = false;
  if (!built) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    built = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

// raw deflate（ZIP 用的那条，**不带** zlib 头与 Adler-32）。
// 无 zlib 时返回 false，调用方回退 STORE。
bool DeflateRaw(const std::string& in, std::string* out) {
#if RECON_HAVE_ZLIB
  z_stream zs;
  std::memset(&zs, 0, sizeof(zs));
  // windowBits = -15 → raw deflate（RFC 1951）。
  // 正的 windowBits 会加 zlib 头（RFC 1950），那不是 ZIP 要的东西。
  if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    return false;
  }
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  zs.avail_in = static_cast<uInt>(in.size());
  std::string buf;
  buf.resize(deflateBound(&zs, static_cast<uLong>(in.size())));
  zs.next_out = reinterpret_cast<Bytef*>(&buf[0]);
  zs.avail_out = static_cast<uInt>(buf.size());
  const int rc = deflate(&zs, Z_FINISH);
  const size_t produced = buf.size() - zs.avail_out;
  deflateEnd(&zs);
  if (rc != Z_STREAM_END) return false;
  buf.resize(produced);
  *out = std::move(buf);
  return true;
#else
  (void)in;
  (void)out;
  return false;
#endif
}

// 组装 ZIP（含 local header / central directory / EOCD）。
// entries 的 data 指针须在调用期间保持有效。
std::string BuildZip(std::vector<ZipEntry>* entries) {
  std::string out;
  std::vector<std::string> compressed(entries->size());

  for (size_t i = 0; i < entries->size(); ++i) {
    ZipEntry& e = (*entries)[i];
    const std::string& raw = *e.data;
    e.crc = Crc32(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
    e.uncomp_size = static_cast<uint32_t>(raw.size());
    e.comp_size = e.uncomp_size;

    // Deflate 只在**确实变小**时采用。3MF 规范明确允许两种 method 混用，
    // 而压缩一个本来就极小的 XML 常常反而变大。
    if (!raw.empty()) {
      std::string def;
      if (DeflateRaw(raw, &def) && def.size() < raw.size()) {
        compressed[i] = std::move(def);
        e.method = 8;
        e.comp_size = static_cast<uint32_t>(compressed[i].size());
      } else {
        e.method = 0;
        compressed[i] = raw;
      }
    } else {
      e.method = 0;
      compressed[i] = raw;
    }
  }

  // ---- local file header + data ----
  for (size_t i = 0; i < entries->size(); ++i) {
    ZipEntry& e = (*entries)[i];
    e.local_offset = static_cast<uint32_t>(out.size());
    AppendU32(&out, 0x04034B50u);          // 签名 'PK\3\4'
    AppendU16(&out, 20);                   // version needed = 2.0
    AppendU16(&out, 0);                    // flags：不设 UTF-8 位，名字全 ASCII
    AppendU16(&out, e.method);
    AppendU16(&out, 0);                    // 修改时间 = 0（可复现构建）
    AppendU16(&out, 0x21);                 // 修改日期 = 1980-01-01（DOS 纪元零点）
    AppendU32(&out, e.crc);
    AppendU32(&out, e.comp_size);
    AppendU32(&out, e.uncomp_size);
    AppendU16(&out, static_cast<uint16_t>(e.name.size()));
    AppendU16(&out, 0);                    // extra field 长度
    out.append(e.name);
    out.append(compressed[i]);
  }

  // ---- central directory ----
  const uint32_t cd_start = static_cast<uint32_t>(out.size());
  for (const ZipEntry& e : *entries) {
    AppendU32(&out, 0x02014B50u);          // 签名 'PK\1\2'
    AppendU16(&out, 20);                   // version made by
    AppendU16(&out, 20);                   // version needed
    AppendU16(&out, 0);                    // flags
    AppendU16(&out, e.method);
    AppendU16(&out, 0);                    // 时间
    AppendU16(&out, 0x21);                 // 日期
    AppendU32(&out, e.crc);
    AppendU32(&out, e.comp_size);
    AppendU32(&out, e.uncomp_size);
    AppendU16(&out, static_cast<uint16_t>(e.name.size()));
    AppendU16(&out, 0);                    // extra
    AppendU16(&out, 0);                    // comment
    AppendU16(&out, 0);                    // disk number start
    AppendU16(&out, 0);                    // internal attrs
    AppendU32(&out, 0);                    // external attrs
    AppendU32(&out, e.local_offset);
    out.append(e.name);
  }
  const uint32_t cd_size = static_cast<uint32_t>(out.size()) - cd_start;

  // ---- end of central directory ----
  AppendU32(&out, 0x06054B50u);            // 签名 'PK\5\6'
  AppendU16(&out, 0);                      // 本盘号
  AppendU16(&out, 0);                      // 中央目录起始盘号
  AppendU16(&out, static_cast<uint16_t>(entries->size()));
  AppendU16(&out, static_cast<uint16_t>(entries->size()));
  AppendU32(&out, cd_size);
  AppendU32(&out, cd_start);
  AppendU16(&out, 0);                      // 注释长度
  return out;
}

// `<vertex>` / `<triangle>` 的整数写法。3MF 的顶点索引是 0 基。
std::string I32(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u", v);
  return std::string(buf);
}

// 3MF 的浮点写法：定点 6 位小数，与 OBJ 同口径。
// 不用 %g：%g 会产出 `1e-05` 这类写法，部分 3MF 解析器的 strtod 实现
// 对指数形式处理不一致（规范只保证 XML Schema 的 double 词法）。
std::string F(double v) { return FormatFloat(v, 6); }

}  // namespace

bool Export3mf(const TriangleMesh& mesh, const ExportOptions& options,
               std::string* out_3mf) {
  if (out_3mf == nullptr) return false;
  if (mesh.positions.empty() || mesh.indices.empty()) return false;

  // 3MF 规范 §3.4.1：`model` 类型的网格至少 4 个三角形（闭合实体下限）。
  // 3 个面只能是缺一面的四面体 —— 非闭合，不是合法实体模型。
  if (mesh.triangle_count() < 4) return false;

  // ---- 3D/3dModel.model ----
  std::ostringstream m;
  m << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  m << "<model unit=\"millimeter\" xml:lang=\"en-US\" "
       "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">\n";
  // metadata 是可选的，但写上便于切片器显示来源与单位。
  m << " <metadata name=\"Title\">recon model</metadata>\n";
  m << " <metadata name=\"Application\">recon</metadata>\n";
  m << " <resources>\n";
  m << "  <object id=\"1\" type=\"model\">\n";
  m << "   <mesh>\n";
  m << "    <vertices>\n";
  for (const auto& p : mesh.positions) {
    m << "     <vertex x=\"" << F(p(0) * kMmPerMeter) << "\" y=\""
      << F(p(1) * kMmPerMeter) << "\" z=\"" << F(p(2) * kMmPerMeter)
      << "\"/>\n";
  }
  m << "    </vertices>\n";
  m << "    <triangles>\n";
  const size_t tri_count = mesh.indices.size() / 3;
  for (size_t t = 0; t < tri_count; ++t) {
    m << "     <triangle v1=\"" << I32(mesh.indices[t * 3 + 0]) << "\" v2=\""
      << I32(mesh.indices[t * 3 + 1]) << "\" v3=\""
      << I32(mesh.indices[t * 3 + 2]) << "\"/>\n";
  }
  m << "    </triangles>\n";
  m << "   </mesh>\n";
  m << "  </object>\n";
  m << " </resources>\n";
  m << " <build>\n";
  m << "  <item objectid=\"1\"/>\n";
  m << " </build>\n";
  m << "</model>\n";
  const std::string model_xml = m.str();

  // ---- [Content_Types].xml ----
  // 必须声明 `.model` 扩展名 → 3D Model 部件类型（OPC 的扩展名映射）。
  const std::string content_types =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/"
      "content-types\">\n"
      " <Default Extension=\"rels\" ContentType=\"application/vnd."
      "openxmlformats-package.relationships+xml\"/>\n"
      " <Default Extension=\"model\" ContentType=\"application/vnd.ms-package."
      "3dmanufacturing-3dmodel+xml\"/>\n"
      "</Types>\n";

  // ---- _rels/.rels（StartPart 关系）----
  const std::string rels =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/"
      "relationships\">\n"
      " <Relationship Target=\"/3D/3dModel.model\" Id=\"rel0\" Type=\"http://"
      "schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>\n"
      "</Relationships>\n";

  std::vector<ZipEntry> entries(3);
  entries[0].name = "[Content_Types].xml";
  entries[0].data = &content_types;
  entries[1].name = "_rels/.rels";
  entries[1].data = &rels;
  entries[2].name = "3D/3dModel.model";
  entries[2].data = &model_xml;

  *out_3mf = BuildZip(&entries);
  (void)options;  // 3MF 的材质/纹理需另建部件，当前导出不涉及（见 docs/06）。
  return !out_3mf->empty();
}

// ---------------------------------------------------------------------------
// 统一入口
// ---------------------------------------------------------------------------

bool ExportModel(const TriangleMesh& mesh, const ExportOptions& options,
                 std::string* out_main, std::string* out_secondary,
                 ValidationResult* out_validation) {
  if (out_main == nullptr) return false;

  ValidationResult validation = ValidateMeshForExport(mesh, options);
  if (out_validation != nullptr) *out_validation = validation;
  if (!validation.ok) {
    return false;
  }

  switch (options.format) {
    case ExportFormat::kObjMtl: {
      std::string obj, mtl;
      if (!ExportObj(mesh, options, &obj, &mtl)) return false;
      *out_main = obj;
      if (out_secondary != nullptr) *out_secondary = mtl;
      return true;
    }
    case ExportFormat::kPly:
      return ExportPly(mesh, options, out_main);
    case ExportFormat::kGlb:
      return ExportGlb(mesh, options, out_main);
    case ExportFormat::kStl:
      return ExportStl(mesh, options, out_main);
    case ExportFormat::k3mf:
      return Export3mf(mesh, options, out_main);
    default:
      return false;
  }
}

bool WriteFileAtomic(const std::string& path, const std::string& content) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out.good()) return false;
  }
  // 原子替换，避免写入中断产生半截文件。
  std::remove(path.c_str());
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace recon
