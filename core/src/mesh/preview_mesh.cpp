// SPDX-License-Identifier: BSD-3-Clause
//
// 预览网格构建实现。见 core/include/recon/preview_mesh.h 的契约说明。

#include "recon/preview_mesh.h"

#include <cstddef>

namespace recon {

PreviewBuildResult BuildPreviewMesh(const TriangleMesh& src,
                                    uint32_t max_triangles) {
  PreviewBuildResult out;
  const size_t src_tris = src.indices.size() / 3;
  out.mesh.source_triangle_count = static_cast<uint32_t>(src_tris);

  if (src_tris == 0 || src.positions.empty()) {
    out.mesh.decimation_step = 1;
    return out;  // 空结果，ok 保持 true（「没有几何」不是「数据损坏」）
  }

  // 0 = 使用默认上限。limit==0 时退化为「全保留」，而不是除零。
  const size_t limit = (max_triangles == 0)
                           ? static_cast<size_t>(kPreviewDefaultMaxTriangles)
                           : static_cast<size_t>(max_triangles);
  size_t step = (limit == 0) ? src_tris : (src_tris + limit - 1) / limit;
  if (step == 0) step = 1;
  out.mesh.decimation_step = static_cast<uint32_t>(step);

  // 索引重映射：原顶点索引 → 新索引；-1 表示尚未被引用。
  std::vector<int32_t> remap(src.positions.size(), -1);
  out.mesh.positions.reserve(src.positions.size() * 3);
  out.mesh.indices.reserve((src_tris / step + 1) * 3);

  for (size_t t = 0; t < src_tris; t += step) {
    const size_t base = t * 3;
    for (size_t k = 0; k < 3; ++k) {
      const uint32_t vi = src.indices[base + k];
      if (vi >= src.positions.size()) {
        // 索引越界 = 网格数据损坏。返回空结果并标记失败，由调用方转错误码。
        out.mesh.positions.clear();
        out.mesh.indices.clear();
        out.ok = false;
        return out;
      }
      int32_t& slot = remap[vi];
      if (slot < 0) {
        slot = static_cast<int32_t>(out.mesh.positions.size() / 3);
        const Vec3d& p = src.positions[vi];
        // float32 XYZ 交错：渲染侧精度足够，省一半带宽。
        out.mesh.positions.push_back(static_cast<float>(p(0)));
        out.mesh.positions.push_back(static_cast<float>(p(1)));
        out.mesh.positions.push_back(static_cast<float>(p(2)));
      }
      out.mesh.indices.push_back(static_cast<uint32_t>(slot));
    }
  }
  return out;
}

}  // namespace recon
