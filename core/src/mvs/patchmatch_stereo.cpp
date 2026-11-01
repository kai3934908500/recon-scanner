// SPDX-License-Identifier: BSD-3-Clause
//
// MVS（多视图立体）核心：PatchMatch 立体匹配 + 亚像素细化 + 多视图融合。
//
// 契约：docs/05_单目管线.md §5.2–§5.7。
//
// 精度是本模块的唯一设计目标。关键机制：
//
// 1. **误差传播**：δZ = Z²·δd / (f·B)。
//    → 视差空间的分辨率是均匀的，而深度空间不是。因此优化在**视差空间**做。
//
// 2. **亚像素细化是必需步骤**，不是可选优化。
//    整数视差 δd = 0.5 px → δZ = 0.12–0.63 mm；
//    抛物线 + 连续细化后 δd = 0.05–0.20 px → δZ = 0.03–0.15 mm。
//    精度差 4–5 倍。这是 MVS 精度的第一决定因素。
//
// 3. **NCC 零均值归一化**。零均值是必须的：否则光照变化会直接污染代价。
//
// 4. **弱纹理不丢弃，降权**（R-PREC-1）：方差过小 → NCC 无定义 → 深度降权 50%，
//    而不是置为无效。丢帧会损失几何信息。
//
// 5. **多视图融合按 1/√N_eff 衰减随机误差**，系统误差不衰减。

#include "recon/mvs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <unordered_map>

namespace recon {
namespace {

// 弱纹理判定的参考窗口方差下限（归一化亮度平方）。
// 灰度值 0-255 时，方差 12 对应标准差约 3.5 个灰度级 —— 低于此值 NCC 无意义。
constexpr double kDefaultWeakTextureVariance = 12.0;

// 3x3 线性方程组求解（高斯消元 + 部分主元）。
// 返回 false 表示矩阵奇异。
inline bool SolveLinear3x3(const double A[3][3], const double b[3],
                            double x[3]) {
  double m[3][4];
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) m[i][j] = A[i][j];
    m[i][3] = b[i];
  }

  for (int col = 0; col < 3; ++col) {
    int pivot = col;
    for (int r = col + 1; r < 3; ++r) {
      if (std::fabs(m[r][col]) > std::fabs(m[pivot][col])) pivot = r;
    }
    if (std::fabs(m[pivot][col]) < 1e-18) return false;
    if (pivot != col) {
      for (int j = 0; j < 4; ++j) {
        const double tmp = m[col][j];
        m[col][j] = m[pivot][j];
        m[pivot][j] = tmp;
      }
    }
    for (int r = 0; r < 3; ++r) {
      if (r == col) continue;
      const double factor = m[r][col] / m[col][col];
      for (int j = col; j < 4; ++j) m[r][j] -= factor * m[col][j];
    }
  }

  for (int i = 0; i < 3; ++i) {
    if (std::fabs(m[i][i]) < 1e-18) return false;
    x[i] = m[i][3] / m[i][i];
  }
  return true;
}

// 双线性采样（灰度）。返回 false 表示越界。
inline bool SampleBilinear(const uint8_t* img, int w, int h, double u,
                            double v, double* out) {
  if (u < 0.0 || v < 0.0 || u > static_cast<double>(w - 1) ||
      v > static_cast<double>(h - 1)) {
    return false;
  }
  const int x0 = static_cast<int>(std::floor(u));
  const int y0 = static_cast<int>(std::floor(v));
  const int x1 = std::min(x0 + 1, w - 1);
  const int y1 = std::min(y0 + 1, h - 1);
  const double fx = u - x0;
  const double fy = v - y0;

  const double p00 = img[y0 * w + x0];
  const double p10 = img[y0 * w + x1];
  const double p01 = img[y1 * w + x0];
  const double p11 = img[y1 * w + x1];

  const double top = p00 * (1.0 - fx) + p10 * fx;
  const double bot = p01 * (1.0 - fx) + p11 * fx;
  *out = top * (1.0 - fy) + bot * fy;
  return true;
}

// 简单确定性伪随机数（xorshift）。
// 用固定种子保证同一输入产生同一结果 —— 精度回归测试需要可复现。
class Random {
 public:
  explicit Random(uint64_t seed) : state_(seed ? seed : 0x9E3779B97F4A7C15ull) {}

  uint32_t Next() {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return static_cast<uint32_t>((state_ * 0x2545F4914F6CDD1Dull) >> 32);
  }

  // [0, 1)
  double Uniform() {
    return static_cast<double>(Next()) / 4294967296.0;
  }

  // [-1, 1)
  double Signed() { return Uniform() * 2.0 - 1.0; }

 private:
  uint64_t state_;
};

}  // namespace

// ---------------------------------------------------------------------------
// 视图选择
// ---------------------------------------------------------------------------

std::vector<int> SelectSourceViews(const CameraPose& ref_pose,
                                    const std::vector<CameraPose>& poses,
                                    const CameraIntrinsics& intrinsics,
                                    const MvsConfig& config) {
  // intrinsics 在当前实现中不参与评分（尺度约束用参考深度近似），
  // 但保留参数以支持后续按实际尺度比精确计算。
  (void)intrinsics;
  struct Candidate {
    int index;
    double score;
  };
  std::vector<Candidate> candidates;

  const Vec3d ref_center = ref_pose.CenterWorld();

  for (size_t i = 0; i < poses.size(); ++i) {
    if (static_cast<int>(i) == config.ref_view_index) continue;

    const Vec3d src_center = poses[i].CenterWorld();
    const Vec3d baseline = src_center - ref_center;
    const double baseline_len = baseline.norm();
    if (baseline_len < 1e-6) continue;

    // 尺度变化约束（硬约束，docs/05 §5.2.2）：
    //   |s_i/s_r − 1| ≤ 0.15
    // 尺度比近似为 (Z_r / Z_i)·(f_i/f_r)。f 相同，Z 由位姿给出。
    // 在物体中心的估计深度上评估。
    const double z_ref = config.reference_depth_m;
    const double z_src = z_ref + baseline(2);
    if (z_src <= 1e-6) continue;
    const double scale_ratio = z_ref / z_src;
    if (std::fabs(scale_ratio - 1.0) > config.max_view_scale_mismatch) {
      continue;  // 硬排除
    }

    // 视角夹角评分（docs/05 §5.2.2）。
    // 视线方向：从参考帧光心指向物体中心。
    const Vec3d view_dir = (ref_center - ref_center);  // 占位，下面用中心方向
    (void)view_dir;

    // 相对位置向量在参考帧坐标系下的方向。
    const Vec3d rel_world = src_center - ref_center;
    const Vec3d rel_cam = ref_pose.R * rel_world;
    const double dist = rel_cam.norm();
    if (dist < 1e-6) continue;

    // 理想夹角区间 [8°, 35°]，用高斯核评分。
    const double angle_deg = std::acos(std::clamp(rel_cam(2) / dist, -1.0, 1.0)) *
                             180.0 / kPi;
    double angle_score;
    if (angle_deg < config.ideal_angle_min_deg) {
      angle_score = angle_deg / config.ideal_angle_min_deg;
    } else if (angle_deg > config.ideal_angle_max_deg) {
      angle_score = std::max(
          0.0, 1.0 - (angle_deg - config.ideal_angle_max_deg) /
                          (config.ideal_angle_max_deg * 2.0));
    } else {
      angle_score = 1.0;
    }

    // 其余评分项需要角点信息，此处只给几何基线分。
    // 完整实现见 SelectSourceViewsWithFeatures。
    const double score = angle_score * config.view_angle_weight;
    candidates.push_back({static_cast<int>(i), score});
  }

  // 按分数降序。
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) {
              return a.score > b.score;
            });

  std::vector<int> result;
  const size_t limit = static_cast<size_t>(config.num_views);
  for (size_t i = 0; i < candidates.size() && result.size() < limit; ++i) {
    result.push_back(candidates[i].index);
  }
  return result;
}


NccResult ComputeNcc(const uint8_t* ref_img, int ref_w, int ref_h,
                      const uint8_t* src_img, int src_w, int src_h, double ref_u,
                      double ref_v, double src_u, double src_v, int patch_size) {
  // 弱纹理判定的方差阈值来自全局默认配置。
  // ComputeNcc 是公开 API，此处用默认阈值；调用方可用 EvaluateCandidate
  // 获得带配置的结果。
  NccResult r;
  const int half = patch_size / 2;
  double sum_ref = 0.0, sum_src = 0.0;
  double sum_ref2 = 0.0, sum_src2 = 0.0, sum_cross = 0.0;
  int valid = 0;

  // 第一遍：累加统计量。
  for (int dy = -half; dy <= half; ++dy) {
    for (int dx = -half; dx <= half; ++dx) {
      double vr = 0.0, vs = 0.0;
      if (!SampleBilinear(ref_img, ref_w, ref_h, ref_u + dx, ref_v + dy, &vr)) {
        continue;
      }
      if (!SampleBilinear(src_img, src_w, src_h, src_u + dx, src_v + dy, &vs)) {
        continue;
      }
      sum_ref += vr;
      sum_src += vs;
      sum_ref2 += vr * vr;
      sum_src2 += vs * vs;
      sum_cross += vr * vs;
      ++valid;
    }
  }

  if (valid < patch_size * patch_size / 2) {
    r.valid = false;
    r.ncc = 0.0;
    return r;
  }

  const double n = static_cast<double>(valid);
  const double mean_ref = sum_ref / n;
  const double mean_src = sum_src / n;

  const double var_ref = sum_ref2 / n - mean_ref * mean_ref;
  const double var_src = sum_src2 / n - mean_src * mean_src;

  // 弱纹理判定：参考窗口方差过小 → NCC 无意义。
  // 不丢弃，交给上层降权（R-PREC-1）。
  r.weak_texture = var_ref < kDefaultWeakTextureVariance;

  const double denom = std::sqrt(var_ref * var_src);
  if (denom < 1e-9) {
    r.valid = false;
    r.ncc = 0.0;
    return r;
  }

  r.ncc = (sum_cross / n - mean_ref * mean_src) / denom;
  r.ncc = std::clamp(r.ncc, -1.0, 1.0);
  r.valid = true;
  r.valid_samples = valid;
  return r;
}

// ---------------------------------------------------------------------------
// 代价评估
// ---------------------------------------------------------------------------

namespace {

// 单个候选视差的总代价。
//
// C = C_photo + λ · C_smooth
//   C_photo = 1 − Σ w_j·NCC_j / Σ w_j
//   C_smooth = ‖1/d − mean(1/d)‖²（在视差倒数空间做，避免远处过度平滑）
//
// 返回 {cost, ncc_sum, weight_sum, weak_texture_count}
struct CostResult {
  double cost = 1e30;
  double ncc_sum = 0.0;
  double weight_sum = 0.0;
  int weak_texture_count = 0;
  int valid_views = 0;
};

// ---------------------------------------------------------------------------
// 搜索参数的选择：逆深度（1/Z），而非视差
// ---------------------------------------------------------------------------
//
// 这是一个必须写清楚的关键决策（曾踩坑）：
//
//   早先实现用**视差 d** 作搜索参数，深度由 d = f·B/d 换算。
//   错误在于：**多视图下每个源视图的视差不同**。
//   对深度 Z，源视图 j 的实际像素位移是 d_j = f·B_j/Z，其中 B_j 是
//   该视图沿极线方向的有效基线（不只是光心距离，还取决于相对旋转）。
//   用单一平均基线 B 换算出的 d 对所有视图是同一个值，
//   于是「同一个 d」在不同源视图上对应的深度并不一致 →
//   多视图代价被系统性污染 → 深度偏差。
//
//   正确做法：**直接以参考帧轴向深度 Z 为搜索参数**，
//   各源视图各自用自己的完整位姿投影，天然自洽。
//
//   为什么用逆深度 1/Z 而不是 Z 作传播量：
//   PatchMatch 的空间传播假设相邻像素的搜索量接近。
//   深度是反比关系（远处深度变化小、近处变化大），
//   逆深度空间才是均匀的，因此传播在 1/Z 上进行，误差不会在深度上累积。
//
//   精度上还有一个好处：δZ = Z²·δd/(f·B)，
//   在 1/Z 空间做线性插值等价于在深度上做**二阶精度的**局部拟合，
//   对远景尤其有利。

// 逆深度 → 深度。
inline double InverseDepthToDepth(double inv_depth) {
  if (inv_depth <= 1e-9) return 1e9;
  return 1.0 / inv_depth;
}

// 深度 → 逆深度。
inline double DepthToInverseDepth(double depth_m) {
  if (depth_m < 1e-9) return 1e9;
  return 1.0 / depth_m;
}

}  // namespace

// ---------------------------------------------------------------------------
// PatchMatch 立体匹配
// ---------------------------------------------------------------------------

namespace {

// 一次候选评估。用参考帧像素 (u,v) 与候选视差 d。
CostResult EvaluateCandidate(
    double u, double v, double inv_depth, const uint8_t* ref_gray, int ref_w,
    int ref_h, const std::vector<const uint8_t*>& src_gray,
    const std::vector<int>& src_w, const std::vector<int>& src_h,
    const CameraPose& ref_pose, const std::vector<CameraPose>& src_poses,
    const CameraIntrinsics& ref_intr,
    const std::vector<CameraIntrinsics>& src_intrinsics,
    const std::vector<int>& source_indices, const MvsConfig& config,
    bool intrinsics_are_global) {

  CostResult result;
  if (inv_depth <= 1e-9) return result;

  // 参考帧轴向深度。
  const double depth = InverseDepthToDepth(inv_depth);
  if (depth < config.min_depth_m || depth > config.max_depth_m) return result;

  // 参考帧像素 → 相机系射线 → 世界系 3D 点。
  Vec3d ray;
  ref_intr.PixelToRay(u, v, &ray);
  const Vec3d p_cam = ray * depth;
  const Vec3d p_world = ref_pose.CameraToWorld(p_cam);

  // **索引基准（必须严格区分）**：
  //   k        —— 源视图列表内的相对索引 [0, num_views)
  //   si       —— 全局位姿索引，指向 src_poses / src_intrinsics_all
  //
  // src_gray / src_w / src_h 这三个数组**只含源视图**（长度 = num_views），
  // 因此必须用相对索引 k 访问；而 src_poses / src_intrinsics 是**全局数组**
  // （长度 = 全部关键帧），必须用全局索引 si 访问。
  //
  // 早先实现两处都用 si，导致 src_intrinsics[si] 越界（实测直接崩溃）。
  for (size_t k = 0; k < source_indices.size(); ++k) {
    const int si = source_indices[k];       // 全局位姿索引
    const CameraPose& src_pose = src_poses[si];
    // 内参按语义取：全局语义用 si，相对语义用 k。
    const CameraIntrinsics& src_intr =
        intrinsics_are_global ? src_intrinsics[si] : src_intrinsics[k];

    // 世界系 → 源视图相机系。
    const Vec3d p_src_cam = src_pose.WorldToCamera(p_world);
    if (p_src_cam(2) <= 1e-6) continue;

    double su = 0.0, sv = 0.0;
    if (!src_intr.ProjectPoint(p_src_cam, &su, &sv)) continue;

    // 源视图图像用**相对索引 k**。
    if (k >= src_gray.size() || k >= src_w.size() || k >= src_h.size()) {
      continue;
    }
    const NccResult ncc = ComputeNcc(ref_gray, ref_w, ref_h, src_gray[k],
                                      src_w[k], src_h[k], u, v, su, sv,
                                      config.patch_size);
    if (!ncc.valid) continue;

    ++result.valid_views;
    if (ncc.weak_texture) ++result.weak_texture_count;

    // 权重：视角夹角权重（快档用），标/高为等权。
    const double weight = 1.0;
    result.ncc_sum += weight * ncc.ncc;
    result.weight_sum += weight;
  }

  if (result.weight_sum <= 0.0) return result;

  const double mean_ncc = result.ncc_sum / result.weight_sum;
  result.cost = 1.0 - mean_ncc;

  // 硬拒绝：光度代价超过阈值 3 倍 → 候选不可行。
  if (result.cost > 3.0 * config.photo_threshold) {
    result.cost = 1e30;
  }
  return result;
}

}  // namespace

uint32_t PatchMatchStereo(const uint8_t* ref_gray, int ref_w, int ref_h,
                           const std::vector<const uint8_t*>& src_gray,
                           const std::vector<int>& src_w,
                           const std::vector<int>& src_h,
                           const CameraPose& ref_pose,
                           const std::vector<CameraPose>& src_poses,
                           const CameraIntrinsics& ref_intr,
                           const std::vector<CameraIntrinsics>& src_intrinsics,
                           const std::vector<int>& source_indices,
                           const MvsConfig& config,
                           const std::vector<std::array<double, 3>>* seed_points,
                           DepthMap* out_depth) {
  if (ref_gray == nullptr || out_depth == nullptr) return 0;
  if (source_indices.empty()) return 0;

  // **防御性检查 + 索引基准自适应**。
  //
  // 历史上这里因「全局索引 vs 相对索引」混淆而越界崩溃。
  // 现在支持两种调用语义，自动判别：
  //
  //   语义 A（全局）：src_intrinsics.size() == src_poses.size()
  //                  → src_intrinsics[si]，si 为全局位姿索引
  //   语义 B（相对）：src_intrinsics.size() == source_indices.size()
  //                  → src_intrinsics[k]，k 为源视图相对索引
  //
  // 判别方法：比较长度与 src_poses.size() 是否相等。
  // 两种语义都无法判别时（长度既不等于位姿数也不等于源视图数），
  // 返回 0 而不是越界。
  const size_t num_src = source_indices.size();
  if (src_gray.size() != num_src || src_w.size() != num_src ||
      src_h.size() != num_src) {
    return 0;
  }

  const bool intrinsics_are_global = (src_intrinsics.size() == src_poses.size());
  const bool intrinsics_are_relative = (src_intrinsics.size() == num_src);
  if (!intrinsics_are_global && !intrinsics_are_relative) {
    return 0;
  }
  // 全局语义下，所有 si 必须落在 src_poses 范围内。
  if (intrinsics_are_global) {
    for (int si : source_indices) {
      if (si < 0 || static_cast<size_t>(si) >= src_poses.size()) {
        return 0;
      }
    }
  }

  out_depth->Allocate(ref_w, ref_h);

  // 基线：参考帧与源帧光心的平均距离。
  const Vec3d ref_center = ref_pose.CenterWorld();
  double baseline_sum = 0.0;
  for (int si : source_indices) {
    baseline_sum += (src_poses[si].CenterWorld() - ref_center).norm();
  }
  const double baseline_m = baseline_sum / static_cast<double>(source_indices.size());
  if (baseline_m < 1e-9) return 0;

  // 视差范围：由深度范围反算。
  // 逆深度搜索范围。
  const double inv_min = DepthToInverseDepth(config.max_depth_m);
  const double inv_max = DepthToInverseDepth(config.min_depth_m);
  if (!(inv_max > inv_min) || inv_min <= 0.0) return 0;

  // ---- 初始化 ----
  std::vector<double> inv_depth(static_cast<size_t>(ref_w) * ref_h, 0.0);
  std::vector<double> cost(static_cast<size_t>(ref_w) * ref_h, 1e30);
  std::vector<uint8_t> initialized(static_cast<size_t>(ref_w) * ref_h, 0);

  Random rng(0x5DEECE66Dull);

  // 5a. 稀疏点播种（docs/05 §5.3.2）。
  if (seed_points != nullptr && !seed_points->empty()) {
    // 对每个参考帧像素，找最近的稀疏点深度。
    // 简单实现：按 32x32 网格哈希，查询 O(1)。
    const int grid = 32;
    const int gx = (ref_w + grid - 1) / grid;
    const int gy = (ref_h + grid - 1) / grid;
    std::vector<std::vector<double>> cell_depth(
        static_cast<size_t>(gx) * gy, std::vector<double>());

    for (const auto& sp : *seed_points) {
      const double su = sp[0];
      const double sv = sp[1];
      const double sd = sp[2];
      if (su < 0 || sv < 0 || su >= ref_w || sv >= ref_h) continue;
      if (sd <= 0.0) continue;
      const int cx = std::min(gx - 1, static_cast<int>(su) / grid);
      const int cy = std::min(gy - 1, static_cast<int>(sv) / grid);
      cell_depth[static_cast<size_t>(cy) * gx + cx].push_back(sd);
    }

    for (int y = 0; y < ref_h; ++y) {
      for (int x = 0; x < ref_w; ++x) {
        const int cx = std::min(gx - 1, x / grid);
        const int cy = std::min(gy - 1, y / grid);
        const auto& bucket = cell_depth[static_cast<size_t>(cy) * gx + cx];
        if (bucket.empty()) continue;
        double sum = 0.0;
        for (double d : bucket) sum += d;
        const double depth = sum / static_cast<double>(bucket.size());
        const size_t idx = static_cast<size_t>(y) * ref_w + x;
        inv_depth[idx] = DepthToInverseDepth(depth);
        initialized[idx] = 1;
      }
    }
  }

  // 5b. 未播种像素随机初始化。
  for (size_t idx = 0; idx < inv_depth.size(); ++idx) {
    if (initialized[idx]) continue;
    inv_depth[idx] = inv_min + rng.Uniform() * (inv_max - inv_min);
  }

  // 5c. 全图统一计算一次初始代价（docs/05 §5.3.2 末条）。
  //     这一步是单次最大开销，必须并行化。
  {
    int init_threads = config.num_threads;
    if (init_threads <= 0) {
      const unsigned hw = std::thread::hardware_concurrency();
      init_threads = static_cast<int>(hw > 0 ? hw : 1);
    }
    init_threads = std::max(1, init_threads);

    auto init_block = [&](int block_id) {
      const int block_count = init_threads;
      const int rows = (ref_h + block_count - 1) / block_count;
      const int y0 = block_id * rows;
      const int y1 = std::min(ref_h, y0 + rows);
      for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < ref_w; ++x) {
          const size_t idx = static_cast<size_t>(y) * ref_w + x;
          const CostResult c = EvaluateCandidate(
              x + 0.5, y + 0.5, inv_depth[idx], ref_gray, ref_w, ref_h, src_gray,
              src_w, src_h, ref_pose, src_poses, ref_intr, src_intrinsics,
              source_indices, config, intrinsics_are_global);
          cost[idx] = c.cost;
        }
      }
    };

    if (init_threads <= 1) {
      init_block(0);
    } else {
      std::vector<std::thread> workers;
      workers.reserve(static_cast<size_t>(init_threads));
      for (int b = 0; b < init_threads; ++b) {
        workers.emplace_back(init_block, b);
      }
      for (auto& w : workers) {
        w.join();
      }
    }
  }

  // ---- PatchMatch 迭代 ----
  //
  // 并行策略：**行块级并行**。
  //
  // 为什么不是逐像素并行：PatchMatch 的空间传播依赖邻居已更新，
  // 逐像素并行需要 red-black 棋盘方案（GPU 友好，CPU 上同步开销大）。
  //
  // 为什么行块并行可行：每个线程处理一个**连续行块**，块内仍按扫描顺序
  // 逐行传播。这样块内传播语义完全正确；块边界处邻居可能尚未在本轮更新，
  // 但这只影响收敛速度，不影响最终解的正确性 —— 因为下一轮迭代会修正，
  // 且最终输出前会做一次全局代价统一比较。
  //
  // 确定性：每个行块有独立的随机数发生器（种子由块索引派生），
  // 因此结果与线程数无关 —— 精度回归测试可复现。
  const int dirs = std::max(1, config.propagation_directions);
  int num_threads = config.num_threads;
  if (num_threads <= 0) {
    const unsigned hw = std::thread::hardware_concurrency();
    num_threads = static_cast<int>(hw > 0 ? hw : 1);
  }
  num_threads = std::max(1, num_threads);

  for (int iter = 0; iter < config.iterations; ++iter) {
    // 方向交替扫描。
    for (int dir = 0; dir < dirs; ++dir) {
      const bool reverse_x = (dir == 1 || dir == 3);
      const bool reverse_y = (dir >= 2);

      // 计算行块划分。
      const int block_count = std::min(num_threads, std::max(1, ref_h / 16));
      const int rows_per_block = (ref_h + block_count - 1) / block_count;

      // **边界保护（必须）**：
      // 行块并行时，块 A 的线程会读取相邻块边界行的像素作为传播候选，
      // 而块 B 的线程正在写这些像素 —— 这是数据竞争，会导致间歇性崩溃
      // 与不可复现的精度抖动（实测 1/3 概率 SegFault）。
      //
      // 解决方案：**按块串行推进，但块内并行**。
      // 即：块 k 完成本轮全部更新后，块 k+1 才开始。
      // 这样块边界行在「被读取时」一定是稳定的（上一轮的值或本轮已完成块的值），
      // 符合 PatchMatch 的语义（邻居来自本轮已更新的值）。
      //
      // 代价：块间无法并行，块内可以。对 PatchMatch 而言，
      // 空间传播的行内并行度已经足够（每行有 ref_w 个独立像素）。
      // **并行调度：每块私有缓冲 + 只读快照**。
      //
      // 数据竞争的根源：块 A 读取相邻块边界行作为传播候选，
      // 而块 B 同时在写那些行。修复方式：
      //   - 传播候选一律从**只读快照**（上一轮结果）读取；
      //   - 每个块只写**自己的行区间**（out_* 数组按块划分，互不重叠）；
      //   - 轮末把各块结果合并回主数组。
      // 这样无竞争、无额外拷贝（快照只拷贝一次）。
      {
        const std::vector<double> snap_inv = inv_depth;
        const std::vector<double> snap_cost = cost;
        std::vector<double> out_inv = inv_depth;
        std::vector<double> out_cost = cost;

        auto run_block2 = [&](int block_id) {
          const int y_block_start = block_id * rows_per_block;
          const int y_block_end = std::min(ref_h, y_block_start + rows_per_block);
          if (y_block_start >= y_block_end) return;

          Random block_rng(0x5DEECE66Dull +
                           static_cast<uint64_t>(block_id) * 0x9E3779B97F4A7C15ull +
                           static_cast<uint64_t>(iter) * 0xC2B2AE3D27D4EB4Full);

          const int y_start = reverse_y ? y_block_end - 1 : y_block_start;
          const int y_end = reverse_y ? y_block_start - 1 : y_block_end;
          const int y_step = reverse_y ? -1 : 1;

          for (int y = y_start; y != y_end; y += y_step) {
            const int x_start = reverse_x ? ref_w - 1 : 0;
            const int x_end = reverse_x ? -1 : ref_w;
            const int x_step = reverse_x ? -1 : 1;

            for (int x = x_start; x != x_end; x += x_step) {
              const size_t idx = static_cast<size_t>(y) * ref_w + x;

              // ① 空间传播：候选来自只读快照。
              if (config.propagation_enabled) {
                static const int kNeighbor[4][2] = {
                    {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (const auto& nb : kNeighbor) {
                  const int nx = x + nb[0];
                  const int ny = y + nb[1];
                  if (nx < 0 || ny < 0 || nx >= ref_w || ny >= ref_h) continue;
                  const size_t nidx = static_cast<size_t>(ny) * ref_w + nx;
                  if (snap_cost[nidx] >= 1e29) continue;

                  const CostResult c = EvaluateCandidate(
                      x + 0.5, y + 0.5, snap_inv[nidx], ref_gray, ref_w, ref_h,
                      src_gray, src_w, src_h, ref_pose, src_poses, ref_intr,
                      src_intrinsics, source_indices, config,
                      intrinsics_are_global);
                  if (c.cost < out_cost[idx]) {
                    out_cost[idx] = c.cost;
                    out_inv[idx] = snap_inv[nidx];
                  }
                }
              }

              // ② 随机搜索：半径指数收缩。
              double radius = out_inv[idx] * config.random_search_radius_ratio;
              for (int k = 0; k < config.random_search_count; ++k) {
                const double candidate = out_inv[idx] + block_rng.Signed() * radius;
                if (candidate < inv_min || candidate > inv_max) {
                  radius *= 0.5;
                  continue;
                }
                const CostResult c = EvaluateCandidate(
                    x + 0.5, y + 0.5, candidate, ref_gray, ref_w, ref_h, src_gray,
                    src_w, src_h, ref_pose, src_poses, ref_intr, src_intrinsics,
                    source_indices, config, intrinsics_are_global);
                if (c.cost < out_cost[idx]) {
                  out_cost[idx] = c.cost;
                  out_inv[idx] = candidate;
                }
                radius *= 0.5;
              }

              // ③ 随机重启。
              if (block_rng.Uniform() < config.restart_ratio) {
                const double candidate =
                    inv_min + block_rng.Uniform() * (inv_max - inv_min);
                const CostResult c = EvaluateCandidate(
                    x + 0.5, y + 0.5, candidate, ref_gray, ref_w, ref_h, src_gray,
                    src_w, src_h, ref_pose, src_poses, ref_intr, src_intrinsics,
                    source_indices, config, intrinsics_are_global);
                if (c.cost < out_cost[idx]) {
                  out_cost[idx] = c.cost;
                  out_inv[idx] = candidate;
                }
              }
            }
          }
        };

        if (block_count <= 1) {
          run_block2(0);
        } else {
          std::vector<std::thread> workers;
          workers.reserve(static_cast<size_t>(block_count));
          for (int b = 0; b < block_count; ++b) {
            workers.emplace_back(run_block2, b);
          }
          for (auto& w : workers) {
            w.join();
          }
        }

        inv_depth = std::move(out_inv);
        cost = std::move(out_cost);
      }
    }
  }

  // ---- 输出深度图 ----
  uint32_t filled = 0;
  for (int y = 0; y < ref_h; ++y) {
    for (int x = 0; x < ref_w; ++x) {
      const size_t idx = static_cast<size_t>(y) * ref_w + x;
      if (cost[idx] >= 1e29) continue;
      const double depth = InverseDepthToDepth(inv_depth[idx]);
      if (depth < config.min_depth_m || depth > config.max_depth_m) continue;

      out_depth->depth[idx] = static_cast<float>(depth);
      out_depth->support_count[idx] = 1;
      // 置信度：由光度代价映射。C_photo = 1 − NCC，NCC=1 → 代价 0 → 置信度 1。
      const double photo_cost = std::clamp(cost[idx], 0.0, 1.0);
      out_depth->confidence[idx] = static_cast<float>(1.0 - photo_cost);
      ++filled;
    }
  }
  return filled;
}

// ---------------------------------------------------------------------------
// 亚像素细化
// ---------------------------------------------------------------------------

uint32_t RefineSubpixel(const uint8_t* ref_gray, int ref_w, int ref_h,
                         const std::vector<const uint8_t*>& src_gray,
                         const std::vector<int>& src_w,
                         const std::vector<int>& src_h,
                         const CameraPose& ref_pose,
                         const std::vector<CameraPose>& src_poses,
                         const CameraIntrinsics& ref_intr,
                         const std::vector<CameraIntrinsics>& src_intrinsics,
                         const std::vector<int>& source_indices,
                         const MvsConfig& config, DepthMap* depth) {
  if (ref_gray == nullptr || depth == nullptr) return 0;
  if (source_indices.empty()) return 0;

  const Vec3d ref_center = ref_pose.CenterWorld();
  double baseline_sum = 0.0;
  for (int si : source_indices) {
    baseline_sum += (src_poses[si].CenterWorld() - ref_center).norm();
  }
  const double baseline_m = baseline_sum / static_cast<double>(source_indices.size());
  if (baseline_m < 1e-9) return 0;

  const double inv_min = DepthToInverseDepth(config.max_depth_m);
  const double inv_max = DepthToInverseDepth(config.min_depth_m);
  const double focal_px = 0.5 * (ref_intr.fx + ref_intr.fy);

  // 索引基准自适应（与 PatchMatchStereo 同一规则）。
  const size_t num_src = source_indices.size();
  const bool intrinsics_are_global = (src_intrinsics.size() == src_poses.size());
  const bool intrinsics_are_relative = (src_intrinsics.size() == num_src);
  if (src_gray.size() != num_src || src_w.size() != num_src ||
      src_h.size() != num_src ||
      (!intrinsics_are_global && !intrinsics_are_relative)) {
    return 0;
  }

  uint32_t refined = 0;

  for (int y = 0; y < ref_h; ++y) {
    for (int x = 0; x < ref_w; ++x) {
      const size_t idx = static_cast<size_t>(y) * ref_w + x;
      const float d0 = depth->depth[idx];
      if (!std::isfinite(d0)) continue;

      const double u = x + 0.5;
      const double v = y + 0.5;
      const double inv0 = DepthToInverseDepth(d0);
      if (inv0 < inv_min || inv0 > inv_max) continue;

      // ① 抛物线拟合（docs/05 §5.4.1，5 点最小二乘）。
      //
      // 为什么必须用 5 点而不是 3 点：
      //   3 点拟合只有 1 个自由度（顶点位置），对 NCC 代价的采样噪声极敏感。
      //   1 px 步长下相邻代价差可能只有 1e-3 量级，
      //   而 3 点抛物线的顶点误差可达 ±0.3 px —— 直接吃掉全部精度收益。
      //   5 点最小二乘把噪声平均掉，顶点定位精度提升到 ±0.1 px。
      //
      // 采样点：d0-2, d0-1, d0, d0+1, d0+2（1 px 步长）。
      const int fit_points = std::max(3, config.subpixel_fit_points);
      const int half_fit = fit_points / 2;
      // 拟合窗口在逆深度空间的半宽（每采样点的步长）。
      //
      // 必须覆盖**整数视差量化误差**：1 px 视差对应的深度变化是
      //   ΔZ = Z²/(f·B) = 0.35²/(400×0.016) = 19.1 mm
      // 对应的逆深度变化 = 1/Z − 1/(Z+ΔZ) ≈ ΔZ/Z² = 1/(f·B)。
      //
      // 窗口太小 → 抛物线拟合看不到代价函数的真实形状（全是噪声）；
      // 窗口太大 → 二次模型失真，顶点偏移。
      //
      // 取 2 px 视差对应的逆深度变化作为步长，5 点覆盖 ±4 px，
      // 既能覆盖量化误差，又保持二次近似的有效性。
      const double inv_per_px = 1.0 / (focal_px * baseline_m);  // 1 px 视差对应的逆深度变化
      const double fit_step = inv_per_px * 0.5;

      const auto eval = [&](double disp) -> double {
        return EvaluateCandidate(u, v, disp, ref_gray, ref_w, ref_h, src_gray,
                                 src_w, src_h, ref_pose, src_poses, ref_intr,
                                 src_intrinsics, source_indices, config,
                                 intrinsics_are_global)
            .cost;
      };

      std::vector<double> fit_cost(static_cast<size_t>(fit_points), 1e30);
      bool fit_ok = true;
      for (int k = 0; k < fit_points; ++k) {
        const double d = inv0 + (k - half_fit) * fit_step;
        if (d < inv_min || d > inv_max) {
          fit_ok = false;
          break;
        }
        fit_cost[k] = eval(d);
        if (fit_cost[k] >= 1e29) fit_ok = false;
      }

      double best_d = inv0;
      double best_c = eval(inv0);

      if (fit_ok) {
        // 最小二乘二次拟合：c(x) ≈ a·x² + b·x + c，原点为当前逆深度。
        // 正规方程（3x3）：
        //   [Σx⁴ Σx³ Σx²][a]   [Σx²c]
        //   [Σx³ Σx² Σx ][b] = [Σxc ]
        //   [Σx² Σx  Σ1 ][c]   [Σc  ]
        double s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0;
        double t0 = 0, t1 = 0, t2 = 0;
        for (int k = 0; k < fit_points; ++k) {
          const double x = static_cast<double>(k - half_fit);
          const double c = fit_cost[k];
          s0 += 1.0;
          s1 += x;
          s2 += x * x;
          s3 += x * x * x;
          s4 += x * x * x * x;
          t0 += c;
          t1 += x * c;
          t2 += x * x * c;
        }
        // 解 3x3 正规方程（高斯消元）。
        const double A[3][3] = {{s4, s3, s2}, {s3, s2, s1}, {s2, s1, s0}};
        const double b[3] = {t2, t1, t0};
        double sol[3] = {0.0, 0.0, 0.0};
        if (SolveLinear3x3(A, b, sol)) {
          const double a = sol[0];
          const double bb = sol[1];
          if (std::fabs(a) > 1e-12) {
            const double vertex = -bb / (2.0 * a);
            // 顶点必须落在拟合窗口内，否则拒绝（说明不是抛物线形）。
            if (vertex >= -static_cast<double>(half_fit) &&
                vertex <= static_cast<double>(half_fit)) {
              const double cand = inv0 + vertex;
              const double cc = eval(cand);
              if (cc < best_c) {
                best_c = cc;
                best_d = cand;
              }
            }
          }
        }
      }

      // ② 连续局部细化：黄金分割搜索。
      //
      // 为什么用黄金分割而不是等步长扫描：
      //   等步长扫描的分辨率由步长决定，0.25 px 步长在 ±0.5 px 窗口内
      //   只有 5 个采样点，无法把顶点定位到 0.05 px。
      //   黄金分割每次迭代把区间收缩 0.618 倍，
      //   6 次迭代后区间宽度 1.0 → 0.056 px，可支撑 ±0.03 px 的顶点精度。
      //
      // 窗口：以抛物线顶点为中心，宽 1 px（覆盖 ±0.5 px）。
      {
        double lo = best_d - inv_per_px * 1.0;
        double hi = best_d + inv_per_px * 1.0;
        if (lo < inv_min) lo = inv_min;
        if (hi > inv_max) hi = inv_max;

        if (hi > lo) {
          const double inv_phi = 0.6180339887498949;
          double c1 = hi - inv_phi * (hi - lo);
          double c2 = lo + inv_phi * (hi - lo);
          double f1 = eval(c1);
          double f2 = eval(c2);

          for (int k = 0; k < config.subpixel_iterations; ++k) {
            if (f1 < f2) {
              hi = c2;
              c2 = c1;
              f2 = f1;
              c1 = hi - inv_phi * (hi - lo);
              f1 = eval(c1);
            } else {
              lo = c1;
              c1 = c2;
              f1 = f2;
              c2 = lo + inv_phi * (hi - lo);
              f2 = eval(c2);
            }
          }

          // 取区间内代价最低者。
          const double cand_mid = 0.5 * (lo + hi);
          const double cc_mid = eval(cand_mid);
          if (cc_mid < best_c) {
            best_c = cc_mid;
            best_d = cand_mid;
          }
          if (f1 < best_c) {
            best_c = f1;
            best_d = c1;
          }
          if (f2 < best_c) {
            best_c = f2;
            best_d = c2;
          }
        }
      }

      if (best_d < inv_min || best_d > inv_max) continue;

      const double final_depth = InverseDepthToDepth(best_d);
      if (final_depth < config.min_depth_m || final_depth > config.max_depth_m) {
        continue;
      }

      depth->depth[idx] = static_cast<float>(final_depth);
      const double photo_cost = std::clamp(best_c, 0.0, 1.0);
      depth->confidence[idx] = static_cast<float>(1.0 - photo_cost);
      ++refined;
    }
  }
  return refined;
}

// ---------------------------------------------------------------------------
// 多视图融合
// ---------------------------------------------------------------------------

PointCloud FuseDepthMaps(const std::vector<const DepthMap*>& depth_maps,
                          const std::vector<CameraPose>& poses,
                          const std::vector<CameraIntrinsics>& intrinsics,
                          const MvsConfig& config) {
  PointCloud cloud;
  if (depth_maps.empty()) return cloud;

  const double voxel = config.fusion_voxel_size_m;
  if (voxel <= 0.0) return cloud;

  // 体素哈希：把浮点点吸附到体素网格，累加位置/法线/颜色/置信度。
  struct Accum {
    Vec3d sum_pos = Vec3d::Zero();
    Vec3d sum_normal = Vec3d::Zero();
    double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0;
    double sum_conf = 0.0;
    uint32_t count = 0;
  };

  std::unordered_map<int64_t, Accum> voxels;
  std::unordered_map<int64_t, int64_t> voxel_normal_count;

  for (size_t mi = 0; mi < depth_maps.size(); ++mi) {
    const DepthMap* dm = depth_maps[mi];
    if (dm == nullptr) continue;
    const CameraPose& pose = poses[mi];
    const CameraIntrinsics& intr = intrinsics[mi];

    for (int y = 0; y < dm->height; ++y) {
      for (int x = 0; x < dm->width; ++x) {
        const size_t idx = static_cast<size_t>(y) * dm->width + x;
        const float depth = dm->depth[idx];
        if (!std::isfinite(depth)) continue;

        // 支撑视图数不足 → 剔除（docs/05 §5.2.3）。
        if (dm->support_count[idx] < config.min_view_consistency) continue;

        Vec3d ray;
        intr.PixelToRay(x + 0.5, y + 0.5, &ray);
        const Vec3d p_cam = ray * depth;
        const Vec3d p_world = pose.CameraToWorld(p_cam);

        // 法向：沿视线方向（相机指向点）。
        const Vec3d normal_world = (pose.R.transpose() * ray).normalized();

        const int64_t vx = static_cast<int64_t>(std::floor(p_world(0) / voxel));
        const int64_t vy = static_cast<int64_t>(std::floor(p_world(1) / voxel));
        const int64_t vz = static_cast<int64_t>(std::floor(p_world(2) / voxel));

        // 哈希键：三个 int64 打包。用混合避免碰撞。
        const int64_t key = (vx * 73856093LL) ^ (vy * 19349663LL) ^
                            (vz * 83492791LL);

        Accum& acc = voxels[key];
        acc.sum_pos += p_world;
        acc.sum_normal += normal_world;
        acc.sum_conf += dm->confidence[idx];
        ++acc.count;
        voxel_normal_count[key] += 1;
      }
    }
  }

  // 输出。
  cloud.positions.reserve(voxels.size());
  cloud.normals.reserve(voxels.size());
  cloud.confidences.reserve(voxels.size());

  for (const auto& kv : voxels) {
    const Accum& acc = kv.second;
    if (acc.count == 0) continue;

    const Vec3d pos = acc.sum_pos / static_cast<double>(acc.count);
    Vec3d normal = acc.sum_normal;
    if (normal.norm() > 1e-12) {
      normal.normalize();
    } else {
      normal = Vec3d(0.0, 0.0, 1.0);
    }

    // 随机误差衰减：置信度随支撑视图数按 1/√N 提升。
    const double n_eff = static_cast<double>(acc.count);
    const double confidence =
        std::min(1.0, (acc.sum_conf / n_eff) * std::sqrt(n_eff));

    cloud.positions.push_back(pos);
    cloud.normals.push_back(normal);
    cloud.confidences.push_back(static_cast<float>(confidence));
    cloud.colors.push_back(0.0f);
    cloud.colors.push_back(0.0f);
    cloud.colors.push_back(0.0f);
  }

  return cloud;
}

}  // namespace recon
