// SPDX-License-Identifier: BSD-3-Clause
//
// 尺度标定接口。契约：docs/07_尺度校准.md。

#ifndef RECON_CORE_SCALE_CALIBRATION_H_
#define RECON_CORE_SCALE_CALIBRATION_H_

#include "recon/linalg.h"
#include "recon/recon_c_api.h"

namespace recon {

// 标定配置。默认值取自 docs/07 §2.10。
struct ScaleCalibrationConfig {
  double max_reprojection_px = 2.0;      // 角点重投影残差上限
  double min_incidence_deg = 25.0;       // 最小入射角
  double max_depth_consistency = 0.03;   // 长短边深度解一致性
  double min_aspect_consistency = 0.90;  // 长宽比一致性下限
  double max_flatness_ratio = 0.05;      // 非平面度上限
  double ref_size_tolerance_mm = 0.20;   // 参照物尺寸公差
};

// 标定结果。
struct ScaleCalibrationResult {
  bool ok = false;
  recon_status status = RECON_OK;
  const char* reason = "";

  double reprojection_residual_px = 0.0;  // 角点重投影残差
  double incidence_deg = 0.0;             // 视线与卡片法向夹角
  double depth_consistency = 0.0;         // 长短边深度解相对差
  double aspect_consistency = 0.0;        // 长宽比一致性
  double flatness_ratio = 0.0;            // 非平面度
  double depth_mm = 0.0;                  // 解算深度（毫米）
  double relative_sigma = 0.0;            // 相对不确定度

  // 尺度因子 s = 真实长度(米) / 重建单位。
  //
  // 只有 `SolveScaleFromReconCorners` 会填充它 —— `SolveScaleFromCorners`
  // 走的是「图像角点 → 单应 → 度量深度」路径，本身不产生尺度因子
  // （尺度要拿重建几何去比），故那里恒为 0（表示「本路径不输出」）。
  //
  // **为什么用 0 表示不输出而不是「什么都不做」**：0 是物理上非法尺度，
  // 调用方若不检查就会污染下游（乘 0 把模型压成一点）。用 0 让「忘了检查」
  // 立刻表现为可见的退化，而不是静默给出一个看似合理的错误尺度。
  double scale = 0.0;
};

/**
 * 由检测到的卡片四角求解尺度几何信息。
 *
 * corners: 8 个 double，顺序为左上、右上、右下、左下，即
 *          (x0,y0, x1,y1, x2,y2, x3,y3)。
 * fx/fy/cx/cy: 相机内参（像素）。
 * ref_width_mm / ref_height_mm: 参照物真实尺寸。
 */
ScaleCalibrationResult SolveScaleFromCorners(
    const double corners[8], double fx, double fy, double cx, double cy,
    double ref_width_mm, double ref_height_mm,
    const ScaleCalibrationConfig& config);

/**
 * 由实测尺寸做几何合理性校验（不求解尺度，只校验角点质量）。
 * 用于交叉验证 SolveScaleFromCorners 的结果。
 */
ScaleCalibrationResult SolveScaleFromImageEdges(
    const double corners[8], double measured_width_mm,
    double measured_height_mm, const ScaleCalibrationConfig& config);

/**
 * 由卡片四角的**重建坐标**解尺度因子 s = 真实长度(米) / 重建单位。
 *
 * 这是 docs/07 §2.7.2 的闭式解：
 *   · 四条相邻边 a₁..a₄（重建单位），对应真实边长 L₁=L₃=W/1000、L₂=L₄=H/1000；
 *   · 等权最小二乘 s = (Σ aₖ Lₖ) / (Σ aₖ²)；
 *   · 两个单约束独立解 s_W = W / ((a₁+a₃)/2)、s_H = H / ((a₂+a₄)/2)；
 *   · 一致性 ρ = |s_W − s_H| / max(s_W, s_H)，> 0.03 判病态拒用。
 *
 * @param recon_corners 四角的重建坐标（顺序：左上、右上、右下、左下）
 * @param ref_width_mm  参照物宽（毫米）
 * @param ref_height_mm 参照物高（毫米）
 * @param config        判据阈值（用 max_depth_consistency 作为 ρ 上限）
 */
ScaleCalibrationResult SolveScaleFromReconCorners(
    const Vec3d recon_corners[4], double ref_width_mm, double ref_height_mm,
    const ScaleCalibrationConfig& config);

}  // namespace recon

#endif  // RECON_CORE_SCALE_CALIBRATION_H_
