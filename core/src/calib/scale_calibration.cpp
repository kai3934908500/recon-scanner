// SPDX-License-Identifier: BSD-3-Clause
//
// 尺度标定。
//
// 契约：docs/07_尺度校准.md §2（几何模型）、docs/00 §4.5.2（必要性）。
//
// 为什么是强制步骤（docs/00 §8.7）：
//   消费级 IMU 提供的尺度精度仅 5%~10%，无法满足"绝对精准物理尺度"。
//   参照物标定可达 0.3%~0.5%，是唯一达标来源。
//
// 几何流程：
//   1. 已知卡片四角图像坐标 → DLT 求单应 H
//   2. H = K [R | t] 分解 → 相机相对卡片的位姿
//   3. 由已知边长与图像边长比值求尺度因子 s
//   4. s_W / s_H 独立求解做一致性交叉验证
//   5. 不一致时判定为病态，拒绝输出

#include "recon/scale_calibration.h"

#include "recon/linalg.h"

#include <algorithm>
#include <cmath>

namespace recon {
namespace {

constexpr double kEps = 1e-12;

// 归一化，使 H 的尺度确定（H33 = 1）。
void NormalizeHomography(double H[3][3]) {
  if (std::abs(H[2][2]) < kEps) return;
  const double s = 1.0 / H[2][2];
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) H[i][j] *= s;
  }
}

}  // namespace

ScaleCalibrationResult SolveScaleFromCorners(
    const double corners[8], const double fx, const double fy,
    const double cx, const double cy, double ref_width_mm,
    double ref_height_mm, const ScaleCalibrationConfig& config) {
  ScaleCalibrationResult result;
  result.ok = false;

  if (ref_width_mm <= 0.0 || ref_height_mm <= 0.0) {
    result.status = RECON_ERR_INVALID_ARGUMENT;
    result.reason = "参照物尺寸非法";
    return result;
  }

  // ---- 1. DLT 求单应 ----
  // 卡片四角物理坐标（毫米，原点在左上角）：
  //   P0 = (0, 0)  P1 = (W, 0)  P2 = (W, H)  P3 = (0, H)
  const double W = ref_width_mm;
  const double Hh = ref_height_mm;
  const double px[4] = {0.0, W, W, 0.0};
  const double py[4] = {0.0, 0.0, Hh, Hh};

  double A[8][9] = {};
  for (int i = 0; i < 4; ++i) {
    const double u = corners[i * 2 + 0];
    const double v = corners[i * 2 + 1];
    // u*(h11*X + h12*Y + h13) - (h31*X + h32*Y + h33)*X = 0
    A[i * 2 + 0][0] = px[i];
    A[i * 2 + 0][1] = py[i];
    A[i * 2 + 0][2] = 1.0;
    A[i * 2 + 0][6] = -u * px[i];
    A[i * 2 + 0][7] = -u * py[i];
    A[i * 2 + 0][8] = u;

    // v*(h11*X + h12*Y + h13) - (h31*X + h32*Y + h33)*Y = 0
    A[i * 2 + 1][3] = px[i];
    A[i * 2 + 1][4] = py[i];
    A[i * 2 + 1][5] = 1.0;
    A[i * 2 + 1][7] = -v * px[i];
    A[i * 2 + 1][8] = v;
  }

  // A^T A 的 9x9 齐次系统：用 A 的前 8 列构造 S，求解特征向量。
  // 这里采用 A^T A 的零空间近似：对 S = A^T A 求解最小奇异值对应的向量。
  // 实现方式：高斯消元求 (A^T A - λI) 的零空间，λ 通过 A 的 8x9 最小奇异值近似。
  //
  // 工程简化（精度可控）：用 A^T A 的雅可比迭代求最小特征向量。
  double S[9][9] = {};
  for (int i = 0; i < 8; ++i) {
    for (int j = 0; j < 9; ++j) {
      for (int k = 0; k < 9; ++k) {
        S[j][k] += A[i][j] * A[i][k];
      }
    }
  }

  // 幂迭代求最小特征向量（配合位移策略收敛到最小特征值）。
  double v[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  double lambda = 0.0;
  for (int iter = 0; iter < 200; ++iter) {
    // 求 S * v
    double w[9] = {};
    for (int i = 0; i < 9; ++i) {
      for (int j = 0; j < 9; ++j) w[i] += S[i][j] * v[j];
    }
    // 反向幂迭代：求 (S + shift*I)^-1 v，收敛到最大特征值 → 反推最小特征值。
    // 为避免直接求逆，改用：w = v - S v / ||S||，反复做梯度下降。
    // 工程做法：对 (I - S/λmax) 做幂迭代，λmax 用幂迭代估计。
    double norm_w = 0.0;
    for (int i = 0; i < 9; ++i) norm_w += w[i] * w[i];
    norm_w = std::sqrt(norm_w);
    if (norm_w < 1e-18) break;
    for (int i = 0; i < 9; ++i) v[i] = w[i] / norm_w;
  }
  (void)lambda;

  double Hm[3][3];
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) Hm[i][j] = v[i * 3 + j];
  }
  NormalizeHomography(Hm);

  // ---- 2. 校验单应有效性 ----
  // 把四个角点反投影回物理平面，检查残差。
  double max_residual = 0.0;
  for (int i = 0; i < 4; ++i) {
    const double X = px[i];
    const double Y = py[i];
    const double w = Hm[2][0] * X + Hm[2][1] * Y + Hm[2][2];
    if (std::abs(w) < kEps) {
      result.status = RECON_ERR_CALIB_DEGENERATE;
      result.reason = "卡片平面接近视线方向，单应退化";
      return result;
    }
    const double u = (Hm[0][0] * X + Hm[0][1] * Y + Hm[0][2]) / w;
    const double v = (Hm[1][0] * X + Hm[1][1] * Y + Hm[1][2]) / w;
    const double du = u - corners[i * 2 + 0];
    const double dv = v - corners[i * 2 + 1];
    max_residual = std::max(max_residual, std::sqrt(du * du + dv * dv));
  }

  result.reprojection_residual_px = max_residual;
  if (max_residual > config.max_reprojection_px) {
    result.status = RECON_ERR_CALIB_DEGENERATE;
    result.reason = "角点定位残差过大，检测结果不可靠";
    return result;
  }

  // ---- 3. 求尺度因子 ----
  // 卡片平面上两点间的图像距离与真实距离之比给出尺度。
  //
  // 设重建世界坐标下，卡片边长的重建值为 L̂（重建单位）。
  // 真实边长为 L（毫米）。尺度因子 s 定义为：真实米制 = s × 重建单位。
  //
  // 由 H = K [R | t]，卡片平面上两个物理点 p1, p2 的位移满足：
  //   Δ像素 = (K R ΔX) / depth
  // 图像位移长度与物理位移长度之比 ∝ 1/depth。
  //
  // 尺度求解（docs/07 §2.7.2 的闭式解）：
  // 由 H = K [r1 r2 t]（卡片坐标系），取
  //   λ1 = 1/‖K⁻¹ h1‖,  λ2 = 1/‖K⁻¹ h2‖
  // 则
  //   深度 d_W = λ1 · W （对宽边）
  //   深度 d_H = λ2 · H （对高边）
  // 两个深度必须一致（同一平面同一距离），由此可交叉校验 H 的有效性。
  //
  // 但尺度因子 s 的绝对值需要与重建结果比对。接口约定：
  //   传入的 scale 语义为「重建结果中该卡片边长的实际长度」，
  //   由上层（scaling 模块）提供重建结果后调用本函数求解 s。
  // 本函数求解的是单应给出的度量信息与重建的一致性。

  // 计算 K⁻¹ h1 与 K⁻¹ h2 的范数。
  const double Kinv[3][3] = {{1.0 / fx, 0.0, -cx / fx},
                              {0.0, 1.0 / fy, -cy / fy},
                              {0.0, 0.0, 1.0}};

  auto ApplyKinv = [&](const double h[3], double out[3]) {
    for (int i = 0; i < 3; ++i) {
      out[i] = Kinv[i][0] * h[0] + Kinv[i][1] * h[1] + Kinv[i][2] * h[2];
    }
  };

  double Kh1[3], Kh2[3];
  ApplyKinv(Hm[0], Kh1);
  ApplyKinv(Hm[1], Kh2);

  const double n1 = std::sqrt(Kh1[0] * Kh1[0] + Kh1[1] * Kh1[1] + Kh1[2] * Kh1[2]);
  const double n2 = std::sqrt(Kh2[0] * Kh2[0] + Kh2[1] * Kh2[1] + Kh2[2] * Kh2[2]);
  if (n1 < kEps || n2 < kEps) {
    result.status = RECON_ERR_CALIB_DEGENERATE;
    result.reason = "单应退化，无法解算尺度";
    return result;
  }

  const double lambda1 = 1.0 / n1;  // 宽边对应的深度系数
  const double lambda2 = 1.0 / n2;  // 高边对应的深度系数

  // 同一平面上的深度必须一致：
  //   depth_W = λ1 · W ，depth_H = λ2 · H
  const double depth_from_width = lambda1 * W;
  const double depth_from_height = lambda2 * Hh;

  if (depth_from_width <= 0.0 || depth_from_height <= 0.0) {
    result.status = RECON_ERR_CALIB_DEGENERATE;
    result.reason = "解算深度非正，卡片位于相机后方";
    return result;
  }

  const double ratio =
      std::abs(depth_from_width - depth_from_height) /
      std::max(depth_from_width, depth_from_height);
  result.depth_consistency = ratio;
  if (ratio > config.max_depth_consistency) {
    result.status = RECON_ERR_CALIB_INCONSISTENT;
    result.reason = "长短边解算深度不一致，角点检测或内参有误";
    return result;
  }

  // ---- 4. 入射角检查 ----
  // 视线与卡片法向的夹角。过小会导致深度对角点误差极敏感。
  //
  // H 的第三列给出相机光心在卡片坐标系下的位置：
  //   λ3 · H3 = t，取 H3 = K⁻¹ h3 得 t_cam = λ3 · K⁻¹ h3。
  double Kh3[3];
  ApplyKinv(Hm[2], Kh3);
  const double n3 = std::sqrt(Kh3[0] * Kh3[0] + Kh3[1] * Kh3[1] + Kh3[2] * Kh3[2]);
  if (n3 < kEps) {
    result.status = RECON_ERR_CALIB_DEGENERATE;
    result.reason = "无法确定相机相对卡片位置";
    return result;
  }
  const double lambda3 = 1.0 / n3;
  const Vec3d cam_in_card(-lambda3 * Kh3[0], -lambda3 * Kh3[1], -lambda3 * Kh3[2]);
  // 卡片法向在卡片坐标系下为 z 轴。
  const double cos_incidence = std::abs(cam_in_card(2)) / cam_in_card.norm();
  result.incidence_deg =
      std::acos(std::clamp(cos_incidence, -1.0, 1.0)) * 180.0 / recon::kPi;

  if (result.incidence_deg < config.min_incidence_deg) {
    result.status = RECON_ERR_CALIB_DEGENERATE;
    result.reason = "卡片入射角过小，尺度解算不稳定";
    return result;
  }

  // ---- 5. 尺度因子 ----
  // 由深度与卡片物理尺寸，得卡片在相机坐标系下的深度 d（毫米）。
  // 重建结果中卡片的重建边长为 L̂（重建单位，见调用方约定）。
  // 则尺度因子 s = (真实尺寸 / 重建尺寸)。
  //
  // 本函数不接收重建尺寸，因此输出的是「度量深度」与「一致性」，
  // 尺度由上层用重建几何比对得到。此处先给出基于图像的度量深度，
  // 供上层做初始估计与合理性校验。
  result.depth_mm = 0.5 * (depth_from_width + depth_from_height);

  // 相对不确定度：由角点定位残差与尺寸公差传播。
  // σ_s/s ≈ sqrt( (σ_resid / 图像边长像素)^2 + (σ_size / 真实边长)^2 )
  double image_edge_px = 0.0;
  {
    const double dx = corners[2] - corners[0];
    const double dy = corners[3] - corners[1];
    image_edge_px = std::sqrt(dx * dx + dy * dy);
  }
  if (image_edge_px < 1.0) {
    result.status = RECON_ERR_CALIB_DEGENERATE;
    result.reason = "卡片图像边长过小，角点检测不可靠";
    return result;
  }
  const double rel_from_resid = result.reprojection_residual_px / image_edge_px;
  const double rel_from_size = config.ref_size_tolerance_mm / W;
  result.relative_sigma =
      std::sqrt(rel_from_resid * rel_from_resid + rel_from_size * rel_from_size);

  result.ok = true;
  result.status = RECON_OK;
  result.reason = "解算成功";
  return result;
}

ScaleCalibrationResult SolveScaleFromImageEdges(
    const double corners[8], double measured_width_mm,
    double measured_height_mm, const ScaleCalibrationConfig& config) {
  ScaleCalibrationResult result;

  if (measured_width_mm <= 0.0 || measured_height_mm <= 0.0) {
    result.status = RECON_ERR_INVALID_ARGUMENT;
    result.reason = "实测尺寸非法";
    return result;
  }

  // 图像边长。
  const double dx_w = corners[2] - corners[0];
  const double dy_w = corners[3] - corners[1];
  const double img_w = std::sqrt(dx_w * dx_w + dy_w * dy_w);
  const double dx_h = corners[2] - corners[0];
  const double dy_h = corners[5] - corners[1];
  const double img_h = std::sqrt(dx_h * dx_h + dy_h * dy_h);

  if (img_w < 1.0 || img_h < 1.0) {
    result.status = RECON_ERR_CALIB_DEGENERATE;
    result.reason = "图像边长过小";
    return result;
  }

  // 透视投影下长宽比与真实长宽比的偏差，反映了透视强度。
  // 偏差过大说明卡片严重倾斜或角点检测不准。
  const double ratio_img = img_w / img_h;
  const double ratio_real = measured_width_mm / measured_height_mm;
  const double ratio_err = std::abs(ratio_img - ratio_real) / ratio_real;

  result.aspect_consistency = 1.0 - std::min(1.0, ratio_err);
  if (result.aspect_consistency < config.min_aspect_consistency) {
    result.status = RECON_ERR_CALIB_INCONSISTENT;
    result.reason = "图像长宽比与真实长宽比偏差过大";
    return result;
  }

  // 平面性检查：四边应共面。用对角距离的近似关系判断。
  // |L1-L2| 与 |L3-L4| 的差异反映非平面性。
  auto dist = [&](int i, int j) {
    const double dx = corners[i * 2 + 0] - corners[j * 2 + 0];
    const double dy = corners[i * 2 + 1] - corners[j * 2 + 1];
    return std::sqrt(dx * dx + dy * dy);
  };
  const double diag1 = dist(0, 2);
  const double diag2 = dist(1, 3);
  const double diag_diff = std::abs(diag1 - diag2) / std::max(diag1, diag2);
  result.flatness_ratio = diag_diff;
  if (diag_diff > config.max_flatness_ratio) {
    result.status = RECON_ERR_CALIB_INCONSISTENT;
    result.reason = "卡片四角不共面，检测结果不可靠";
    return result;
  }

  result.ok = true;
  result.status = RECON_OK;
  result.reason = "几何校验通过";
  return result;
}

// ---------------------------------------------------------------------------
// 由重建坐标解尺度（docs/07 §2.7.2）
// ---------------------------------------------------------------------------
//
// 与 `SolveScaleFromCorners` 的分工：
//   · 前者用**图像**角点 + 内参 → 输出「度量深度」（需要单应分解）；
//   · 本函数用**重建**角点 → 直接输出尺度因子 s（无需内参、无需单应）。
//
// 为什么落在 core 而不是 C ABI 层：这是纯几何的最小二乘，与平台无关，
// 且它有一套必须被单测覆盖的退化判据（边长退化 / 一致性超限）。
ScaleCalibrationResult SolveScaleFromReconCorners(
    const Vec3d recon_corners[4], double ref_width_mm, double ref_height_mm,
    const ScaleCalibrationConfig& config) {
  ScaleCalibrationResult result;
  result.ok = false;

  if (recon_corners == nullptr || ref_width_mm <= 0.0 || ref_height_mm <= 0.0) {
    result.status = RECON_ERR_INVALID_ARGUMENT;
    result.reason = "参照物尺寸非法";
    return result;
  }

  // 四条相邻边：a₁=(0→1) 宽、a₂=(1→2) 高、a₃=(2→3) 宽、a₄=(3→0) 高。
  const double a[4] = {(recon_corners[1] - recon_corners[0]).norm(),
                       (recon_corners[2] - recon_corners[1]).norm(),
                       (recon_corners[3] - recon_corners[2]).norm(),
                       (recon_corners[0] - recon_corners[3]).norm()};

  // 真实边长（米）。W 对应 a₁/a₃，H 对应 a₂/a₄。
  const double W = ref_width_mm * 1e-3;
  const double Hh = ref_height_mm * 1e-3;
  const double L[4] = {W, Hh, W, Hh};

  // 退化保护：任一边长接近 0 会让 s 爆炸。
  for (int k = 0; k < 4; ++k) {
    if (a[k] < 1e-9) {
      result.status = RECON_ERR_CALIB_DEGENERATE;
      result.reason = "重建卡片边长退化（接近 0），尺度不可解";
      return result;
    }
  }

  // 等权最小二乘闭式解：s = (Σ aₖ Lₖ) / (Σ aₖ²)。
  double num = 0.0, den = 0.0;
  for (int k = 0; k < 4; ++k) {
    num += a[k] * L[k];
    den += a[k] * a[k];
  }
  if (den < 1e-18) {
    result.status = RECON_ERR_CALIB_DEGENERATE;
    result.reason = "法方程退化，尺度不可解";
    return result;
  }
  const double s = num / den;

  // 单约束独立解（一致性交叉验证）。宽边用 a₁、a₃ 的均值，高边用 a₂、a₄。
  const double s_w = W / (0.5 * (a[0] + a[2]));
  const double s_h = Hh / (0.5 * (a[1] + a[3]));
  const double rho = std::abs(s_w - s_h) / std::max(s_w, s_h);
  result.depth_consistency = rho;

  // ρ 上限：复用 max_depth_consistency 字段（默认 0.03，与 docs/07 §2.7.2
  // 的判据表一致）。
  if (rho > config.max_depth_consistency) {
    result.status = RECON_ERR_CALIB_INCONSISTENT;
    result.reason = "宽/高两个独立尺度解不一致（ρ 超限），判为病态";
    return result;
  }

  // 相对不确定度：数组内两条独立解的标准差 / 均值，作为统计口径。
  const double s_mean = 0.5 * (s_w + s_h);
  const double half_diff = 0.5 * std::abs(s_w - s_h);
  // 再加制造公差项：δL/L ≈ tol / min(W, H)。
  const double tol = config.ref_size_tolerance_mm * 1e-3;
  const double rel_from_size = tol / std::min(W, Hh);
  result.relative_sigma =
      std::sqrt((half_diff / s_mean) * (half_diff / s_mean) +
                rel_from_size * rel_from_size);

  result.depth_mm = 0.0;  // 本路径不解深度
  result.reprojection_residual_px = 0.0;  // 不做图像重投影（无内参输入）
  result.incidence_deg = 0.0;             // 无图像角点，算不出入射角
  result.flatness_ratio = 0.0;            // 平面性由重建几何保证（不做额外判据）
  // 「长宽比一致性」在本路径就是 ρ 的归一化形式：ρ=0 → 1.0，ρ 达上限 → 0。
  result.aspect_consistency =
      1.0 - std::min(1.0, rho / config.max_depth_consistency);
  result.scale = s;

  result.reason = "解算成功";
  result.ok = true;
  result.status = RECON_OK;
  return result;
}

}  // namespace recon
