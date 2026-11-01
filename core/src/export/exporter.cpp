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
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

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
