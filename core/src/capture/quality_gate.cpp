// SPDX-License-Identifier: BSD-3-Clause
//
// 质量门限与异常检测。
//
// 契约：docs/02_采集与同步.md §4（六类异常检测）。
//
// 设计要点：
//   1. 门限是「帧能否进入重建的必要条件」，不是关键帧判定。
//      关键帧入选由 docs/03 的增益判据决定。
//   2. 严重异常拒帧；警告帧保留但提示用户（不损失精度）。
//   3. 检测不修改帧内容，只输出判定结果。

#include "recon/quality_gate.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace recon {
namespace {

// 图像亮度（Rec.601）。
inline double Luma(uint8_t r, uint8_t g, uint8_t b) {
  return 0.299 * r + 0.587 * g + 0.114 * b;
}

// 3x3 Sobel 梯度幅值。
inline double SobelMag(const uint8_t* p, int stride, int w, int h, int x, int y) {
  const int xm = std::max(0, x - 1);
  const int xp = std::min(w - 1, x + 1);
  const int ym = std::max(0, y - 1);
  const int yp = std::min(h - 1, y + 1);
  const auto g = [&](int xx, int yy) {
    return static_cast<double>(Luma(p[yy * stride + xx * 4 + 0],
                                     p[yy * stride + xx * 4 + 1],
                                     p[yy * stride + xx * 4 + 2]));
  };
  const double gx = g(xp, ym) + 2.0 * g(xp, y) + g(xp, yp) -
                    g(xm, ym) - 2.0 * g(xm, y) - g(xm, yp);
  const double gy = g(xm, yp) + 2.0 * g(x, yp) + g(xp, yp) -
                    g(xm, ym) - 2.0 * g(x, ym) - g(xp, ym);
  return std::sqrt(gx * gx + gy * gy);
}

}  // namespace

double ComputeLaplacianVariance(const uint8_t* rgba, int width, int height,
                                int stride) {
  if (rgba == nullptr || width < 3 || height < 3) return 0.0;

  double sum = 0.0;
  double sum_sq = 0.0;
  int64_t count = 0;

  // 步进采样，降低计算量。采样率不影响方差估计的统计性质。
  const int step_x = std::max(1, width / 320);
  const int step_y = std::max(1, height / 320);

  for (int y = 1; y < height - 1; y += step_y) {
    for (int x = 1; x < width - 1; x += step_x) {
      const double c = Luma(rgba[y * stride + x * 4 + 0],
                            rgba[y * stride + x * 4 + 1],
                            rgba[y * stride + x * 4 + 2]);
      const double l = Luma(rgba[y * stride + (x - 1) * 4 + 0],
                            rgba[y * stride + (x - 1) * 4 + 1],
                            rgba[y * stride + (x - 1) * 4 + 2]);
      const double r = Luma(rgba[y * stride + (x + 1) * 4 + 0],
                            rgba[y * stride + (x + 1) * 4 + 1],
                            rgba[y * stride + (x + 1) * 4 + 2]);
      const double u = Luma(rgba[(y - 1) * stride + x * 4 + 0],
                            rgba[(y - 1) * stride + x * 4 + 1],
                            rgba[(y - 1) * stride + x * 4 + 2]);
      const double d = Luma(rgba[(y + 1) * stride + x * 4 + 0],
                            rgba[(y + 1) * stride + x * 4 + 1],
                            rgba[(y + 1) * stride + x * 4 + 2]);
      const double lap = 4.0 * c - l - r - u - d;
      sum += lap;
      sum_sq += lap * lap;
      ++count;
    }
  }

  if (count == 0) return 0.0;
  const double mean = sum / static_cast<double>(count);
  return sum_sq / static_cast<double>(count) - mean * mean;
}

double ComputeSaturatedRatio(const uint8_t* rgba, int width, int height,
                             int stride, uint8_t threshold) {
  if (rgba == nullptr || width <= 0 || height <= 0) return 0.0;

  int64_t saturated = 0;
  int64_t total = 0;
  const int step_x = std::max(1, width / 320);
  const int step_y = std::max(1, height / 320);

  for (int y = 0; y < height; y += step_y) {
    for (int x = 0; x < width; x += step_x) {
      const uint8_t r = rgba[y * stride + x * 4 + 0];
      const uint8_t g = rgba[y * stride + x * 4 + 1];
      const uint8_t b = rgba[y * stride + x * 4 + 2];
      if (r >= threshold || g >= threshold || b >= threshold) {
        ++saturated;
      }
      ++total;
    }
  }
  if (total == 0) return 0.0;
  return static_cast<double>(saturated) / static_cast<double>(total);
}

double ComputeLowTextureRatio(const uint8_t* rgba, int width, int height,
                              int stride, int grid_size) {
  if (rgba == nullptr || width <= 0 || height <= 0) return 0.0;
  if (grid_size < 4) grid_size = 4;

  const int gx_count = std::max(1, width / grid_size);
  const int gy_count = std::max(1, height / grid_size);
  std::vector<double> cell_sum(static_cast<size_t>(gx_count) * gy_count, 0.0);
  std::vector<int64_t> cell_count(static_cast<size_t>(gx_count) * gy_count, 0);

  const int step_x = std::max(1, width / 320);
  const int step_y = std::max(1, height / 320);

  for (int y = 1; y < height - 1; y += step_y) {
    for (int x = 1; x < width - 1; x += step_x) {
      const int cx = std::min(gx_count - 1, x * gx_count / width);
      const int cy = std::min(gy_count - 1, y * gy_count / height);
      const size_t idx = static_cast<size_t>(cy) * gx_count + cx;
      cell_sum[idx] += SobelMag(rgba, stride, width, height, x, y);
      ++cell_count[idx];
    }
  }

  int64_t low_cells = 0;
  int64_t valid_cells = 0;
  for (size_t i = 0; i < cell_sum.size(); ++i) {
    if (cell_count[i] == 0) continue;
    ++valid_cells;
    if (cell_sum[i] / static_cast<double>(cell_count[i]) < 1.0) {
      ++low_cells;
    }
  }
  if (valid_cells == 0) return 1.0;
  return static_cast<double>(low_cells) / static_cast<double>(valid_cells);
}

QualityGateResult EvaluateQualityGate(const uint8_t* rgba, int width,
                                       int height, int stride,
                                       const QualityGateConfig& config,
                                       const FrameSignals& signals) {
  QualityGateResult r;

  r.blur_variance = ComputeLaplacianVariance(rgba, width, height, stride);
  r.blur_severe = r.blur_variance < config.blur_variance_min * 0.4;
  r.blur_warning = r.blur_variance < config.blur_variance_min;

  r.saturated_ratio =
      ComputeSaturatedRatio(rgba, width, height, stride, config.saturation_level);
  r.specular_severe = r.saturated_ratio > config.saturated_ratio_severe;
  r.specular_warning = r.saturated_ratio > config.saturated_ratio_warning;

  r.low_texture_ratio =
      ComputeLowTextureRatio(rgba, width, height, stride, config.texture_grid_size);
  r.low_texture_severe = r.low_texture_ratio > config.low_texture_ratio_severe;
  r.low_texture_warning = r.low_texture_ratio > config.low_texture_ratio_warning;

  r.angular_speed_rad_s = signals.angular_speed_rad_s;
  r.linear_speed_m_s = signals.linear_speed_m_s;
  r.too_fast_severe = (r.angular_speed_rad_s > config.max_angular_speed * 1.6) ||
                      (r.linear_speed_m_s > config.max_linear_speed * 1.6);
  r.too_fast_warning = (r.angular_speed_rad_s > config.max_angular_speed) ||
                       (r.linear_speed_m_s > config.max_linear_speed);

  r.overlap_ratio = signals.overlap_ratio;
  r.low_overlap_severe = r.overlap_ratio < config.min_overlap_ratio * 0.6;
  r.low_overlap_warning = r.overlap_ratio < config.min_overlap_ratio;

  int severe_count = 0;
  int warning_count = 0;
  if (r.blur_severe) ++severe_count; else if (r.blur_warning) ++warning_count;
  if (r.specular_severe) ++severe_count; else if (r.specular_warning) ++warning_count;
  if (r.low_texture_severe) ++severe_count; else if (r.low_texture_warning) ++warning_count;
  if (r.too_fast_severe) ++severe_count; else if (r.too_fast_warning) ++warning_count;
  if (r.low_overlap_severe) ++severe_count; else if (r.low_overlap_warning) ++warning_count;

  r.severe_count = severe_count;
  r.warning_count = warning_count;

  // 严重异常拒帧；警告帧保留并提示（保留精度数据）。
  r.accepted = severe_count == 0;
  r.severity = (severe_count > 0) ? QualitySeverity::kSevere
                                  : (warning_count > 0 ? QualitySeverity::kWarning
                                                       : QualitySeverity::kOk);
  return r;
}

}  // namespace recon
