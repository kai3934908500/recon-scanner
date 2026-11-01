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
//
// **序号即 C ABI 的 `recon_export_format` 取值**（docs/01 §2.2.7）。
// 三端（核心 / iOS / Android）必须逐一对应，不得靠 rawValue 撞运气。
// 新增格式只能**追加**，不得插入或重排 —— 重排会静默改掉已发布的契约。
enum class ExportFormat : uint8_t {
  kGlb = 0,
  kObjMtl = 1,
  kPly = 2,
  kStl = 3,   // 二进制 STL（无单位字段，业界约定毫米；见 docs/06 §3.2）
  k3mf = 4,   // 3MF（OPC/ZIP 容器，`unit="millimeter"`；见 docs/06 §3.2）
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
 * 导出 STL（二进制）。
 *
 * **单位口径（易错，务必看）**：STL **格式本身没有单位字段**（规范原文
 * `Coordinates are unitless floating-point numbers`），3D 打印业界的事实约定
 * 是**毫米**。因此本函数把导出坐标（米）**乘以 1000** 后写出。若下游按米
 * 读会得到小 1000 倍的模型 —— 这是 STL 的格式缺陷，无法在文件内声明，
 * 只能在文档与 UI 上提示（docs/06 §3.2）。
 *
 * 不写 ASCII STL：ASCII 体积约为二进制的 6 倍且无精度收益，而本格式的
 * 目标消费者是切片器（拓竹 Bambu Studio / PrusaSlicer），两者对二进制
 * STL 均为首选支持。`write_binary` 对 STL 无定义语义（docs/14 缺口 AK）。
 */
bool ExportStl(const TriangleMesh& mesh, const ExportOptions& options,
               std::string* out_stl);

/**
 * 导出 3MF（OPC 容器 = ZIP，核心部件 `3D/3dModel.model`）。
 *
 * 单位：`<model unit="millimeter">`，故导出坐标（米）**乘以 1000**。
 * 与 STL 不同，3MF **在文件内声明单位**，因此是"无歧义毫米"的那一个 ——
 * 需要尺寸可信的场合应优先选 3MF。
 *
 * 容器按 OPC 最小集构造：`[Content_Types].xml` + `_rels/.rels` +
 * `3D/3dModel.model`，全部 Deflate 压缩（zlib 不可用时回退 STORE，
 * 两种 method 对 ZIP 均合法）。
 */
bool Export3mf(const TriangleMesh& mesh, const ExportOptions& options,
               std::string* out_3mf);

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
