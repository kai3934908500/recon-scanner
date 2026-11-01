// SPDX-License-Identifier: BSD-3-Clause
//
// 导出接口。契约：docs/06_网格化与后处理.md §3 与 §5。

#ifndef RECON_CORE_EXPORTER_H_
#define RECON_CORE_EXPORTER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "recon/linalg.h"
#include "recon/mesh_repair.h"

namespace recon {

// 导出格式。
enum class ExportFormat : uint8_t {
  kGlb = 0,
  kObjMtl = 1,
  kPly = 2,
};

// 导出选项。
struct ExportOptions {
  ExportFormat format = ExportFormat::kGlb;
  double scale_factor = 1.0;      // 尺度因子（真实米 / 重建单位）
  bool apply_scale = true;
  bool write_binary = false;
  bool embed_texture = true;
  bool generate_normals = true;

  std::string texture_file_name = "recon_texture.png";
  std::string mtl_file_name = "recon.mtl";
  std::string texture_image;       // PNG 字节，embed_texture 时使用
};

// 校验结果。
struct ValidationResult {
  bool ok = true;
  std::vector<std::string> errors;
  uint32_t non_manifold_edges = 0;
  uint32_t boundary_loops = 0;
  Vec3d bbox_min = Vec3d::Zero();
  Vec3d bbox_max = Vec3d::Zero();
};

/**
 * 导出前校验。
 *
 * 校验口径以 **docs/06 §4.1 的 V1~V17 表**为准（本实现按该表逐项对应）：
 *   V1 计数一致、V2 索引越界、V3 重复索引、V4 NaN/Inf、V5 法线单位化、
 *   V6 UV 越界、**V16 非流形残留**、包围盒合理性、尺度因子。
 *
 * **注意**：V16（非流形边）是**硬闸门**；而**边界环数不是导出闸门**，
 * 仅作为指标填入 `boundary_loops` 供 UI / `recon_mesh_info` 展示
 * （docs/01 的导出错误码表与 docs/06 §4.1 均无「边界环」对应错误）。
 * 详见 `exporter.cpp` 中该段注释的实测依据。
 */
ValidationResult ValidateMeshForExport(const TriangleMesh& mesh,
                                       const ExportOptions& options);

// 导出 OBJ + MTL。out_obj / out_mtl 不得为 nullptr。
bool ExportObj(const TriangleMesh& mesh, const ExportOptions& options,
               std::string* out_obj, std::string* out_mtl);

// 导出 PLY（ASCII）。
bool ExportPly(const TriangleMesh& mesh, const ExportOptions& options,
               std::string* out_ply);

// 导出 GLB（二进制 glTF 2.0）。
bool ExportGlb(const TriangleMesh& mesh, const ExportOptions& options,
               std::string* out_glb);

/**
 * 统一导出入口。
 *
 * 先校验，校验失败直接返回 false，不产出文件。
 * OBJ 格式下 out_secondary 为 MTL 内容，其余格式为 nullptr。
 */
bool ExportModel(const TriangleMesh& mesh, const ExportOptions& options,
                 std::string* out_main, std::string* out_secondary,
                 ValidationResult* out_validation);

// 原子写文件：先写临时文件再重命名，避免半截文件。
bool WriteFileAtomic(const std::string& path, const std::string& content);

}  // namespace recon

#endif  // RECON_CORE_EXPORTER_H_
