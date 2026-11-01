// SPDX-License-Identifier: BSD-3-Clause
//
// S8 UV 展开（回退路径）。契约：docs/06_网格化与后处理.md §2.8。

#include "recon/uv_unwrap.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <utility>

#include "recon/mesh_repair.h"

namespace recon {
namespace {

struct EdgeKey {
  uint32_t a;
  uint32_t b;
  bool operator<(const EdgeKey& o) const {
    if (a != o.a) return a < o.a;
    return b < o.b;
  }
  bool operator==(const EdgeKey& o) const { return a == o.a && b == o.b; }
};

EdgeKey MakeEdge(uint32_t i, uint32_t j) {
  return i < j ? EdgeKey{i, j} : EdgeKey{j, i};
}

// 一个 chart：一组面 + 投影平面 + 在 2D 中的包围盒。
struct Chart {
  std::vector<uint32_t> faces;
  Vec3d normal = Vec3d::Zero();
  Vec3d e1 = Vec3d::Zero();
  Vec3d e2 = Vec3d::Zero();
  // 2D 投影后的包围盒（未缩放）。
  double u0 = 0.0, v0 = 0.0, u1 = 0.0, v1 = 0.0;
  double area3d = 0.0;
  double area2d = 0.0;  // 平面投影后的**实际**面积（不是包围盒面积）
  // 打包到图集后的矩形（归一化 [0,1]）。
  double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
};

// 构造与 n 垂直的正交基（e1 × e2 = n）。
void BuildFrame(const Vec3d& n, Vec3d* e1, Vec3d* e2) {
  Vec3d nn = n;
  if (nn.norm() < 1e-12) nn = Vec3d(0.0, 0.0, 1.0);
  nn.normalize();
  const double ax = std::fabs(nn(0));
  const double ay = std::fabs(nn(1));
  const double az = std::fabs(nn(2));
  Vec3d helper;
  if (ax <= ay && ax <= az) {
    helper = Vec3d(1.0, 0.0, 0.0);
  } else if (ay <= az) {
    helper = Vec3d(0.0, 1.0, 0.0);
  } else {
    helper = Vec3d(0.0, 0.0, 1.0);
  }
  *e1 = helper.cross(nn);
  if ((*e1).norm() < 1e-12) *e1 = Vec3d(1.0, 0.0, 0.0);
  (*e1).normalize();
  *e2 = nn.cross(*e1);
}

// shelf 打包：把若干带尺寸的矩形装进正方形。
//
// 返回 false 表示装不下（调用方放大边长重试）。
bool ShelfPack(std::vector<Chart>* charts, double box) {
  // 按高度降序（经典 shelf 启发式）。
  std::vector<size_t> order(charts->size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return (*charts)[a].h > (*charts)[b].h;
  });

  double cursor_x = 0.0;
  double shelf_y = 0.0;
  double shelf_h = 0.0;
  for (size_t idx : order) {
    Chart& c = (*charts)[idx];
    if (c.w > box) return false;
    if (cursor_x + c.w > box) {  // 换行
      shelf_y += shelf_h;
      cursor_x = 0.0;
      shelf_h = 0.0;
    }
    if (shelf_y + c.h > box) return false;
    c.x = cursor_x;
    c.y = shelf_y;
    cursor_x += c.w;
    shelf_h = std::max(shelf_h, c.h);
  }
  return true;
}

}  // namespace

UvAtlas UnwrapMesh(const TriangleMesh& mesh, const UvConfig& config) {
  UvAtlas out;
  out.atlas_size = std::max(1, config.atlas_size);

  const size_t tri_count = mesh.indices.size() / 3;
  const size_t vertex_count = mesh.positions.size();
  if (tri_count == 0 || vertex_count == 0) return out;

  // ---- 1. 面法向与面积 ----
  std::vector<Vec3d> face_normal(tri_count, Vec3d(0.0, 0.0, 1.0));
  std::vector<double> face_area(tri_count, 0.0);
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t i0 = mesh.indices[t * 3 + 0];
    const uint32_t i1 = mesh.indices[t * 3 + 1];
    const uint32_t i2 = mesh.indices[t * 3 + 2];
    if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) continue;
    const Vec3d n = (mesh.positions[i1] - mesh.positions[i0])
                        .cross(mesh.positions[i2] - mesh.positions[i0]);
    const double len = n.norm();
    face_area[t] = 0.5 * len;
    if (len > 1e-20) face_normal[t] = n / len;
  }

  // ---- 2. 面邻接（仅通过恰有 2 个面的内部边） ----
  std::map<EdgeKey, std::vector<uint32_t>> edge_faces;
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t i0 = mesh.indices[t * 3 + 0];
    const uint32_t i1 = mesh.indices[t * 3 + 1];
    const uint32_t i2 = mesh.indices[t * 3 + 2];
    if (i0 == i1 || i1 == i2 || i2 == i0) continue;
    if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) continue;
    const uint32_t ft = static_cast<uint32_t>(t);
    edge_faces[MakeEdge(i0, i1)].push_back(ft);
    edge_faces[MakeEdge(i1, i2)].push_back(ft);
    edge_faces[MakeEdge(i2, i0)].push_back(ft);
  }
  std::vector<std::vector<uint32_t>> face_adj(tri_count);
  for (const auto& kv : edge_faces) {
    if (kv.second.size() != 2) continue;  // 边界边 / 非流形边不连
    face_adj[kv.second[0]].push_back(kv.second[1]);
    face_adj[kv.second[1]].push_back(kv.second[0]);
  }

  // ---- 3. 区域生长分割 chart ----
  //
  // **必须同时要求「法向相似」与「面连通」**：
  //   · 只按法向聚类 → 同一 chart 可能包含互不连通的两块 → 平面投影后
  //     两块重叠在 UV 上 → 纹理错乱；
  //   · 只按连通 → 一整块曲面投到同一平面 → 严重拉伸。
  //
  // 法向比较统一对**种子面**做（而非逐面相邻比较）→ chart 内所有面的法向
  // 都落在种子 ±join_angle 内 → **投影拉伸有界** ✓
  const double cos_thresh =
      std::cos(std::clamp(config.join_angle_deg, 1.0, 89.0) * kPi / 180.0);

  std::vector<int> chart_of(tri_count, -1);
  std::vector<Chart> charts;
  for (size_t seed = 0; seed < tri_count; ++seed) {
    if (chart_of[seed] >= 0) continue;
    if (face_area[seed] <= 0.0) continue;  // 退化面不参与

    const int cid = static_cast<int>(charts.size());
    Chart c;
    chart_of[seed] = cid;
    c.faces.push_back(static_cast<uint32_t>(seed));
    const Vec3d seed_n = face_normal[seed];

    for (size_t i = 0; i < c.faces.size(); ++i) {
      for (uint32_t nb : face_adj[c.faces[i]]) {
        if (chart_of[nb] >= 0) continue;
        if (face_area[nb] <= 0.0) continue;
        if (face_normal[nb].dot(seed_n) < cos_thresh) continue;
        chart_of[nb] = cid;
        c.faces.push_back(nb);
      }
    }
    charts.push_back(std::move(c));
  }
  if (charts.empty()) return out;
  out.chart_count = static_cast<int>(charts.size());

  // ---- 4. 逐 chart 平面投影 ----
  for (auto& c : charts) {
    Vec3d n = Vec3d::Zero();
    double total_area = 0.0;
    for (uint32_t f : c.faces) {
      n += face_normal[f] * face_area[f];
      total_area += face_area[f];
    }
    c.area3d = total_area;
    if (n.norm() < 1e-15) n = face_normal[c.faces[0]];
    c.normal = n.normalized();
    BuildFrame(c.normal, &c.e1, &c.e2);

    c.u0 = c.v0 = 1e30;
    c.u1 = c.v1 = -1e30;
    c.area2d = 0.0;
    for (uint32_t f : c.faces) {
      Vec2d uv[3];
      for (int k = 0; k < 3; ++k) {
        const Vec3d& p = mesh.positions[mesh.indices[f * 3 + k]];
        const double u = p.dot(c.e1);
        const double v = p.dot(c.e2);
        uv[k] = Vec2d(u, v);
        c.u0 = std::min(c.u0, u);
        c.u1 = std::max(c.u1, u);
        c.v0 = std::min(c.v0, v);
        c.v1 = std::max(c.v1, v);
      }
      // 2D 投影面积（未缩放）。
      c.area2d += 0.5 * std::fabs((uv[1](0) - uv[0](0)) * (uv[2](1) - uv[0](1)) -
                                  (uv[1](1) - uv[0](1)) * (uv[2](0) - uv[0](0)));
    }
  }

  // ---- 5. 定尺寸：按 3D 面积比例分配 UV 面积（保证 texel 密度均匀） ----
  //
  // 目标：chart 的 UV 面积 ∝ 它的 3D 面积。
  // 先按「2D 包围盒宽高比」定形状，再按 sqrt(目标面积 / 包围盒面积) 缩放。
  double total_3d = 0.0;
  for (const auto& c : charts) total_3d += c.area3d;
  if (total_3d <= 0.0) return out;

  for (auto& c : charts) {
    const double bw = std::max(1e-12, c.u1 - c.u0);
    const double bh = std::max(1e-12, c.v1 - c.v0);
    const double target_area = c.area3d / total_3d;  // 在单位图集里的占比
    // **按 2D 投影的「实际面积」定尺度，不是包围盒面积**。
    //
    // 不规则 chart 在包围盒里会留大片空白；若按包围盒缩放，实际 UV 面积
    // 只有目标面积的 60% 左右 → 纹理分辨率被白白浪费
    // （实测球面 mean_stretch = 0.595）。按实际面积缩放后 UV 面积 = 目标值 ✓
    const double ref_area = std::max(1e-18, c.area2d);
    const double scale = std::sqrt(target_area / ref_area);
    c.w = bw * scale;
    c.h = bh * scale;
  }

  // ---- 6. 打包（边长从理论值开始，装不下就放大） ----
  double sum_rect = 0.0;
  for (const auto& c : charts) sum_rect += c.w * c.h;
  // 理想边长；shelf 打包通常占 60~80%，故乘 1.25 起步。
  double box = std::sqrt(std::max(1e-12, sum_rect)) * 1.25;
  bool packed = false;
  for (int attempt = 0; attempt < 24 && !packed; ++attempt) {
    packed = ShelfPack(&charts, box);
    if (!packed) box *= 1.15;
  }
  if (!packed) return out;  // 装不下（极端长条形），交回上层重试或走其它方案

  // ---- 7. 归一化到 [0,1] 并留 padding / gutter ----
  //
  // padding 与 gutter 都以像素计，换算到归一化单位。
  const double inv_px = 1.0 / static_cast<double>(out.atlas_size);
  const double pad = static_cast<double>(std::max(0, config.padding)) * inv_px;
  const double gut = static_cast<double>(std::max(0, config.gutter)) * inv_px;
  const double box_margin = pad + gut * 0.5;

  double used = 0.0;
  for (auto& c : charts) {
    // 归一化：chart 矩形 → [margin, 1-margin] 内
    const double avail = 1.0 - 2.0 * box_margin;
    c.x = box_margin + (c.x / box) * avail;
    c.y = box_margin + (c.y / box) * avail;
    c.w = (c.w / box) * avail;
    c.h = (c.h / box) * avail;
    used += c.w * c.h;
  }
  out.used_area_ratio = used;

  // ---- 8. 生成展开顶点（接缝处分裂） ----
  //
  // 键 = (chart, 原顶点)。同一原顶点在不同 chart 里各得一个 UV 顶点。
  std::map<std::pair<int, uint32_t>, uint32_t> key_to_uv;
  out.uvs.reserve(vertex_count);
  out.source_vertex.reserve(vertex_count);
  out.indices.reserve(mesh.indices.size());

  for (int ci = 0; ci < static_cast<int>(charts.size()); ++ci) {
    const Chart& c = charts[ci];
    const double inv_w = 1.0 / std::max(1e-12, c.u1 - c.u0);
    const double inv_h = 1.0 / std::max(1e-12, c.v1 - c.v0);
    for (uint32_t f : c.faces) {
      uint32_t tri_uv[3];
      for (int k = 0; k < 3; ++k) {
        const uint32_t v = mesh.indices[f * 3 + k];
        const std::pair<int, uint32_t> key{ci, v};
        auto it = key_to_uv.find(key);
        if (it != key_to_uv.end()) {
          tri_uv[k] = it->second;
          continue;
        }
        // 投影 → 在 chart 的 2D 包围盒内归一化 → 映射到打包矩形
        const Vec3d& p = mesh.positions[v];
        const double u = (p.dot(c.e1) - c.u0) * inv_w;
        const double vv = (p.dot(c.e2) - c.v0) * inv_h;
        // 内缩 pad（防采样渗染）
        const double w_in = std::max(0.0, c.w - 2.0 * pad);
        const double h_in = std::max(0.0, c.h - 2.0 * pad);
        const double uu = c.x + pad + std::clamp(u, 0.0, 1.0) * w_in;
        const double vvv = c.y + pad + std::clamp(vv, 0.0, 1.0) * h_in;

        const uint32_t nid = static_cast<uint32_t>(out.uvs.size());
        out.uvs.push_back(Vec2d(std::clamp(uu, 0.0, 1.0),
                                std::clamp(vvv, 0.0, 1.0)));
        out.source_vertex.push_back(v);
        key_to_uv.emplace(key, nid);
        tri_uv[k] = nid;
      }
      if (tri_uv[0] == tri_uv[1] || tri_uv[1] == tri_uv[2] ||
          tri_uv[2] == tri_uv[0]) {
        continue;  // 分裂后索引撞车（理论上不会）
      }
      out.indices.push_back(tri_uv[0]);
      out.indices.push_back(tri_uv[1]);
      out.indices.push_back(tri_uv[2]);
    }
  }

  if (out.indices.empty()) return out;
  out.seam_vertex_count =
      static_cast<int>(out.uvs.size()) - static_cast<int>(vertex_count) > 0
          ? static_cast<int>(out.uvs.size()) - static_cast<int>(vertex_count)
          : 0;

  // ---- 9. 拉伸统计与零面积 UV 三角形 ----
  //
  // 面积拉伸比 = (UV 面积 / 图集面积) / (3D 面积 / 总 3D 面积)。
  // 理想值 1；> 1 表示该处被放大，< 1 表示被压缩。
  // **拉伸比的分母必须用「当前打包尺度下的 UV 面积 / 3D 面积」**。
  //
  // 打包利用率天然 < 100%（shelf 通常 60~80%），若分母仍用「理想占比」
  // `area3d / total_3d`，就会把「整体尺度缩小」误算成「处处被拉伸」——
  // 实测平板网格会被算成 mean_stretch = 0.62（而它其实是完美展开）。
  // 先累加**实际** UV 三角形面积（不是包围盒面积）。
  double total_uv_area = 0.0;
  for (size_t t = 0; t < out.indices.size() / 3; ++t) {
    const Vec2d& ua = out.uvs[out.indices[t * 3 + 0]];
    const Vec2d& ub = out.uvs[out.indices[t * 3 + 1]];
    const Vec2d& uc = out.uvs[out.indices[t * 3 + 2]];
    total_uv_area += 0.5 * std::fabs((ub(0) - ua(0)) * (uc(1) - ua(1)) -
                                     (ub(1) - ua(1)) * (uc(0) - ua(0)));
  }
  const double uv_per_3d =
      (total_3d > 0.0) ? (total_uv_area / total_3d) : 0.0;
  double sum_ratio = 0.0;
  double max_ratio = 0.0;
  size_t counted = 0;
  for (size_t t = 0; t < out.indices.size() / 3; ++t) {
    const uint32_t a = out.indices[t * 3 + 0];
    const uint32_t b = out.indices[t * 3 + 1];
    const uint32_t c = out.indices[t * 3 + 2];
    const Vec2d& ua = out.uvs[a];
    const Vec2d& ub = out.uvs[b];
    const Vec2d& uc = out.uvs[c];
    const double uv_area =
        0.5 * std::fabs((ub(0) - ua(0)) * (uc(1) - ua(1)) -
                        (ub(1) - ua(1)) * (uc(0) - ua(0)));
    if (uv_area < 1e-12) {
      ++out.zero_area_uv_triangles;
      continue;
    }
    // 3D 面积：用展开顶点的原始顶点回查。
    const Vec3d& p0 = mesh.positions[out.source_vertex[a]];
    const Vec3d& p1 = mesh.positions[out.source_vertex[b]];
    const Vec3d& p2 = mesh.positions[out.source_vertex[c]];
    const double area3d = 0.5 * (p1 - p0).cross(p2 - p0).norm();
    if (area3d < 1e-16) continue;
    const double expected = area3d * uv_per_3d;  // 当前尺度下的期望 UV 面积
    if (expected <= 0.0) continue;
    const double ratio = uv_area / expected;
    sum_ratio += ratio;
    max_ratio = std::max(max_ratio, ratio);
    ++counted;
  }
  if (counted > 0) {
    out.mean_stretch = sum_ratio / static_cast<double>(counted);
    out.max_stretch = max_ratio;
  }

  out.ok = true;
  return out;
}

}  // namespace recon
