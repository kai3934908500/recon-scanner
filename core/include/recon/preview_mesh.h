// SPDX-License-Identifier: BSD-3-Clause
//
// 预览网格构建（UI 线框渲染用）。
//
// 契约：docs/09_UI规格.md §3.7 U-R-1..U-R-4。
//   · U-R-1 只做线框 —— 故只需位置与索引，不需要 UV / 法向 / 纹理；
//   · U-R-2 三角面 > 阈值时按**确定性**步长抽稀；
//   · U-R-4 预览数据**不得**由 UI 层自行读文件或算几何 —— 本模块即
//     「核心算、UI 只画」的那一半（另一半是 C ABI 的 recon_get_preview_mesh）。
//
// 抽稀逻辑独立成纯函数（不依赖 Context），使其可被单元测试直接覆盖；
// C ABI 层只负责取网格 + 分配缓冲 + 所有权移交。
#ifndef RECON_PREVIEW_MESH_H_
#define RECON_PREVIEW_MESH_H_

#include <cstdint>
#include <vector>

#include "recon/mesh_repair.h"

namespace recon {

/// 预览网格。位置为 float32 XYZ 交错，索引为 uint32（每 3 个一三角面）。
///
/// 坐标：直接取核心网格坐标（**不施加** `scale_factor`）。预览用于肉眼检查
/// **形状**，等比缩放不改变形状；刻意不复刻导出路径的尺度语义，以免新增
/// 第三种解释（docs/01 §4 缺口 AJ 登记）。
struct PreviewMesh {
  std::vector<float> positions;      // 3 × vertex_count
  std::vector<uint32_t> indices;     // 3 × triangle_count
  uint32_t source_triangle_count = 0;  // 抽稀前原始面数
  uint32_t decimation_step = 1;        // 抽稀步长 N（1 = 未抽稀）

  size_t vertex_count() const { return positions.size() / 3; }
  size_t triangle_count() const { return indices.size() / 3; }
};

/// 预览默认三角面上限。与 docs/09 U-R-2 的阈值同源。
constexpr uint32_t kPreviewDefaultMaxTriangles = 2000000;

/// 由网格构建预览。`max_triangles == 0` 时使用 kPreviewDefaultMaxTriangles。
///
/// 抽稀规则（确定性，无随机）：
///   step = ceil(源面数 / max_triangles)，至少 1；
///   保留面序号 t = 0, step, 2*step, ... 的三角面。
/// 只保留被**选中面**引用的顶点，并重映射索引 —— 预览缓冲尽可能小。
///
/// 返回：始终成功（空网格时返回空的 PreviewMesh，由调用方判定错误码）。
/// 索引越界视为数据损坏，此时返回空结果且 `ok() == false`。
struct PreviewBuildResult {
  PreviewMesh mesh;
  bool ok = true;
};

PreviewBuildResult BuildPreviewMesh(const TriangleMesh& src,
                                    uint32_t max_triangles);

}  // namespace recon

#endif  // RECON_PREVIEW_MESH_H_
