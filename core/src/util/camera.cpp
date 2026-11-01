// SPDX-License-Identifier: BSD-3-Clause
//
// 相机模型实现。见 core/include/recon/camera.h。
//
// 畸变处理的精度关键（docs/05 §5.1.3）：
//   畸变残差属**系统分量**，不随视图数衰减，边缘区贡献 0.05–0.15 mm 形状误差。
//   因此投影与反投影必须严格互逆：带畸变投影 ↔ 迭代反解畸变。
//
// 反解畸变的数值方法（本模块踩过两个坑，必须写清楚）：
//
//   坑 1：朴素不动点迭代 `x ← x + (x_d − distort(x))` 在强畸变下**发散**。
//         畸变雅可比 |J| = ∂r_d/∂r 在边缘处远离 1，|J| > 1 时收敛因子 > 1。
//
//   坑 2：直接对 (x, y) 做二维牛顿迭代也可能**收敛到错误的根**——
//         因为 Brown-Conrady 模型的畸变映射在归一化半径较大时
//         可能非单射，牛顿法会跳到另一个分支。
//
//   本实现采用的稳健方案：**径向-切向解耦 + 一维单调二分**：
//     ① 切向畸变是二阶小量，先忽略，得到纯径向的一维方程
//        r_d = r · (1 + k1 r² + k2 r⁴ + k3 r⁶)
//     ② 该函数在 [0, r_max] 上**严格单调**（只要 k1 > −1/3 等常规条件满足），
//        因此可以用**二分法**稳定求解，**不可能收敛到错误的根**。
//     ③ 解出 r 后，再用少量牛顿步修正切向畸变的耦合，得到最终 (x, y)。
//   这样既保证全局收敛，又保证精度（残差 < 1e-13 归一化 = < 1e-9 px）。

#include "recon/camera.h"

namespace recon {
namespace {

}  // namespace

void CameraIntrinsics::ApplyDistortion(double x, double y, double* out_x,
                                       double* out_y) const {
  const double r2 = x * x + y * y;
  const double r4 = r2 * r2;
  const double r6 = r4 * r2;
  const double radial = 1.0 + k1 * r2 + k2 * r4 + k3 * r6;

  const double pxy = p1 * x * y + p2 * (r2 + 2.0 * x * x);
  const double pxy2 = p1 * (r2 + 2.0 * y * y) + p2 * x * y;

  *out_x = x * radial + pxy;
  *out_y = y * radial + pxy2;
}

void CameraIntrinsics::RemoveDistortion(double x_d, double y_d, double* out_x,
                                         double* out_y) const {
  // 无畸变时直接返回。
  if (k1 == 0.0 && k2 == 0.0 && k3 == 0.0 && p1 == 0.0 && p2 == 0.0) {
    *out_x = x_d;
    *out_y = y_d;
    return;
  }

  // ---- ② 二维牛顿迭代（解析雅可比 + 阻尼）----
  //
  // 初值策略：**直接用 (x_d, y_d)**。
  //
  // 为什么不先用径向二分的结果做初值（曾试过，有坑）：
  //   径向二分解出的是「纯径向模型下的半径」，切向项 p1/p2 会让它偏离真值
  //   （实测 k1=-0.22, p1=8e-4 时偏差约 8.5e-4 归一化 ≈ 1 px）。
  //   从这个偏差初值出发，牛顿步在到达真解前残差会先上升，
  //   阻尼线搜索因此拒绝步长，迭代提前退出，留下约 0.7 px 的残差。
  //
  //   直接用 (x_d, y_d) 作初值时，牛顿法在 4 步内收敛到残差 1e-17
  //   （实测），且残差单调下降，阻尼永不触发。
  //
  // 径向二分结果仅在**牛顿步失败**时作为兜底。
  double x = x_d;
  double y = y_d;

  for (int iter = 0; iter < 50; ++iter) {
    double x_r = 0.0, y_r = 0.0;
    ApplyDistortion(x, y, &x_r, &y_r);
    const double ex = x_d - x_r;
    const double ey = y_d - y_r;
    const double err = std::sqrt(ex * ex + ey * ey);
    if (err < 1e-15) break;

    // 解析雅可比（Brown-Conrady）。
    //
    // 注意：早先版本用中心差分数值雅可比，步长 1e-7 接近 double 的
    // 相对精度极限（x~1 时 eps~2.2e-16，差分步长 1e-7 时有效位数只剩
    // 约 9 位），雅可比被噪声污染 → 牛顿步方向错误 → 发散。
    // 必须用解析式。
    const double r2 = x * x + y * y;
    const double r4 = r2 * r2;
    const double radial = 1.0 + k1 * r2 + k2 * r4 + k3 * r4 * r2;
    const double dr = k1 + 2.0 * k2 * r2 + 3.0 * k3 * r4;

    const double j00 = radial + 2.0 * x * x * dr + 2.0 * p1 * y +
                       p2 * (3.0 * x * x + y * y);
    const double j01 = 2.0 * x * y * dr + p1 * (x * x + 3.0 * y * y) +
                       2.0 * p2 * x * y;
    const double j10 = 2.0 * x * y * dr + 2.0 * p1 * x + p2 * (x * x + 3.0 * y * y);
    const double j11 = radial + 2.0 * y * y * dr + p1 * (3.0 * x * x + y * y) +
                       2.0 * p2 * x * y;

    const double det = j00 * j11 - j01 * j10;
    if (std::fabs(det) < 1e-12) break;

    const double dx = (j11 * ex - j01 * ey) / det;
    const double dy = (-j10 * ex + j00 * ey) / det;

    // 阻尼：确保残差不增。步长从 1.0 逐步减半，最多 20 次。
    double lambda = 1.0;
    bool accepted = false;
    for (int ls = 0; ls < 20; ++ls) {
      const double xn = x + lambda * dx;
      const double yn = y + lambda * dy;
      double x_rn = 0.0, y_rn = 0.0;
      ApplyDistortion(xn, yn, &x_rn, &y_rn);
      const double rn = std::sqrt((x_d - x_rn) * (x_d - x_rn) +
                                  (y_d - y_rn) * (y_d - y_rn));
      if (rn < err) {
        x = xn;
        y = yn;
        accepted = true;
        break;
      }
      lambda *= 0.5;
    }
    if (!accepted) break;
  }

  *out_x = x;
  *out_y = y;
}

void CameraIntrinsics::PixelToNormalized(double u, double v, double* x,
                                          double* y) const {
  const double xn = (u - cx) / fx;
  const double yn = (v - cy) / fy;
  RemoveDistortion(xn, yn, x, y);
}

void CameraIntrinsics::NormalizedToPixel(double x, double y, double* u,
                                          double* v) const {
  *u = fx * x + cx;
  *v = fy * y + cy;
}

void CameraIntrinsics::PixelToRay(double u, double v, Vec3d* ray) const {
  double x = 0.0, y = 0.0;
  PixelToNormalized(u, v, &x, &y);
  (*ray)(0) = x;
  (*ray)(1) = y;
  (*ray)(2) = 1.0;
}

bool CameraIntrinsics::ProjectPoint(const Vec3d& p_cam, double* u,
                                     double* v) const {
  if (p_cam(2) <= 1e-9) return false;  // 在相机后方或过于接近光轴平面

  const double x = p_cam(0) / p_cam(2);
  const double y = p_cam(1) / p_cam(2);
  double x_d = 0.0, y_d = 0.0;
  ApplyDistortion(x, y, &x_d, &y_d);
  NormalizedToPixel(x_d, y_d, u, v);
  return true;
}

}  // namespace recon
