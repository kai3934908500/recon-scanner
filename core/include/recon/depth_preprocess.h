// SPDX-License-Identifier: BSD-3-Clause
//
// 深度预处理。契约：docs/04_RGBD管线.md §3（总体流程 §3.1 / 置信度掩码 §3.2 /
// 飞行点剔除 §3.3 / BMRE 双边最小区间估计滤波 §3.4 / 深度→RGB 对齐 §3.5 /
// 保留原始副本铁律 §3.6）。
//
// 定位：
//   · 本模块只做「深度帧预处理」，产出**滤波副本** d_filt 与**对齐派生素引**；
//   · **铁律 IR-1/IR-3**：绝不修改 d_raw —— 所有输出写入独立缓冲，调用方
//     自行保存原始帧（`frames.bin` 深度段）。
//   · **IR-2/RA-5**：TSDF 几何积分走**原生分辨率**，不经本模块的对齐产物；
//     对齐图仅用于颜色关联与交叉校验。
//
// 输入输出口径（与 02 §5.5 / 04 §2.3 一致）：
//   · 深度：uint16，**毫米**，沿光轴 z（ARCore/ARKit 口径）；
//   · 置信度：uint8（0–255），可为空（空则内部按 255 处理并走推导路径标注）。
//
// 单位换算：模块内部一律用 double 米做判据；int 毫米仅用于进出边界。

#ifndef RECON_CORE_DEPTH_PREPROCESS_H_
#define RECON_CORE_DEPTH_PREPROCESS_H_

#include <cstdint>
#include <vector>

#include "recon/camera.h"
#include "recon/recon_c_api.h"

namespace recon {

// 深度源噪声模型（docs/04 §2.4.2 / §3.3.2）。
//   HW_TOF / HW_LIDAR：绝对 σ_z（米）。
//   ALGO_DEPTH：σ_z(z) = rel·z + abs（随深度增长）。
struct DepthNoiseModel {
  recon_depth_tier tier = RECON_DEPTH_TOF;

  double sigma_tof_m = 0.002;     // rgbd_sigma_tof
  double sigma_lidar_m = 0.012;   // rgbd_sigma_lidar
  double sigma_algo_rel = 0.03;   // rgbd_sigma_algo_rel
  double sigma_algo_abs = 0.0005; // rgbd_sigma_algo_abs

  // z 处单像素深度噪声 σ_z（米）。
  double SigmaZ(double z_m) const;
};

// 预处理配置。默认值取自 docs/08 G6 组「标准档」。
struct DepthPreprocessConfig {
  bool enabled = true;  // rgbd_depth_preprocess_enabled

  // §3.2 置信度掩码。
  int confidence_threshold = 0;  // rgbd_confidence_threshold，0 = 不硬删（RM-1）

  // §3.3 飞行点。
  double flying_pixel_threshold_m = 0.002;  // rgbd_flying_pixel_threshold（跳变幅度下限）
  double flying_pixel_noise_k = 3.0;        // rgbd_flying_pixel_noise_k
  double flying_ratio_warn = 0.05;          // §3.3.3 步骤 2 落盘阈值
  double flying_ratio_downweight = 0.15;    // §3.3.3 步骤 3 整帧降权阈值

  // §3.4 BMRE。
  double bilateral_sigma_color = 0.08;   // rgbd_bilateral_sigma_color
  int bilateral_sigma_space = 4;         // rgbd_bilateral_sigma_space（px）
  double bilateral_support_ratio = 0.50; // rgbd_bilateral_support_ratio
  double bilateral_edge_confidence = 0.50;   // rgbd_bilateral_edge_confidence
  bool bilateral_min_branch_median = true;   // rgbd_bilateral_min_branch_median
  double bilateral_fg_color_min = 0.35;      // rgbd_bilateral_fg_color_min

  // §3.5 对齐。
  double align_derived_conf_scale = 0.50;  // rgbd_align_derived_conf_scale（RA-6）

  // §3.5.5 基线硬校验。
  double align_max_baseline_m = 0.05;      // rgbd_align_max_baseline

  DepthNoiseModel noise;
};

// 预处理统计（落盘，docs/04 §3.2 RM-4 / §3.3.3）。
struct DepthPreprocessStats {
  uint32_t total_pixels = 0;
  uint32_t valid_input = 0;       // 输入中有效（深度 ≠ 0 且过置信硬门槛）
  uint32_t invalid_platform = 0;  // RM-2：平台标记无效（深度 0 或 conf 0）
  uint32_t masked_hard = 0;       // RM-1：被置信硬门槛置无效
  uint32_t flying_pixels = 0;     // 飞行点剔除数
  uint32_t edge_pixels = 0;       // BMRE 判为边缘（区间为空/无支持）
  uint32_t branch_recovered = 0;  // §3.4.4 最小分支恢复生效数

  double mean_confidence = 0.0;   // RM-4：mean(c)
  double p05_confidence = 0.0;    // RM-4：P05(c)
  double valid_ratio = 0.0;       // RM-4：valid_ratio
  double flying_ratio = 0.0;      // flying_pixels / valid_input
  bool frame_downweighted = false;  // §3.3.3 步骤 3：flying_ratio > 0.15
  double frame_weight_scale = 1.0;  // 该帧整体融合权重系数（正常 1.0，降权 0.5）

  // 16×16 网格的 low_conf_ratio（RM-4）。行主序，16*16。
  std::vector<float> low_conf_grid;
};

// 预处理结果。所有输出缓冲与输入独立（铁律 IR-3）。
struct DepthPreprocessResult {
  // 滤波后深度（毫米），与输入同尺寸；0 表示无效。
  std::vector<uint16_t> depth_filtered;
  // 滤波后置信度（0–255）。
  std::vector<uint8_t> confidence_filtered;
  // 飞行点掩码（1 = 被剔除）。
  std::vector<uint8_t> flying_mask;
  // 边缘标志（1 = BMRE 判为边缘）。
  std::vector<uint8_t> edge_flag;
  int width = 0;
  int height = 0;
  bool ok = false;
  recon_status status = RECON_OK;
  DepthPreprocessStats stats;
};

// 深度→RGB 对齐结果（派生副本，RA-5/RA-6）。
struct DepthAlignmentResult {
  std::vector<uint16_t> depth_aligned;    // RGB 分辨率，毫米（P_c.z 换算）
  std::vector<uint8_t> confidence_aligned; // 折扣后置信度
  std::vector<uint8_t> derived;            // 1 = 由深度像点覆盖（RA-6）；0 = 空洞（RA-4）
  int width = 0;   // RGB 宽
  int height = 0;  // RGB 高
  double measured_baseline_m = 0.0;  // ‖t_cd‖
  bool baseline_ok = true;           // §3.5.5 `‖t_cd‖ ≤ align_max_baseline`
  recon_status status = RECON_OK;
};

// ---------------------------------------------------------------------------
// 主入口
// ---------------------------------------------------------------------------

// 深度帧预处理：置信度掩码 → 飞行点剔除 → BMRE 滤波。
//
// depth_mm：uint16 毫米，沿光轴 z，尺寸 w×h。
// confidence：uint8，可空（空则内部视为 255）。
// rgb：对齐后的 RGB（BGRA8 或灰度），用于 BMRE 的颜色权重；可空（空则退化
//      为**空间权重**双边，并注意：此时边缘判据仍成立但保边能力下降）。
// rgb_channels：3 或 4（BGRA）；灰度传 1。
//
// **不修改** depth_mm / confidence 指向的内存。
DepthPreprocessResult PreprocessDepth(const uint16_t* depth_mm,
                                      const uint8_t* confidence, const uint8_t* rgb,
                                      int rgb_channels, int width, int height,
                                      const DepthPreprocessConfig& config);

// 深度 → RGB 对齐（§3.5）。运行于原生深度分辨率 → RGB 分辨率。
//
// depth_cam：深度相机内参 K_d（fx/fy/cx/cy，width/height 为深度分辨率）。
// rgb_cam：RGB 相机内参 K_c。
// R_cd / t_cd：深度相机 → RGB 相机外参（P_c = R_cd·P_d + t_cd）。
//   ARKit/ARCore 同光心路径传 R=I, t=0（§3.5.3）。
// depth_cam_width/height：深度图尺寸（与 K_d 的 width/height 一致）。
DepthAlignmentResult AlignDepthToRgb(const uint16_t* depth_mm,
                                     const uint8_t* confidence, int depth_width,
                                     int depth_height,
                                     const CameraIntrinsics& depth_cam,
                                     const CameraIntrinsics& rgb_cam,
                                     const Mat3d& R_cd, const Vec3d& t_cd,
                                     const DepthPreprocessConfig& config);

// §3.3.2 阈值 τ_eff = max(阈值下限, k·σ_z)。暴露供测试与文档核对。
double EffectiveFlyingThreshold(const DepthPreprocessConfig& config, double z_m);

}  // namespace recon

#endif  // RECON_CORE_DEPTH_PREPROCESS_H_
