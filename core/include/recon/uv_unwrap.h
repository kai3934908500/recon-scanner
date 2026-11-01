// SPDX-License-Identifier: BSD-3-Clause
//
// S8 UV 展开。契约：docs/06_网格化与后处理.md §2.8。
//
// 本模块实现的是规格里的**回退路径**（`W_MESH_UV_FALLBACK_CUBE` 的同族方案）：
//   1. 按「法向相似 + 面连通」做**区域生长**，把网格切成若干 chart；
//   2. 每个 chart 用其平均法向做**平面正投影**；
//   3. 各 chart 归一化后做 **shelf 打包**进正方形图集，留 padding / gutter。
//
// xatlas（MIT）是规格里的**主路径**，它额外做 chart 优化与参数化
// （LSCM / ABF），接缝更少、拉伸更小。本回退路径的价值：
//   · 零外部依赖，可离线、可静态链接进移动端；
//   · 是规格明确要求的失败回退（xatlas 超时 / 抛异常时改用）；
//   · 对**块状 / 多面体**扫描物体，平面投影的拉伸本来就小。
//
// 对球形 / 细长物体的拉伸会偏大，此时应优先走 xatlas。

#ifndef RECON_CORE_UV_UNWRAP_H_
#define RECON_CORE_UV_UNWRAP_H_

#include <cstdint>
#include <vector>

#include "recon/linalg.h"

namespace recon {

// 网格（前向声明，实际定义见 mesh_repair.h）。
struct TriangleMesh;

// UV 展开配置。参数名与 docs/06 §2.8 的 `post_uv_*` 一一对应。
struct UvConfig {
  // 图集边长（像素）。规格：2048 / 4096 / 8192。
  int atlas_size = 4096;
  // chart 内边距（像素），防采样渗染。
  int padding = 4;
  // chart 之间的额外间隙（像素）。
  int gutter = 8;
  // chart 分割的法向阈值（度）。相邻面法向夹角超过此值即断开。
  // 规格 `post_uv_join_angle_deg`：快 30 / 标 25 / 高 20。
  double join_angle_deg = 25.0;
  // 说明：**不限制单个 chart 的面数**。UV 面积是按 3D 面积比例分配的，
  // 单个大 chart 不会「挤压」别的小 chart（它本来就该占更多 UV 面积）。
  // 早期版本按面数占比硬切，导致平板网格被切成 4 块（本应 1 块）、
  // 接缝与拉伸双双变差。
};

// UV 展开结果。
//
// **展开后的网格与输入网格的顶点数不同**：接缝处同一几何顶点在不同 chart
// 里需要不同的 UV，因此必须**分裂顶点**。`indices` 是展开后的索引，
// 与 `uvs` 一一对应；`source_vertex` 记录每个展开顶点来自原始顶点，
// 便于烘焙阶段回查几何与法向。
struct UvAtlas {
  bool ok = false;
  int atlas_size = 0;
  // 展开后的顶点 UV（范围 [0,1]）。
  std::vector<Vec2d> uvs;
  // 展开后的三角形索引（指向 uvs）。
  std::vector<uint32_t> indices;
  // 展开顶点 → 原始网格顶点。
  std::vector<uint32_t> source_vertex;

  int chart_count = 0;
  int seam_vertex_count = 0;   // 因接缝而新增的顶点数
  double used_area_ratio = 0.0;  // 图集利用率（各 chart 矩形面积和 / 1）
  double max_stretch = 1.0;    // 最大面积拉伸比（>1 表示局部被放大）
  double mean_stretch = 1.0;   // 平均面积拉伸比
  // 零面积 UV 三角形数（规格 R4：应在导出前剔除）。
  uint32_t zero_area_uv_triangles = 0;
};

// S8：UV 展开（回退路径）。
UvAtlas UnwrapMesh(const TriangleMesh& mesh, const UvConfig& config);

}  // namespace recon

#endif  // RECON_CORE_UV_UNWRAP_H_
