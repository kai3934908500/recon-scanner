// SPDX-License-Identifier: BSD-3-Clause
//
// 质量门限接口。契约：docs/02_采集与同步.md §4。

#ifndef RECON_CORE_QUALITY_GATE_H_
#define RECON_CORE_QUALITY_GATE_H_

#include <cstdint>

namespace recon {

// 异常严重度。
enum class QualitySeverity : uint8_t {
  kOk = 0,
  kWarning = 1,  // 提示用户，帧仍保留
  kSevere = 2,   // 拒帧
};

// 门限配置。默认值取自 docs/02 §4.8 汇总表。
struct QualityGateConfig {
  double blur_variance_min = 60.0;          // 拉普拉斯方差下限
  double saturated_ratio_warning = 0.02;    // 饱和像素比例警告线
  double saturated_ratio_severe = 0.05;     // 饱和像素比例严重线
  uint8_t saturation_level = 250;           // 饱和判定电平
  double low_texture_ratio_warning = 0.15;  // 低纹理占比警告线
  double low_texture_ratio_severe = 0.30;   // 低纹理占比严重线
  int texture_grid_size = 64;               // 纹理检测网格边长
  double max_angular_speed = 2.5;           // 角速度上限 rad/s
  double max_linear_speed = 0.6;            // 线速度上限 m/s
  double min_overlap_ratio = 0.55;          // 帧重叠度下限
};

// 帧级信号。由平台层或跟踪模块提供。
struct FrameSignals {
  double angular_speed_rad_s = 0.0;  // 由 IMU 角速度估计
  double linear_speed_m_s = 0.0;     // 由视觉流或 IMU 估计
  double overlap_ratio = 1.0;        // 与前一关键帧的特征匹配覆盖率
};

// 质量门限判定结果。
struct QualityGateResult {
  bool accepted = true;
  QualitySeverity severity = QualitySeverity::kOk;
  int severe_count = 0;
  int warning_count = 0;

  double blur_variance = 0.0;
  double saturated_ratio = 0.0;
  double low_texture_ratio = 0.0;
  double angular_speed_rad_s = 0.0;
  double linear_speed_m_s = 0.0;
  double overlap_ratio = 1.0;

  bool blur_severe = false;
  bool blur_warning = false;
  bool specular_severe = false;
  bool specular_warning = false;
  bool low_texture_severe = false;
  bool low_texture_warning = false;
  bool too_fast_severe = false;
  bool too_fast_warning = false;
  bool low_overlap_severe = false;
  bool low_overlap_warning = false;
};

// 拉普拉斯方差（模糊度量）。越高越清晰。
double ComputeLaplacianVariance(const uint8_t* rgba, int width, int height,
                                int stride);

// 饱和像素比例。
double ComputeSaturatedRatio(const uint8_t* rgba, int width, int height,
                             int stride, uint8_t threshold);

// 低纹理网格占比。
double ComputeLowTextureRatio(const uint8_t* rgba, int width, int height,
                              int stride, int grid_size);

// 质量门限总判定。
QualityGateResult EvaluateQualityGate(const uint8_t* rgba, int width,
                                       int height, int stride,
                                       const QualityGateConfig& config,
                                       const FrameSignals& signals);

}  // namespace recon

#endif  // RECON_CORE_QUALITY_GATE_H_
