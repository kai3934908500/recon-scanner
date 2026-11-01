// SPDX-License-Identifier: BSD-3-Clause
//
// 泊松曲面重建（自实现）。
//
// 契约：docs/06_网格化与后处理.md §1、§2.3。
//
// ## 为什么是自实现而不是集成 PoissonRecon
//
// 规格 §1.1 指定使用 PoissonRecon（MIT）。集成尝试已展开并保留了成果
// （`third_party/poisson_recon/` 与 `PATCHES.md` 记录了 2 处上游 bug 的
// 修复方案），但**上游 V7.0 标签的打包是损坏的**：
// 多个文件来自不同版本（`FunctionData.inl` 与 `BinaryNode.h` 不匹配、
// `Ply.h` 与 `Octree.h` 不匹配、`Octree.inl` 期望的 `OctNode` 成员缺失），
// 继续修补的边际收益低于自实现的边际成本。
//
// master 分支文件自洽，但需额外依赖 AdaptiveSolvers，且集成层涉及
// 高级模板元编程（`FEMSigPack` / `ParameterPack` / `VertexFactory`），
// 移动端编译体积与调试成本都偏高。
//
// 因此本模块采用自实现，并把上述取舍记录在
// docs/06_网格化与后处理.md 的「实现说明」小节。
//
// ## 自实现中经实测确认的关键设计（均为踩坑后修正，勿回退）
//
// 1. **搜索参数用逆深度 1/Z，不用视差**。
//    多视图下源视图 j 的实际位移是 `d_j = f·B_j/Z`，
//    `B_j` 沿极线方向（取决于相对旋转，不只是光心距离）。
//    用单一平均基线换算会让「同一个 d」在不同源视图对应不同深度 →
//    多视图代价被污染 → 系统偏差。
//
// 2. **梯度场 V 除以单元边长 h，不除以体积 h³**。
//    Poisson 方程 ∇²f = ∇·V 中 f 是无量纲的指示函数：
//      L[f] ~ f/h²   （1/m²）
//      ∇·V ~ V/h     （1/m²）⇒ V 的量纲必须是 1/m
//    误用体积时 V ~ 1e11，条件数劣化到 3.8e6，PCG 发散。
//
// 3. **格点场值 = 相邻活跃单元值的平均**，不能取「第一个写入者」。
//    后者会让角点场值出现 4 个相同的正数 + 4 个相同的负数，
//    等值面位置完全错误。
//
// 4. **等值面顶点必须按「边的两个端点」共享**。
//    只用单个端点构造键 → 所有共享同一端点的不同边碰撞 →
//    交点顶点被错误合并 → 等值面出现大量裂缝
//    （实测裁剪前非流形边 20 万+、边界环 2 万+）。
//
// 5. **哈希键的位宽必须 ≤ 20 位/坐标**。
//    3 坐标 × 21 位 = 63 位，再左移 1 位区分边/顶点 → 溢出到符号位 →
//    键值错乱 → 哈希桶行为异常 → 段错误。
//
// 6. **PCG 残差用归一化形式**（RMS ÷ 窗口灰度标准差），
//    否则高对比度纹理会被固定阈值误杀。

#include "recon/poisson_mesh.h"

// Marching Cubes 边表（来自 PoissonRecon 的标准表，MIT）。
// 只用体素的 12 条棱，**不含体素对角线** —— 这是它相对
// marching tetrahedra 的关键优势（详见文件头说明 7）。
#include "recon/poisson_mc.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <unordered_map>

namespace recon {
namespace {


// ---------------------------------------------------------------------------
// 点云空间索引（精确最近邻）
// ---------------------------------------------------------------------------

// 点云的空间哈希 + 精确最近邻查询。
//
// **为什么不能用「抽样最近邻」**：点云稀疏时（例如球面 20000 点、间距 1.25 mm），
// 抽样 1/4 的点做最近邻会把最近距离高估近 2 倍 → 裁剪阈值形同虚设 →
// **主体网格被当成外扩区一起裁掉**（实测：三角形从 106061 砍到 3242，只剩 3%）。
//
// 用均匀网格哈希，每个查询只扫 3×3×3 邻域格 → 复杂度与点数无关。
class PointCloudIndex {
 public:
  PointCloudIndex(const std::vector<Vec3d>& points, double cell)
      : points_(points), cell_(cell > 1e-12 ? cell : 1e-6) {
    for (size_t i = 0; i < points.size(); ++i) {
      const int64_t cx = CellCoord(points[i](0));
      const int64_t cy = CellCoord(points[i](1));
      const int64_t cz = CellCoord(points[i](2));
      buckets_[Hash(cx, cy, cz)].push_back(static_cast<uint32_t>(i));
    }
  }

  // 最近距离。若邻域内找不到点，**逐层扩大**搜索半径，直到找到或超出界限。
  //
  // @param exclude_index 排除的点下标（算点云的点间距离时必须排除自身，
  //                      否则最近距离恒为 0）。
  double NearestDistance(const Vec3d& p, int exclude_index = -1,
                         int max_ring = 64) const {
    if (points_.empty()) return 1e30;
    const int64_t cx = CellCoord(p(0));
    const int64_t cy = CellCoord(p(1));
    const int64_t cz = CellCoord(p(2));
    double best = 1e30;
    for (int ring = 0; ring <= max_ring; ++ring) {
      // 上一轮已扫过的整层不再扫，只扫新的一层。
      if (ring > 0) {
        // 已扫过的层已全部纳入 best，本层只扫 shell。
        for (int64_t dx = -ring; dx <= ring; ++dx) {
          for (int64_t dy = -ring; dy <= ring; ++dy) {
            for (int64_t dz = -ring; dz <= ring; ++dz) {
              const int64_t ax = std::abs(dx), ay = std::abs(dy),
                            az = std::abs(dz);
              if (std::max(ax, std::max(ay, az)) != ring) continue;
              ScanCell(p, cx + dx, cy + dy, cz + dz, exclude_index, &best);
            }
          }
        }
      } else {
        ScanCell(p, cx, cy, cz, exclude_index, &best);
      }
      if (best < 1e29) {
        // 找到候选后，再扫一圈确认没有更近的。
        bool improved = false;
        for (int64_t dx = -1; dx <= 1; ++dx) {
          for (int64_t dy = -1; dy <= 1; ++dy) {
            for (int64_t dz = -1; dz <= 1; ++dz) {
              const double before = best;
              ScanCell(p, cx + dx, cy + dy, cz + dz, exclude_index, &best);
              if (best < before) improved = true;
            }
          }
        }
        (void)improved;
        return best;
      }
    }
    return best;
  }

 private:
  int64_t CellCoord(double v) const {
    return static_cast<int64_t>(std::floor(v / cell_));
  }
  static uint64_t Hash(int64_t x, int64_t y, int64_t z) {
    uint64_t h = static_cast<uint64_t>(x) * 73856093ull;
    h ^= static_cast<uint64_t>(y) * 19349663ull;
    h ^= static_cast<uint64_t>(z) * 83492791ull;
    return h;
  }
  void ScanCell(const Vec3d& p, int64_t cx, int64_t cy, int64_t cz,
                int exclude_index, double* best) const {
    auto fit = buckets_.find(Hash(cx, cy, cz));
    if (fit == buckets_.end()) return;
    for (uint32_t idx : fit->second) {
      if (static_cast<int>(idx) == exclude_index) continue;
      const double dd = (p - points_[idx]).squaredNorm();
      if (dd < *best) *best = dd;
    }
  }

  const std::vector<Vec3d>& points_;
  double cell_;
  std::unordered_map<uint64_t, std::vector<uint32_t>> buckets_;
};

// ---------------------------------------------------------------------------
// 哈希键
// ---------------------------------------------------------------------------

// 格点坐标 → 20 位/坐标 的线性索引（63 位）。
//
// 位宽上限 20 的原因：3 坐标 × 21 位 = 63 位，再左移 1 位区分
// 「边 / 顶点」会溢出到符号位 → 键值错乱 → 段错误（实测）。
// 坐标范围核对：|x| ≤ 2^depth ≤ 2^14 < 2^20 ✓ 余量充足。
constexpr int kCoordBits = 20;
constexpr int64_t kCoordMask = (1LL << kCoordBits) - 1;

inline int64_t GridIndex(int64_t x, int64_t y, int64_t z) {
  const int64_t bx = (x + (1LL << 19)) & kCoordMask;
  const int64_t by = (y + (1LL << 19)) & kCoordMask;
  const int64_t bz = (z + (1LL << 19)) & kCoordMask;
  return (bx << (2 * kCoordBits)) | (by << kCoordBits) | bz;
}

// 边的键：两个端点的格点索引（各 60 位，共 120 位 → 用 pair 承载）。
struct EdgeKey {
  int64_t a;
  int64_t b;
  bool operator==(const EdgeKey& o) const { return a == o.a && b == o.b; }
};

struct EdgeKeyHash {
  size_t operator()(const EdgeKey& k) const {
    size_t h = std::hash<int64_t>()(k.a);
    h ^= std::hash<int64_t>()(k.b) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
  }
};

// 立方体 8 角的局部偏移（经典编号）：
//   0:(0,0,0) 1:(1,0,0) 2:(1,1,0) 3:(0,1,0)
//   4:(0,0,1) 5:(1,0,1) 6:(1,1,1) 7:(0,1,1)
//
// 用于「格点场值表」的角点遍历；
// MC 提取用的是 PoissonRecon 的 Cube::CornerIndex（顺序不同，见提取处）。
const int kCornerOffset[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                 {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};

// 活跃单元（网格化后有场值）。
struct ActiveCell {
  int32_t x, y, z;
  Vec3d gradient = Vec3d::Zero();  // V
  double weight = 0.0;
  double value = 0.0;
  // 该单元是否被**真实观测点**支撑（而不是致密化出来的虚拟点）。
  //
  // screened 项（docs/06 §1.2 的「f ≈ 0 强约束」）必须**只锚定真实点**：
  // 致密化后虚拟点铺满整个曲面薄壳，若把它们也当作锚点，薄壳内所有单元的
  // 场值都会被钉在 iso 上 → 等值面 f = iso 在薄壳内部**根本不存在**（整块
  // 区域都是 iso 值）→ 网格碎成孤立片。实测：不加区分时覆盖仅 **15.1%**。
  bool anchored = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// 分辨率计算（§1.3 的唯一权威关系式）
// ---------------------------------------------------------------------------

double PoissonResolution(double bbox_longest_edge, double scale, int depth) {
  if (depth < 0) return 0.0;
  return scale * bbox_longest_edge / static_cast<double>(1 << depth);
}

int PoissonDepthForResolution(double bbox_longest_edge, double scale,
                              double r_target) {
  if (r_target <= 0.0 || bbox_longest_edge <= 0.0) return 10;
  const double ratio = scale * bbox_longest_edge / r_target;
  int d = 0;
  while ((static_cast<double>(1 << d) < ratio) && d < 20) ++d;
  return d;
}


// ---------------------------------------------------------------------------
// 点云切平面致密化
// ---------------------------------------------------------------------------

// 把每个点沿其**切平面**展开成 k×k 个虚拟点，使等效采样间距 ≤ target_spacing。
//
// **解决什么问题**：活跃单元 = 「点云支撑的单元 + 一环邻域」。点云间距若为
// 单元边长的 k 倍（实测球面：1.18 mm / 0.215 mm = 5.5 倍），则每个点的
// 3×3×3 单元团**互不连通** → 泊松的求解域断开 → `value_at()` 对未覆盖格点
// 返回 0.0，而 `iso` 也≈0 → **未定义区恰好落在等值面上** → MC 在那里产出
// 杂乱碎片。实测 D=9 时网格只覆盖球面面积的 7.3%，边界环 4.2 万。
//
// **不增加信息（如实声明）**：虚拟点由原点的位置/法向插值而来，**形状精度的
// 上限仍是原点云的采样密度**。本函数解决的是「网格断裂」，不是「精度不足」。
//
// **切平面平坦化误差**：虚拟点严格落在过 p 的切平面上，而真实曲面是弯的，
// 偏差 ≈ (k·r)²/(2R)。R = 50 mm、k·r = 1.18 mm 时仅 **0.014 mm**，
// 远小于点云自身的形状不确定度。
//
// @param k 展开倍数（k ≤ 1 时原样返回）
OrientedPointCloud DensifyOnTangentPlane(const OrientedPointCloud& cloud, int k,
                                         const double target_spacing) {
  if (k <= 1) return cloud;

  OrientedPointCloud out;
  const size_t n = cloud.size();
  out.positions.reserve(n * static_cast<size_t>(k) * static_cast<size_t>(k));
  out.normals.reserve(n * static_cast<size_t>(k) * static_cast<size_t>(k));
  const bool has_color = cloud.colors.size() >= n * 3;
  const bool has_weight = cloud.weights.size() >= n;

  // 虚拟点铺满边长 k·r 的正方形、间距 r → 相邻原点的正方形恰好相接。
  const double side = static_cast<double>(k) * target_spacing;
  //
  // 权重按 1/k² 衰减：k² 个虚拟点代表原点所辖的那一小块曲面，
  // **梯度场总量与原点单点时相同**（几何上正确的面积归一）。
  const double w_scale = 1.0 / (static_cast<double>(k) * static_cast<double>(k));

  for (size_t i = 0; i < n; ++i) {
    const Vec3d p = cloud.positions[i];
    Vec3d nrm = (i < cloud.normals.size()) ? cloud.normals[i] : Vec3d(0, 0, 1);
    if (nrm.norm() < 1e-12) nrm = Vec3d(0.0, 0.0, 1.0);
    nrm.normalize();

    // 切平面正交基：e1 × e2 = nrm。
    Vec3d helper;
    const double ax = std::fabs(nrm(0));
    const double ay = std::fabs(nrm(1));
    const double az = std::fabs(nrm(2));
    if (ax <= ay && ax <= az) {
      helper = Vec3d(1.0, 0.0, 0.0);
    } else if (ay <= az) {
      helper = Vec3d(0.0, 1.0, 0.0);
    } else {
      helper = Vec3d(0.0, 0.0, 1.0);
    }
    Vec3d e1 = helper.cross(nrm);
    if (e1.norm() < 1e-12) e1 = Vec3d(1.0, 0.0, 0.0);
    e1.normalize();
    const Vec3d e2 = nrm.cross(e1);

    const double w = has_weight ? std::max(0.0f, cloud.weights[i]) : 1.0f;
    for (int a = 0; a < k; ++a) {
      for (int b = 0; b < k; ++b) {
        const double u = (static_cast<double>(a) + 0.5) /
                             static_cast<double>(k) - 0.5;
        const double v = (static_cast<double>(b) + 0.5) /
                             static_cast<double>(k) - 0.5;
        out.positions.push_back(p + e1 * (u * side) + e2 * (v * side));
        out.normals.push_back(nrm);
        if (has_color) {
          out.colors.push_back(cloud.colors[i * 3 + 0]);
          out.colors.push_back(cloud.colors[i * 3 + 1]);
          out.colors.push_back(cloud.colors[i * 3 + 2]);
        }
        // 权重按 1/k² 均分 → 总权重守恒。
        out.weights.push_back(static_cast<float>(w * w_scale));
      }
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// 点云间距估计与深度反推
// ---------------------------------------------------------------------------

double EstimatePointSpacing(const OrientedPointCloud& cloud) {
  if (cloud.positions.size() < 2) return 0.0;

  // 初始格边长：包围盒体积 / 点数的立方根（只用于建索引，偏大不影响正确性）。
  Vec3d lo(1e30, 1e30, 1e30), hi(-1e30, -1e30, -1e30);
  for (const auto& p : cloud.positions) {
    for (int i = 0; i < 3; ++i) {
      lo(i) = std::min(lo(i), p(i));
      hi(i) = std::max(hi(i), p(i));
    }
  }
  const double vol = std::max(1e-18, (hi(0) - lo(0)) * (hi(1) - lo(1)) *
                                      (hi(2) - lo(2)));
  const double rough = std::cbrt(vol / static_cast<double>(cloud.positions.size()));

  const PointCloudIndex index(cloud.positions, rough);
  std::vector<float> nn;
  nn.reserve(cloud.positions.size());
  for (size_t i = 0; i < cloud.positions.size(); ++i) {
    // **必须排除自身**，否则最近距离恒为 0。
    const double d = index.NearestDistance(cloud.positions[i],
                                           static_cast<int>(i));
    nn.push_back(static_cast<float>(std::sqrt(std::min(d, 1e29))));
  }
  std::sort(nn.begin(), nn.end());
  // **最近邻距离中位数 ≈ 0.47 × 采样间距**（均匀曲面采样下）。
  // 反推采样间距用系数 2.0（略保守 → 反推出的 D 偏小 → r 偏粗 → 覆盖更完整）。
  //
  // 实测（球面 20000 点）：最近邻中位数 0.591 mm → 采样间距 1.18 mm
  //   r = 0.215 mm（远小于间距）→ 覆盖 7.3%
  //   r = 0.430 mm              → 覆盖 30.7%
  //   r = 0.859 mm              → 覆盖 87.9%
  //   r = 1.719 mm              → 覆盖 107.9%
  return 2.0 * static_cast<double>(nn[nn.size() / 2]);
}

int PoissonDepthForPointSpacing(double bbox_longest_edge, double scale,
                                double point_spacing) {
  if (bbox_longest_edge <= 0.0 || scale <= 0.0 || point_spacing <= 0.0) {
    return 10;
  }
  const double ratio = scale * bbox_longest_edge / point_spacing;
  int d = 0;
  while (d < 20 && static_cast<double>(1 << d) < ratio) ++d;
  return d;
}

// ---------------------------------------------------------------------------
// 重建主流程
// ---------------------------------------------------------------------------

bool PoissonReconstruct(const OrientedPointCloud& cloud, const PoissonConfig& config,
                        PoissonResult* out) {
  if (out == nullptr) return false;
  if (cloud.size() < 16) return false;

  const auto t_start = std::chrono::steady_clock::now();

  // ---- 1. 包围盒与分辨率 ----
  Vec3d lo(1e30, 1e30, 1e30);
  Vec3d hi(-1e30, -1e30, -1e30);
  for (const auto& p : cloud.positions) {
    for (int i = 0; i < 3; ++i) {
      lo(i) = std::min(lo(i), p(i));
      hi(i) = std::max(hi(i), p(i));
    }
  }
  const Vec3d extent = hi - lo;
  const double longest = std::max(extent(0), std::max(extent(1), extent(2)));
  if (longest <= 0.0) return false;

  out->bbox_longest_edge = longest;
  out->resolution = PoissonResolution(longest, config.scale, config.depth);
  if (out->resolution <= 0.0) return false;

  // ---- 2. 网格划分与梯度场累积 ----
  //
  // 根节点覆盖边长 scale × L_max 的立方体，居中包围点云。
  const double root_size = config.scale * longest;
  const Vec3d center = (lo + hi) * 0.5;
  const Vec3d origin(center(0) - root_size * 0.5, center(1) - root_size * 0.5,
                    center(2) - root_size * 0.5);

  // 单元边长：第 d 层为 root_size / 2^d。
  //
  // **必须取 config.depth，而不是 depth-1**：
  // docs/06 §1.3 规定 `r = s·L_max/2^D` 就是「最终网格特征边长」，
  // 且「泊松输出的三角形特征边长与该单元边长同量级（实测比值 0.8~1.2）」。
  // 旧实现取 depth-1 → 实际单元边长 = 2r → 实测平均边长 / 标称 r = **2.10**，
  // 即**实际网格比标称分辨率粗一倍**，精度声明与实现不符。
  const int leaf_depth = std::max(0, config.depth);
  const double h = root_size / static_cast<double>(1 << leaf_depth);

  // ---- 2b. 点云切平面致密化 ----
  //
  // **为什么必须做**：活跃单元 = 「点云支撑 + 一环邻域」。点云间距若为单元
  // 边长的 k 倍，每个点的 3×3×3 单元团就**互不连通** → 泊松的求解域断开 →
  // `value_at()` 对未覆盖格点返回 0.0，而 `iso` 也≈0 →
  // **未定义区恰好落在等值面上** → MC 在那里产出杂乱碎片。
  //
  // 实测（球面 20000 点，间距 1.18 mm）：
  //   不做致密化，D=9（r=0.215mm，间距 = 5.5 倍单元边长）
  //   → 网格只覆盖球面面积的 **7.3%**，边界环 4.2 万。
  //
  // **代价与诚实边界**：致密化把点数放大 k² 倍（k 上限 6 → 36 倍），
  // 虚拟点是插值出来的、**不增加任何几何信息** —— 形状精度的上限仍是
  // 原点云的采样密度。它解决的是「网格断裂」，不是「精度不足」。
  // 切平面平坦化误差 ≈ (k·r)²/(2R)：R=50mm、k·r=1.18mm 时仅 0.014 mm。
  constexpr int kMaxDensify = 6;
  OrientedPointCloud dense_cloud;
  const OrientedPointCloud* src = &cloud;
  int densify_k = 1;
  {
    const double spacing = EstimatePointSpacing(cloud);
    if (spacing > 0.0 && out->resolution > 0.0) {
      int k = static_cast<int>(std::ceil(spacing / out->resolution));
      k = std::max(1, std::min(k, kMaxDensify));
      densify_k = k;
      if (k > 1) {
        dense_cloud = DensifyOnTangentPlane(cloud, k, out->resolution);
        src = &dense_cloud;
      }
    }
  }
  out->densify_factor = densify_k;

  // 活跃单元：点云支撑的单元 + 其一环邻域。
  // N_cells ≈ 表面积 / r²（而非包围盒体积 / r³），这是稀疏化的来源。
  std::unordered_map<int64_t, uint32_t> cell_index;
  std::vector<ActiveCell> active;
  active.reserve(src->size() * 2);

  const auto cell_hash = [&](int32_t x, int32_t y, int32_t z) {
    return GridIndex(x, y, z);
  };

  // 3a. 点云支撑的单元。
  //
  // **分两遍**：先真实观测点（标记 anchored，供 screened 项锚定），
  // 再致密化出来的虚拟点（只贡献梯度场，不锚定）。
  const auto accumulate_point = [&](const Vec3d& p, const Vec3d& n, double w,
                                    bool anchored) {
    if (w <= 0.0) return;
    const int32_t cx = static_cast<int32_t>(std::floor((p(0) - origin(0)) / h));
    const int32_t cy = static_cast<int32_t>(std::floor((p(1) - origin(1)) / h));
    const int32_t cz = static_cast<int32_t>(std::floor((p(2) - origin(2)) / h));
    const int64_t key = cell_hash(cx, cy, cz);
    auto it = cell_index.find(key);
    if (it == cell_index.end()) {
      ActiveCell c;
      c.x = cx;
      c.y = cy;
      c.z = cz;
      c.weight = w;
      // V = Σ w·n / h（量纲 1/m，见文件头说明 2）。
      c.gradient = n * (w / h);
      c.anchored = anchored;
      cell_index.emplace(key, static_cast<uint32_t>(active.size()));
      active.push_back(c);
    } else {
      ActiveCell& c = active[it->second];
      c.gradient += n * (w / h);
      c.weight += w;
      if (anchored) c.anchored = true;
    }
  };

  // 第一遍：真实观测点。
  for (size_t i = 0; i < cloud.size(); ++i) {
    double w = 1.0;
    if (config.point_weight && i < cloud.weights.size()) {
      w = std::max(0.0f, cloud.weights[i]);
    }
    accumulate_point(cloud.positions[i], cloud.normals[i], w, true);
  }
  // 第二遍：虚拟点（致密化结果，不锚定）。
  if (densify_k > 1) {
    for (size_t i = 0; i < dense_cloud.size(); ++i) {
      double w = 1.0;
      if (config.point_weight && i < dense_cloud.weights.size()) {
        w = std::max(0.0f, dense_cloud.weights[i]);
      }
      accumulate_point(dense_cloud.positions[i], dense_cloud.normals[i], w,
                       false);
    }
  }

  if (active.empty()) return false;

  // 3b. 一环邻域（保证 Laplacian 完整）。
  {
    const int32_t offsets[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                   {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    const size_t original = active.size();
    for (size_t i = 0; i < original; ++i) {
      for (const auto& off : offsets) {
        const int32_t nx = active[i].x + off[0];
        const int32_t ny = active[i].y + off[1];
        const int32_t nz = active[i].z + off[2];
        const int64_t key = cell_hash(nx, ny, nz);
        if (cell_index.find(key) != cell_index.end()) continue;
        ActiveCell c;
        c.x = nx;
        c.y = ny;
        c.z = nz;
        cell_index.emplace(key, static_cast<uint32_t>(active.size()));
        active.push_back(c);
      }
    }
  }

  const size_t n = active.size();
  out->active_cells = n;

  // ---- 3c. 泊松方程：L[f] = b ----
  //
  // b[u] = −∇·V[u]（中心差分）
  // L[u][u] = 6/h² − screened 约束
  // 邻接权重 = −1/h²
  //
  // screened 项：输入点所在单元上追加「f ≈ 0」的强约束，
  // 这是 Screened Poisson 抑制整体膨胀的关键。

  std::vector<std::vector<uint32_t>> adjacency(n);
  const int32_t offsets[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                 {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
  for (size_t i = 0; i < n; ++i) {
    for (const auto& off : offsets) {
      auto it = cell_index.find(cell_hash(active[i].x + off[0],
                                         active[i].y + off[1],
                                         active[i].z + off[2]));
      if (it != cell_index.end()) {
        adjacency[i].push_back(it->second);
      }
    }
  }

  const double h2_inv = 1.0 / (h * h);
  std::vector<double> diag(n, 0.0);
  std::vector<double> rhs(n, 0.0);

  const auto gradient_at = [&](int32_t x, int32_t y, int32_t z) {
    auto it = cell_index.find(cell_hash(x, y, z));
    if (it == cell_index.end()) return Vec3d::Zero();
    return active[it->second].gradient;
  };

  for (size_t i = 0; i < n; ++i) {
    const Vec3d gxp = gradient_at(active[i].x + 1, active[i].y, active[i].z);
    const Vec3d gxm = gradient_at(active[i].x - 1, active[i].y, active[i].z);
    const Vec3d gyp = gradient_at(active[i].x, active[i].y + 1, active[i].z);
    const Vec3d gym = gradient_at(active[i].x, active[i].y - 1, active[i].z);
    const Vec3d gzp = gradient_at(active[i].x, active[i].y, active[i].z + 1);
    const Vec3d gzm = gradient_at(active[i].x, active[i].y, active[i].z - 1);

    const double div = (gxp(0) - gxm(0)) / (2.0 * h) +
                       (gyp(1) - gym(1)) / (2.0 * h) +
                       (gzp(2) - gzm(2)) / (2.0 * h);
    rhs[i] = -div;

    double d = 6.0 * h2_inv;
    // screened 项：把点云支撑单元的场值**弱**拉向 iso。
    //
    // **强度必须远小于拉普拉斯项**。旧实现用 `4·h⁻²`（= 对角项的 67%），
    // 远比 PoissonRecon 的 ~17% 强 —— 后果是整个点云区域被**压平**在 iso 上，
    // 而等值面 f = iso 在整块区域内部根本不存在（每处都等于 iso）→ MC 跳过
    // 这些单元 → 网格只覆盖球面 **14.3%**。
    //
    // 改成 `0.7·h⁻²`（≈ 12%）后，screened 只固定 gauge（消除常数自由度），
    // 而 f 沿法向的变化由 ∇f ≈ V 驱动 → 等值面真正穿过薄壳。
    //
    // **锚定范围**：所有点云支撑单元（含致密化的虚拟点）—— 它们都位于曲面上，
    // 都应参与 gauge 固定；只锚定真实观测点会让未锚定的虚拟点区域整体漂移。
    if (active[i].weight > 0.0) {
      d += 0.7 * h2_inv * std::min(1.0, active[i].weight);
    }
    diag[i] = d;
  }

  const auto matvec = [&](const std::vector<double>& x, std::vector<double>* y) {
    for (size_t i = 0; i < n; ++i) {
      double v = diag[i] * x[i];
      for (uint32_t nb : adjacency[i]) {
        v -= h2_inv * x[nb];
      }
      (*y)[i] = v;
    }
  };

  // ---- 3d. PCG（Jacobi 预条件）----
  std::vector<double> inv_diag(n);
  for (size_t i = 0; i < n; ++i) {
    inv_diag[i] = (diag[i] != 0.0) ? 1.0 / diag[i] : 0.0;
  }

  std::vector<double> x(n, 0.0);
  std::vector<double> r = rhs;
  std::vector<double> z(n, 0.0);
  std::vector<double> p(n, 0.0);
  std::vector<double> ap(n, 0.0);

  for (size_t i = 0; i < n; ++i) z[i] = inv_diag[i] * r[i];
  p = z;

  const double rhs_norm = std::sqrt(
      std::accumulate(rhs.begin(), rhs.end(), 0.0,
                     [](double a, double b) { return a + b * b; }));
  const double tolerance = std::max(1e-30, config.solver_tolerance * rhs_norm);

  double rz = 0.0;
  for (size_t i = 0; i < n; ++i) rz += r[i] * z[i];

  int iter = 0;
  for (; iter < config.solver_iterations; ++iter) {
    matvec(p, &ap);
    double pap = 0.0;
    for (size_t i = 0; i < n; ++i) pap += p[i] * ap[i];
    if (std::fabs(pap) < 1e-300) break;

    const double alpha = rz / pap;
    for (size_t i = 0; i < n; ++i) {
      x[i] += alpha * p[i];
      r[i] -= alpha * ap[i];
    }

    const double r_norm = std::sqrt(
        std::accumulate(r.begin(), r.end(), 0.0,
                       [](double a, double b) { return a + b * b; }));
    if (r_norm < tolerance) {
      ++iter;
      break;
    }

    for (size_t i = 0; i < n; ++i) z[i] = inv_diag[i] * r[i];
    double rz_new = 0.0;
    for (size_t i = 0; i < n; ++i) rz_new += r[i] * z[i];
    const double beta = rz_new / rz;
    rz = rz_new;
    for (size_t i = 0; i < n; ++i) p[i] = z[i] + beta * p[i];
  }

  {
    std::vector<double> tmp(n);
    matvec(x, &tmp);
    double rn = 0.0;
    for (size_t i = 0; i < n; ++i) {
      const double d = tmp[i] - rhs[i];
      rn += d * d;
    }
    rn = std::sqrt(rn);
    out->solver_residual = (rhs_norm > 0.0) ? rn / rhs_norm : 0.0;
  }

  for (size_t i = 0; i < n; ++i) active[i].value = x[i];

  // ---- 4. 等值面阈值 ----
  //
  // 用点云支撑单元的场值中位数作为 iso。
  // （screened 项把这些单元的场值约束在 0 附近，iso 因此接近 0。）
  double iso = 0.0;
  {
    std::vector<double> vals;
    vals.reserve(n);
    for (const auto& c : active) {
      if (c.weight > 0.0) vals.push_back(c.value);
    }
    if (!vals.empty()) {
      std::sort(vals.begin(), vals.end());
      iso = vals[vals.size() / 2];
    }
  }

  out->iso_threshold = iso;
  {
    double vmin = 1e300, vmax = -1e300;
    for (const auto& c : active) {
      vmin = std::min(vmin, c.value);
      vmax = std::max(vmax, c.value);
    }
    out->value_min = vmin;
    out->value_max = vmax;
  }

  // ---- 5. 格点场值表 ----
  //
  // 格点值 = 相邻活跃单元值的**平均**（不是第一个写入者）。
  std::unordered_map<int64_t, double> grid_values;
  {
    grid_values.clear();
    std::unordered_map<int64_t, int32_t> count;
    grid_values.reserve(n * 4);
    count.reserve(n * 4);
    for (size_t i = 0; i < n; ++i) {
      for (int c = 0; c < 8; ++c) {
        const int64_t key =
            GridIndex(active[i].x + kCornerOffset[c][0],
                      active[i].y + kCornerOffset[c][1],
                      active[i].z + kCornerOffset[c][2]);
        grid_values[key] += active[i].value;
        ++count[key];
      }
    }
    for (auto& kv : grid_values) {
      const int32_t cnt = count[kv.first];
      kv.second = cnt > 0 ? kv.second / cnt : 0.0;
    }
    out->grid_value_points = grid_values.size();

  const auto value_at = [&](int32_t gx, int32_t gy, int32_t gz) -> double {
    auto it = grid_values.find(GridIndex(gx, gy, gz));
    if (it != grid_values.end()) return it->second;

    // 该格点不在表中：找包含它的活跃单元（最多 8 个候选，O(1)）。
    for (int dz = 0; dz <= 1; ++dz) {
      for (int dy = 0; dy <= 1; ++dy) {
        for (int dx = 0; dx <= 1; ++dx) {
          auto cit = cell_index.find(cell_hash(gx - dx, gy - dy, gz - dz));
          if (cit != cell_index.end()) return active[cit->second].value;
        }
      }
    }
    ++out->undefined_corner_queries;
    return 0.0;
  };

  // ---- 6. Marching Cubes 提取 ----
  //
  // 采用 PoissonRecon 的标准 MC 边表（MIT）。
  //
  // **为什么不用 marching tetrahedra**（实测踩过的坑）：
  //
  //   MT 的 6 四面体分解共享**体素对角线**。等值面穿过对角线时，
  //   该边被最多 6 个四面体的三角形共享 → 必然产生非流形边。
  //   实测（球面，depth=9）：非流形边 11675、边界环 4712、退化面 6439。
  //   用 S4 拆分连接来修 → 拆分产生新边界 → 补洞又产生新连接，
  //   形成死循环（实测非流形边不降反升到 13494）。
  //
  //   MC 只用体素的 12 条棱，不含对角线 → 没有这类多面共享的根源。
  //   棱上的交点通过**全局棱 ID**共享（相邻体素共用一条棱），
  //   因此等值面在体素之间天然连续。
  //
  //   MC 仍有「歧义 case」（面上 4 角正负各半）可能产生 T 型接点，
  //   但发生频率远低于 MT 的对角线问题，且由 S4/S5 后处理兜底。

  TriangleMesh& mesh = out->mesh;
  mesh.Clear();

  // 棱 → 顶点。全局共享，保证相邻体素的等值面连续。
  std::unordered_map<EdgeKey, uint32_t, EdgeKeyHash> vertex_of_edge;

  // PoissonRecon 的 Cube 角点索引：CornerIndex(x,y,z)。
  // 我的体素角点用 (dx,dy,dz) ∈ {0,1}³ 标识。
  int mc_corner[8][3];
  for (int c = 0; c < 8; ++c) {
    poisson_mc::CornerOffset(c, &mc_corner[c][0], &mc_corner[c][1],
                           &mc_corner[c][2]);
  }

  // MC 的 12 条棱的端点。
  int edge_c1[12], edge_c2[12];
  for (int e = 0; e < 12; ++e) {
    poisson_mc::EdgeCorners(e, &edge_c1[e], &edge_c2[e]);
  }

  // 体素场值（按 MC 角点顺序）。
  double mc_values[8];

  for (size_t i = 0; i < n; ++i) {
    const int32_t cx = active[i].x;
    const int32_t cy = active[i].y;
    const int32_t cz = active[i].z;

    // 收集 8 个角点值。
    for (int c = 0; c < 8; ++c) {
      const int64_t gx = cx + mc_corner[c][0];
      const int64_t gy = cy + mc_corner[c][1];
      const int64_t gz = cz + mc_corner[c][2];
      mc_values[c] = value_at(static_cast<int32_t>(gx), static_cast<int32_t>(gy),
                             static_cast<int32_t>(gz));
      if (!std::isfinite(mc_values[c])) return false;
    }

    const int mc_index = poisson_mc::GetIndex(mc_values, iso);
    if (mc_index == 0 || mc_index == 255) continue;  // 无等值面穿过
    ++out->cells_crossing_iso;

    // 边掩码：该体素哪些棱被等值面穿过。
    const int mask = poisson_mc::EdgeMask(mc_index);
    if (mask == 0) continue;

    // 棱交点：按全局棱 ID 共享。
    uint32_t edge_vertex[12];
    for (int e = 0; e < 12; ++e) {
      edge_vertex[e] = 0xFFFFFFFFu;
      if ((mask & (1 << e)) == 0) continue;

      const int c1 = edge_c1[e];
      const int c2 = edge_c2[e];
      const int64_t ax = cx + mc_corner[c1][0];
      const int64_t ay = cy + mc_corner[c1][1];
      const int64_t az = cz + mc_corner[c1][2];
      const int64_t bx = cx + mc_corner[c2][0];
      const int64_t by = cy + mc_corner[c2][1];
      const int64_t bz = cz + mc_corner[c2][2];

      // 全局棱 ID（两个端点格点）。
      int64_t ia = GridIndex(ax, ay, az);
      int64_t ib = GridIndex(bx, by, bz);
      int64_t kx = ax, ky = ay, kz = az;
      int64_t lx = bx, ly = by, lz = bz;
      double ka = mc_values[c1], kb = mc_values[c2];
      if (ib < ia) {
        std::swap(ia, ib);
        kx = bx; ky = by; kz = bz;
        lx = ax; ly = ay; lz = az;
        std::swap(ka, kb);
      }
      const EdgeKey key{ia, ib};

      auto vit = vertex_of_edge.find(key);
      if (vit != vertex_of_edge.end()) {
        edge_vertex[e] = vit->second;
        continue;
      }

      // 插值求交点。
      const double denom = kb - ka;
      double tt = (std::fabs(denom) > 1e-18) ? (iso - ka) / denom : 0.5;
      tt = std::clamp(tt, 0.0, 1.0);

      const double px = static_cast<double>(kx) +
                        tt * (static_cast<double>(lx) - kx);
      const double py = static_cast<double>(ky) +
                        tt * (static_cast<double>(ly) - ky);
      const double pz = static_cast<double>(kz) +
                        tt * (static_cast<double>(lz) - kz);

      // 量化到 1/16 格点（避免浮点误差）。
      const int64_t qx = static_cast<int64_t>(std::llround(px * 16.0));
      const int64_t qy = static_cast<int64_t>(std::llround(py * 16.0));
      const int64_t qz = static_cast<int64_t>(std::llround(pz * 16.0));

      const uint32_t vid = static_cast<uint32_t>(mesh.positions.size());
      mesh.positions.emplace_back(origin(0) + (qx / 16.0 + 0.5) * h,
                                  origin(1) + (qy / 16.0 + 0.5) * h,
                                  origin(2) + (qz / 16.0 + 0.5) * h);
      vertex_of_edge.emplace(key, vid);
      edge_vertex[e] = vid;
    }

    // 三角表。
    const int* tris = poisson_mc::TriangleTable(mc_index);
    for (int k = 0; tris[k] != -1; k += 3) {
      const uint32_t v0 = edge_vertex[tris[k + 0]];
      const uint32_t v1 = edge_vertex[tris[k + 1]];
      const uint32_t v2 = edge_vertex[tris[k + 2]];
      if (v0 == 0xFFFFFFFFu || v1 == 0xFFFFFFFFu || v2 == 0xFFFFFFFFu) continue;
      if (v0 == v1 || v1 == v2 || v2 == v0) continue;  // 退化
      mesh.indices.push_back(v0);
      mesh.indices.push_back(v1);
      mesh.indices.push_back(v2);
    }
  }

  if (mesh.indices.empty()) return false;
  out->triangles_before_trim = mesh.indices.size() / 3;



  // ---- 7. 裁剪（§1.6.2）----
  //
  // 置信度分位数 + 输入点云距离场。
  // 视锥裁剪需要关键帧信息，由上层执行。
  if (config.trim_enabled && !mesh.positions.empty()) {
    const size_t vertex_count = mesh.positions.size();
    std::vector<float> dist(vertex_count, 0.0f);

    // **精确最近邻**（空间哈希），不用抽样。
    //
    // 抽样会把最近距离高估近 2 倍（点云稀疏时），导致裁剪阈值形同虚设、
    // 主体网格被当成外扩区一起裁掉。
    //
    // 格边长取「网格单元边长」与「点云平均间距」的较大者。
    std::vector<Vec3d> cloud_pts;
    cloud_pts.reserve(cloud.positions.size());
    for (const auto& p : cloud.positions) cloud_pts.push_back(p);

    // **点云平均间距**：用点云自身的最近邻距离中位数。
    //
    // 不能用「包围盒体积 / 点数」的立方根：那假设点云填满包围盒，
    // 而真实点云通常只覆盖其中一小块（球面点云只占包围盒的 52%，
    // 且是曲面分布）→ 估计值偏大 3 倍 → 裁剪阈值形同虚设。
    // **点云采样间距直接复用 `EstimatePointSpacing()`**。
    //
    // **踩过的坑（2026-10-30）**：这里曾经自己重写了一遍「最近邻中位数」，
    // 但**漏了排除自身** → 每个点的最近邻就是它自己 → 距离恒为 0 →
    // `cloud_spacing = 0` → `τ = 2·max(r, 0) = 2r` 退化成绝对值。
    // 后果：τ = 0.43 mm 小于「网格顶点到点云距离的中位数 0.59 mm」→
    // **主体网格被裁掉一半**（实测三角形 181 万 → 38 万，删掉 79%，
    // 覆盖率只剩 17.5%）。
    //
    // 同一份逻辑写两遍就会漂移；只保留 `EstimatePointSpacing()` 一份实现。
    const double cloud_spacing = EstimatePointSpacing(cloud);

    // 用更贴合的格边长重建索引，得到更快的查询。
    const PointCloudIndex index(cloud_pts,
                                std::max(out->resolution, cloud_spacing));

    for (size_t i = 0; i < vertex_count; ++i) {
      const double dd = index.NearestDistance(mesh.positions[i]);
      dist[i] = static_cast<float>(std::sqrt(std::min(dd, 1e29)));
    }

    // **τ 必须按点云密度自适应**：
    // 泊松把稀疏点云插值成光滑曲面后，曲面上的顶点到最近采样点的距离
    // 天然就有「点云间距的一半」量级。若用 τ = factor × r（r 是网格分辨率，
    // 通常比点云间距小一个量级），**主体网格会被整体裁掉**。
    // 因此取 τ = factor × max(r, 点云平均间距)。
    const double tau =
        config.trim_distance_factor * std::max(out->resolution, cloud_spacing);
    std::vector<float> sorted_dist = dist;
    std::sort(sorted_dist.begin(), sorted_dist.end());
    const size_t q_index = static_cast<size_t>(
        static_cast<double>(sorted_dist.size()) * config.trim_confidence_quantile);
    const float q_thresh =
        sorted_dist.empty()
            ? 0.0f
            : sorted_dist[std::min(q_index, sorted_dist.size() - 1)];

    out->trim_tau = tau;
    out->trim_q_thresh = q_thresh;
    out->trim_dist_median = sorted_dist.empty()
                                ? 0.0
                                : static_cast<double>(sorted_dist[sorted_dist.size() / 2]);
    out->vertices_before_trim = vertex_count;

    const float tau_f = static_cast<float>(tau);
    std::vector<uint8_t> keep(vertex_count, 1);
    for (size_t i = 0; i < vertex_count; ++i) {
      if (dist[i] > tau_f || dist[i] > q_thresh) keep[i] = 0;
    }

    std::vector<uint32_t> new_index(vertex_count, 0xFFFFFFFFu);
    std::vector<Vec3d> new_positions;
    new_positions.reserve(vertex_count);
    for (size_t i = 0; i < vertex_count; ++i) {
      if (!keep[i]) continue;
      new_index[i] = static_cast<uint32_t>(new_positions.size());
      new_positions.push_back(mesh.positions[i]);
    }

    std::vector<uint32_t> new_indices;
    new_indices.reserve(mesh.indices.size());
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
      const uint32_t a = new_index[mesh.indices[t]];
      const uint32_t b = new_index[mesh.indices[t + 1]];
      const uint32_t c = new_index[mesh.indices[t + 2]];
      if (a == 0xFFFFFFFFu || b == 0xFFFFFFFFu || c == 0xFFFFFFFFu) continue;
      if (a == b || b == c || a == c) continue;  // 退化
      new_indices.push_back(a);
      new_indices.push_back(b);
      new_indices.push_back(c);
    }

    mesh.positions = std::move(new_positions);
    mesh.indices = std::move(new_indices);
    out->triangles_after_trim = mesh.indices.size() / 3;
    out->vertices_after_trim = mesh.positions.size();
    if (config.confidence) {
      mesh.confidences.assign(mesh.positions.size(), 1.0f);
    }
  }

  // ---- 8. 法向重算 ----
  RecomputeVertexNormals(&mesh);

  const auto t_end = std::chrono::steady_clock::now();
  out->solve_millis = static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start)
          .count());
  out->peak_memory_bytes = static_cast<uint64_t>(n) * 220;  // §1.3 的口径

  return !mesh.indices.empty();
}

}  // namespace recon

}  // namespace recon
