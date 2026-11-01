// SPDX-License-Identifier: BSD-3-Clause
//
// SfM 特征提取、描述子、匹配与匹配图。契约：docs/05 §4.2–§4.3。
//
// 描述子：旋转归一化的梯度方向直方图（128 维 uint8），与 SIFT 同源但不含
// 尺度空间 —— 差异与代价见 sfm.h 的 SfmFeature 注释，不假装等价。
//
// 踩坑记录：
//   1. **描述子必须做旋转对齐**，否则视图间的旋转会让直方图 bin 整体偏移，
//      匹配召回率断崖下跌（实测：不对齐时同一纹理绕光轴转 15° 后
//      匹配数从 ~90% 掉到 ~20%）。
//   2. **比值检验必须配合双向一致性**（§4.3.2 的说明）：重复纹理上
//      单靠 ratio test 会产生「互为最近邻但都错」的成片外点。

#include "recon/sfm.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "recon/vio.h"

namespace recon {
namespace {

// 双线性采样灰度（越界返回 0 并标记失败）。
inline bool SampleBilinear(const uint8_t* gray, int w, int h, int stride,
                           double x, double y, double* out) {
  if (x < 0.0 || y < 0.0 || x >= w - 1.0 || y >= h - 1.0) return false;
  const int x0 = static_cast<int>(x);
  const int y0 = static_cast<int>(y);
  const double fx = x - x0;
  const double fy = y - y0;
  const uint8_t* r0 = gray + static_cast<size_t>(y0) * stride;
  const uint8_t* r1 = r0 + stride;
  const double v = (1.0 - fx) * (1.0 - fy) * r0[x0] + fx * (1.0 - fy) * r0[x0 + 1] +
                   (1.0 - fx) * fy * r1[x0] + fx * fy * r1[x0 + 1];
  *out = v;
  return true;
}

// 主方向：以特征为中心、半径 R 的梯度方向直方图峰值（36 bin）。
// 返回弧度。平坦区域返回 0。
double ComputeDominantAngle(const uint8_t* gray, int w, int h, int stride,
                            double cx, double cy) {
  const int R = 8;
  double hist[36] = {0.0};
  for (int dy = -R; dy <= R; ++dy) {
    for (int dx = -R; dx <= R; ++dx) {
      const double r2 = static_cast<double>(dx * dx + dy * dy);
      if (r2 > static_cast<double>(R * R)) continue;
      const double x = cx + dx, y = cy + dy;
      double gx = 0.0, gy = 0.0;
      double l = 0.0, r = 0.0, u = 0.0, d = 0.0;
      if (!SampleBilinear(gray, w, h, stride, x + 1.0, y, &r)) continue;
      if (!SampleBilinear(gray, w, h, stride, x - 1.0, y, &l)) continue;
      if (!SampleBilinear(gray, w, h, stride, x, y + 1.0, &d)) continue;
      if (!SampleBilinear(gray, w, h, stride, x, y - 1.0, &u)) continue;
      gx = r - l;
      gy = d - u;
      const double mag = std::sqrt(gx * gx + gy * gy);
      if (mag < 1e-9) continue;
      double ang = std::atan2(gy, gx);
      if (ang < 0.0) ang += 2.0 * kPi;
      const int bin = static_cast<int>(ang / (2.0 * kPi) * 36.0) % 36;
      // 高斯加权（尺度 = R/2）。
      const double wgt = mag * std::exp(-r2 / (2.0 * (R * 0.5) * (R * 0.5)));
      hist[bin] += wgt;
    }
  }

  int best = 0;
  double best_v = -1.0;
  for (int i = 0; i < 36; ++i) {
    if (hist[i] > best_v) { best_v = hist[i]; best = i; }
  }
  if (best_v <= 0.0) return 0.0;
  // 抛物线插值提高角度分辨率。
  const int prev = (best + 35) % 36;
  const int next = (best + 1) % 36;
  const double denom = hist[prev] - 2.0 * hist[best] + hist[next];
  double offset = 0.0;
  if (std::fabs(denom) > 1e-12) offset = 0.5 * (hist[prev] - hist[next]) / denom;
  return ((static_cast<double>(best) + offset) / 36.0) * 2.0 * kPi;
}

// 描述子：4×4 子区域 × 8 方向 = 128 维。子区域边长 = 4 px（patch 16×16）。
// 围绕主方向旋转对齐后累计梯度幅值，再做 L2 归一化 → 量化到 uint8。
void ComputeDescriptor(const uint8_t* gray, int w, int h, int stride, double cx,
                       double cy, double angle, std::vector<uint8_t>* out) {
  const int kGrid = 4;      // 4×4 子区域
  const int kOrient = 8;    // 8 方向
  const double kHalf = 8.0; // patch 半边长（16×16）
  out->assign(static_cast<size_t>(kGrid * kGrid * kOrient), 0);

  const double ca = std::cos(angle);
  const double sa = std::sin(angle);

  double hist[16][8] = {};
  for (int dy = -static_cast<int>(kHalf); dy < static_cast<int>(kHalf); ++dy) {
    for (int dx = -static_cast<int>(kHalf); dx < static_cast<int>(kHalf); ++dx) {
      // 旋转到主方向坐标系：图像偏移 = R(+angle)·规范偏移。
      //
      // 符号必须与下面的梯度 bin（atan2 − angle）自洽：图像整体旋转 θ 时，
      // 梯度方向随图像 +θ，方位角也 +θ，故 bin 不变；采样偏移也必须随图像
      // +θ（即用 R(+angle)），否则两者相差 2θ，描述子失去旋转不变性。
      // 曾误用 R(−angle)（rx = c·dx + s·dy）→ 绕光轴 15° 时匹配召回从
      // 正常值跌到 0.04，测试直接变红。
      const double rx = ca * dx - sa * dy;
      const double ry = sa * dx + ca * dy;
      const double x = cx + rx, y = cy + ry;
      double l = 0.0, r = 0.0, u = 0.0, d = 0.0;
      if (!SampleBilinear(gray, w, h, stride, x + 1.0, y, &r)) continue;
      if (!SampleBilinear(gray, w, h, stride, x - 1.0, y, &l)) continue;
      if (!SampleBilinear(gray, w, h, stride, x, y + 1.0, &d)) continue;
      if (!SampleBilinear(gray, w, h, stride, x, y - 1.0, &u)) continue;
      const double gx = r - l;
      const double gy = d - u;
      const double mag = std::sqrt(gx * gx + gy * gy);
      if (mag < 1e-9) continue;
      double ang = std::atan2(gy, gx) - angle;
      while (ang < 0.0) ang += 2.0 * kPi;
      while (ang >= 2.0 * kPi) ang -= 2.0 * kPi;

      // 子区域下标（把 [-kHalf, kHalf) 映射到 [0, kGrid)）。
      int gx_i = static_cast<int>((dx + kHalf) / (2.0 * kHalf) * kGrid);
      int gy_i = static_cast<int>((dy + kHalf) / (2.0 * kHalf) * kGrid);
      gx_i = std::max(0, std::min(kGrid - 1, gx_i));
      gy_i = std::max(0, std::min(kGrid - 1, gy_i));
      const int ob = static_cast<int>(ang / (2.0 * kPi) * kOrient) % kOrient;
      hist[gy_i * kGrid + gx_i][ob] += mag;
    }
  }

  // 展平 → L2 归一化 → 截断 0.2 → 再归一化（SIFT 的标准流程）→ uint8。
  double vec[128];
  for (int gy_i = 0; gy_i < kGrid; ++gy_i) {
    for (int gx_i = 0; gx_i < kGrid; ++gx_i) {
      for (int o = 0; o < kOrient; ++o) {
        vec[(gy_i * kGrid + gx_i) * kOrient + o] = hist[gy_i * kGrid + gx_i][o];
      }
    }
  }
  double norm = 0.0;
  for (int i = 0; i < 128; ++i) norm += vec[i] * vec[i];
  norm = std::sqrt(norm);
  if (norm < 1e-9) return;  // 全零描述子（平坦区域）
  for (int i = 0; i < 128; ++i) vec[i] /= norm;
  for (int i = 0; i < 128; ++i) vec[i] = std::min(vec[i], 0.2);
  norm = 0.0;
  for (int i = 0; i < 128; ++i) norm += vec[i] * vec[i];
  norm = std::sqrt(norm);
  if (norm < 1e-9) return;
  for (int i = 0; i < 128; ++i) {
    const double v = vec[i] / norm;
    (*out)[static_cast<size_t>(i)] =
        static_cast<uint8_t>(std::max(0.0, std::min(255.0, std::round(v * 512.0))));
  }
}

// 描述子欧氏距离。
double DescriptorDistance(const std::vector<uint8_t>& a,
                          const std::vector<uint8_t>& b) {
  if (a.size() != b.size()) return 1e30;
  double s = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    s += d * d;
  }
  return std::sqrt(s);
}

}  // namespace

// ---------------------------------------------------------------------------
// 特征提取
// ---------------------------------------------------------------------------

SfmFeatureSet ExtractSfmFeatures(const uint8_t* gray, int width, int height,
                                 int stride, int image_id,
                                 const SfmConfig& config) {
  SfmFeatureSet set;
  set.image_id = image_id;
  if (gray == nullptr || width <= 0 || height <= 0) return set;

  // 复用 VIO 的 Shi-Tomasi（含亚像素精化，定位精度 0.05 px）。
  VioConfig vcfg;
  vcfg.feature_count = config.feature_count;
  vcfg.grid_cell_size = config.feature_grid_size;
  vcfg.min_pixel_distance = config.feature_min_distance;
  vcfg.shi_tomasi_quality = config.feature_quality;
  vcfg.shi_tomasi_block = config.feature_block;

  const CornerDetection corners =
      DetectCorners(gray, width, height, stride, vcfg, nullptr);

  set.features.reserve(corners.points.size());
  for (size_t i = 0; i < corners.points.size(); ++i) {
    SfmFeature f;
    f.uv = corners.points[i];
    f.response = i < corners.response.size() ? corners.response[i] : 0.0;
    // 先算主方向再算描述子：不能把两者塞进同一个实参列表 ——
    // C++ 的函数实参求值顺序未规定，`f.angle = X` 与随后读 `f.angle`
    // 放在同一调用里是未定义行为。
    f.angle = ComputeDominantAngle(gray, width, height, stride, f.uv(0), f.uv(1));
    ComputeDescriptor(gray, width, height, stride, f.uv(0), f.uv(1), f.angle,
                      &f.descriptor);
    if (f.descriptor.empty()) continue;  // 平坦区无有效描述子
    set.features.push_back(f);
  }
  return set;
}

// ---------------------------------------------------------------------------
// 匹配
// ---------------------------------------------------------------------------

std::vector<SfmMatch> MatchFeatureSets(const SfmFeatureSet& a,
                                       const SfmFeatureSet& b,
                                       const SfmConfig& config) {
  std::vector<SfmMatch> out;
  const size_t na = a.features.size();
  const size_t nb = b.features.size();
  if (na == 0 || nb == 0) return out;

  std::vector<int> best_b(na, -1);
  std::vector<double> best_d(na, 1e30);
  std::vector<double> second_d(na, 1e30);

  // 前向：a → b 的最近 / 次近邻。
  for (size_t i = 0; i < na; ++i) {
    for (size_t j = 0; j < nb; ++j) {
      const double d = DescriptorDistance(a.features[i].descriptor,
                                          b.features[j].descriptor);
      if (d < best_d[i]) {
        second_d[i] = best_d[i];
        best_d[i] = d;
        best_b[i] = static_cast<int>(j);
      } else if (d < second_d[i]) {
        second_d[i] = d;
      }
    }
  }

  // 反向：b → a 的最近邻（用于双向一致性）。
  std::vector<int> best_a(nb, -1);
  std::vector<double> best_da(nb, 1e30);
  if (config.cross_check_enabled) {
    for (size_t j = 0; j < nb; ++j) {
      for (size_t i = 0; i < na; ++i) {
        const double d = DescriptorDistance(b.features[j].descriptor,
                                            a.features[i].descriptor);
        if (d < best_da[j]) { best_da[j] = d; best_a[j] = static_cast<int>(i); }
      }
    }
  }

  for (size_t i = 0; i < na; ++i) {
    if (best_b[i] < 0) continue;
    // 比值检验。
    if (second_d[i] > 0.0 && best_d[i] / second_d[i] > config.match_ratio_threshold) {
      continue;
    }
    // 双向一致性（互为最近邻）。
    if (config.cross_check_enabled) {
      const int j = best_b[i];
      if (best_a[static_cast<size_t>(j)] != static_cast<int>(i)) continue;
    }
    SfmMatch m;
    m.index0 = static_cast<int>(i);
    m.index1 = best_b[i];
    m.distance = best_d[i];
    out.push_back(m);
  }

  // 按距离升序，超上限时截断（保留更可信的）。
  std::sort(out.begin(), out.end(),
            [](const SfmMatch& x, const SfmMatch& y) { return x.distance < y.distance; });
  if (config.max_matches_per_image > 0 &&
      static_cast<int>(out.size()) > config.max_matches_per_image) {
    out.resize(static_cast<size_t>(config.max_matches_per_image));
  }
  return out;
}

// ---------------------------------------------------------------------------
// 匹配图
// ---------------------------------------------------------------------------

std::vector<SfmCandidatePair> BuildMatchGraph(
    const std::vector<SfmFeatureSet>& sets, const std::vector<CameraPose>* poses,
    const SfmConfig& config) {
  std::vector<SfmCandidatePair> pairs;
  const int n = static_cast<int>(sets.size());
  if (n < 2) return pairs;

  std::vector<std::vector<int>> adjacency(static_cast<size_t>(n));
  auto add_edge = [&](int i, int j) {
    if (i == j || i < 0 || j < 0 || i >= n || j >= n) return;
    if (i > j) std::swap(i, j);
    for (int e : adjacency[static_cast<size_t>(i)]) {
      if (e == j) return;
    }
    adjacency[static_cast<size_t>(i)].push_back(j);
    SfmCandidatePair p;
    p.index0 = i;
    p.index1 = j;
    pairs.push_back(p);
  };

  // MM-1：序列邻接（保证主链连通）。
  const int span = std::max(1, config.neighbor_sequence_span);
  for (int i = 0; i < n; ++i) {
    for (int k = 1; k <= span; ++k) {
      if (i + k < n) add_edge(i, i + k);
    }
  }

  // MM-2：空间近邻（按相机中心的方位差）。仅在给定位姿时启用。
  if (poses != nullptr && static_cast<int>(poses->size()) == n) {
    for (int i = 0; i < n; ++i) {
      // 每个视图只保留方位差最小的 max_num_neighbors 个邻居。
      std::vector<std::pair<double, int>> scored;
      for (int j = 0; j < n; ++j) {
        if (i == j) continue;
        const Vec3d ci = (*poses)[static_cast<size_t>(i)].CenterWorld();
        const Vec3d cj = (*poses)[static_cast<size_t>(j)].CenterWorld();
        if (ci.norm() < 1e-9 || cj.norm() < 1e-9) continue;
        const double dot = std::max(-1.0, std::min(1.0, ci.normalized().dot(cj.normalized())));
        const double ang = std::acos(dot) * 180.0 / kPi;
        if (ang <= config.spatial_neighbor_angle_deg) scored.push_back({ang, j});
      }
      std::sort(scored.begin(), scored.end());
      const int lim = std::min<int>(config.max_num_neighbors,
                                    static_cast<int>(scored.size()));
      for (int k = 0; k < lim; ++k) add_edge(i, scored[static_cast<size_t>(k)].second);
    }
  }

  // MM-4：对数上限（16N），超出时按序列距离截断。
  const size_t cap = static_cast<size_t>(n) * 16;
  if (pairs.size() > cap) {
    pairs.resize(cap);
  }
  return pairs;
}

bool VerifyPairGeometry(const SfmFeatureSet& a, const SfmFeatureSet& b,
                        const std::vector<SfmMatch>& matches,
                        const SfmConfig& config,
                        std::vector<SfmMatch>* inlier_matches, Mat3d* out_e_or_f,
                        bool* out_is_essential) {
  inlier_matches->clear();
  if (matches.size() < 8) return false;

  std::vector<Vec2d> p0(matches.size()), p1(matches.size());
  for (size_t i = 0; i < matches.size(); ++i) {
    p0[i] = a.features[static_cast<size_t>(matches[i].index0)].uv;
    p1[i] = b.features[static_cast<size_t>(matches[i].index1)].uv;
  }
  // 已标定内参（用 a 的内参近似；§4.3.3 假定同设备同分辨率）。
  const CameraIntrinsics& k = a.intrinsics_valid ? a.intrinsics : b.intrinsics;
  if (k.fx <= 0.0) return false;

  std::vector<Vec2d> n0(matches.size()), n1(matches.size());
  for (size_t i = 0; i < matches.size(); ++i) {
    k.PixelToNormalized(p0[i](0), p0[i](1), &n0[i](0), &n0[i](1));
    k.PixelToNormalized(p1[i](0), p1[i](1), &n1[i](0), &n1[i](1));
  }

  Mat3d e;
  RansacResult re = EstimateEssentialMatrix(n0, n1, k, config, &e);
  bool is_essential = false;
  RansacResult used = re;
  Mat3d model = e;
  if (re.ok && re.inliers >= config.ransac_min_inliers) {
    is_essential = true;
  } else {
    Mat3d f;
    RansacResult rf = EstimateFundamentalMatrix(n0, n1, k, config, &f);
    if (!rf.ok) return false;
    used = rf;
    model = f;
  }

  const double inlier_ratio =
      static_cast<double>(used.inliers) / static_cast<double>(matches.size());
  if (used.inliers < config.ransac_min_inliers ||
      inlier_ratio < config.min_inlier_ratio) {
    return false;
  }

  for (size_t i = 0; i < matches.size(); ++i) {
    if (used.inlier_mask[i]) inlier_matches->push_back(matches[i]);
  }
  if (out_e_or_f != nullptr) *out_e_or_f = model;
  if (out_is_essential != nullptr) *out_is_essential = is_essential;
  return true;
}

std::vector<int> ScoreCandidatePairs(const std::vector<SfmFeatureSet>& sets,
                                     std::vector<SfmCandidatePair>* pairs,
                                     const SfmConfig& config) {
  std::vector<int> order;
  for (size_t idx = 0; idx < pairs->size(); ++idx) {
    SfmCandidatePair& p = (*pairs)[idx];
    const std::vector<SfmMatch> m =
        MatchFeatureSets(sets[static_cast<size_t>(p.index0)],
                         sets[static_cast<size_t>(p.index1)], config);
    p.matches = static_cast<int>(m.size());
    std::vector<SfmMatch> inl;
    Mat3d model;
    bool is_e = false;
    p.verified = VerifyPairGeometry(sets[static_cast<size_t>(p.index0)],
                                    sets[static_cast<size_t>(p.index1)], m,
                                    config, &inl, &model, &is_e);
    p.inliers = static_cast<int>(inl.size());
    p.is_essential = is_e;
    order.push_back(static_cast<int>(idx));
  }
  // 按内点数降序（§4.4.1 的「新增三角化点数 / 计算代价」近似为内点数）。
  std::sort(order.begin(), order.end(), [&](int i, int j) {
    return (*pairs)[static_cast<size_t>(i)].inliers >
           (*pairs)[static_cast<size_t>(j)].inliers;
  });
  return order;
}

}  // namespace recon
