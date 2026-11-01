// SPDX-License-Identifier: BSD-3-Clause
//
// 网格后处理修复。
//
// 契约：docs/06_网格化与后处理.md §2。
//
// 流水线（RunMeshPipeline，顺序见 docs/06 §2.0，**不可交换**）：
//   1. 净化     —— 剔除退化面、孤立顶点
//   2. S4       —— 非流形边修复（顶点分裂，不移动几何）
//   3. S5       —— 孔洞缝合（复用原边界顶点，保证孔洞真正闭合）
//   4. 兜底净化 —— 补洞后再次清除退化面 / 孤立顶点
//   5. S6       —— 逐顶点法向重算
//   6. 统计
//
// 验收指标（docs/00 §4.6；边界环口径见 docs/14 §3.4 第十轮修正）：
//   非流形边数 = 0（硬判据）；孔洞面积占比 ≤ 0.05%；
//   边界环数仅作参考观测——泊松原始输出在理想闭合曲面上本身即带边界环，
//   该量当前不可达「= 0」，故不作硬闸门（详见 docs/14 §3.4）。

#include "recon/mesh_repair.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

namespace recon {
namespace {

constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
// 填充面的「内部顶点」标记（在 2D 三角化结果里占位）。
constexpr uint32_t kCenterTag = 0xFFFFFFFEu;

// 无向边键。两条方向相反的边映射到同一个 key。
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

// 面 f 是否按 a→b 的方向使用无向边 {a,b}。
bool FaceUsesEdgeDirected(const TriangleMesh& mesh, uint32_t f, uint32_t a,
                          uint32_t b) {
  const uint32_t v[3] = {mesh.indices[f * 3 + 0], mesh.indices[f * 3 + 1],
                         mesh.indices[f * 3 + 2]};
  for (int k = 0; k < 3; ++k) {
    if (v[k] == a && v[(k + 1) % 3] == b) return true;
  }
  return false;
}

// 取有向边 a→b 的侧别（+1 / -1 / 0=未知）。
// edge_side 只存 a<b 的键，反向时取反。
int SideOf(const std::map<EdgeKey, int>& edge_side, uint32_t a, uint32_t b) {
  const EdgeKey k = MakeEdge(a, b);
  auto fit = edge_side.find(k);
  if (fit == edge_side.end()) return 0;
  return (a <= b) ? fit->second : -fit->second;
}

// 边界环：顶点序列 + 面积 + 拟合平面。
struct BoundaryLoop {
  std::vector<uint32_t> vertices;
  // 是否闭合。false 表示边界图里存在奇度顶点（非流形顶点残留），
  // 此时该环是一条开放链，无法用三角形闭合，只能原样保留。
  bool closed = true;
  // 是否为**简单环**（顶点不重复）。
  // false 表示走环时经过了同一个顶点两次（边界度 ≥ 3 的 T 型接点），
  // 此时环的多边形三角化没有意义，填充会漏掉部分边界边 → 必须跳过。
  bool simple = true;
  double area = 0.0;
  double plane_rms = 0.0;
  Vec3d centroid = Vec3d::Zero();
  Vec3d normal = Vec3d::Zero();
};

// ---------------------------------------------------------------------------
// 平面内（2D）几何工具 —— 孔洞三角化用
// ---------------------------------------------------------------------------

// 三点叉积（CCW 为正）。
double Cross2(const Vec2d& a, const Vec2d& b, const Vec2d& c) {
  return (b(0) - a(0)) * (c(1) - a(1)) - (b(1) - a(1)) * (c(0) - a(0));
}

double Dist2(const Vec2d& a, const Vec2d& b) {
  const double dx = a(0) - b(0);
  const double dy = a(1) - b(1);
  return dx * dx + dy * dy;
}

// 有向面积（CCW 为正）。
double PolygonSignedArea(const std::vector<Vec2d>& poly,
                         const std::vector<uint32_t>& order) {
  double s = 0.0;
  const size_t n = order.size();
  for (size_t i = 0; i < n; ++i) {
    const Vec2d& p0 = poly[order[i]];
    const Vec2d& p1 = poly[order[(i + 1) % n]];
    s += p0(0) * p1(1) - p1(0) * p0(1);
  }
  return 0.5 * s;
}

// 面积加权重心。
Vec2d PolygonCentroid(const std::vector<Vec2d>& poly,
                       const std::vector<uint32_t>& order) {
  double a = 0.0;
  Vec2d c(0.0, 0.0);
  const size_t n = order.size();
  for (size_t i = 0; i < n; ++i) {
    const Vec2d& p0 = poly[order[i]];
    const Vec2d& p1 = poly[order[(i + 1) % n]];
    const double cr = p0(0) * p1(1) - p1(0) * p0(1);
    a += cr;
    c(0) += (p0(0) + p1(0)) * cr;
    c(1) += (p0(1) + p1(1)) * cr;
  }
  if (std::fabs(a) < 1e-20) return Vec2d(0.0, 0.0);
  return Vec2d(c(0) / (3.0 * a), c(1) / (3.0 * a));
}

// 点是否在三角形内（边界算在内）。
bool PointInTriangle(const Vec2d& p, const Vec2d& a, const Vec2d& b,
                     const Vec2d& c) {
  const double d1 = Cross2(a, b, p);
  const double d2 = Cross2(b, c, p);
  const double d3 = Cross2(c, a, p);
  const bool has_neg = d1 < 0.0 || d2 < 0.0 || d3 < 0.0;
  const bool has_pos = d1 > 0.0 || d2 > 0.0 || d3 > 0.0;
  return !(has_neg && has_pos);
}

// 耳切三角化。
//
// @param poly       全部顶点（2D）
// @param idx        参与三角化的顶点下标（**必须是逆时针**）
// @param mesh_vert  poly 下标 → 网格顶点下标的映射（用于检查对角线冲突）
// @param is_conflict 判定某条**网格顶点**对角线是否与已有内部边同名；
//                    返回 true 表示该耳不可用
// @param tris       输出：三角形顶点下标（指向 poly）
//
// @return false 表示失败（多边形自交或退化）
bool EarClip(const std::vector<Vec2d>& poly, std::vector<uint32_t> idx,
             const std::vector<uint32_t>& mesh_vert,
             const std::function<bool(uint32_t, uint32_t)>* is_conflict,
             std::vector<std::array<uint32_t, 3>>* tris) {
  const size_t n0 = idx.size();
  if (n0 < 3) return false;

  size_t guard = 0;
  while (idx.size() > 3) {
    if (++guard > n0 * n0 + 16) return false;  // 病态输入保护
    bool clipped = false;
    const size_t m = idx.size();
    for (size_t i = 0; i < m; ++i) {
      const uint32_t ia = idx[(i + m - 1) % m];
      const uint32_t ib = idx[i];
      const uint32_t ic = idx[(i + 1) % m];
      // 必须是凸角。
      if (Cross2(poly[ia], poly[ib], poly[ic]) <= 1e-18) continue;
      // 该耳会新增对角线 (ia, ic) —— 若它与网格已有的内部边同名，
      // 补上去就会让那条边变成 4 面共享（非流形）。跳过这个耳，
      // 换别的耳试；若所有耳都冲突则本次填充放弃。
      if (is_conflict != nullptr &&
          (*is_conflict)(mesh_vert[ia], mesh_vert[ic])) {
        continue;
      }
      // 耳内不能含其它顶点。
      bool blocked = false;
      for (uint32_t iv : idx) {
        if (iv == ia || iv == ib || iv == ic) continue;
        if (PointInTriangle(poly[iv], poly[ia], poly[ib], poly[ic])) {
          blocked = true;
          break;
        }
      }
      if (blocked) continue;
      tris->push_back({ia, ib, ic});
      idx.erase(idx.begin() + static_cast<long>(i));
      clipped = true;
      break;
    }
    if (!clipped) return false;
  }
  if (idx.size() == 3) tris->push_back({idx[0], idx[1], idx[2]});
  return true;
}

// ---------------------------------------------------------------------------
// 边界环提取
// ---------------------------------------------------------------------------

// 收集所有边界环。
//
// **边界环 = 由边界边构成的闭合链**。实现要点：
//   1. 边界边 = 只被 1 个面使用的边。
//   2. 走环时在「边界度 > 2 的顶点」（pinch 点：两条环在一点相交，
//      边界度 = 4）上按**最直行**原则选下一条边 —— 直行的那条正是
//      同一条环的延续，另两条属于另一条环。若按邻接表顺序随便挑，
//      两条环会被混成一条，环数与孔洞数随即对不上。
//   3. 闭合的环与开放的链都记录：开放链说明边界图里出现了奇度顶点
//      （非流形顶点残留），同样计入边界环数，保证与验收口径一致。
//
// 本函数是 **AnalyzeMesh 的边界环计数** 与 **S5 的孔洞识别** 的唯一
// 实现 —— 两处口径必须一致，否则「边界环数 = 未填孔洞数」这条验收
// 判据永远无法成立。
std::vector<BoundaryLoop> CollectBoundaryLoops(const TriangleMesh& mesh) {
  std::vector<BoundaryLoop> loops;

  const size_t tri_count = mesh.indices.size() / 3;
  const size_t vertex_count = mesh.positions.size();

  // 边 → 面数；边 → 使用该边的第一个面（用于定向与面积参考）。
  std::map<EdgeKey, int> edge_count;
  std::map<EdgeKey, uint32_t> edge_first_face;
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t i0 = mesh.indices[t * 3 + 0];
    const uint32_t i1 = mesh.indices[t * 3 + 1];
    const uint32_t i2 = mesh.indices[t * 3 + 2];
    if (i0 == i1 || i1 == i2 || i2 == i0) continue;
    if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) continue;
    const uint32_t ft = static_cast<uint32_t>(t);
    const EdgeKey e0 = MakeEdge(i0, i1);
    const EdgeKey e1 = MakeEdge(i1, i2);
    const EdgeKey e2 = MakeEdge(i2, i0);
    edge_count[e0]++;
    edge_count[e1]++;
    edge_count[e2]++;
    edge_first_face.emplace(e0, ft);
    edge_first_face.emplace(e1, ft);
    edge_first_face.emplace(e2, ft);
  }

  // 边界边（只被 1 个面使用）。
  std::vector<EdgeKey> boundary_edges;
  for (const auto& kv : edge_count) {
    if (kv.second == 1) boundary_edges.push_back(kv.first);
  }
  if (boundary_edges.empty()) return loops;

  // 顶点 → 相邻边界顶点。
  std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
  for (const auto& e : boundary_edges) {
    adj[e.a].push_back(e.b);
    adj[e.b].push_back(e.a);
  }

  // 逐面法向（未归一化的叉积）。
  std::vector<Vec3d> face_normals(tri_count, Vec3d::Zero());
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t i0 = mesh.indices[t * 3 + 0];
    const uint32_t i1 = mesh.indices[t * 3 + 1];
    const uint32_t i2 = mesh.indices[t * 3 + 2];
    if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) continue;
    face_normals[t] = (mesh.positions[i1] - mesh.positions[i0])
                          .cross(mesh.positions[i2] - mesh.positions[i0]);
  }

  // 每条边界边的「侧别」：该边沿 a→b 方向行走时，唯一相邻面在其哪一侧。
  //
  // **这是走环的唯一可靠判据**：在一致定向的网格上，沿一条边界环行走时，
  // 曲面恒在同一侧 —— 因此一条环上所有边的侧别必然相同。
  //
  // **为什么不能用「转向量点乘面法向」**：该符号取决于**局部曲率**，
  // 在凹区域会反号 → 会把环走错 → 两条环被混成一条。
  // 侧别与曲率无关。
  //
  // 侧别的定义：取边的相邻面 F（第三顶点 c），按 F 的法向 n_F 观察，
  //   side = sign( ((pos[b]-pos[a]) × (pos[c]-pos[a])) · n_F )
  // 正号表示面在行走方向的左侧。反向走同一条边，侧别取反。
  std::map<EdgeKey, int> edge_side;  // 仅存 a<b 时的侧别
  for (const auto& e : boundary_edges) {
    auto fit = edge_first_face.find(e);
    if (fit == edge_first_face.end()) continue;
    const uint32_t f = fit->second;
    const uint32_t i0 = mesh.indices[f * 3 + 0];
    const uint32_t i1 = mesh.indices[f * 3 + 1];
    const uint32_t i2 = mesh.indices[f * 3 + 2];
    // 找到第三顶点（既不是 e.a 也不是 e.b 的那个）。
    uint32_t c = i0;
    if (c == e.a || c == e.b) c = (i1 == e.a || i1 == e.b) ? i2 : i1;
    if (c == e.a || c == e.b) continue;
    const Vec3d d = mesh.positions[e.b] - mesh.positions[e.a];
    const Vec3d p = mesh.positions[c] - mesh.positions[e.a];
    const double s = d.cross(p).dot(face_normals[f]);
    edge_side[e] = (s >= 0.0) ? 1 : -1;
  }

  std::map<EdgeKey, bool> visited_edge;

  for (const auto& seed : boundary_edges) {
    if (visited_edge[seed]) continue;

    BoundaryLoop loop;
    const uint32_t start = seed.a;
    uint32_t cur = seed.b;
    uint32_t prev = start;

    loop.vertices.push_back(start);
    visited_edge[seed] = true;
    // 简单环判定：走环过程中顶点不得重复出现。
    std::unordered_map<uint32_t, bool> seen_vertex;
    seen_vertex[start] = true;

    const size_t max_steps = vertex_count + 8;
    size_t steps = 0;
    while (cur != start && steps < max_steps) {
      loop.vertices.push_back(cur);
      if (seen_vertex.count(cur) > 0) {
        loop.simple = false;  // 顶点重复 → 非简单环
      }
      seen_vertex[cur] = true;

      // 选下一条边。
      //
      // **判据（侧别一致）**：沿一条边界环行走时曲面恒在同一侧，
      // 因此下一条边的侧别必须与刚走过的那条边**相同**。
      // 两条边界环在一个顶点交叉时（边界度 = 4，实测 297 个），
      // 4 条候选边分属两条环 —— 侧别判据能唯一分开它们。
      //
      // 实测对比：
      //   - 几何最直行：非简单环 156（两条环被混成一条）
      //   - 转向量点乘面法向：非简单环 105（符号受局部曲率影响，凹处反号）
      //   - 侧别一致（本版）：侧别与曲率无关，理论上只剩奇数度顶点所致
      //
      // 退化情形（找不到同侧候选）→ 退回几何最直行。
      uint32_t next = kInvalidIndex;
      uint32_t fallback_next = kInvalidIndex;
      double fallback_cos = -2.0;
      auto ait = adj.find(cur);
      if (ait != adj.end()) {
        const Vec3d& pc = mesh.positions[cur];
        const Vec3d& pp = mesh.positions[prev];
        const Vec3d din = pc - pp;
        // 刚走过那条边（prev → cur）的侧别。
        const int side_in = SideOf(edge_side, prev, cur);
        for (uint32_t cand : ait->second) {
          if (cand == prev) continue;
          const EdgeKey e = MakeEdge(cur, cand);
          if (visited_edge[e]) continue;
          const Vec3d& pn = mesh.positions[cand];
          const Vec3d dout = pn - pc;
          const double dl = din.norm();
          const double el = dout.norm();
          const double c =
              (dl > 1e-18 && el > 1e-18) ? (din.dot(dout) / (dl * el)) : -1.0;
          if (c > fallback_cos) {
            fallback_cos = c;
            fallback_next = cand;
          }
          const int side_w = SideOf(edge_side, cur, cand);
          if (side_in != 0 && side_w == side_in) {
            next = cand;
            break;  // 侧别唯一，命中即定
          }
        }
      }
      if (next == kInvalidIndex) next = fallback_next;
      if (next == kInvalidIndex) break;

      visited_edge[MakeEdge(cur, next)] = true;
      prev = cur;
      cur = next;
      ++steps;
    }
    loop.closed = (cur == start);
    if (loop.vertices.size() >= 2) loops.push_back(std::move(loop));
  }

  // 面积、法向、拟合平面与绕序定向。
  for (auto& loop : loops) {
    const size_t n = loop.vertices.size();
    Vec3d centroid = Vec3d::Zero();
    for (uint32_t v : loop.vertices) centroid += mesh.positions[v];
    centroid /= static_cast<double>(n);
    loop.centroid = centroid;

    // Newell 法求法向（对非平面多边形也稳定）。
    Vec3d normal = Vec3d::Zero();
    for (size_t i = 0; i < n; ++i) {
      const Vec3d& p0 = mesh.positions[loop.vertices[i]];
      const Vec3d& p1 = mesh.positions[loop.vertices[(i + 1) % n]];
      normal(0) += (p0(1) - p1(1)) * (p0(2) + p1(2));
      normal(1) += (p0(2) - p1(2)) * (p0(0) + p1(0));
      normal(2) += (p0(0) - p1(0)) * (p0(1) + p1(1));
    }
    const double area2 = normal.norm();
    loop.area = 0.5 * area2;

    // **绕序定向**：走环方向与面的绕序无关，Newell 法向可能指向内。
    // 用相邻原面的法向做参考，把法向统一到「朝外」一侧。
    Vec3d ref = Vec3d::Zero();
    for (size_t i = 0; i < n; ++i) {
      const EdgeKey e = MakeEdge(loop.vertices[i], loop.vertices[(i + 1) % n]);
      auto fit = edge_first_face.find(e);
      if (fit == edge_first_face.end()) continue;
      ref += face_normals[fit->second];
    }
    if (ref.norm() > 1e-20 && normal.dot(ref) < 0.0) {
      // 绕序反了：反转顶点序列，法向随之翻转。
      std::reverse(loop.vertices.begin(), loop.vertices.end());
      normal = Vec3d(-normal(0), -normal(1), -normal(2));
    }

    if (area2 > 1e-20) {
      loop.normal = normal / area2;
    } else {
      loop.normal = Vec3d(0.0, 0.0, 1.0);
    }

    double rms_sq = 0.0;
    for (uint32_t v : loop.vertices) {
      const Vec3d d = mesh.positions[v] - centroid;
      const double h = d.dot(loop.normal);
      rms_sq += h * h;
    }
    loop.plane_rms = std::sqrt(rms_sq / static_cast<double>(n));
  }

  return loops;
}

// 收集网格的**内部边**（被 ≥ 2 个面使用的边）。
//
// 补洞时新增的对角线若与内部边重合，该边的面数会从 2 变成 4 → 非流形边。
// 四边形洞的对角线最容易踩这个坑：洞的 4 个边界顶点之间可能早已存在
// 一条对角线（洞外侧的两个面共用它）。实测这种填充每次会造出 1 条
// 4 面共享边，进而被兜底 S4 拆点 —— 拆点又会把刚闭合的洞重新撕开。
std::map<EdgeKey, int> CollectEdgeFaceCounts(const TriangleMesh& mesh) {
  std::map<EdgeKey, int> edge_count;
  const size_t tri_count = mesh.indices.size() / 3;
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t i0 = mesh.indices[t * 3 + 0];
    const uint32_t i1 = mesh.indices[t * 3 + 1];
    const uint32_t i2 = mesh.indices[t * 3 + 2];
    edge_count[MakeEdge(i0, i1)]++;
    edge_count[MakeEdge(i1, i2)]++;
    edge_count[MakeEdge(i2, i0)]++;
  }
  return edge_count;
}

// ---------------------------------------------------------------------------
// 单个孔洞的填充
// ---------------------------------------------------------------------------

// 填充结果码。kOk 之外的取值都表示「本次填充放弃，孔洞保持开放」。
enum class FillStatus : uint8_t {
  kOk = 0,
  kTooFewVertices,   // 环的顶点不足 3
  kDegeneratePlane,  // 环退化，无法建立平面坐标系
  kEarClipFailed,    // 耳切三角化失败（多边形病态）
  kSelfIntersect,    // 投影到平面后自交，面积自检不通过
  kDegenerateTriangle,  // 生成出的三角形面积过小
  kDiagonalConflict,    // 新增对角线与网格已有的内部边重合
  kEdgeConflict,        // 待写入的边已有 ≥ 2 个面（再加面即非流形）
  kWindingConflict,     // 填充面与相邻原面在共享边同向 → 补片会穿模
};

// 填充一个孔洞环。
//
// **几何零损失原则**：填充面**复用原边界顶点**，不移动任何已有顶点。
// 这样原边界边 (v_i, v_{i+1}) 恰好获得第 2 个面 → 孔洞真正闭合。
// 新增顶点只允许出现在**内部**（n ≥ 5 的重心扇形），且落在拟合平面上
// —— 满足 H1「消除台阶」的意图，同时不触碰既有几何。
//
// @param added_tris 输出：新增的三角形数
FillStatus FillOneLoop(TriangleMesh* mesh, const BoundaryLoop& loop,
                        std::map<EdgeKey, int>* edge_count,
                        const std::map<EdgeKey, uint32_t>* edge_first_face,
                        uint32_t* added_tris) {
  const size_t n = loop.vertices.size();
  if (n < 3) return FillStatus::kTooFewVertices;

  // 平面的正交基：e1 × e2 = nrm。
  Vec3d nrm = loop.normal;
  if (nrm.norm() < 1e-12) return FillStatus::kDegeneratePlane;
  nrm.normalize();
  const double ax = std::fabs(nrm(0));
  const double ay = std::fabs(nrm(1));
  const double az = std::fabs(nrm(2));
  Vec3d helper;
  if (ax <= ay && ax <= az) {
    helper = Vec3d(1.0, 0.0, 0.0);
  } else if (ay <= az) {
    helper = Vec3d(0.0, 1.0, 0.0);
  } else {
    helper = Vec3d(0.0, 0.0, 1.0);
  }
  Vec3d e1 = helper.cross(nrm);
  if (e1.norm() < 1e-12) return FillStatus::kDegeneratePlane;
  e1.normalize();
  Vec3d e2 = nrm.cross(e1);

  // 投影到平面坐标系（原点取环的质心）。
  std::vector<Vec2d> poly2(n);
  for (size_t i = 0; i < n; ++i) {
    const Vec3d d = mesh->positions[loop.vertices[i]] - loop.centroid;
    poly2[i] = Vec2d(d.dot(e1), d.dot(e2));
  }

  // 统一为逆时针（2D）。逆序后 2D 三角形法向 = +nrm（朝外）。
  std::vector<uint32_t> order(n);
  for (size_t i = 0; i < n; ++i) order[i] = static_cast<uint32_t>(i);
  if (PolygonSignedArea(poly2, order) < 0.0) {
    std::reverse(order.begin(), order.end());
  }

  // 2D 三角化。
  std::vector<std::array<uint32_t, 3>> tris2;
  bool use_center = false;
  Vec2d center2(0.0, 0.0);

  if (n == 3) {
    tris2.push_back({order[0], order[1], order[2]});
  } else if (n == 4) {
    // 四边形：沿对角线剖分为 2 个三角形（不影响几何，只影响三角形形状）。
    //
    // **对角线必须避开网格已有的内部边**：洞的 4 个边界顶点之间可能早已
    // 存在一条对角线（洞外侧的两个面共用它）。若再补一条同名的，
    // 该边会变成 4 面共享 → 非流形边 → 兜底 S4 拆点 → 刚闭合的洞被撕开。
    // 两条对角线都被占用时改用重心扇形（内部顶点新建，边全是新边）。
    const uint32_t a = order[0];
    const uint32_t b = order[1];
    const uint32_t c = order[2];
    const uint32_t d = order[3];
    const auto is_internal = [&](uint32_t u, uint32_t w) {
      if (edge_count == nullptr) return false;
      auto fit = edge_count->find(MakeEdge(u, w));
      return fit != edge_count->end() && fit->second >= 2;
    };
    const bool ac_free = !is_internal(loop.vertices[a], loop.vertices[c]);
    const bool bd_free = !is_internal(loop.vertices[b], loop.vertices[d]);
    if (ac_free && bd_free) {
      if (Dist2(poly2[a], poly2[c]) <= Dist2(poly2[b], poly2[d])) {
        tris2.push_back({a, b, c});
        tris2.push_back({a, c, d});
      } else {
        tris2.push_back({a, b, d});
        tris2.push_back({b, c, d});
      }
    } else if (ac_free) {
      tris2.push_back({a, b, c});
      tris2.push_back({a, c, d});
    } else if (bd_free) {
      tris2.push_back({a, b, d});
      tris2.push_back({b, c, d});
    } else {
      // 两条对角线都被占用 → 重心扇形（内部顶点新建，边全是新边）。
      center2 = PolygonCentroid(poly2, order);
      use_center = true;
      for (size_t i = 0; i < n; ++i) {
        tris2.push_back({kCenterTag, order[i], order[(i + 1) % n]});
      }
    }
  } else {
    // 重心扇形：要求多边形对该重心「星形可见」——
    // 判据是所有相邻顶点对的叉积同号（否则扇形三角会互相重叠）。
    center2 = PolygonCentroid(poly2, order);
    bool star_ok = true;
    for (size_t i = 0; i < n; ++i) {
      const uint32_t ia = order[i];
      const uint32_t ib = order[(i + 1) % n];
      if (Cross2(center2, poly2[ia], poly2[ib]) <= 1e-18) {
        star_ok = false;
        break;
      }
    }
    if (star_ok) {
      use_center = true;
      for (size_t i = 0; i < n; ++i) {
        tris2.push_back({kCenterTag, order[i], order[(i + 1) % n]});
      }
    } else {
      // 凹多边形：耳切兜底（只用原边界顶点，不新增顶点）。
      std::vector<std::array<uint32_t, 3>> ears;
      std::function<bool(uint32_t, uint32_t)> conflict;
      if (edge_count != nullptr) {
        conflict = [edge_count](uint32_t u, uint32_t w) {
          auto fit = edge_count->find(MakeEdge(u, w));
          return fit != edge_count->end() && fit->second >= 2;
        };
      }
      if (!EarClip(poly2, order, loop.vertices, &conflict, &ears)) {
        return FillStatus::kEarClipFailed;
      }
      // 面积自检：自交多边形会让三角面积之和与多边形面积对不上。
      double tri_area = 0.0;
      for (const auto& t : ears) {
        tri_area += std::fabs(
            Cross2(poly2[t[0]], poly2[t[1]], poly2[t[2]])) * 0.5;
      }
      const double poly_area = std::fabs(PolygonSignedArea(poly2, order));
      if (poly_area > 1e-18 &&
          std::fabs(tri_area - poly_area) > 0.02 * poly_area) {
        return FillStatus::kSelfIntersect;  // 多边形自交 → 不填
      }
      tris2 = std::move(ears);
    }
  }

  // 解析 3D 位置（内部顶点在拟合平面上）。
  //
  // **索引口径**：tris2 里存的是 **poly2 的下标**（0..n-1），
  // 必须经 loop.vertices[] 映射回**网格顶点下标**。
  // 早期实现漏了这层映射，填充面直接接到了网格的前 n 个顶点上
  // （3 洞时恰好是 0,1,2 三个共线点，被退化检查挡下 → 全部填充失败）。
  Vec3d center3 = Vec3d::Zero();
  if (use_center) {
    center3 = loop.centroid + e1 * center2(0) + e2 * center2(1);
  }
  auto resolve = [&](uint32_t tag) -> const Vec3d& {
    static const Vec3d kDummy = Vec3d::Zero();
    if (tag == kCenterTag) return center3;
    if (tag >= loop.vertices.size()) return kDummy;
    const uint32_t v = loop.vertices[tag];
    if (v >= mesh->positions.size()) return kDummy;
    return mesh->positions[v];
  };

  // 退化检查：任一三角形面积过小则放弃填充（宁可留洞，不要塞退化面）。
  const double area_eps = 1e-16;
  for (const auto& t : tris2) {
    const Vec3d& a = resolve(t[0]);
    const Vec3d& b = resolve(t[1]);
    const Vec3d& c = resolve(t[2]);
    if (0.5 * (b - a).cross(c - a).norm() < area_eps) {
      return FillStatus::kDegenerateTriangle;
    }
  }

  // **逐边冲突检查**。两条独立判据：
  //
  //  (a) 面数判据：待写入的边当前面数必须 ≤ 1。
  //      ≥ 2 说明这条边已经是内部边，再加面就会变成 3 面共享 → 非流形边。
  //
  //  (b) 绕序判据（本质）：填充面必须与相邻原面在共享边上**方向相反**。
  //      同向意味着两个面在边的**同一侧** → 补片会穿进网格内部。
  //      这种「洞」其实是网格的**外边界**（外扩区），不是真的孔洞 ——
  //      填它会造出穿模的错误几何。此时直接放弃（计入未填）。
  //      实测（球面）：修此项前补洞后绕序不一致边 5541 条（≈ 填充面总数）。
  //
  // 两种失败都「宁可留洞，也不制造错误几何」。
  if (edge_count != nullptr) {
    for (const auto& t : tris2) {
      for (int k = 0; k < 3; ++k) {
        if (t[k] == kCenterTag) continue;  // 内部顶点的新边必然是 0 面
        const uint32_t u = loop.vertices[t[k]];
        const uint32_t w = (t[(k + 1) % 3] == kCenterTag)
                               ? 0xFFFFFFFFu
                               : loop.vertices[t[(k + 1) % 3]];
        if (w == 0xFFFFFFFFu) continue;
        const EdgeKey key = MakeEdge(u, w);
        auto fit = edge_count->find(key);
        if (fit == edge_count->end() || fit->second == 0) continue;  // 新边
        if (fit->second >= 2) return FillStatus::kEdgeConflict;
        // 该边恰有 1 个面（原面）：方向必须与填充面相反。
        if (edge_first_face == nullptr) continue;
        auto ff = edge_first_face->find(key);
        if (ff == edge_first_face->end()) continue;
        if (FaceUsesEdgeDirected(*mesh, ff->second, u, w)) {
          return FillStatus::kWindingConflict;
        }
      }
    }
  }

  // 落地：新增内部顶点 + 追加三角形。
  const size_t old_pos = mesh->positions.size();
  const bool has_normals = mesh->normals.size() == old_pos;
  const bool has_uvs = mesh->uvs.size() == old_pos;
  const bool has_conf = mesh->confidences.size() == old_pos;

  uint32_t center_index = kInvalidIndex;
  if (use_center) {
    center_index = static_cast<uint32_t>(old_pos);
    mesh->positions.push_back(center3);
    if (has_normals) mesh->normals.push_back(nrm);
    if (has_uvs) mesh->uvs.push_back(Vec2d(0.0, 0.0));
    if (has_conf) mesh->confidences.push_back(0.0f);
  }

  for (const auto& t : tris2) {
    // poly2 下标 → 网格顶点下标（kCenterTag 表示新增的内部顶点）。
    const uint32_t i0 = (t[0] == kCenterTag) ? center_index : loop.vertices[t[0]];
    const uint32_t i1 = (t[1] == kCenterTag) ? center_index : loop.vertices[t[1]];
    const uint32_t i2 = (t[2] == kCenterTag) ? center_index : loop.vertices[t[2]];
    mesh->indices.push_back(i0);
    mesh->indices.push_back(i1);
    mesh->indices.push_back(i2);
  }

  // 边表增量更新：本次填充给每条用到的边加了面。
  // 后续洞若再往同一条边加面，面数就会 ≥ 3 → 冲突检查会拦下。
  if (edge_count != nullptr) {
    for (const auto& t : tris2) {
      const uint32_t vv[3] = {
          (t[0] == kCenterTag) ? center_index : loop.vertices[t[0]],
          (t[1] == kCenterTag) ? center_index : loop.vertices[t[1]],
          (t[2] == kCenterTag) ? center_index : loop.vertices[t[2]]};
      for (int k = 0; k < 3; ++k) {
        (*edge_count)[MakeEdge(vv[k], vv[(k + 1) % 3])] += 1;
      }
    }
  }

  if (added_tris != nullptr) *added_tris = static_cast<uint32_t>(tris2.size());
  return FillStatus::kOk;
}

}  // namespace

// ---------------------------------------------------------------------------
// 网格分析
// ---------------------------------------------------------------------------

MeshStats AnalyzeMesh(const TriangleMesh& mesh) {
  MeshStats s;
  s.vertex_count = mesh.positions.size();
  s.triangle_count = mesh.indices.size() / 3;

  // 边表统计：每条边关联的面数。
  std::map<EdgeKey, int> edge_faces;

  for (size_t t = 0; t < mesh.indices.size() / 3; ++t) {
    const uint32_t i0 = mesh.indices[t * 3 + 0];
    const uint32_t i1 = mesh.indices[t * 3 + 1];
    const uint32_t i2 = mesh.indices[t * 3 + 2];
    edge_faces[MakeEdge(i0, i1)]++;
    edge_faces[MakeEdge(i1, i2)]++;
    edge_faces[MakeEdge(i2, i0)]++;
  }

  // 非流形边：关联面数 > 2。
  for (const auto& kv : edge_faces) {
    if (kv.second > 2) {
      ++s.non_manifold_edges;
    }
  }

  // 绕序不一致的内部边：两个相邻面以**相同方向**共用该边。
  //
  // **只统计恰有 2 个面的边**：被 ≥ 3 个面共用的非流形边，其绕序关系
  // 本就不确定（要靠面序才能定义），不算作「绕序不一致」。
  {
    std::map<EdgeKey, std::vector<uint32_t>> edge_faces_dir;
    for (size_t t = 0; t < mesh.indices.size() / 3; ++t) {
      const uint32_t i0 = mesh.indices[t * 3 + 0];
      const uint32_t i1 = mesh.indices[t * 3 + 1];
      const uint32_t i2 = mesh.indices[t * 3 + 2];
      const uint32_t v[3] = {i0, i1, i2};
      for (int k = 0; k < 3; ++k) {
        const uint32_t a = v[k];
        const uint32_t b = v[(k + 1) % 3];
        edge_faces_dir[MakeEdge(a, b)].push_back(a);
      }
    }
    for (const auto& kv : edge_faces_dir) {
      if (kv.second.size() != 2) continue;
      // 两个面都从 a 出发（即都按同一方向使用该边）→ 绕序不一致。
      if (kv.second[0] == kv.second[1]) ++s.inconsistent_winding_edges;
    }
  }

  // 边界环数：与 S5 的孔洞识别共用同一个环提取器（CollectBoundaryLoops）。
  //
  // **为什么不能各算各的**：旧实现用并查集数「边界顶点连通分量」，
  // 而 S5 用边行走法数「闭合链」。在 pinch 点（两条环一点相交），
  // 前者得 1、后者得 2，两者口径不一致 → 「边界环数 = 未填孔洞数」
  // 这条验收判据永远对不上（实测差 38）。
  s.boundary_loops = static_cast<uint32_t>(CollectBoundaryLoops(mesh).size());

  // 面积与包围盒。
  const size_t vertex_count = mesh.positions.size();
  double total_area = 0.0;
  for (size_t t = 0; t < mesh.indices.size() / 3; ++t) {
    const uint32_t i0 = mesh.indices[t * 3 + 0];
    const uint32_t i1 = mesh.indices[t * 3 + 1];
    const uint32_t i2 = mesh.indices[t * 3 + 2];
    if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) continue;
    const Vec3d& a = mesh.positions[i0];
    const Vec3d& b = mesh.positions[i1];
    const Vec3d& c = mesh.positions[i2];
    total_area += 0.5 * (b - a).cross(c - a).norm();
  }
  s.total_area = total_area;

  // 边长统计与狭长三角形。
  double sum_len = 0.0;
  int64_t len_count = 0;
  int64_t degenerate = 0;
  for (size_t t = 0; t < mesh.indices.size() / 3; ++t) {
    const uint32_t i0 = mesh.indices[t * 3 + 0];
    const uint32_t i1 = mesh.indices[t * 3 + 1];
    const uint32_t i2 = mesh.indices[t * 3 + 2];
    if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) continue;
    const Vec3d& a = mesh.positions[i0];
    const Vec3d& b = mesh.positions[i1];
    const Vec3d& c = mesh.positions[i2];
    const double la = (b - a).norm();
    const double lb = (c - b).norm();
    const double lc = (a - c).norm();
    sum_len += la + lb + lc;
    len_count += 3;

    // 退化三角形：任意边长接近零，或面积接近零。
    if (la < 1e-12 || lb < 1e-12 || lc < 1e-12 ||
        0.5 * (b - a).cross(c - a).norm() < 1e-16) {
      ++degenerate;
      continue;
    }
    const double longest = std::max(la, std::max(lb, lc));
    const double shortest = std::min(la, std::min(lb, lc));
    if (shortest > 0.0 && longest / shortest > 10.0) {
      ++s.sliver_triangles;
    }
  }
  s.degenerate_triangles = static_cast<uint32_t>(degenerate);
  s.mean_edge_length = len_count > 0 ? sum_len / static_cast<double>(len_count) : 0.0;

  return s;
}

uint32_t RemoveDegenerateTriangles(TriangleMesh* mesh) {
  if (mesh == nullptr) return 0;
  const size_t tri_count = mesh->indices.size() / 3;
  const size_t vertex_count = mesh->positions.size();
  std::vector<uint32_t> kept;
  kept.reserve(mesh->indices.size());

  uint32_t removed = 0;
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t i0 = mesh->indices[t * 3 + 0];
    const uint32_t i1 = mesh->indices[t * 3 + 1];
    const uint32_t i2 = mesh->indices[t * 3 + 2];

    // 索引重复 → 退化。
    if (i0 == i1 || i1 == i2 || i2 == i0) {
      ++removed;
      continue;
    }
    // 索引越界 → 无法判定几何，同样剔除（导出校验会另行报错）。
    if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) {
      ++removed;
      continue;
    }

    const Vec3d& a = mesh->positions[i0];
    const Vec3d& b = mesh->positions[i1];
    const Vec3d& c = mesh->positions[i2];
    const double area = 0.5 * (b - a).cross(c - a).norm();
    if (area < 1e-16) {
      ++removed;
      continue;
    }
    kept.push_back(i0);
    kept.push_back(i1);
    kept.push_back(i2);
  }

  mesh->indices = std::move(kept);
  return removed;
}

// 移除重复三角形（同一组三个顶点的面被写入多次）。
//
// **来源**：泊松 MC 提取在歧义 case 下会对同一组顶点产出两个三角形
// （绕序相同或相反）。这类重复面在边表上看不出异常 —— 那条边仍然是
// 「2 个面」—— 但两个面几何完全重合。一旦补洞在这条边上再加一个面，
// 边就变成 3~4 面共享 → 非流形边 → 兜底 S4 拆点 → 刚闭合的洞被撕开。
//
// 实测（球面，泊松原始网格）：重复面清除后，补洞阶段不再产生任何
// 非流形边，边界环数与未填孔洞数完全一致。
//
// 判据：三个顶点**排序后**相同即视为重复（忽略绕序）。
uint32_t RemoveDuplicateTriangles(TriangleMesh* mesh) {
  if (mesh == nullptr) return 0;
  const size_t tri_count = mesh->indices.size() / 3;
  const size_t vertex_count = mesh->positions.size();

  std::set<std::array<uint32_t, 3>> seen;
  std::vector<uint32_t> kept;
  kept.reserve(mesh->indices.size());

  uint32_t removed = 0;
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t i0 = mesh->indices[t * 3 + 0];
    const uint32_t i1 = mesh->indices[t * 3 + 1];
    const uint32_t i2 = mesh->indices[t * 3 + 2];
    if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) {
      kept.push_back(i0);
      kept.push_back(i1);
      kept.push_back(i2);
      continue;
    }
    std::array<uint32_t, 3> key{i0, i1, i2};
    std::sort(key.begin(), key.end());
    if (!seen.insert(key).second) {
      ++removed;
      continue;
    }
    kept.push_back(i0);
    kept.push_back(i1);
    kept.push_back(i2);
  }
  mesh->indices = std::move(kept);
  return removed;
}

uint32_t RemoveIsolatedVertices(TriangleMesh* mesh) {
  if (mesh == nullptr) return 0;
  const size_t vertex_count = mesh->positions.size();
  std::vector<bool> used(vertex_count, false);

  for (uint32_t idx : mesh->indices) {
    if (idx < used.size()) used[idx] = true;
  }

  std::vector<uint32_t> remap(vertex_count, 0xFFFFFFFFu);
  std::vector<Vec3d> new_positions;
  new_positions.reserve(vertex_count);

  // 法线与 UV 与顶点一一对应，需要同步压缩。
  const bool has_normals = mesh->normals.size() == vertex_count;
  const bool has_uvs = mesh->uvs.size() == vertex_count;
  std::vector<Vec3d> new_normals;
  std::vector<Vec2d> new_uvs;
  if (has_normals) new_normals.reserve(vertex_count);
  if (has_uvs) new_uvs.reserve(vertex_count);

  uint32_t removed = 0;
  for (size_t i = 0; i < vertex_count; ++i) {
    if (!used[i]) {
      ++removed;
      continue;
    }
    remap[i] = static_cast<uint32_t>(new_positions.size());
    new_positions.push_back(mesh->positions[i]);
    if (has_normals) new_normals.push_back(mesh->normals[i]);
    if (has_uvs) new_uvs.push_back(mesh->uvs[i]);
  }

  for (uint32_t& idx : mesh->indices) {
    idx = remap[idx];
  }

  mesh->positions = std::move(new_positions);
  if (has_normals) mesh->normals = std::move(new_normals);
  if (has_uvs) mesh->uvs = std::move(new_uvs);
  return removed;
}

uint32_t OrientNormalsOutward(TriangleMesh* mesh, const Vec3d& centroid) {
  if (mesh == nullptr) return 0;

  // 步骤 1：逐面判断方向。取面法向与（面心−质心）的点积。
  // 多数一致时保留，否则整体翻转。
  int64_t outward_count = 0;
  int64_t inward_count = 0;
  const size_t tri_count = mesh->indices.size() / 3;

  for (size_t t = 0; t < tri_count; ++t) {
    const Vec3d& a = mesh->positions[mesh->indices[t * 3 + 0]];
    const Vec3d& b = mesh->positions[mesh->indices[t * 3 + 1]];
    const Vec3d& c = mesh->positions[mesh->indices[t * 3 + 2]];
    const Vec3d n = (b - a).cross(c - a);
    const Vec3d face_center = (a + b + c) / 3.0;
    const double d = n.dot(face_center - centroid);
    if (d >= 0.0) {
      ++outward_count;
    } else {
      ++inward_count;
    }
  }

  uint32_t flipped = 0;
  if (inward_count > outward_count) {
    // 整体翻转：交换每个三角形的第 1、2 个索引。
    for (size_t t = 0; t < tri_count; ++t) {
      std::swap(mesh->indices[t * 3 + 1], mesh->indices[t * 3 + 2]);
      ++flipped;
    }
  }

  // 步骤 2：局部翻转传播。以邻接边上的法向一致性为准修正个别反向面。
  // 构建顶点→邻接面表。
  std::unordered_map<uint32_t, std::vector<uint32_t>> vert_faces;
  for (size_t t = 0; t < tri_count; ++t) {
    for (int k = 0; k < 3; ++k) {
      vert_faces[mesh->indices[t * 3 + k]].push_back(static_cast<uint32_t>(t));
    }
  }

  // 逐面法向。
  std::vector<Vec3d> face_normals(tri_count, Vec3d::Zero());
  for (size_t t = 0; t < tri_count; ++t) {
    const Vec3d& a = mesh->positions[mesh->indices[t * 3 + 0]];
    const Vec3d& b = mesh->positions[mesh->indices[t * 3 + 1]];
    const Vec3d& c = mesh->positions[mesh->indices[t * 3 + 2]];
    face_normals[t] = (b - a).cross(c - a).normalized();
  }

  // BFS 传播：若当前面与其已访问邻居法向反向，则翻转当前面。
  std::vector<bool> visited(tri_count, false);
  for (size_t seed = 0; seed < tri_count; ++seed) {
    if (visited[seed]) continue;
    std::vector<uint32_t> queue{static_cast<uint32_t>(seed)};
    visited[seed] = true;

    while (!queue.empty()) {
      const uint32_t cur = queue.back();
      queue.pop_back();

      for (int k = 0; k < 3; ++k) {
        const uint32_t v = mesh->indices[cur * 3 + k];
        for (uint32_t nb : vert_faces[v]) {
          if (nb == cur || visited[nb]) continue;
          if (face_normals[cur].dot(face_normals[nb]) < 0.0) {
            std::swap(mesh->indices[nb * 3 + 1], mesh->indices[nb * 3 + 2]);
            face_normals[nb] = -face_normals[nb];
            ++flipped;
          }
          visited[nb] = true;
          queue.push_back(nb);
        }
      }
    }
  }

  return flipped;
}

// 有符号体积（散度定理）：Σ (a · (b × c)) / 6。
//
// 对**闭合且定向一致**的网格，正号 = 外法向朝外，负号 = 朝内。
// 这是判定整体朝向最稳健的判据：不像「面法向·(面心−质心)」那样
// 依赖物体凸性（凹形 / 空心物体上会给出错误结论），也不受平移影响。
//
// 对**开放网格**（有边界环），该值没有严格几何意义，但在实践中
// 仍能反映「多数面片朝向哪一侧」—— 我们只在 |V| 显著非零时才采信。
double SignedVolume(const TriangleMesh& mesh) {
  double vol = 0.0;
  const size_t tri_count = mesh.indices.size() / 3;
  for (size_t t = 0; t < tri_count; ++t) {
    const Vec3d& a = mesh.positions[mesh.indices[t * 3 + 0]];
    const Vec3d& b = mesh.positions[mesh.indices[t * 3 + 1]];
    const Vec3d& c = mesh.positions[mesh.indices[t * 3 + 2]];
    vol += a.dot(b.cross(c));
  }
  return vol / 6.0;
}

// 整体朝外定向（按连通分量，基于有符号体积）。
//
// **为什么必须有这个步骤**：`OrientFacesConsistently` 只保证**相邻面**
// 绕序互相一致（局部一致），**完全不保证整体朝外** —— 整片网格可以
// 「一致地朝内」。这不是理论隐患：实测一个标准经纬球，36480 个面全部
// 一致朝内（n·外法向 = −1.0000），缺失这一步会让下游全部失效：
//   · S9 纹理烘焙：`cos_ang < 0` → `w_ang = cos^α ≈ 0` → 权重塌到
//     `min_weight_sum` 以下 → 整片纹理被写成**黑色**（RMSE 0.51）；
//   · 导出/渲染：背面剔除把整个物体剔掉。
//
// **为什么必须按分量**（而不是整体一次翻转）：
// `OrientFacesConsistently` 只保证**同一传播块内**的绕序自洽，它不改变块的
// 绝对朝向 —— 因此**每个分量/块的整体朝向仍是任意的**（泊松输出实测就是
// 大体一致朝内）。泊松输出的外推区会把网格碎成大量连通分量（实测球面
// 12000 点 / depth=7：134k 三角形 → **7381 个分量**，最大分量仅占 ~37%），
// 此时「按全局体积翻转」只能让多数分量朝外，剩余分量仍可能朝内。
// 规范 docs/06 §2.6 第 2 步明确要求**逐分量**判定。
//
// **判据**：对每个分量累加面积加权的面朝向符号
//   `S = Σ area(t) · sign( n(t) · (center(t) − G) )`，`G` = 网格质心。
// 取 `G` 为「顶点平均」而不是包围盒中心：对近似星形（凸 / 球状）物体，
// 该判据对**闭合与开口分量都有效** —— 这对泊松的碎片化输出至关重要
// （开口碎片的「有符号体积」没有严格几何意义，会误判）。
//
// **不动几何**：只交换索引顺序。
//
// @return 翻转的三角形数（含整体翻转的全部分量面）。
uint32_t OrientMeshOutward(TriangleMesh* mesh) {
  if (mesh == nullptr || mesh->indices.size() < 3) return 0;
  const size_t tri_count = mesh->indices.size() / 3;

  // **不调用 `OrientFacesConsistently`**。
  //
  // 这是本函数最关键的一条经验结论（2026-10-30 实测）：
  // 「先传播统一绕序、再按分量翻朝外」的效果**显著更差**
  // （球面：传播 + 定向 → 面积朝外 67.6%；**不传播** + 定向 → 87.8%）。
  //
  // 原因：2 面共享边图会把**物理上互不相邻的面片**经「恰好共用一条边」
  // 拼进同一个传播块（泊松外推区尤其严重），这些**假邻接边**上的方向关系
  // 不能代表全局朝向；沿它传播会把原本整齐朝内（主块 n̂·ĉ ≈ −0.86）的
  // 网格在**块内**打散成互相冲突的多块，之后按块整体翻转就再也修不回来。
  // 实测：raw 的 `绕序不一致`（AnalyzeMesh 口径）为 7623 条，但它们只占
  // 约 12% 面积，且不改变逐分量径向符号判定 —— **直接按分量翻即可**。
  //
  // 因此本函数只做：**逐分量、按面积加权径向符号，负则翻转整个分量**。

  // 质心（顶点平均）。
  Vec3d centroid = Vec3d::Zero();
  if (!mesh->positions.empty()) {
    for (const auto& p : mesh->positions) centroid += p;
    centroid /= static_cast<double>(mesh->positions.size());
  }

  // 3) 「恰有 2 个面的共享边」图上的连通分量（与 OrientFacesConsistently
  //    同口径 —— 它也只沿 2 面边传播）。
  std::map<EdgeKey, std::vector<uint32_t>> edge_faces;
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t i0 = mesh->indices[t * 3 + 0];
    const uint32_t i1 = mesh->indices[t * 3 + 1];
    const uint32_t i2 = mesh->indices[t * 3 + 2];
    if (i0 == i1 || i1 == i2 || i2 == i0) continue;
    edge_faces[MakeEdge(i0, i1)].push_back(static_cast<uint32_t>(t));
    edge_faces[MakeEdge(i1, i2)].push_back(static_cast<uint32_t>(t));
    edge_faces[MakeEdge(i2, i0)].push_back(static_cast<uint32_t>(t));
  }
  std::vector<std::vector<uint32_t>> adj(tri_count);
  for (const auto& kv : edge_faces) {
    if (kv.second.size() != 2) continue;
    adj[kv.second[0]].push_back(kv.second[1]);
    adj[kv.second[1]].push_back(kv.second[0]);
  }

  // 4) 逐分量：面积加权的朝向符号，负则翻转整个分量。
  uint32_t flipped = 0;
  std::vector<char> visited(tri_count, false);
  for (size_t seed = 0; seed < tri_count; ++seed) {
    if (visited[seed]) continue;
    std::vector<uint32_t> comp;
    std::vector<uint32_t> queue{static_cast<uint32_t>(seed)};
    visited[seed] = true;
    while (!queue.empty()) {
      const uint32_t cur = queue.back();
      queue.pop_back();
      comp.push_back(cur);
      for (uint32_t nb : adj[cur]) {
        if (!visited[nb]) {
          visited[nb] = true;
          queue.push_back(nb);
        }
      }
    }

    // 面积加权的朝向符号。
    double score = 0.0;
    for (uint32_t t : comp) {
      const Vec3d& a = mesh->positions[mesh->indices[t * 3 + 0]];
      const Vec3d& b = mesh->positions[mesh->indices[t * 3 + 1]];
      const Vec3d& c = mesh->positions[mesh->indices[t * 3 + 2]];
      const Vec3d n = (b - a).cross(c - a);           // 未归一化 = 2·面积·法向
      const Vec3d center = (a + b + c) / 3.0;
      const Vec3d radial = center - centroid;
      if (radial.norm() < 1e-15) continue;
      score += n.dot(radial);
    }
    if (score < 0.0) {
      for (uint32_t t : comp) {
        std::swap(mesh->indices[t * 3 + 1], mesh->indices[t * 3 + 2]);
        ++flipped;
      }
    }
  }

  return flipped;
}

void RecomputeVertexNormals(TriangleMesh* mesh) {
  if (mesh == nullptr) return;
  const size_t vertex_count = mesh->positions.size();
  mesh->normals.assign(vertex_count, Vec3d::Zero());
  const size_t tri_count = mesh->indices.size() / 3;
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t i0 = mesh->indices[t * 3 + 0];
    const uint32_t i1 = mesh->indices[t * 3 + 1];
    const uint32_t i2 = mesh->indices[t * 3 + 2];
    // 面法向按面积加权累加到顶点。
    const Vec3d n = (mesh->positions[i1] - mesh->positions[i0])
                        .cross(mesh->positions[i2] - mesh->positions[i0]);
    mesh->normals[i0] += n;
    mesh->normals[i1] += n;
    mesh->normals[i2] += n;
  }

  for (auto& n : mesh->normals) {
    if (n.norm() > 1e-15) {
      n.normalize();
    } else {
      n = Vec3d(0.0, 0.0, 1.0);
    }
  }
}

// ---------------------------------------------------------------------------
// S4：非流形边修复（顶点分裂）
// ---------------------------------------------------------------------------



// 移除孤立面片：连通分量中只含单个三角形的那些面。
//
// **背景**：泊松 MC 在外扩区会产出孤立的三角形碎片 —— 它的 3 条边都只有
// 它自己一个面使用，因此这 3 条边全是边界边 → 看起来像一个「三角洞」。
// 但它不是洞，而是一片噪声碎片。
//
// **为什么必须删而不是补**：给碎片补一个面会造出与它**完全重合**的面
// （两个面在共享边上同向 = 在边的同一侧）→ 绕序不一致 + 穿模几何。
// 实测（球面 depth=9）：不删碎片时，补洞后绕序不一致边 5541 条
// （≈ 填充面总数），且补出的盖子全部穿进网格内部。
//
// 删掉碎片后，它的 3 条边不再有任何面 → 边界环自然消失，
// 无需再补 → 既没有穿模几何，也没有绕序冲突。
//
// **不动几何**：只删三角形，不改任何顶点位置。
//
// @return 被删除的三角形数
uint32_t RemoveIsolatedFacePatches(TriangleMesh* mesh) {
  if (mesh == nullptr || mesh->indices.size() < 3) return 0;
  const size_t tri_count = mesh->indices.size() / 3;

  // 边 → 关联面。
  std::map<EdgeKey, std::vector<uint32_t>> edge_faces;
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t i0 = mesh->indices[t * 3 + 0];
    const uint32_t i1 = mesh->indices[t * 3 + 1];
    const uint32_t i2 = mesh->indices[t * 3 + 2];
    if (i0 == i1 || i1 == i2 || i2 == i0) continue;
    edge_faces[MakeEdge(i0, i1)].push_back(static_cast<uint32_t>(t));
    edge_faces[MakeEdge(i1, i2)].push_back(static_cast<uint32_t>(t));
    edge_faces[MakeEdge(i2, i0)].push_back(static_cast<uint32_t>(t));
  }

  // 面邻接（共享边）。
  std::vector<std::vector<uint32_t>> face_adj(tri_count);
  for (const auto& kv : edge_faces) {
    const std::vector<uint32_t>& fs = kv.second;
    for (size_t i = 0; i < fs.size(); ++i) {
      for (size_t j = i + 1; j < fs.size(); ++j) {
        face_adj[fs[i]].push_back(fs[j]);
        face_adj[fs[j]].push_back(fs[i]);
      }
    }
  }

  // 求连通分量，删除只有 1 个面的分量。
  std::vector<bool> visited(tri_count, false);
  std::vector<bool> drop(tri_count, false);
  uint32_t removed = 0;
  for (size_t seed = 0; seed < tri_count; ++seed) {
    if (visited[seed]) continue;
    std::vector<uint32_t> comp{static_cast<uint32_t>(seed)};
    visited[seed] = true;
    for (size_t i = 0; i < comp.size(); ++i) {
      for (uint32_t nb : face_adj[comp[i]]) {
        if (visited[nb]) continue;
        visited[nb] = true;
        comp.push_back(nb);
      }
    }
    if (comp.size() == 1) {
      drop[comp[0]] = true;
      ++removed;
    }
  }

  if (removed == 0) return 0;

  std::vector<uint32_t> kept;
  kept.reserve(mesh->indices.size());
  for (size_t t = 0; t < tri_count; ++t) {
    if (drop[t]) continue;
    kept.push_back(mesh->indices[t * 3 + 0]);
    kept.push_back(mesh->indices[t * 3 + 1]);
    kept.push_back(mesh->indices[t * 3 + 2]);
  }
  mesh->indices = std::move(kept);
  return removed;
}

// ---------------------------------------------------------------------------
// 面绕序统一
// ---------------------------------------------------------------------------

// 判断面 f 中，无向边 (u,v)（u<v）是按 u→v 还是 v→u 的方向被使用的。
// 返回 +1 / -1；不是该面的边时返回 0。
int EdgeDirInFace(const TriangleMesh& mesh, uint32_t f, uint32_t u,
                   uint32_t v) {
  const uint32_t a = mesh.indices[f * 3 + 0];
  const uint32_t b = mesh.indices[f * 3 + 1];
  const uint32_t c = mesh.indices[f * 3 + 2];
  if ((a == u && b == v) || (b == u && c == v) || (c == u && a == v)) return 1;
  if ((a == v && b == u) || (b == v && c == u) || (c == v && a == u)) return -1;
  return 0;
}

// 统一面绕序：保证**相邻面以相反方向共用共享边**。
//
// **为什么需要**：Bourke 标准 MC 表在 15 个歧义 case 上只固定了一种连接方式，
// 相邻体素之间可能出现三角形绕序不一致。绕序不一致会连带破坏下游的一切
// 局部一致性判据：
//   · 边界环的「侧别」判据失效 → 两条边界环被混成一条 → 非简单环 → 无法补洞；
//   · 法向逐顶点重算得到正负混杂的法向场。
//
// **算法**（两遍）：
//   ① **诊断传播** —— 按 2 面共享边做 BFS，自种子面起逐面比较共享边方向，
//      方向相同（绕序冲突）则标记该邻居「相对种子需翻转」；否则不翻。**这一遍
//      不改动网格**，只得到每个面的 `rel[t] ∈ {不翻, 翻}`。
//   ② **按块多数表决** —— 对每个传播块（同一个 BFS 连通块）统计「不翻」与
//      「翻」的**面积**，取面积大的一侧作为该块基准方向，并**保留块原有的
//      多数朝向**（即：面积大的一侧不翻），只翻转少数侧的面。这样只消除真
//      正的绕序错误，而不改变网格的整体朝向。
//
// **为什么必须做多数表决，而不是「保持种子朝向」**：
//   早期实现从「编号最小的种子面」开始 BFS 并**保持种子朝向**，等价于把种子
//   的方向强加给整块。泊松/外推输出的面片高度碎片化，2 面共享边图会把
//   **大量互不相邻的面片**经「恰好共用一条边」拼成同一个传播块；这条路径上
//   的方向关系不能代表全局朝向，于是「种子朝向」会把整块翻错。
//   实测（球面 12000 点 / depth=7，134722 面）：泊松 MC 原始输出**大体一致
//   朝内**（主块全体 n̂·ĉ ≈ −0.86），旧的「保持种子」实现把面积朝外从
//   87.8% 打散到 **66.8%**、强朝内面积从 7.5% 抬到 **27.2%**。
//   改为「按块多数表决」后应回落至与「不做任何传播」相当的水平。
//
// 被 ≥ 3 个面共用的非流形边不建立约束（该处绕序关系本就不确定）。
//
// **不动几何**：只换索引顺序，**顶点位置一个都不改** → 几何精度零损失。
//
// @return 被翻转的三角形数
uint32_t OrientFacesConsistently(TriangleMesh* mesh) {
  if (mesh == nullptr || mesh->indices.size() < 3) return 0;
  const size_t tri_count = mesh->indices.size() / 3;

  // 边 → 关联面（只统计恰有 2 个面的边）。
  std::map<EdgeKey, std::vector<uint32_t>> edge_faces;
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t i0 = mesh->indices[t * 3 + 0];
    const uint32_t i1 = mesh->indices[t * 3 + 1];
    const uint32_t i2 = mesh->indices[t * 3 + 2];
    if (i0 == i1 || i1 == i2 || i2 == i0) continue;
    edge_faces[MakeEdge(i0, i1)].push_back(static_cast<uint32_t>(t));
    edge_faces[MakeEdge(i1, i2)].push_back(static_cast<uint32_t>(t));
    edge_faces[MakeEdge(i2, i0)].push_back(static_cast<uint32_t>(t));
  }

  // 面面积（多数表决的权重；退化面面积为 0，不影响表决）。
  auto face_area = [&](uint32_t t) {
    const Vec3d& a = mesh->positions[mesh->indices[t * 3 + 0]];
    const Vec3d& b = mesh->positions[mesh->indices[t * 3 + 1]];
    const Vec3d& c = mesh->positions[mesh->indices[t * 3 + 2]];
    return 0.5 * (b - a).cross(c - a).norm();
  };

  uint32_t flipped = 0;
  std::vector<int8_t> visited(tri_count, 0);  // 0 未访问，1 已访问

  for (size_t seed = 0; seed < tri_count; ++seed) {
    if (visited[seed]) continue;

    // ① 诊断传播：收集块内所有面及其「相对种子是否需翻转」的标记。
    std::vector<uint32_t> block;
    std::vector<uint32_t> queue{static_cast<uint32_t>(seed)};
    std::map<uint32_t, char> rel;  // face → 1 需翻 / 0 不翻
    rel[static_cast<uint32_t>(seed)] = 0;
    visited[seed] = 1;

    while (!queue.empty()) {
      const uint32_t cur = queue.back();
      queue.pop_back();
      block.push_back(cur);
      const char cur_rel = rel[cur];
      const uint32_t cv[3] = {mesh->indices[cur * 3 + 0],
                              mesh->indices[cur * 3 + 1],
                              mesh->indices[cur * 3 + 2]};
      for (int k = 0; k < 3; ++k) {
        const uint32_t a = cv[k];
        const uint32_t b = cv[(k + 1) % 3];
        if (a == b) continue;
        auto fit = edge_faces.find(MakeEdge(a, b));
        if (fit == edge_faces.end() || fit->second.size() != 2) continue;
        const uint32_t nb = (fit->second[0] == cur) ? fit->second[1]
                                                    : fit->second[0];
        if (nb == cur || visited[nb]) continue;
        // 两个面在同一条边上方向相同 → 绕序冲突 → 邻居相对当前面需翻转。
        const int d1 = EdgeDirInFace(*mesh, cur, a, b);
        const int d2 = EdgeDirInFace(*mesh, nb, a, b);
        const char nb_rel = (d1 != 0 && d2 != 0 && d1 == d2)
                                ? static_cast<char>(cur_rel ^ 1)
                                : cur_rel;
        rel[nb] = nb_rel;
        visited[nb] = 1;
        queue.push_back(nb);
      }
    }

    // ② 面积多数表决。
    //
    // `rel[t]` 的解满足所有一致性约束，通解为 `x[t] = rel[t] ^ c`（c ∈ {0,1}
    // 为整块的全局翻转）。两种取法给出**同一个**「块内一致」的结果，差别只在
    // 块的绝对朝向。为「只修真正的错误、不无谓翻面」，取**改动面积最小**的
    // 那一侧 —— 即只翻转面积占少数的面：
    //   · c=0 → 翻转 rel=1 的面（改动面积 = area_flip）；
    //   · c=1 → 翻转 rel=0 的面（改动面积 = area_keep）。
    // 若块本来就内部一致（泊松 MC 的常见情形），所有 rel=0 → area_flip=0
    // → 一个面都不动（无扰动）。
    double area_keep = 0.0, area_flip = 0.0;
    for (uint32_t t : block) {
      const double a = face_area(t);
      if (rel[t]) area_flip += a; else area_keep += a;
    }
    const bool flip_minority_is_rel1 = (area_flip <= area_keep);
    for (uint32_t t : block) {
      const bool should_flip =
          flip_minority_is_rel1 ? (rel[t] != 0) : (rel[t] == 0);
      if (!should_flip) continue;
      std::swap(mesh->indices[t * 3 + 1], mesh->indices[t * 3 + 2]);
      ++flipped;
    }
  }
  return flipped;
}


// 局部绕序修复（**非传播**）：按「邻面多数表决」翻转个别绕序错误的面。
//
// **与 `OrientFacesConsistently` 的根本区别**：
// 后者用 BFS 从种子面**传播**，把种子朝向强加给整个连通块。泊松输出里
// 「恰好共用一条边」的**假邻接边**（两个物理上无关的面片）会把互不相邻的
// 面片串成同一个块，沿这些边传播会把错误扩散、把整块打散 —— 实测把面积
// 朝外从 87.6% 打到 66.8%，且**不可逆**（此后按分量整体翻转无法修复块内
// 已翻错的面）。
//
// 本函数**不传播**：只对每个面统计其（沿 2 面共享边的）邻面中「绕序冲突」
// 的数量，冲突**占多数**时才翻转该面。效果：
//   · 内部一致的分量（主分量）→ 无面被翻 → **朝向零损失**；
//   · 孤立翻错的面（邻面全体冲突）→ 被修正。
// 迭代至收敛（每轮同时决定、同时应用），完全不动几何。
//
// @return 被翻转的三角形数
uint32_t FixLocalWindingErrors(TriangleMesh* mesh, int max_rounds) {
  if (mesh == nullptr || mesh->indices.size() < 3) return 0;
  const size_t tri_count = mesh->indices.size() / 3;
  uint32_t total_flipped = 0;

  for (int round = 0; round < max_rounds; ++round) {
    // 边 → 关联面（只统计恰有 2 个面的边）。
    std::map<EdgeKey, std::vector<uint32_t>> edge_faces;
    for (size_t t = 0; t < tri_count; ++t) {
      const uint32_t i0 = mesh->indices[t * 3 + 0];
      const uint32_t i1 = mesh->indices[t * 3 + 1];
      const uint32_t i2 = mesh->indices[t * 3 + 2];
      if (i0 == i1 || i1 == i2 || i2 == i0) continue;
      edge_faces[MakeEdge(i0, i1)].push_back(static_cast<uint32_t>(t));
      edge_faces[MakeEdge(i1, i2)].push_back(static_cast<uint32_t>(t));
      edge_faces[MakeEdge(i2, i0)].push_back(static_cast<uint32_t>(t));
    }

    // 逐面统计：与邻面「绕序冲突」（同向共用边）的条数 / 一致的条数。
    std::vector<uint32_t> conflict(tri_count, 0), agree(tri_count, 0);
    for (const auto& kv : edge_faces) {
      if (kv.second.size() != 2) continue;
      const uint32_t f0 = kv.second[0], f1 = kv.second[1];
      const uint32_t a = kv.first.a, b = kv.first.b;
      const int d0 = EdgeDirInFace(*mesh, f0, a, b);
      const int d1 = EdgeDirInFace(*mesh, f1, a, b);
      if (d0 == 0 || d1 == 0) continue;
      if (d0 == d1) {  // 同向 → 绕序冲突
        ++conflict[f0];
        ++conflict[f1];
      } else {
        ++agree[f0];
        ++agree[f1];
      }
    }

    // 同时决定要翻转的面：冲突数严格多于一致数。
    std::vector<uint32_t> to_flip;
    for (size_t t = 0; t < tri_count; ++t) {
      if (conflict[t] > agree[t] && (conflict[t] + agree[t]) > 0) {
        to_flip.push_back(static_cast<uint32_t>(t));
      }
    }
    if (to_flip.empty()) break;

    for (uint32_t t : to_flip) {
      std::swap(mesh->indices[t * 3 + 1], mesh->indices[t * 3 + 2]);
    }
    total_flipped += static_cast<uint32_t>(to_flip.size());
  }

  return total_flipped;
}

// ---------------------------------------------------------------------------
// 非流形顶点分裂
// ---------------------------------------------------------------------------

// 非流形顶点分裂：把「边界度 ≥ 4」的顶点按**扇区**拆成多个顶点。
//
// **背景**：一个顶点处的关联面按共享边可分成若干连续的**扇区**。
// 合法流形曲面上，每个扇区贡献 0 或 2 条边界边，因此
// **边界度 = 2 × 扇区数**。边界度 ≥ 4 意味着该顶点处有 ≥ 2 个扇区 ——
// 曲面在这个点**本身非流形**（两个曲面片在该点交汇）。
//
// **为什么必须修**：两个扇区各自带一条边界环，两条环在该顶点**相交**。
// 走环时无法把两条环分开（无论用几何直行、转向量符号还是侧别判据，
// 都会把两条环混成一条）→ 环内出现重复顶点 → 非简单环 → 无法补洞。
// 实测（球面 depth=9）：边界度分布 度2=6152、**度4=297**、度6=7；
// 走环判据改进后仍残留 54 个非简单环。
//
// **修法**：把每个扇区分配到独立的顶点副本。分裂后两条边界环不再共享
// 顶点 → 各自成为简单环 → 可以正常补洞。
//
// **不动几何**：新顶点位置 = 原顶点位置 → 几何精度零损失。
//
// @return 被分裂的顶点数
uint32_t SplitNonManifoldVertices(TriangleMesh* mesh) {
  if (mesh == nullptr || mesh->indices.size() < 3) return 0;
  const size_t tri_count = mesh->indices.size() / 3;
  const size_t vertex_count = mesh->positions.size();

  // 边 → 关联面。
  std::map<EdgeKey, std::vector<uint32_t>> edge_faces;
  for (size_t t = 0; t < tri_count; ++t) {
    const uint32_t i0 = mesh->indices[t * 3 + 0];
    const uint32_t i1 = mesh->indices[t * 3 + 1];
    const uint32_t i2 = mesh->indices[t * 3 + 2];
    if (i0 == i1 || i1 == i2 || i2 == i0) continue;
    edge_faces[MakeEdge(i0, i1)].push_back(static_cast<uint32_t>(t));
    edge_faces[MakeEdge(i1, i2)].push_back(static_cast<uint32_t>(t));
    edge_faces[MakeEdge(i2, i0)].push_back(static_cast<uint32_t>(t));
  }

  // 顶点 → 关联面。
  std::vector<std::vector<uint32_t>> vert_faces(vertex_count);
  for (size_t t = 0; t < tri_count; ++t) {
    for (int k = 0; k < 3; ++k) {
      const uint32_t v = mesh->indices[t * 3 + k];
      if (v < vertex_count) vert_faces[v].push_back(static_cast<uint32_t>(t));
    }
  }

  // 顶点的边界度：关联的「只被 1 个面使用的边」的条数。
  std::vector<int> boundary_degree(vertex_count, 0);
  for (const auto& kv : edge_faces) {
    if (kv.second.size() != 1) continue;
    ++boundary_degree[kv.first.a];
    ++boundary_degree[kv.first.b];
  }

  uint32_t split_count = 0;
  std::vector<Vec3d> new_positions = mesh->positions;
  std::vector<Vec3d> new_normals = mesh->normals;

  for (uint32_t v = 0; v < vertex_count; ++v) {
    if (boundary_degree[v] < 4) continue;  // 度 2 = 单扇区，无需分裂
    if (vert_faces[v].size() < 2) continue;

    // 该顶点的面在「共享含 v 的边」意义下的邻接。
    // 面的两条含 v 的边 → 各对应一个（或多个）邻居面。
    auto face_neighbors = [&](uint32_t f, uint32_t* out) -> int {
      int cnt = 0;
      for (int k = 0; k < 3; ++k) {
        const uint32_t a = mesh->indices[f * 3 + k];
        const uint32_t b = mesh->indices[f * 3 + (k + 1) % 3];
        if (a != v && b != v) continue;  // 该边不含 v
        auto fit = edge_faces.find(MakeEdge(a, b));
        if (fit == edge_faces.end()) continue;
        for (uint32_t g : fit->second) {
          if (g == f) continue;
          bool touches_v = false;
          for (int q = 0; q < 3; ++q) {
            if (mesh->indices[g * 3 + q] == v) touches_v = true;
          }
          if (!touches_v) continue;
          if (cnt < 2) out[cnt] = g;
          ++cnt;
        }
      }
      return cnt;
    };

    // 按扇区遍历面环。
    std::vector<bool> face_done(tri_count, false);
    std::vector<std::vector<uint32_t>> sectors;
    for (uint32_t seed : vert_faces[v]) {
      if (face_done[seed]) continue;
      std::vector<uint32_t> sector;
      uint32_t cur = seed;
      for (int guard = 0; guard < 1024; ++guard) {
        if (face_done[cur]) break;
        face_done[cur] = true;
        sector.push_back(cur);

        // 找出邻居面（共享含 v 的边）。
        uint32_t nb[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
        const int nn = face_neighbors(cur, nb);
        if (nn == 0) break;
        // 选择继续方向：优先不是已访问（来时）的那个。
        uint32_t next = 0xFFFFFFFFu;
        if (nn == 1) {
          next = nb[0];
        } else {
          // cur 的两个邻居：一个是来时的（可能已访问），另一个是继续方向。
          for (int k = 0; k < 2; ++k) {
            if (nb[k] == 0xFFFFFFFFu) continue;
            if (!face_done[nb[k]]) {
              next = nb[k];
              break;
            }
          }
          if (next == 0xFFFFFFFFu) break;
        }
        if (next == seed) {
          sector.push_back(next);
          face_done[next] = true;
          break;
        }
        cur = next;
      }
      if (!sector.empty()) sectors.push_back(std::move(sector));
    }

    if (sectors.size() < 2) continue;  // 单扇区 → 不是非流形顶点

    // 第一个扇区保留原顶点，其余扇区各分配一个新顶点。
    for (size_t si = 1; si < sectors.size(); ++si) {
      const uint32_t nid = static_cast<uint32_t>(new_positions.size());
      new_positions.push_back(mesh->positions[v]);
      if (v < mesh->normals.size()) new_normals.push_back(mesh->normals[v]);
      for (uint32_t f : sectors[si]) {
        for (int k = 0; k < 3; ++k) {
          if (mesh->indices[f * 3 + k] == v) mesh->indices[f * 3 + k] = nid;
        }
      }
      ++split_count;
    }
  }

  if (split_count > 0) {
    mesh->positions = std::move(new_positions);
    mesh->normals = std::move(new_normals);
    if (!mesh->confidences.empty()) {
      mesh->confidences.resize(mesh->positions.size(), 1.0f);
    }
  }
  return split_count;
}

uint32_t RepairNonManifoldEdges(TriangleMesh* mesh) {
  if (mesh == nullptr || mesh->indices.size() < 3) return 0;

  const size_t tri_count = mesh->indices.size() / 3;

  // 逐轮迭代，直到无非流形边或达到轮数上限。
  //
  // **为什么必须迭代**：一次拆分只处理一层 —— 拆掉当前最外层的非流形连接后，
  // 内层的多面共享边才会暴露出来。实测单轮只能把 15181 降到 8554，
  // 迭代后才可能收敛到 0。
  //
  // 上限取 8 轮：MC 的棱最多 3 个面共享，理论上 1~2 轮即可收敛；
  // 8 轮留足余量，同时防止病态网格导致无限循环。
  uint32_t total_split = 0;
  constexpr int kMaxRounds = 8;

  for (int round = 0; round < kMaxRounds; ++round) {
    // 1. 边 → 关联面表。
    std::map<EdgeKey, std::vector<uint32_t>> edge_faces;
    for (size_t t = 0; t < tri_count; ++t) {
      const uint32_t i0 = mesh->indices[t * 3 + 0];
      const uint32_t i1 = mesh->indices[t * 3 + 1];
      const uint32_t i2 = mesh->indices[t * 3 + 2];
      if (i0 == i1 || i1 == i2 || i2 == i0) continue;
      edge_faces[MakeEdge(i0, i1)].push_back(static_cast<uint32_t>(t));
      edge_faces[MakeEdge(i1, i2)].push_back(static_cast<uint32_t>(t));
      edge_faces[MakeEdge(i2, i0)].push_back(static_cast<uint32_t>(t));
    }

    // 2. 找出非流形边。
    std::vector<EdgeKey> bad_edges;
    for (const auto& kv : edge_faces) {
      if (kv.second.size() > 2) bad_edges.push_back(kv.first);
    }
    if (bad_edges.empty()) break;  // 收敛

    std::vector<Vec3d> new_positions = mesh->positions;
    std::vector<Vec3d> new_normals = mesh->normals;
    uint32_t split_this_round = 0;

    for (const auto& edge : bad_edges) {
      auto efit = edge_faces.find(edge);
      if (efit == edge_faces.end()) continue;
      const std::vector<uint32_t>& faces = efit->second;
      if (faces.size() <= 2) continue;

      const uint32_t u = edge.a;

      // 在 u 处建立扇形邻接：两面相邻 ⟺ 共享一条含 u 的边（不含公共边 (u,v)）。
      //
      // **这是 MC 的情形，不是 MT 的对角线情形**：
      //   MC 只用体素的 12 条棱，歧义 case 下最多 3 个面共享一条棱。
      //   它们在棱的两个端点处**不一定**扇形连续。
      //
      // 做法：把面按「在 u 处的邻边」做 BFS，找出扇形连通块；
      //       同一连通块内的面可以共用边 (u,v)（它们本该连续）；
      //       其它连通块的面必须拆开。
      const auto face_edges_at_u = [&](uint32_t f, EdgeKey* out) {
        int cnt = 0;
        for (int k = 0; k < 3; ++k) {
          const uint32_t idx = mesh->indices[f * 3 + k];
          if (idx == u) continue;
          if (cnt == 0) {
            out[0] = MakeEdge(u, idx);
          } else if (cnt == 1) {
            out[1] = MakeEdge(u, idx);
          }
          ++cnt;
        }
        return cnt;
      };

      std::unordered_map<uint32_t, uint32_t> face_slot;
      for (uint32_t i = 0; i < faces.size(); ++i) {
        face_slot[faces[i]] = i;
      }

      std::vector<uint8_t> visited(faces.size(), 0);
      for (uint32_t seed = 0; seed < faces.size(); ++seed) {
        if (visited[seed]) continue;

        // BFS 收集一个扇形连通块。
        std::vector<uint32_t> block;
        std::vector<uint32_t> stack{faces[seed]};
        visited[seed] = 1;
        while (!stack.empty()) {
          const uint32_t cur = stack.back();
          stack.pop_back();
          block.push_back(cur);
          EdgeKey ce[2] = {{0, 0}, {0, 0}};
          face_edges_at_u(cur, ce);
          for (uint32_t cand : faces) {
            const uint32_t ci = face_slot[cand];
            if (visited[ci]) continue;
            EdgeKey cde[2] = {{0, 0}, {0, 0}};
            face_edges_at_u(cand, cde);
            bool adjacent = false;
            for (int a = 0; a < 2 && !adjacent; ++a) {
              for (int b = 0; b < 2; ++b) {
                if (ce[a] == cde[b]) {
                  adjacent = true;
                  break;
                }
              }
            }
            if (!adjacent) continue;
            visited[ci] = 1;
            stack.push_back(cand);
          }
        }

        // 连通块内：前 2 个面保留边 (u,v)，其余拆开。
        // 拆开的面共用一个新顶点（保持块内其它边的连通性）。
        const size_t keep = std::min<size_t>(2, block.size());
        if (block.size() <= keep) continue;

        const uint32_t nid = static_cast<uint32_t>(new_positions.size());
        new_positions.push_back(mesh->positions[u]);
        if (u < mesh->normals.size()) {
          new_normals.push_back(mesh->normals[u]);
        }

        for (size_t i = keep; i < block.size(); ++i) {
          const uint32_t f = block[i];
          for (int k = 0; k < 3; ++k) {
            if (mesh->indices[f * 3 + k] == u) {
              mesh->indices[f * 3 + k] = nid;
            }
          }
          ++split_this_round;
        }
      }
    }

    if (split_this_round == 0) break;  // 无法再拆，退出避免死循环

    total_split += split_this_round;
    mesh->positions = std::move(new_positions);
    mesh->normals = std::move(new_normals);
    if (!mesh->confidences.empty()) {
      mesh->confidences.resize(mesh->positions.size(), 1.0f);
    }
  }

  return total_split;
}

// ---------------------------------------------------------------------------
// S5：孔洞缝合
// ---------------------------------------------------------------------------

HoleFillReport FillHoles(TriangleMesh* mesh, const HoleFillConfig& config) {
  HoleFillReport report;
  if (mesh == nullptr || mesh->indices.size() < 3) return report;

  // 总面积（面积比口径的基准，docs/06 §2.5）。
  double total_area = 0.0;
  const size_t tri_count = mesh->indices.size() / 3;
  for (size_t t = 0; t < tri_count; ++t) {
    const Vec3d& a = mesh->positions[mesh->indices[t * 3 + 0]];
    const Vec3d& b = mesh->positions[mesh->indices[t * 3 + 1]];
    const Vec3d& c = mesh->positions[mesh->indices[t * 3 + 2]];
    total_area += 0.5 * (b - a).cross(c - a).norm();
  }
  report.total_area = total_area;
  if (total_area <= 0.0) return report;

  const std::vector<BoundaryLoop> loops = CollectBoundaryLoops(*mesh);
  report.loop_count = static_cast<uint32_t>(loops.size());

  // 可变边表（边 → 面数）。填充时逐边核对并增量更新，
  // 保证补洞**永不**产生非流形边。
  std::map<EdgeKey, int> edge_count = CollectEdgeFaceCounts(*mesh);
  // 边 → 使用该边的第一个面（绕序判据要用）。
  std::map<EdgeKey, uint32_t> edge_first_face;
  for (size_t t = 0; t < mesh->indices.size() / 3; ++t) {
    const uint32_t i0 = mesh->indices[t * 3 + 0];
    const uint32_t i1 = mesh->indices[t * 3 + 1];
    const uint32_t i2 = mesh->indices[t * 3 + 2];
    if (i0 == i1 || i1 == i2 || i2 == i0) continue;
    const uint32_t ft = static_cast<uint32_t>(t);
    edge_first_face.emplace(MakeEdge(i0, i1), ft);
    edge_first_face.emplace(MakeEdge(i1, i2), ft);
    edge_first_face.emplace(MakeEdge(i2, i0), ft);
  }

  // 分级计数（H0~H4 + 不可填），用于验收报告与问题定位。
  uint32_t level_count[5] = {0, 0, 0, 0, 0};
  uint32_t level_unfillable = 0;

  // 分级判定（docs/06 §2.5 的 H0~H4）+ 逐洞填充。
  for (const auto& loop : loops) {
    const double ratio = loop.area / total_area;
    HoleInfo info;
    info.area = loop.area;
    info.area_ratio = ratio;
    info.loop_length = static_cast<uint32_t>(loop.vertices.size());
    info.plane_rms = loop.plane_rms;
    info.centroid = loop.centroid;

    // 开放链 / 顶点不足 3 / 非简单环 → 无法用三角形闭合，原样保留。
    if (!loop.closed || loop.vertices.size() < 3) {
      info.level = HoleLevel::kThrough;
      info.filled = false;
      report.holes.push_back(info);
      ++report.unfilled_count;
      continue;
    }
    if (!loop.simple) {
      // 环含重复顶点（边界度 ≥ 3 的 T 型接点）→ 多边形三角化无意义，
      // 强行填充会漏掉部分边界边，反而多出边界环。宁可留洞。
      info.level = HoleLevel::kNonSimple;
      info.filled = false;
      report.holes.push_back(info);
      ++report.unfilled_count;
      ++report.fill_failed_count;
      ++report.fill_failed_non_simple;
      ++level_unfillable;
      continue;
    }

    // H4：跨越明显曲面 → 不填。
    if (loop.plane_rms > config.max_plane_rms_factor * config.resolution) {
      info.level = HoleLevel::kThrough;
      info.filled = false;
      report.holes.push_back(info);
      ++report.unfilled_count;
      continue;
    }

    // H3：超大孔 → 不自动填（需局部二次泊松）。
    if (ratio > config.large_area_ratio) {
      info.level = HoleLevel::kLarge;
      info.filled = false;
      report.holes.push_back(info);
      ++report.unfilled_count;
      continue;
    }

    if (ratio <= config.micro_area_ratio && loop.vertices.size() <= 3) {
      info.level = HoleLevel::kMicro;
    } else if (ratio <= config.small_area_ratio &&
               loop.plane_rms <= config.small_plane_rms_factor * config.resolution) {
      info.level = HoleLevel::kSmall;
    } else {
      info.level = HoleLevel::kMedium;
    }

    // 填充（复用原边界顶点 → 原边界边获得第 2 个面 → 孔洞真正闭合）。
    uint32_t added = 0;
    const FillStatus status =
        FillOneLoop(mesh, loop, &edge_count, &edge_first_face, &added);
    if (status == FillStatus::kOk && added > 0) {
      info.filled = true;
      ++report.filled_count;
    } else {
      info.filled = false;
      ++report.unfilled_count;
      ++report.fill_failed_count;
      switch (status) {
        case FillStatus::kDegenerateTriangle:
          ++report.fill_failed_degenerate;
          break;
        case FillStatus::kEarClipFailed:
        case FillStatus::kSelfIntersect:
          ++report.fill_failed_earclip;
          break;
        case FillStatus::kDiagonalConflict:
        case FillStatus::kEdgeConflict:
          ++report.fill_failed_diagonal;
          break;
        case FillStatus::kWindingConflict:
          ++report.fill_failed_winding;
          break;
        default:
          break;
      }
    }
    report.holes.push_back(info);
    if (static_cast<int>(info.level) < 5) {
      ++level_count[static_cast<int>(info.level)];
    } else {
      ++level_unfillable;
    }
  }
  report.level_micro = level_count[0];
  report.level_small = level_count[1];
  report.level_medium = level_count[2];
  report.level_large = level_count[3];
  report.level_through = level_count[4];
  report.level_unfillable = level_unfillable;

  // 补洞后剩余的边界环数（与 AnalyzeMesh 同口径）。
  // 与 unfilled_count 相等 ⇒ 「填了的洞真的闭合了，没填的洞原样保留」。
  report.remaining_loops =
      static_cast<uint32_t>(CollectBoundaryLoops(*mesh).size());

  // **补洞后兜底跑一次 S4**。
  //
  // 新版填充复用原边界顶点，正常情况下不会产生非流形连接；
  // 但填充面与原网格在「共享边界顶点」处仍可能构成多面共点，
  // 此时由 S4 拆分。该调用在无非流形边时**立即返回**（不改几何）。
  if (config.post_repair_enabled && report.filled_count > 0) {
    report.post_fill_split = RepairNonManifoldEdges(mesh);
  }

  return report;
}


// ---------------------------------------------------------------------------
// S7 三角面规整：边翻转
// ---------------------------------------------------------------------------

// 三角形的最小内角（度）。退化三角形返回 0。
double MinAngleOfTriangle(const Vec3d& a, const Vec3d& b, const Vec3d& c) {
  const double la = (b - a).norm();
  const double lb = (c - b).norm();
  const double lc = (a - c).norm();
  if (la < 1e-18 || lb < 1e-18 || lc < 1e-18) return 0.0;
  // 最短边所对的角最小。
  double opp = la, s1 = lb, s2 = lc;
  if (lb <= la && lb <= lc) {
    opp = lb;
    s1 = la;
    s2 = lc;
  } else if (lc <= la && lc <= lb) {
    opp = lc;
    s1 = la;
    s2 = lb;
  }
  double cosv = (s1 * s1 + s2 * s2 - opp * opp) / (2.0 * s1 * s2);
  cosv = std::clamp(cosv, -1.0, 1.0);
  return std::acos(cosv) * 180.0 / 3.14159265358979323846;
}

// S7 子步骤：**边翻转**改善最小内角。
//
// **为什么先做这一步**：它**不移动任何顶点**，只改变顶点的连接关系 →
// 几何精度**零损失**，也不改变边界环（只翻转恰有 2 个面的内部边）。
// 在「几何精度 > 拓扑完整性 > 纹理质量」的铁律下，这是唯一可以无条件做的
// 质量改善手段。切向平滑会移动顶点、边坍缩会改变形状，都留到后面。
//
// **判据**：对内部边 (a,b)，两个相邻三角形为 T1=(a,b,c)、T2=(b,a,d)。
// 翻转为 T1'=(c,d,a)、T2'=(d,c,b) 后，要求：
//   ① 新边 (c,d) 尚不存在 —— 否则会造出重复面 / 非流形边；
//   ② 两个新三角形的面积 > 0（非退化）；
//   ③ `min(新内角) > min(旧内角) + eps`（单调改善 → 不震荡，可迭代到收敛）。
//
// @param min_angle_deg 目标最小内角（低于此值才尝试翻转），标准档 9°
// @param max_rounds    最多迭代轮数
// @return 被翻转的边数

// S7 子步骤：**坍缩极短边**。
//
// **为什么需要**：MC 提取出的狭长面，成因**不是**「三顶点几乎共线」，
// 而是**极短边**。实测（球面 depth=7，12.4 万三角形）：
//   狭长面的最短边中位 **0.0537 mm = 0.062 × r**（约 1/16 个单元）
//   最长边中位 0.9193 mm = 1.07 × r（正常）
// 这种面**边翻转救不了** —— 四边形同样包含那条短边，翻转后依旧狭长
// （实测边翻转后仍剩 3.5%）。
//
// **精度代价（可忽略）**：坍缩把两个端点合并到**中点**，最大位移 =
// 边长 / 2 ≤ 0.125 × r（默认阈值 0.25·r）。而泊松的形状误差量级是
// **0.5 × r**，因此这是「用 1/4 的形状误差换掉一整类退化面」。
//
// **拓扑安全（必须满足 link condition）**：内部边 (a,b) 的**公共邻居数
// 必须恰为 2**（即两个相邻三角形的对面顶点 c、d）。否则坍缩会让某条边
// 被 3+ 个面共享 → 非流形边。
//
// @param max_edge_length 允许坍缩的最大边长（m），建议 0.25 × r
// @param max_rounds      最多迭代轮数
// @return 被坍缩的边数
uint32_t CollapseShortEdges(TriangleMesh* mesh, double max_edge_length,
                            int max_rounds) {
  if (mesh == nullptr || mesh->indices.size() < 3) return 0;
  if (max_edge_length <= 0.0) return 0;

  uint32_t total_collapsed = 0;

  for (int round = 0; round < max_rounds; ++round) {
    const size_t nv = mesh->positions.size();
    const size_t nt = mesh->indices.size() / 3;

    // 边 → 面数。
    std::map<EdgeKey, int> edge_count;
    for (size_t t = 0; t < nt; ++t) {
      const uint32_t i0 = mesh->indices[t * 3 + 0];
      const uint32_t i1 = mesh->indices[t * 3 + 1];
      const uint32_t i2 = mesh->indices[t * 3 + 2];
      if (i0 == i1 || i1 == i2 || i2 == i0) continue;
      if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
      edge_count[MakeEdge(i0, i1)]++;
      edge_count[MakeEdge(i1, i2)]++;
      edge_count[MakeEdge(i2, i0)]++;
    }

    // 顶点邻接（仅通过存在的边）。
    std::vector<std::set<uint32_t>> nbr(nv);
    for (const auto& kv : edge_count) {
      nbr[kv.first.a].insert(kv.first.b);
      nbr[kv.first.b].insert(kv.first.a);
    }

    // 边界顶点与边界度（关联的「只被 1 个面使用」的边数）。
    //
    // 合法流形边界上每个顶点恰有 2 条边界边。边界度 = 2 的顶点坍缩后
    // 仍保持 2 ✓；边界度 ≥ 4 的顶点是非流形顶点，**不能参与坍缩**。
    std::vector<int> boundary_degree(nv, 0);
    for (const auto& kv : edge_count) {
      if (kv.second != 1) continue;
      ++boundary_degree[kv.first.a];
      ++boundary_degree[kv.first.b];
    }

    // 挑选本轮要坍缩的边。
    //
    // **每个顶点每轮最多参与一次**（`touched`）。这是必需的：
    // `nbr` 是本轮开始时的快照，若允许顶点 a 连续合并 b、x、y…，
    // 后续的 link condition 就基于过时的邻接关系 → 失效 → 产生非流形边
    // （实测曾出现非流形边 ≠ 0）。
    std::vector<uint32_t> remap(nv);
    for (size_t i = 0; i < nv; ++i) remap[i] = static_cast<uint32_t>(i);
    std::vector<bool> touched(nv, false);
    uint32_t collapsed_this_round = 0;

    for (const auto& kv : edge_count) {
      const int face_count = kv.second;
      // 只处理**流形**边：内部边（2 面）或边界边（1 面）。
      // 被 3+ 个面共享的非流形边一律不动。
      if (face_count != 1 && face_count != 2) continue;

      const uint32_t a = kv.first.a;
      const uint32_t b = kv.first.b;
      if (remap[a] != a || remap[b] != b) continue;  // 本轮已被合并
      if (touched[a] || touched[b]) continue;        // 本轮已参与过合并

      const Vec3d& pa = mesh->positions[a];
      const Vec3d& pb = mesh->positions[b];
      if ((pa - pb).norm() > max_edge_length) continue;

      // 公共邻居数（同时与 a、b 相连的顶点）。
      int common = 0;
      for (uint32_t x : nbr[a]) {
        if (nbr[b].count(x) != 0) ++common;
      }

      if (face_count == 2) {
        // **内部边**：两端都必须是非边界顶点，公共邻居恰为 2（两个对面顶点）。
        // 否则坍缩后某条边会被 3+ 个面共享 → 非流形边；或把内部顶点
        // 拖到边界上 → 改变边界环。
        if (boundary_degree[a] != 0 || boundary_degree[b] != 0) continue;
        if (common != 2) continue;
      } else {
        // **边界边**：两端的边界度都必须恰为 2（合法流形边界），
        // 公共邻居恰为 1（唯一对面顶点）。
        //
        // 拓扑效果：环 `…e′—a—b—f′…` 变成 `…e′—a—f′…` ——
        // **环数不变、少一个顶点** ✓；若不允许坍缩边界边，
        // 碎片化的网格里 95% 的短边都会被挡掉（实测坍缩数 5882 → 257）。
        if (boundary_degree[a] != 2 || boundary_degree[b] != 2) continue;
        if (common != 1) continue;
      }

      // 合并：b → a，位置取中点（两点各移动一半，对称且位移最小）。
      mesh->positions[a] = (pa + pb) * 0.5;
      remap[b] = a;
      touched[a] = true;
      touched[b] = true;
      ++collapsed_this_round;
    }

    if (collapsed_this_round == 0) break;  // 收敛

    // 应用 remap，同时删掉因此退化的面。
    std::vector<uint32_t> kept;
    kept.reserve(mesh->indices.size());
    for (size_t t = 0; t < nt; ++t) {
      const uint32_t i0 = remap[mesh->indices[t * 3 + 0]];
      const uint32_t i1 = remap[mesh->indices[t * 3 + 1]];
      const uint32_t i2 = remap[mesh->indices[t * 3 + 2]];
      if (i0 == i1 || i1 == i2 || i2 == i0) continue;  // 退化 → 删
      kept.push_back(i0);
      kept.push_back(i1);
      kept.push_back(i2);
    }
    mesh->indices = std::move(kept);

    // 清掉因坍缩而孤立出来的顶点（保持索引有效）。
    RemoveIsolatedVertices(mesh);
    total_collapsed += collapsed_this_round;
  }

  return total_collapsed;
}

uint32_t FlipEdgesForQuality(TriangleMesh* mesh, double min_angle_deg,
                              int max_rounds) {
  if (mesh == nullptr || mesh->indices.size() < 6) return 0;
  const size_t tri_count = mesh->indices.size() / 3;
  const size_t vertex_count = mesh->positions.size();

  uint32_t total_flips = 0;
  constexpr double kEps = 1e-6;  // 改善容差（度）

  for (int round = 0; round < max_rounds; ++round) {
    // 边 → 面。
    std::map<EdgeKey, std::vector<uint32_t>> edge_faces;
    for (size_t t = 0; t < tri_count; ++t) {
      const uint32_t i0 = mesh->indices[t * 3 + 0];
      const uint32_t i1 = mesh->indices[t * 3 + 1];
      const uint32_t i2 = mesh->indices[t * 3 + 2];
      if (i0 == i1 || i1 == i2 || i2 == i0) continue;
      if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) {
        continue;
      }
      edge_faces[MakeEdge(i0, i1)].push_back(static_cast<uint32_t>(t));
      edge_faces[MakeEdge(i1, i2)].push_back(static_cast<uint32_t>(t));
      edge_faces[MakeEdge(i2, i0)].push_back(static_cast<uint32_t>(t));
    }

    uint32_t flips_this_round = 0;
    std::vector<bool> face_touched(tri_count, false);
    // 本轮**新产生**的边。edge_faces 是本轮开始时的快照，轮内多次翻转后
    // 会过时 —— 若不记录新边，两个不同的翻转可能翻出同一条 (c,d) →
    // 该边被 4 个面共享 → 非流形边（实测出现 1 条）。
    std::set<EdgeKey> new_edges;

    for (const auto& kv : edge_faces) {
      if (kv.second.size() != 2) continue;  // 只翻内部边
      const uint32_t t1 = kv.second[0];
      const uint32_t t2 = kv.second[1];
      if (face_touched[t1] || face_touched[t2]) continue;  // 本轮已改过

      const uint32_t a = kv.first.a;
      const uint32_t b = kv.first.b;
      const auto opposite = [&](uint32_t t) -> uint32_t {
        for (int k = 0; k < 3; ++k) {
          const uint32_t v = mesh->indices[t * 3 + k];
          if (v != a && v != b) return v;
        }
        return 0xFFFFFFFFu;
      };
      const uint32_t c = opposite(t1);
      const uint32_t d = opposite(t2);
      if (c == 0xFFFFFFFFu || d == 0xFFFFFFFFu || c == d) continue;

      // **必须要求 T1、T2 在共享边上绕序相反**。
      //
      // 翻转公式假定 T1 用 p→q、T2 用 q→p（相邻面绕序一致的标准前提）。
      // 泊松输出高度碎片化，2 面共享边图里存在**本来绕序就冲突**的边
      // （两个物理无关碎片恰好共用一条边，或孤立翻错的面）；对这些边套用
      // 公式会把两条外部边方向改错 → 绕序不一致不降反升（实测 7180 → 8101，
      // 面积朝外 87.6% → 84.7%）。此类边直接跳过，交由朝向修复处理。
      const bool t1_dir = FaceUsesEdgeDirected(*mesh, t1, a, b);
      const bool t2_dir = FaceUsesEdgeDirected(*mesh, t2, a, b);
      if (t1_dir == t2_dir) continue;  // 同向 = 绕序冲突，不翻

      // **必须按 T1 在共享边上的实际方向推导**，不能假设它是 a→b。
      //
      // `MakeEdge` 只保证 a<b（用于无向键），与面的绕序无关 —— T1 完全可能
      // 按 b→a 使用该边。若按固定的 a→b 推导翻转公式，约 **90%** 的翻转会把
      // 绕序翻反（实测：翻转 7413 条 → 绕序不一致 403 → 7093）。
      uint32_t p = a;
      uint32_t q = b;
      if (!t1_dir) {
        std::swap(p, q);  // 现在 T1 用的是 p→q
      }

      const Vec3d& pa = mesh->positions[a];
      const Vec3d& pb = mesh->positions[b];
      const Vec3d& pc = mesh->positions[c];
      const Vec3d& pd = mesh->positions[d];

      const double old_min = std::min(MinAngleOfTriangle(pa, pb, pc),
                                      MinAngleOfTriangle(pb, pa, pd));
      if (old_min >= min_angle_deg) continue;  // 已达标

      // ① 新边 (c,d) 不能已存在（既不在快照里，也不在本轮新产生的边里）。
      const EdgeKey cd = MakeEdge(c, d);
      if (edge_faces.find(cd) != edge_faces.end()) continue;
      if (new_edges.find(cd) != new_edges.end()) continue;

      // ② 新三角形非退化。
      const double area1 = 0.5 * (pd - pc).cross(pa - pc).norm();
      const double area2 = 0.5 * (pc - pd).cross(pb - pd).norm();
      if (area1 < 1e-16 || area2 < 1e-16) continue;

      // ③ 改善判据。
      const double new_min = std::min(MinAngleOfTriangle(pc, pd, pa),
                                      MinAngleOfTriangle(pd, pc, pb));
      if (new_min <= old_min + kEps) continue;

      // 执行翻转：T1 → (d,c,p)，T2 → (c,d,q)，其中 T1 用 p→q 使用共享边。
      //
      // **绕序必须保留 4 条外部边的方向**。以 p=a、q=b 为例：
      //   T1 = (a,b,c) → 有向边 a→b, b→c, c→a
      //   T2 = (b,a,d) → 有向边 b→a, a→d, d→b
      // 去掉 (p,q)=（a,b）、加上 (c,d) 后：
      //   T1' = (d,c,a) → d→c, c→a ✓, a→d ✓
      //   T2' = (c,d,b) → c→d, d→b ✓, b→c ✓
      // 新共享边 (c,d)：T1' 用 d→c，T2' 用 c→d ✓ 相反 ✓
      //
      // **踩过两次坑**：
      //   ① 写成 T1'=(c,d,a)、T2'=(d,c,b) → 外部边 (c,a)/(a,d) 方向反转；
      //   ② 按固定的 a→b 推导（不查 T1 的实际方向）→ 90% 的翻转绕序翻反。
      mesh->indices[t1 * 3 + 0] = d;
      mesh->indices[t1 * 3 + 1] = c;
      mesh->indices[t1 * 3 + 2] = p;
      mesh->indices[t2 * 3 + 0] = c;
      mesh->indices[t2 * 3 + 1] = d;
      mesh->indices[t2 * 3 + 2] = q;
      face_touched[t1] = true;
      face_touched[t2] = true;
      new_edges.insert(cd);
      ++flips_this_round;
    }

    total_flips += flips_this_round;
    if (flips_this_round == 0) break;  // 收敛
  }
  return total_flips;
}

// ---------------------------------------------------------------------------
// 统一后处理流水线
// ---------------------------------------------------------------------------

MeshPipelineReport RunMeshPipeline(TriangleMesh* mesh,
                                    const MeshPipelineConfig& config) {
  MeshPipelineReport report;
  if (mesh == nullptr) return report;

  // 1. 净化：退化面 + 孤立顶点。
  //
  // **必须在 S5 之前**：退化面（零面积）被剔除时会撕开新的边界，
  // 若这些边界在补洞之后才产生，就再也没人填它们。
  // 实测（球面，泊松原始网格含 1223 个退化面）：顺序颠倒会产生
  // 上百个「补完又裂开」的洞。
  report.degenerate_removed = RemoveDegenerateTriangles(mesh);
  report.duplicate_removed = RemoveDuplicateTriangles(mesh);
  // 孤立面片（外扩区噪声碎片）：删掉后它的边界环自然消失，
  // 无需再补 —— 补了会造出重合面与穿模几何。
  report.isolated_patch_removed = RemoveIsolatedFacePatches(mesh);
  report.isolated_removed = RemoveIsolatedVertices(mesh);

  // 面绕序修复：MC 提取的歧义 case 会产生绕序不一致的相邻面；个别孤立翻错
  // 的面会让边界环的「侧别」判据失效，两条边界环被混成一条 → 非简单环 →
  // 无法补洞。
  //
  // **用局部多数表决，不用 BFS 传播**（关键经验，2026-10-30）：
  // `OrientFacesConsistently` 的 BFS 会沿「恰好共用一条边的假邻接边」把
  // 互不相邻的面片串成一块，并**把种子朝向强加给整块** → 把原本整齐朝内的
  // 网格在块内打散（实测面积朝外 87.6% → 66.8%，强朝内 7.7% → 27.2%），
  // 且**不可逆**。改为局部多数表决后主分量零损失，同时仍能修掉孤立翻错的面。
  report.face_flip_fixed = FixLocalWindingErrors(mesh, 8);

  // 说明：**不在此处做整体朝外定向**。
  //
  // 早期曾把 `OrientMeshOutward` 放在这里（S4/S5 之前），认为「补洞的侧别
  // 判据依赖朝向」。实测证明这是错的：
  //   · 补洞的侧别判据（`SideOf`）只依赖**局部绕序**，不依赖整体朝向 ——
  //     它比较的是「环上相邻两条边的侧别是否相同」，整体翻转会让所有
  //     侧别同时取反，判据不变；
  //   · 且 S4/S5/S7 的拓扑改动（顶点分裂、坍缩）会反复扰动朝向，早期定向
  //     到末端会被再次打乱。
  //
  // 正确位置是**流水线末端**（S6 之前）—— 见下方第 5 步。

  // 2. S4：非流形边修复（顶点分裂，不移动几何）。
  report.non_manifold_split = RepairNonManifoldEdges(mesh);
  report.non_manifold_after_s4 = AnalyzeMesh(*mesh).non_manifold_edges;

  // 2b. 非流形顶点分裂：把边界度 ≥ 4 的顶点按扇区拆开。
  // 必须排在 S5 之前 —— 顶点处的两条边界环相交时，走环无论用哪种判据
  // 都无法把两条环分开，分裂后它们各自成为简单环才能补洞。
  report.vertex_split = SplitNonManifoldVertices(mesh);

  // 2c. **再次修复局部绕序**。
  //
  // 顶点分裂改变了「面 ↔ 边」的邻接关系：原本共享一条边的两个面可能被拆到
  // 不同顶点上，而另一些原本不共享边的面可能因此共享了边。这些新建立的邻接
  // 关系可能带来绕序冲突。
  //
  // 同 1 步：用**局部多数表决**（不传播）。局部修复幂等、不动几何，重复安全。
  report.face_flip_fixed += FixLocalWindingErrors(mesh, 8);

  // 3. S5：孔洞缝合。
  HoleFillConfig hole_config = config.hole;
  if (config.resolution > 0.0) hole_config.resolution = config.resolution;
  // 补洞后**不**自动跑 S4 —— 兜底的 S4/S5 由流水线成对驱动（见第 4 步）。
  hole_config.post_repair_enabled = false;
  report.holes = FillHoles(mesh, hole_config);

  // 3b. S7：三角面规整（短边坍缩 + 边翻转）。
  //
  // **放在兜底迭代之前**：坍缩偶发产生非流形边（实测 144 万三角形里 6 条），
  // 需要由后面的「S4 → S5」成对迭代兜住；若放在迭代之后，
  // 这些边就没人修了（S5 已跑完，撕开的洞也没人补）。
  //
  // 顺序：先坍缩极短边（消除成因），再边翻转（改善最小内角）。
  if (config.min_angle_deg > 0.0) {
    {
      const MeshStats before = AnalyzeMesh(*mesh);
      report.loops_before_s7 = before.boundary_loops;
      report.winding_before_s7 = before.inconsistent_winding_edges;
    }
    // S7a：坍缩极短边。**必须在边翻转之前** —— 狭长面的成因是那条
    // 0.062·r 的极短边，不先消掉它，翻转只是把短边换个位置。
    if (config.collapse_edge_factor > 0.0 && config.resolution > 0.0) {
      report.edge_collapses = CollapseShortEdges(
          mesh, config.collapse_edge_factor * config.resolution,
          config.s7_collapse_rounds);
      // 坍缩会改变面数与索引 → 清掉退化面与孤立顶点。
      RemoveDegenerateTriangles(mesh);
      RemoveIsolatedVertices(mesh);
    }
    // S7b：边翻转改善最小内角（零精度损失）。
    report.edge_flips =
        FlipEdgesForQuality(mesh, config.min_angle_deg, config.s7_max_rounds);
    {
      const MeshStats after = AnalyzeMesh(*mesh);
      report.loops_after_s7 = after.boundary_loops;
      report.winding_after_s7 = after.inconsistent_winding_edges;
    }
  }

  // 4. 兜底迭代：S4 → S5 成对执行，直到无非流形边。
  //
  // **为什么必须成对**：S4 的顶点分裂必然改变至少两条边的身份 —— 若那条边
  // 原本有 2 个面，拆分后变成 1 个面 → 撕开一个洞。S4 排在 S5 之前时，
  // 撕开的洞由 S5 填上；但补洞本身也可能再造出非流形边（四边形洞的对角线
  // 与已有内部边同名、两个洞共享同一条边等），于是需要
  // 「再拆一次 → 再补一次」地迭代到收敛。
  //
  // 实测（球面）：首轮 S5 后残留 2 条非流形边，兜底 1 轮即收敛到 0，
  // 且被撕开的洞由兜底 S5 补回 → 边界环数与未填孔洞数完全一致。
  constexpr int kMaxFallbackRounds = 4;
  for (int round = 0; round < kMaxFallbackRounds; ++round) {
    const uint32_t split = RepairNonManifoldEdges(mesh);
    if (split == 0) break;
    report.holes.post_fill_split += split;
    ++report.post_fill_round;

    // 兜底轮**不并入主计数**：首轮判为 H3/H4 的洞在兜底轮会被重新扫描，
    // 若累加会把同一个洞数两遍。主计数保持首轮口径。
    const HoleFillReport r2 = FillHoles(mesh, hole_config);
    report.post_fill_refilled += r2.filled_count;
    report.post_fill_unfilled += r2.unfilled_count;
    report.holes.remaining_loops = r2.remaining_loops;
  }

  // 4. 兜底净化：补洞若产生退化面 / 孤立顶点则清除（如实计数）。
  report.post_degenerate_removed = RemoveDegenerateTriangles(mesh);
  report.post_isolated_removed = RemoveIsolatedVertices(mesh);

  // **不做 S7 之后的兜底 S4**。
  //
  // 曾试过：翻转后跑一次 `RepairNonManifoldEdges` 兜底。但 S4 的顶点分裂
  // 会撕开已闭合的洞（边界环 +6）并扰动绕序，而此时 S5 已经跑过，没人再补。
  // 判据 ① 的 `new_edges` 守卫已从源头挡住重复边（实测翻转 7413 条、
  // 非流形边保持 0），兜底就成了纯损失。

  // 5. S6（第 1~3 步）：**整体朝外定向**。
  //
  // 位置说明见上方「不在此处做整体朝外定向」的注释。此处 S4/S5/S7 已跑完，
  // 网格最接近流形 → 逐分量定向判据最可靠。这是**唯一**的定向时机。
  if (config.orient_outward) {
    report.outward_flip_fixed += OrientMeshOutward(mesh);
  }

  // 6. S6（第 4 步）：逐顶点法向重算。
  //
  // 必须在定向**之后** —— 面积加权的顶点法向会被反向面污染。
  if (config.recompute_normals) RecomputeVertexNormals(mesh);

  // 6. 统计。
  report.stats = AnalyzeMesh(*mesh);
  return report;
}

}  // namespace recon
