// SPDX-License-Identifier: BSD-3-Clause
//
// 深度预处理实现。契约：docs/04_RGBD管线.md §3。
//
// 实现要点与理由：
//   · 顺序不可交换（§3.1）：②置信度掩码 → ③飞行点 → ④BMRE，严格按序；
//   · τ_eff 噪声自适应（§3.3.2）：ALGO_DEPTH 在 0.35 m 处 σ_z = 11 mm，
//     若不放大阈值，几乎每个像素都成"跳变候选"，检测完全失效；
//   · 飞行点二级判据 (c) 的后半段（读数偏背景侧）是区分 ToF 飞行点与
//     "真实中间深度小凸起"的关键，缺了它会把真实几何误剔（§3.3.1）；
//   · BMRE 用**区间求交**而非加权平均：断层处直接判"不一致"并放弃平滑，
//     保守且正确（§3.4.3）；
//   · 最小分支恢复**必须带颜色判据**，否则斜面近端邻域会被误判前景（§3.4.4）；
//   · 输出全部写入独立缓冲，d_raw 只读（铁律 IR-1/IR-3）。

#include "recon/depth_preprocess.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace recon {
namespace {

constexpr int kLowConfGrid = 16;  // RM-4：16×16 网格
constexpr double kMetersPerMm = 0.001;

// 4-邻域偏移。
constexpr int kDx[4] = {1, -1, 0, 0};
constexpr int kDy[4] = {0, 0, 1, -1};

bool InBounds(int x, int y, int w, int h) {
  return x >= 0 && y >= 0 && x < w && y < h;
}

// 取某像素的 RGB 颜色（归一化到 [0,1]），rgb 为空返回 false。
bool SampleColor(const uint8_t* rgb, int channels, int width, int x, int y,
                 double* out_rgb3) {
  if (rgb == nullptr) return false;
  const size_t base = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                       static_cast<size_t>(x)) *
                      static_cast<size_t>(channels);
  if (channels == 1) {
    const double v = static_cast<double>(rgb[base]) / 255.0;
    out_rgb3[0] = out_rgb3[1] = out_rgb3[2] = v;
  } else {
    // BGRA 或 BGR：取前 3 通道。
    out_rgb3[0] = static_cast<double>(rgb[base + 0]) / 255.0;
    out_rgb3[1] = static_cast<double>(rgb[base + 1]) / 255.0;
    out_rgb3[2] = static_cast<double>(rgb[base + 2]) / 255.0;
  }
  return true;
}

double ColorWeight(const double* a, const double* b, double sigma_c) {
  if (sigma_c <= 0.0) return 1.0;
  double d2 = 0.0;
  for (int i = 0; i < 3; ++i) {
    const double d = a[i] - b[i];
    d2 += d * d;
  }
  return std::exp(-d2 / (2.0 * sigma_c * sigma_c));
}

double Median(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

}  // namespace

// ---------------------------------------------------------------------------
// 噪声模型
// ---------------------------------------------------------------------------

double DepthNoiseModel::SigmaZ(double z_m) const {
  switch (tier) {
    case RECON_DEPTH_ALGO:
      return sigma_algo_rel * std::fabs(z_m) + sigma_algo_abs;
    case RECON_DEPTH_LIDAR:
      return sigma_lidar_m;
    case RECON_DEPTH_TOF:
    case RECON_DEPTH_NONE:
    default:
      return sigma_tof_m;
  }
}

double EffectiveFlyingThreshold(const DepthPreprocessConfig& config, double z_m) {
  const double adaptive = config.flying_pixel_noise_k * config.noise.SigmaZ(z_m);
  return std::max(config.flying_pixel_threshold_m, adaptive);
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------

DepthPreprocessResult PreprocessDepth(const uint16_t* depth_mm,
                                      const uint8_t* confidence, const uint8_t* rgb,
                                      int rgb_channels, int width, int height,
                                      const DepthPreprocessConfig& config) {
  DepthPreprocessResult result;
  result.width = width;
  result.height = height;

  if (depth_mm == nullptr || width <= 0 || height <= 0) {
    result.status = RECON_ERR_INVALID_ARGUMENT;
    return result;
  }
  const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);

  // 输出缓冲全部新建（铁律 IR-3：d_raw 只读）。
  result.depth_filtered.assign(n, 0);
  result.confidence_filtered.assign(n, 0);
  result.flying_mask.assign(n, 0);
  result.edge_flag.assign(n, 0);
  result.stats.total_pixels = static_cast<uint32_t>(n);
  result.stats.low_conf_grid.assign(static_cast<size_t>(kLowConfGrid) * kLowConfGrid,
                                    0.0f);

  // ---- ② 置信度掩码（§3.2）----
  // 先把每个像素的有效性与置信度定下来（尚未剔除飞行点）。
  std::vector<uint8_t> valid(n, 0);   // 通过掩码、仍有效
  std::vector<uint8_t> conf(n, 255);  // 归一化置信度
  double conf_sum = 0.0;
  uint32_t conf_count = 0;
  for (size_t i = 0; i < n; ++i) {
    const uint16_t d = depth_mm[i];
    const uint8_t c = (confidence != nullptr) ? confidence[i] : 255;
    conf[i] = c;

    // RM-2：平台标记无效（深度 0 或 conf 0）直接置无效。
    if (d == 0 || c == 0) {
      ++result.stats.invalid_platform;
      continue;
    }
    // RM-1：用户显式设阈值时，c < threshold 置无效（默认 0 = 不硬删）。
    if (config.confidence_threshold > 0 && c < config.confidence_threshold) {
      ++result.stats.masked_hard;
      continue;
    }
    valid[i] = 1;
    ++result.stats.valid_input;
    conf_sum += static_cast<double>(c);
    ++conf_count;
  }

  // RM-4 统计量。
  if (conf_count > 0) {
    result.stats.mean_confidence = conf_sum / static_cast<double>(conf_count);
    std::vector<uint8_t> sorted;
    sorted.reserve(conf_count);
    for (size_t i = 0; i < n; ++i) {
      if (valid[i]) sorted.push_back(conf[i]);
    }
    std::sort(sorted.begin(), sorted.end());
    const size_t p05_idx = static_cast<size_t>(0.05 * static_cast<double>(sorted.size()));
    result.stats.p05_confidence =
        static_cast<double>(sorted[std::min(p05_idx, sorted.size() - 1)]);
    result.stats.valid_ratio =
        static_cast<double>(result.stats.valid_input) / static_cast<double>(n);
  }

  // ---- ③ 飞行点剔除（§3.3）----
  // 阈值随深度自适应（逐像素用各自 z 求 τ_eff）。
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t i = static_cast<size_t>(y) * width + x;
      if (!valid[i]) continue;
      const double d_p = static_cast<double>(depth_mm[i]) * kMetersPerMm;
      const double tau = EffectiveFlyingThreshold(config, d_p);

      // 一级：邻域 4-邻域深度跳变。
      double d_lo = std::numeric_limits<double>::max();
      double d_hi = -std::numeric_limits<double>::max();
      double delta_max = 0.0;
      int nbr = 0;
      for (int k = 0; k < 4; ++k) {
        const int nx = x + kDx[k];
        const int ny = y + kDy[k];
        if (!InBounds(nx, ny, width, height)) continue;
        const size_t j = static_cast<size_t>(ny) * width + nx;
        if (!valid[j]) continue;
        const double d_q = static_cast<double>(depth_mm[j]) * kMetersPerMm;
        ++nbr;
        delta_max = std::max(delta_max, std::fabs(d_p - d_q));
        d_lo = std::min(d_lo, d_q);
        d_hi = std::max(d_hi, d_q);
      }
      if (nbr < 2) continue;                 // 邻域不足，无可判定
      if (delta_max <= tau) continue;        // (a) 不成立

      // 二级：轮廓型判定。
      const double m_lo = d_lo;
      const double m_hi = d_hi;
      const double span = m_hi - m_lo;
      const bool cond_b = span > tau;                          // (b)
      const bool cond_c = (m_lo < d_p && d_p < m_hi) &&        // (c)
                          (d_p > m_lo + 0.5 * span);
      if (cond_b && cond_c) {
        result.flying_mask[i] = 1;
        ++result.stats.flying_pixels;
        valid[i] = 0;  // 置无效（§3.3.3 步骤 1）
      }
    }
  }

  if (result.stats.valid_input > 0) {
    result.stats.flying_ratio =
        static_cast<double>(result.stats.flying_pixels) /
        static_cast<double>(result.stats.valid_input);
  }
  // §3.3.3 步骤 3：飞行点占比过高 → 整帧降权 ×0.5。
  if (result.stats.flying_ratio > config.flying_ratio_downweight) {
    result.stats.frame_downweighted = true;
    result.stats.frame_weight_scale = 0.5;
  }

  // ---- ④ BMRE（§3.4）----
  // 拷贝一份"掩码后"的深度作为输入（飞行点已置无效）。
  std::vector<uint16_t> d_masked(n, 0);
  for (size_t i = 0; i < n; ++i) {
    if (valid[i]) d_masked[i] = depth_mm[i];
  }

  const int radius = std::max(1, config.bilateral_sigma_space * 3);
  const double sigma_s = static_cast<double>(std::max(1, config.bilateral_sigma_space));
  const double sigma_c = config.bilateral_sigma_color;

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t i = static_cast<size_t>(y) * width + x;
      if (!valid[i]) {
        result.confidence_filtered[i] = 0;
        continue;
      }
      const double d_p = static_cast<double>(d_masked[i]) * kMetersPerMm;
      const double tau = EffectiveFlyingThreshold(config, d_p);

      double rgb_p[3] = {0.0, 0.0, 0.0};
      const bool have_color = SampleColor(rgb, rgb_channels, width, x, y, rgb_p);

      // 收集窗口内有效邻域的双边权重与深度。
      struct Nbr { double w; double d; int dx; int dy; };
      std::vector<Nbr> nbrs;
      double w_max = 0.0;
      const int x0 = std::max(0, x - radius);
      const int x1 = std::min(width - 1, x + radius);
      const int y0 = std::max(0, y - radius);
      const int y1 = std::min(height - 1, y + radius);
      for (int qy = y0; qy <= y1; ++qy) {
        for (int qx = x0; qx <= x1; ++qx) {
          if (qx == x && qy == y) { /* 含自身 */ }
          const size_t j = static_cast<size_t>(qy) * width + qx;
          if (!valid[j]) continue;
          // 颜色权重（无颜色则恒 1）。
          double w_c = 1.0;
          if (have_color) {
            double rgb_q[3];
            if (SampleColor(rgb, rgb_channels, width, qx, qy, rgb_q)) {
              w_c = ColorWeight(rgb_p, rgb_q, sigma_c);
            }
          }
          const double dxp = static_cast<double>(qx - x);
          const double dyp = static_cast<double>(qy - y);
          const double w_s = std::exp(-(dxp * dxp + dyp * dyp) /
                                      (2.0 * sigma_s * sigma_s));
          const double w = w_c * w_s;
          if (w <= 0.0) continue;
          nbrs.push_back({w, static_cast<double>(d_masked[j]) * kMetersPerMm, qx - x,
                          qy - y});
          w_max = std::max(w_max, w);
        }
      }

      // 支持度集合 S_50：w(p,q) ≥ w_sup · max w。
      double d_min_s = std::numeric_limits<double>::max();
      double d_max_s = -std::numeric_limits<double>::max();
      int support = 0;
      const double w_thr = config.bilateral_support_ratio * w_max;
      for (const Nbr& nb : nbrs) {
        if (nb.w >= w_thr && nb.w > 0.0) {
          d_min_s = std::min(d_min_s, nb.d);
          d_max_s = std::max(d_max_s, nb.d);
          ++support;
        }
      }

      bool interval_empty = true;
      if (support > 0 && (d_max_s - d_min_s) <= 2.0 * tau) {
        interval_empty = false;
        // I_p = [d_max_s − τ, d_min_s + τ]；d̂ = clamp(d_p, I_p)。
        const double lo = d_max_s - tau;
        const double hi = d_min_s + tau;
        double d_hat = std::min(std::max(d_p, lo), hi);
        result.depth_filtered[i] =
            static_cast<uint16_t>(std::lround(d_hat / kMetersPerMm));
        result.confidence_filtered[i] = conf[i];
      }

      if (interval_empty) {
        // 边缘分支（§3.4.3 / §3.4.4）。
        result.edge_flag[i] = 1;
        ++result.stats.edge_pixels;
        const double c_edge = static_cast<double>(conf[i]) *
                              config.bilateral_edge_confidence;
        result.confidence_filtered[i] = static_cast<uint8_t>(
            std::min(255.0, std::max(0.0, std::round(c_edge))));

        // 前景组 F_valid：d_q < d_p 且颜色权重 ≥ w_fg_min（仅在 4-邻域）。
        std::vector<double> f_valid;
        for (int k = 0; k < 4; ++k) {
          const int nx = x + kDx[k];
          const int ny = y + kDy[k];
          if (!InBounds(nx, ny, width, height)) continue;
          const size_t j = static_cast<size_t>(ny) * width + nx;
          if (!valid[j]) continue;
          const double d_q = static_cast<double>(d_masked[j]) * kMetersPerMm;
          if (d_q >= d_p) continue;
          if (have_color) {
            double rgb_q[3];
            if (SampleColor(rgb, rgb_channels, width, nx, ny, rgb_q) &&
                ColorWeight(rgb_p, rgb_q, sigma_c) < config.bilateral_fg_color_min) {
              continue;  // 颜色不像 → 不算前景（§3.4.4）
            }
          }
          f_valid.push_back(d_q);
        }
        if (f_valid.size() >= 2) {
          double d_rec;
          if (config.bilateral_min_branch_median) {
            d_rec = Median(f_valid);
          } else {
            d_rec = *std::min_element(f_valid.begin(), f_valid.end());
          }
          result.depth_filtered[i] =
              static_cast<uint16_t>(std::lround(d_rec / kMetersPerMm));
          ++result.stats.branch_recovered;
        } else {
          result.depth_filtered[i] = static_cast<uint16_t>(std::lround(d_p / kMetersPerMm));
        }
      }
    }
  }

  // RM-4：16×16 网格 low_conf_ratio（低置信 = conf < 64 的约定阈值）。
  {
    const double cell_w = static_cast<double>(width) / kLowConfGrid;
    const double cell_h = static_cast<double>(height) / kLowConfGrid;
    std::vector<uint32_t> cell_total(static_cast<size_t>(kLowConfGrid) * kLowConfGrid, 0);
    std::vector<uint32_t> cell_low(static_cast<size_t>(kLowConfGrid) * kLowConfGrid, 0);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const size_t i = static_cast<size_t>(y) * width + x;
        if (depth_mm[i] == 0) continue;
        const int gx = std::min(kLowConfGrid - 1,
                                static_cast<int>(static_cast<double>(x) / cell_w));
        const int gy = std::min(kLowConfGrid - 1,
                                static_cast<int>(static_cast<double>(y) / cell_h));
        const size_t gi = static_cast<size_t>(gy) * kLowConfGrid + gx;
        ++cell_total[gi];
        if (conf[i] < 64) ++cell_low[gi];
      }
    }
    for (size_t gi = 0; gi < cell_total.size(); ++gi) {
      result.stats.low_conf_grid[gi] =
          cell_total[gi] > 0
              ? static_cast<float>(cell_low[gi]) / static_cast<float>(cell_total[gi])
              : 0.0f;
    }
  }

  result.ok = true;
  result.status = RECON_OK;
  return result;
}

// ---------------------------------------------------------------------------
// 深度 → RGB 对齐（§3.5）
// ---------------------------------------------------------------------------

DepthAlignmentResult AlignDepthToRgb(const uint16_t* depth_mm,
                                     const uint8_t* confidence, int depth_width,
                                     int depth_height,
                                     const CameraIntrinsics& depth_cam,
                                     const CameraIntrinsics& rgb_cam,
                                     const Mat3d& R_cd, const Vec3d& t_cd,
                                     const DepthPreprocessConfig& config) {
  DepthAlignmentResult result;
  result.width = rgb_cam.width;
  result.height = rgb_cam.height;

  if (depth_mm == nullptr || depth_width <= 0 || depth_height <= 0 ||
      rgb_cam.width <= 0 || rgb_cam.height <= 0) {
    result.status = RECON_ERR_INVALID_ARGUMENT;
    return result;
  }

  // §3.5.5 基线硬校验。
  result.measured_baseline_m = t_cd.norm();
  result.baseline_ok = result.measured_baseline_m <= config.align_max_baseline_m;

  const size_t n_rgb = static_cast<size_t>(rgb_cam.width) * rgb_cam.height;
  result.depth_aligned.assign(n_rgb, 0);
  result.confidence_aligned.assign(n_rgb, 0);
  result.derived.assign(n_rgb, 0);  // RA-4：默认空洞 = 0
  // 临时最小 z 缓冲（z-buffer 消解，§3.5.4 保留最近者）。
  std::vector<double> min_z(n_rgb, std::numeric_limits<double>::max());

  for (int v = 0; v < depth_height; ++v) {
    for (int u = 0; u < depth_width; ++u) {
      const size_t i = static_cast<size_t>(v) * depth_width + u;
      const uint16_t d = depth_mm[i];
      if (d == 0) continue;
      const uint8_t c = (confidence != nullptr) ? confidence[i] : 255;
      if (c == 0) continue;

      const double z_ax = static_cast<double>(d) * kMetersPerMm;
      // ② 轴向深度反投影（RA-1：无 cos θ 因子）。
      const double x_d = (static_cast<double>(u) - depth_cam.cx) / depth_cam.fx;
      const double y_d = (static_cast<double>(v) - depth_cam.cy) / depth_cam.fy;
      const Vec3d P_d(z_ax * x_d, z_ax * y_d, z_ax);
      // ③ 外参到 RGB 相机系。
      const Vec3d P_c = R_cd * P_d + t_cd;
      if (P_c(2) <= 0.0) continue;  // ④ 相机后方
      // ④ 透视投影。
      const double u_c = rgb_cam.fx * P_c(0) / P_c(2) + rgb_cam.cx;
      const double v_c = rgb_cam.fy * P_c(1) / P_c(2) + rgb_cam.cy;
      const int ui = static_cast<int>(std::lround(u_c));
      const int vi = static_cast<int>(std::lround(v_c));
      if (!InBounds(ui, vi, rgb_cam.width, rgb_cam.height)) continue;

      const size_t idx = static_cast<size_t>(vi) * rgb_cam.width + ui;
      // ⑤ 对齐后深度值 = P_c.z（**不是** z_ax）。
      // §3.5.4 z-buffer：保留最近者。
      if (P_c(2) < min_z[idx]) {
        min_z[idx] = P_c(2);
        result.depth_aligned[idx] = static_cast<uint16_t>(
            std::lround(P_c(2) / kMetersPerMm));
        // RA-6：折扣后置信度。
        const double c_align = static_cast<double>(c) * config.align_derived_conf_scale;
        result.confidence_aligned[idx] = static_cast<uint8_t>(
            std::min(255.0, std::max(0.0, std::round(c_align))));
        result.derived[idx] = 1;
      }
    }
  }

  result.status = RECON_OK;
  return result;
}

}  // namespace recon
