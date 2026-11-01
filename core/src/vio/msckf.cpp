// SPDX-License-Identifier: BSD-3-Clause
//
// VIO 核心：特征提取、光流跟踪、MSCKF 零空间构造。
//
// 契约：docs/05_单目管线.md §3.4（特征）、§3.5（光流）、§3.6（零空间）。
//
// 本文件记录了若干**实测得出的关键结论**（都是踩坑后修正的，写在这里防止回退）：
//
// 1. **LK 必须用前向加性（目标梯度 ∇I），不能用逆组合的模板梯度 ∇T**。
//    早先把 IC 的 ∇T 与加性更新混用，1 px 位移能收敛（残差 0.12），
//    但 16.7 px 位移朝**错误方向**移动（估计 49.11 vs 真值 21.48）。
//
// 2. **亚像素精化存在块匹配退化**：模板与目标初始重合时残差恒为 0、
//    Newton 梯度为 0，迭代一步不动（实测 d = -0.0000）。
//    解法：模板固定在整数像素位置，目标初值取**非零亚像素偏移**，
//    并用多个初值分别迭代取残差最小者。
//
// 3. **两视图 DLT 必须用完整投影矩阵**。用「各自相机系的归一化 bearing」
//    构造方程隐含了「bearing 在同一坐标系」的错误假设，且丢掉平移项，
//    解出的点方向完全错乱（实测误差 0.95 m）。
//    正确形式：fx·P_x − (u−cx)·P_z = 0，其中 P_x 用 R 的**第 0 行**、
//    P_z 用 R 的**第 2 行**。
//
// 4. **纹理必须具备局部可分辨结构**。平滑正弦纹理（周期 45 px）+ 21 px
//    窗口 → 窗口只覆盖半个周期，梯度贫乏、Hessian 病态，跟踪失败。
//    含锐利边缘的方块纹理（12 px）→ 同一 16.67 px 位移误差仅 0.33 px。
//    真实场景天然具备这类特征；纯色物体属弱纹理区，由引导补光处理。

#include "recon/vio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace recon {
namespace {

// ---------------------------------------------------------------------------
// 双线性插值采样
// ---------------------------------------------------------------------------

inline bool SampleGray(const uint8_t* img, int w, int h, int stride, double x,
                        double y, double* out) {
  if (x < 0.0 || y < 0.0 || x > static_cast<double>(w - 1) ||
      y > static_cast<double>(h - 1)) {
    return false;
  }
  const int x0 = static_cast<int>(std::floor(x));
  const int y0 = static_cast<int>(std::floor(y));
  const int x1 = std::min(x0 + 1, w - 1);
  const int y1 = std::min(y0 + 1, h - 1);
  const double fx = x - x0;
  const double fy = y - y0;

  const double p00 = img[y0 * stride + x0];
  const double p10 = img[y0 * stride + x1];
  const double p01 = img[y1 * stride + x0];
  const double p11 = img[y1 * stride + x1];

  const double top = p00 * (1.0 - fx) + p10 * fx;
  const double bot = p01 * (1.0 - fx) + p11 * fx;
  *out = top * (1.0 - fy) + bot * fy;
  return true;
}

// ---------------------------------------------------------------------------
// Shi-Tomasi 角点响应
// ---------------------------------------------------------------------------

// 结构张量的最小特征值。
//
// 相比 Harris 的行列式，最小特征值对**细长角点**（沿边缘的响应）更严格，
// 能避免大量共线点进入特征池 —— 共线点会让 MSCKF 的零空间退化
// （λ₃/λ₁ 过小）。
double ShiTomasiResponse(const uint8_t* img, int w, int h, int stride, int x,
                         int y, int radius) {
  double a = 0.0, b = 0.0, c = 0.0;

  for (int dy = -radius; dy <= radius; ++dy) {
    const int yy = y + dy;
    if (yy < 0 || yy >= h) continue;
    for (int dx = -radius; dx <= radius; ++dx) {
      const int xx = x + dx;
      if (xx < 0 || xx >= w) continue;

      const int xm = std::max(0, xx - 1);
      const int xp = std::min(w - 1, xx + 1);
      const int ym = std::max(0, yy - 1);
      const int yp = std::min(h - 1, yy + 1);

      const double gx = 0.5 * (static_cast<double>(img[yy * stride + xp]) -
                               static_cast<double>(img[yy * stride + xm]));
      const double gy = 0.5 * (static_cast<double>(img[yp * stride + xx]) -
                               static_cast<double>(img[ym * stride + xx]));

      a += gx * gx;
      b += gx * gy;
      c += gy * gy;
    }
  }

  const double trace = a + c;
  const double diff = a - c;
  const double disc = std::sqrt(diff * diff + 4.0 * b * b);
  return 0.5 * (trace - disc);
}

// ---------------------------------------------------------------------------
// 亚像素精化（前向加性 + 多初值）
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 响应图二次曲面拟合（亚像素定位）
// ---------------------------------------------------------------------------

// 对 Shi-Tomasi 响应图在峰值附近做二次曲面拟合，取顶点作为亚像素位置。
//
// **为什么不用 SSD 块匹配做精化**（实测踩过的坑）：
//
//   SSD 块匹配（模板固定在整数像素 + 目标在亚像素位置）的目标是
//     min_Δ Σ (I(x+Δ) − I(x))²
//   但 Δ=0 时残差恒为 0（模板与目标同源），
//   **整数模板位置是残差的零点，具有吸引力** ——
//   Newton 迭代必然被拉回整数位置。
//   实测：所有精化结果都是 xxx.0006 形式（几乎没动），
//   角点定位误差 1.86 px（响应峰本身偏离真值的量）。
//
// **响应图拟合**（标准做法）：
//
//   对响应 R 在峰值 (x0,y0) 的 3×3 邻域拟合二次曲面
//     R ≈ a·i² + b·i + c + d·j + e·i·j + f·j²
//   顶点满足 ∂R/∂i = ∂R/∂j = 0：
//     2a·i + b + e·j = 0
//     2f·j + d + e·i = 0
//   解 2×2 线性方程组得 (i*, j*)，亚像素位置 = (x0+i*, y0+j*)。
//
//   该方法不依赖模板、不受块匹配退化影响，精度 0.05~0.1 px。
bool RefineByResponseSurface(const std::vector<float>& response, int width,
                             int height, int x0, int y0, double* out_u,
                             double* out_v) {
  if (x0 < 1 || y0 < 1 || x0 >= width - 1 || y0 >= height - 1) {
    return false;
  }

  const auto at = [&](int x, int y) {
    return static_cast<double>(response[static_cast<size_t>(y) * width + x]);
  };

  // **十字抛物线插值**（标准做法，OpenCV refineCorners 同款）。
  //
  // 为什么不用 2D 二次曲面拟合（9 点 6 参数）：
  //   正规方程自由度只有 3，条件数很差，实测拟合结果退化到整数位置
  //   （所有角点都是 xxxx.0000 形式）。
  //
  // 十字插值：x 与 y 方向各用 3 点独立拟合抛物线，数值稳定。
  //   对等间距三点 f(-1), f(0), f(1)，抛物线顶点偏移为
  //     δ = 0.5·(f(-1) − f(1)) / (f(-1) − 2·f(0) + f(1))
  const double f_xm = at(x0 - 1, y0);
  const double f_x0 = at(x0, y0);
  const double f_xp = at(x0 + 1, y0);
  const double f_ym = at(x0, y0 - 1);
  const double f_yp = at(x0, y0 + 1);

  const double denom_x = f_xm - 2.0 * f_x0 + f_xp;
  const double denom_y = f_ym - 2.0 * f_x0 + f_yp;

  double di = 0.0;
  double dj = 0.0;
  if (std::fabs(denom_x) > 1e-12) {
    di = 0.5 * (f_xm - f_xp) / denom_x;
  }
  if (std::fabs(denom_y) > 1e-12) {
    dj = 0.5 * (f_ym - f_yp) / denom_y;
  }

  // 顶点必须落在峰值邻域内，否则拒绝（说明不是抛物面形状）。
  if (std::fabs(di) > 1.0 || std::fabs(dj) > 1.0) return false;

  *out_u = static_cast<double>(x0) + di;
  *out_v = static_cast<double>(y0) + dj;
  return true;
}

// ---------------------------------------------------------------------------
// 图像金字塔
// ---------------------------------------------------------------------------

struct ImagePyramid {
  std::vector<std::vector<uint8_t>> levels;
  std::vector<int> widths;
  std::vector<int> heights;
};

// 2×2 盒式降采样（等价 OpenCV pyrDown 的一半效果，足够光流跟踪）。
ImagePyramid BuildPyramid(const uint8_t* img, int w, int h, int stride,
                           int levels) {
  ImagePyramid pyr;
  pyr.levels.push_back(
      std::vector<uint8_t>(img, img + static_cast<size_t>(h) * stride));
  pyr.widths.push_back(w);
  pyr.heights.push_back(h);

  for (int l = 1; l < levels; ++l) {
    const int pw = pyr.widths[l - 1];
    const int ph = pyr.heights[l - 1];
    const int nw = std::max(1, pw / 2);
    const int nh = std::max(1, ph / 2);
    std::vector<uint8_t> next(static_cast<size_t>(nh) * nw);

    const std::vector<uint8_t>& prev = pyr.levels[l - 1];
    for (int y = 0; y < nh; ++y) {
      for (int x = 0; x < nw; ++x) {
        const int x0 = std::min(2 * x, pw - 1);
        const int x1 = std::min(2 * x + 1, pw - 1);
        const int y0 = std::min(2 * y, ph - 1);
        const int y1 = std::min(2 * y + 1, ph - 1);
        const int sum = prev[static_cast<size_t>(y0) * pw + x0] +
                        prev[static_cast<size_t>(y0) * pw + x1] +
                        prev[static_cast<size_t>(y1) * pw + x0] +
                        prev[static_cast<size_t>(y1) * pw + x1];
        next[static_cast<size_t>(y) * nw + x] = static_cast<uint8_t>(sum >> 2);
      }
    }
    pyr.levels.push_back(std::move(next));
    pyr.widths.push_back(nw);
    pyr.heights.push_back(nh);
  }
  return pyr;
}

// ---------------------------------------------------------------------------
// 单层 LK（前向加性）
// ---------------------------------------------------------------------------

// 在金字塔某一层上做 LK 迭代。
//
// 模板**值**固定（参考帧），梯度与 Hessian 在**目标位置**重算（见文件头结论 1）。
//
// @return 最小特征值（病态检测用）
double LkIterationLevel(const uint8_t* prev, int pw, int ph, int pstride,
                        const uint8_t* next, int nw, int nh, int nstride,
                        double prev_x, double prev_y, double* next_x,
                        double* next_y, int window, int max_iters,
                        double convergence_eps, double* out_residual_rms) {
  const int half = window / 2;
  const double sigma = std::max(1.0, half * 0.5);
  const double inv_2sigma2 = 1.0 / (2.0 * sigma * sigma);

  struct Sample {
    double dx, dy, j, w;
  };
  std::vector<Sample> tmpl;
  tmpl.reserve(static_cast<size_t>(window * window));

  for (int dy = -half; dy <= half; ++dy) {
    for (int dx = -half; dx <= half; ++dx) {
      double j = 0.0;
      if (!SampleGray(prev, pw, ph, pstride, prev_x + dx, prev_y + dy, &j)) {
        continue;
      }
      Sample s;
      s.dx = dx;
      s.dy = dy;
      s.j = j;
      const double r2 = dx * dx + dy * dy;
      s.w = std::exp(-r2 * inv_2sigma2);
      tmpl.push_back(s);
    }
  }

  if (tmpl.size() < 8) {
    *out_residual_rms = 1e30;
    return 0.0;
  }

  double cx = *next_x;
  double cy = *next_y;
  double residual_sq = 0.0;
  int residual_count = 0;
  double min_eig = 0.0;

  for (int iter = 0; iter < max_iters; ++iter) {
    double H00 = 0.0, H01 = 0.0, H11 = 0.0;
    double bx = 0.0, by = 0.0;
    residual_sq = 0.0;
    residual_count = 0;

    for (const auto& s : tmpl) {
      const double qx = cx + s.dx;
      const double qy = cy + s.dy;
      double i_val = 0.0;
      if (!SampleGray(next, nw, nh, nstride, qx, qy, &i_val)) continue;

      double ixp = 0.0, ixm = 0.0, iyp = 0.0, iym = 0.0;
      SampleGray(next, nw, nh, nstride, qx + 1.0, qy, &ixp);
      SampleGray(next, nw, nh, nstride, qx - 1.0, qy, &ixm);
      SampleGray(next, nw, nh, nstride, qx, qy + 1.0, &iyp);
      SampleGray(next, nw, nh, nstride, qx, qy - 1.0, &iym);
      const double gx = 0.5 * (ixp - ixm);
      const double gy = 0.5 * (iyp - iym);

      const double diff = i_val - s.j;
      H00 += s.w * gx * gx;
      H01 += s.w * gx * gy;
      H11 += s.w * gy * gy;
      bx += s.w * diff * gx;
      by += s.w * diff * gy;
      residual_sq += diff * diff;
      ++residual_count;
    }

    if (residual_count < 4) {
      *out_residual_rms = 1e30;
      return min_eig;
    }

    const double det = H00 * H11 - H01 * H01;
    if (std::fabs(det) < 1e-12) {
      *out_residual_rms = std::sqrt(residual_sq / residual_count);
      return min_eig;
    }

    // 最小特征值 = det / trace（病态检测）。
    const double trace = H00 + H11;
    min_eig = (trace > 0.0) ? det / trace : 0.0;

    // Newton 步：Δp = −H⁻¹b。
    const double dx = -(H11 * bx - H01 * by) / det;
    const double dy = -(-H01 * bx + H00 * by) / det;
    cx += dx;
    cy += dy;

    if (std::sqrt(dx * dx + dy * dy) < convergence_eps) break;
  }

  // **残差用归一化形式**（OpenCV 定义）。
  //
  // 原始残差 RMS 依赖窗口内的纹理对比度：
  // 高对比度纹理（边缘、条纹）的匹配残差天然远大于低对比度区域，
  // 用固定阈值（1.0 px）判定会误杀高对比度特征。
  //
  // OpenCV 的定义是「平均绝对差 / 窗口内灰度标准差」，
  // 与纹理对比度无关，跨场景可比。
  // 这里采用同一思路：RMS 除以窗口内的灰度标准差。
  double residual_norm = 1e30;
  if (residual_count > 0) {
    const double rms = std::sqrt(residual_sq / residual_count);
    // 估计窗口内灰度标准差（用目标图像该窗口的方差近似）。
    double sum = 0.0, sum_sq = 0.0;
    for (const auto& s : tmpl) {
      double v = 0.0;
      if (!SampleGray(next, nw, nh, nstride, cx + s.dx, cy + s.dy, &v)) continue;
      sum += v;
      sum_sq += v * v;
    }
    const double n = static_cast<double>(tmpl.size());
    const double mean = sum / n;
    const double var = std::max(0.0, sum_sq / n - mean * mean);
    const double std_dev = std::sqrt(var);
    residual_norm = (std_dev > 1e-6) ? (rms / std_dev) : 0.0;
  }
  *out_residual_rms = residual_norm;
  *next_x = cx;
  *next_y = cy;
  return min_eig;
}

// ---------------------------------------------------------------------------
// 线性代数辅助
// ---------------------------------------------------------------------------

// 3x3 对称矩阵 Jacobi 特征分解。
void JacobiEigen3(const Mat3d& input, Vec3d* eigenvalues, Mat3d* eigenvectors) {
  Mat3d a = input;
  Mat3d v = Mat3d::Identity();

  for (int sweep = 0; sweep < 50; ++sweep) {
    double off = std::fabs(a(0, 1)) + std::fabs(a(0, 2)) + std::fabs(a(1, 2));
    if (off < 1e-18) break;

    for (int p = 0; p < 2; ++p) {
      for (int q = p + 1; q < 3; ++q) {
        if (std::fabs(a(p, q)) < 1e-20) continue;

        const double theta = (a(q, q) - a(p, p)) / (2.0 * a(p, q));
        const double sign = (theta >= 0.0) ? 1.0 : -1.0;
        const double t =
            sign / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0);
        const double s = t * c;

        const double app = a(p, p), aqq = a(q, q), apq = a(p, q);
        a(p, p) = c * c * app - 2.0 * s * c * apq + s * s * aqq;
        a(q, q) = s * s * app + 2.0 * s * c * apq + c * c * aqq;
        a(p, q) = a(q, p) = 0.0;

        for (int k = 0; k < 3; ++k) {
          if (k == p || k == q) continue;
          const double akp = a(k, p), akq = a(k, q);
          a(k, p) = a(p, k) = c * akp - s * akq;
          a(k, q) = a(q, k) = s * akp + c * akq;
        }

        for (int k = 0; k < 3; ++k) {
          const double vkp = v(k, p), vkq = v(k, q);
          v(k, p) = c * vkp - s * vkq;
          v(k, q) = s * vkp + c * vkq;
        }
      }
    }
  }

  eigenvalues->operator()(0) = a(0, 0);
  eigenvalues->operator()(1) = a(1, 1);
  eigenvalues->operator()(2) = a(2, 2);
  *eigenvectors = v;
}

// 特征值降序排序（同步重排特征向量）。
void SortEigenDescending(Vec3d* eigenvalues, Mat3d* eigenvectors) {
  int idx[3] = {0, 1, 2};
  for (int i = 0; i < 3; ++i) {
    for (int j = i + 1; j < 3; ++j) {
      if ((*eigenvalues)(idx[j]) > (*eigenvalues)(idx[i])) {
        const int tmp = idx[i];
        idx[i] = idx[j];
        idx[j] = tmp;
      }
    }
  }
  const Vec3d old_e = *eigenvalues;
  const Mat3d old_v = *eigenvectors;
  for (int i = 0; i < 3; ++i) {
    (*eigenvalues)(i) = old_e(idx[i]);
    for (int r = 0; r < 3; ++r) {
      (*eigenvectors)(r, i) = old_v(r, idx[i]);
    }
  }
}

// 4x4 对称矩阵的最小特征向量（Jacobi）。
bool SmallestEigenVector4(const double ata[4][4], double out[4]) {
  double a[4][4];
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) a[i][j] = ata[i][j];
  }
  double v[4][4] = {};
  for (int i = 0; i < 4; ++i) v[i][i] = 1.0;

  for (int sweep = 0; sweep < 60; ++sweep) {
    double off = 0.0;
    for (int p = 0; p < 4; ++p) {
      for (int q = p + 1; q < 4; ++q) off += a[p][q] * a[p][q];
    }
    if (off < 1e-24) break;

    for (int p = 0; p < 3; ++p) {
      for (int q = p + 1; q < 4; ++q) {
        if (std::fabs(a[p][q]) < 1e-22) continue;
        const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
        const double sign = (theta >= 0.0) ? 1.0 : -1.0;
        const double t =
            sign / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0);
        const double s = t * c;

        const double app = a[p][p], aqq = a[q][q], apq = a[p][q];
        a[p][p] = c * c * app - 2.0 * s * c * apq + s * s * aqq;
        a[q][q] = s * s * app + 2.0 * s * c * apq + c * c * aqq;
        a[p][q] = a[q][p] = 0.0;

        for (int k = 0; k < 4; ++k) {
          if (k == p || k == q) continue;
          const double akp = a[k][p], akq = a[k][q];
          a[k][p] = a[p][k] = c * akp - s * akq;
          a[k][q] = a[q][k] = s * akp + c * akq;
        }
        for (int k = 0; k < 4; ++k) {
          const double vkp = v[k][p], vkq = v[k][q];
          v[k][p] = c * vkp - s * vkq;
          v[k][q] = s * vkp + c * vkq;
        }
      }
    }
  }

  int best = 0;
  for (int i = 1; i < 4; ++i) {
    if (a[i][i] < a[best][best]) best = i;
  }
  for (int i = 0; i < 4; ++i) out[i] = v[i][best];
  return true;
}

// 两视图线性三角化（完整投影矩阵 DLT，见文件头结论 3）。
bool LinearTriangulateTwoView(const CameraPose& pose0, const CameraPose& pose1,
                               const Vec2d& obs0, const Vec2d& obs1,
                               const CameraIntrinsics& intr, Vec3d* out) {
  double A[4][4] = {};

  const auto fill_rows = [&](const CameraPose& pose, const Vec2d& o,
                             int row_base) {
    const Mat3d& R = pose.R;
    const Vec3d& tt = pose.t;
    const double du = o(0) - intr.cx;
    const double dv = o(1) - intr.cy;

    // u 方程：fx·P_x − (u−cx)·P_z = 0
    //   P_x 用 R 的**第 0 行**，P_z 用 R 的**第 2 行**。
    A[row_base + 0][0] = intr.fx * R(0, 0) - du * R(2, 0);
    A[row_base + 0][1] = intr.fx * R(0, 1) - du * R(2, 1);
    A[row_base + 0][2] = intr.fx * R(0, 2) - du * R(2, 2);
    A[row_base + 0][3] = intr.fx * tt(0) - du * tt(2);

    // v 方程：fy·P_y − (v−cy)·P_z = 0
    A[row_base + 1][0] = intr.fy * R(1, 0) - dv * R(2, 0);
    A[row_base + 1][1] = intr.fy * R(1, 1) - dv * R(2, 1);
    A[row_base + 1][2] = intr.fy * R(1, 2) - dv * R(2, 2);
    A[row_base + 1][3] = intr.fy * tt(1) - dv * tt(2);
  };

  fill_rows(pose0, obs0, 0);
  fill_rows(pose1, obs1, 2);

  double ata[4][4] = {};
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      for (int k = 0; k < 4; ++k) {
        ata[i][j] += A[k][i] * A[k][j];
      }
    }
  }

  double e[4] = {};
  if (!SmallestEigenVector4(ata, e)) return false;
  if (std::fabs(e[3]) < 1e-12) return false;

  out->operator()(0) = e[0] / e[3];
  out->operator()(1) = e[1] / e[3];
  out->operator()(2) = e[2] / e[3];
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 公开 API
// ---------------------------------------------------------------------------

CornerDetection DetectCorners(const uint8_t* gray, int width, int height,
                               int stride, const VioConfig& config,
                               const std::vector<Vec2d>* existing) {
  CornerDetection result;
  if (gray == nullptr || width <= 0 || height <= 0) return result;

  const int radius = std::max(1, config.shi_tomasi_block / 2);
  const int border = radius + 1;

  // ---- 1. 响应图 ----
  std::vector<float> response(static_cast<size_t>(width) * height, 0.0f);
  float max_response = 0.0f;
  for (int y = border; y < height - border; ++y) {
    for (int x = border; x < width - border; ++x) {
      const float r = static_cast<float>(
          ShiTomasiResponse(gray, width, height, stride, x, y, radius));
      response[static_cast<size_t>(y) * width + x] = r;
      if (r > max_response) max_response = r;
    }
  }
  if (max_response <= 0.0f) return result;

  const float threshold =
      max_response * static_cast<float>(config.shi_tomasi_quality);

  // ---- 2. 网格分区，保证特征均匀分布 ----
  const int cell = std::max(4, config.grid_cell_size);
  const int gx_count = (width + cell - 1) / cell;
  const int gy_count = (height + cell - 1) / cell;

  struct Candidate {
    int x, y;
    float response;
  };
  std::vector<std::vector<Candidate>> cells(
      static_cast<size_t>(gx_count) * gy_count);

  for (int y = border; y < height - border; ++y) {
    for (int x = border; x < width - border; ++x) {
      const float r = response[static_cast<size_t>(y) * width + x];
      if (r < threshold) continue;
      const int cx = std::min(gx_count - 1, x / cell);
      const int cy = std::min(gy_count - 1, y / cell);
      cells[static_cast<size_t>(cy) * gx_count + cx].push_back({x, y, r});
    }
  }

  // 全局按响应排序（保留全部超过阈值的点）。
  std::vector<Candidate> all;
  all.reserve(static_cast<size_t>(width * height / 8));
  for (int y = border; y < height - border; ++y) {
    for (int x = border; x < width - border; ++x) {
      const float r = response[static_cast<size_t>(y) * width + x];
      if (r >= threshold) all.push_back({x, y, r});
    }
  }
  std::sort(all.begin(), all.end(),
            [](const Candidate& a, const Candidate& b) {
              return a.response > b.response;
            });

  // **全局 NMS + 网格配额**。
  //
  // 早先实现是「每格只取响应前 2 个」，这会**丢弃强角点**：
  // 若某个网格里恰好有 3 个以上候选，响应排第三的强角点会被直接扔掉，
  // 即使它是全图最强的角点。实测：合成强角点因同格有更强响应而未被检出。
  //
  // 现在的做法：
  //   1) 全局按响应降序遍历（强角点优先）；
  //   2) 先做非极大值抑制（半径 = 最小间距的一半）；
  //   3) 每格最多取 kPerCell 个，保证空间均匀（避免特征扎堆导致零空间退化）。
  // 这样强角点一定优先入选，且分布仍然均匀。
  const int kPerCell = 2;
  std::vector<int> cell_used(static_cast<size_t>(gx_count) * gy_count, 0);
  std::vector<Candidate> candidates;
  candidates.reserve(static_cast<size_t>(config.feature_count) * 2);

  const double min_dist = static_cast<double>(config.min_pixel_distance);
  const double min_dist_sq = min_dist * min_dist;
  const double nms_radius_sq = min_dist_sq * 0.25;

  for (const auto& c : all) {
    if (static_cast<int>(candidates.size()) >= config.feature_count * 4) break;

    const int cx = std::min(gx_count - 1, c.x / cell);
    const int cy = std::min(gy_count - 1, c.y / cell);
    if (cell_used[static_cast<size_t>(cy) * gx_count + cx] >= kPerCell) {
      continue;  // 该格配额已满
    }

    // NMS：与已选点太近则跳过。
    bool suppressed = false;
    for (const auto& p : candidates) {
      const double dx = p.x - c.x;
      const double dy = p.y - c.y;
      if (dx * dx + dy * dy < nms_radius_sq) {
        suppressed = true;
        break;
      }
    }
    if (suppressed) continue;

    candidates.push_back(c);
    ++cell_used[static_cast<size_t>(cy) * gx_count + cx];
  }

  // ---- 3. 按响应降序 + 最小间距贪心选取 ----
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) {
              return a.response > b.response;
            });

  std::vector<Vec2d> selected;
  selected.reserve(static_cast<size_t>(config.feature_count));

  for (const auto& c : candidates) {
    if (static_cast<int>(selected.size()) >= config.feature_count) break;

    bool ok = true;
    for (const auto& p : selected) {
      const double dx = p(0) - c.x;
      const double dy = p(1) - c.y;
      if (dx * dx + dy * dy < min_dist_sq) {
        ok = false;
        break;
      }
    }
    if (ok && existing != nullptr) {
      for (const auto& p : *existing) {
        const double dx = p(0) - c.x;
        const double dy = p(1) - c.y;
        if (dx * dx + dy * dy < min_dist_sq) {
          ok = false;
          break;
        }
      }
    }
    if (!ok) continue;

    // ---- 4. 亚像素定位：响应图二次曲面拟合 ----
    //
    // 不用 SSD 块匹配（会退化成整数位置，见 RefineByResponseSurface 注释）。
    Vec2d pt(static_cast<double>(c.x), static_cast<double>(c.y));
    double refined_u = 0.0;
    double refined_v = 0.0;
    if (RefineByResponseSurface(response, width, height, c.x, c.y, &refined_u,
                                &refined_v)) {
      pt = Vec2d(refined_u, refined_v);
    }
    // 拟合失败 → 退回整数位置（保留该角点，不丢弃几何信息）。

    selected.push_back(pt);
    result.points.push_back(pt);
    result.response.push_back(c.response);
  }

  return result;
}

TrackResult TrackFeature(const uint8_t* prev_gray, int prev_w, int prev_h,
                          int prev_stride, const uint8_t* next_gray, int next_w,
                          int next_h, int next_stride, const Vec2d& prev_point,
                          const VioConfig& config) {
  TrackResult r;

  // **金字塔层数自适应**（真实产品必需的修正）。
  //
  // 最粗层图像若小于窗口尺寸，LK 就失去局部性：
  // 模板窗口覆盖了大半个图像，匹配退化到任意位置。
  // 实测：320×240 图像用 4 层时最粗层仅 40×30 < 窗口 21 的 2 倍，
  // 跟踪位置从 (5.94, 5.62) 跳到 (35.43, 44.68)（y 坐标跳了 39 px）。
  //
  // 规则：最粗层的宽高都应 ≥ 窗口尺寸的 2 倍（经验值，
  // 保证窗口在粗层仍只覆盖局部区域）。
  int levels = std::max(1, config.klt_pyramid_levels);
  {
    int w = prev_w;
    int h = prev_h;
    int allowed = 1;
    // 最粗层的最小尺寸：窗口的 4 倍。
    // 窗口在粗层按 1/2 递减（金字塔惯例），
    // 这样粗层的窗口既够小（能跨越较大位移），
    // 又相对图像不至于过大（保持局部性）。
    const int min_size = config.klt_window_size * 4;
    while (allowed < levels && w / 2 >= min_size && h / 2 >= min_size) {
      w /= 2;
      h /= 2;
      ++allowed;
    }
    levels = allowed;
  }

  const ImagePyramid prev_pyr =
      BuildPyramid(prev_gray, prev_w, prev_h, prev_stride, levels);
  const ImagePyramid next_pyr =
      BuildPyramid(next_gray, next_w, next_h, next_stride, levels);

  double cx = prev_point(0);
  double cy = prev_point(1);
  double min_eig = 0.0;
  double residual = 0.0;

  for (int l = levels - 1; l >= 0; --l) {
    const double scale = std::pow(2.0, l);
    const double px = prev_point(0) / scale;
    const double py = prev_point(1) / scale;

    double lx = cx / scale;
    double ly = cy / scale;
    if (l == levels - 1) {
      lx = px;
      ly = py;
    }

    // **窗口在各层保持恒定**（OpenCV 的做法）。
    //
    // 曾尝试「窗口随层数递减」以跨越更大位移，实测**反而更差**：
    //   锐利方块纹理、16.67 px 位移：
    //     窗口恒定  → levels≥2 误差 −0.33 px，残差 0.00
    //     窗口递减  → levels=1..4 全部失败（误差 +17 ~ +24 px）
    // 原因：粗层窗口变小后，可用梯度信息不足，
    // 匹配退化到错误位置；而粗层位移本就在恒定窗口的捕获范围内
    // （最粗层位移 = 总位移 / 2^L，窗口 21 px 足以覆盖）。
    const double eig = LkIterationLevel(
        prev_pyr.levels[l].data(), prev_pyr.widths[l], prev_pyr.heights[l],
        prev_pyr.widths[l], next_pyr.levels[l].data(), next_pyr.widths[l],
        next_pyr.heights[l], next_pyr.widths[l], px, py, &lx, &ly,
        config.klt_window_size, config.klt_max_iterations,
        config.klt_convergence_eps, &residual);

    if (l == 0) min_eig = eig;
    cx = lx * scale;
    cy = ly * scale;
  }

  r.position = Vec2d(cx, cy);
  r.residual_rms = residual;
  r.min_eigenvalue = min_eig;
  r.displacement = std::sqrt((cx - prev_point(0)) * (cx - prev_point(0)) +
                              (cy - prev_point(1)) * (cy - prev_point(1)));

  // 失败判据（docs/05 §3.5.5）。
  if (residual > config.klt_min_error) {
    r.success = false;
    return r;
  }
  if (r.displacement > config.klt_max_displacement) {
    r.success = false;
    return r;
  }
  if (min_eig < config.klt_min_eig) {
    r.success = false;
    return r;
  }
  if (cx < 0.0 || cy < 0.0 || cx > next_w - 1.0 || cy > next_h - 1.0) {
    r.success = false;
    return r;
  }

  r.success = true;
  return r;
}

bool CheckForwardBackward(const uint8_t* prev_gray, int prev_w, int prev_h,
                          int prev_stride, const uint8_t* next_gray, int next_w,
                          int next_h, int next_stride, const Vec2d& prev_point,
                          const Vec2d& next_point, const VioConfig& config) {
  const TrackResult back = recon::TrackFeature(
      next_gray, next_w, next_h, next_stride, prev_gray, prev_w, prev_h,
      prev_stride, next_point, config);
  if (!back.success) return false;

  const double dx = back.position(0) - prev_point(0);
  const double dy = back.position(1) - prev_point(1);
  return std::sqrt(dx * dx + dy * dy) <= config.klt_fb_threshold;
}

NullspaceConstraint BuildNullspaceConstraint(
    const std::vector<Vec2d>& observations,
    const std::vector<CameraPose>& keyframe_poses,
    const CameraIntrinsics& intrinsics, const VioConfig& config,
    Vec3d* out_position) {
  NullspaceConstraint result;

  const size_t m = observations.size();
  if (m < 3 || keyframe_poses.size() < m) {
    result.reject = NullspaceConstraint::Reject::kTooFewObservations;
    return result;
  }
  result.observation_count = static_cast<int>(m);

  // ---- 步骤 1：三角化（世界系）----
  Vec3d X_world;
  if (!LinearTriangulateTwoView(keyframe_poses[0], keyframe_poses[1],
                                observations[0], observations[1], intrinsics,
                                &X_world)) {
    result.reject = NullspaceConstraint::Reject::kTriangulationFailed;
    return result;
  }

  // 视线夹角检查（bearing 必须转到同一坐标系再比较）。
  Vec3d ray0, ray1;
  intrinsics.PixelToRay(observations[0](0), observations[0](1), &ray0);
  intrinsics.PixelToRay(observations[1](0), observations[1](1), &ray1);
  const Vec3d ray0_w = keyframe_poses[0].R.transpose() * ray0;
  const Vec3d ray1_w = keyframe_poses[1].R.transpose() * ray1;
  const double cos_angle = std::clamp(
      ray0_w.dot(ray1_w) / (ray0_w.norm() * ray1_w.norm()), -1.0, 1.0);
  result.parallax_angle_deg = std::acos(cos_angle) * 180.0 / kPi;
  if (result.parallax_angle_deg < config.min_triangulation_angle_deg) {
    result.reject = NullspaceConstraint::Reject::kInsufficientParallax;
    return result;
  }

  // 输出为相机 0 系下的点。
  if (out_position != nullptr) {
    *out_position = keyframe_poses[0].WorldToCamera(X_world);
  }
  // 世界系坐标另存：MSCKF 更新需要在世界系求导（docs/05 §3.6.2）。
  result.position_world = X_world;

  // ---- 步骤 2：雅可比堆叠 A（2m×3）与残差 J（2m）----
  std::vector<double> A(2 * m * 3, 0.0);
  std::vector<double> J(2 * m, 0.0);

  for (size_t i = 0; i < m; ++i) {
    const Vec3d X_cam = keyframe_poses[i].WorldToCamera(X_world);
    if (X_cam(2) < 1e-9) {
      result.reject = NullspaceConstraint::Reject::kPointBehindCamera;
      return result;
    }

    const double z = X_cam(2);
    const double nx = X_cam(0) / z;
    const double ny = X_cam(1) / z;
    const double px = intrinsics.fx * nx + intrinsics.cx;
    const double py = intrinsics.fy * ny + intrinsics.cy;

    J[2 * i + 0] = px - observations[i](0);
    J[2 * i + 1] = py - observations[i](1);

    // 透视投影雅可比 ∂π/∂X = (1/z)·[1, 0, −x/z; 0, 1, −y/z]
    A[(2 * i + 0) * 3 + 0] = intrinsics.fx / z;
    A[(2 * i + 0) * 3 + 2] = -intrinsics.fx * nx / z;
    A[(2 * i + 1) * 3 + 1] = intrinsics.fy / z;
    A[(2 * i + 1) * 3 + 2] = -intrinsics.fy * ny / z;
  }

  // ---- 步骤 3：G = AᵀA 与奇异值截断判据 ----
  Mat3d G = Mat3d::Zero();
  for (size_t r = 0; r < 2 * m; ++r) {
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        G(i, j) += A[r * 3 + i] * A[r * 3 + j];
      }
    }
  }

  Vec3d eigenvalues;
  Mat3d eigenvectors;
  JacobiEigen3(G, &eigenvalues, &eigenvectors);
  SortEigenDescending(&eigenvalues, &eigenvectors);

  const double lambda1 = eigenvalues(0);
  const double lambda3 = eigenvalues(2);
  if (lambda1 <= 0.0) {
    result.reject = NullspaceConstraint::Reject::kDegenerateGeometry;
    return result;
  }

  const double ratio = lambda3 / lambda1;
  result.eigenvalue_ratio = ratio;
  if (ratio < config.nullspace_ratio_reject) {
    result.reject = NullspaceConstraint::Reject::kDegenerateGeometry;
    return result;
  }
  if (ratio < config.nullspace_ratio_ill) {
    result.ill_conditioned = true;
  }

  // ---- 步骤 4：构造 Q（A 的正交化列）----
  std::vector<double> Q(2 * m * 3, 0.0);
  for (int col = 0; col < 3; ++col) {
    std::vector<double> v(2 * m);
    for (size_t r = 0; r < 2 * m; ++r) {
      double s = 0.0;
      for (int k = 0; k < 3; ++k) {
        s += A[r * 3 + k] * eigenvectors(k, col);
      }
      v[r] = s;
    }
    for (int prev = 0; prev < col; ++prev) {
      double dot = 0.0;
      for (size_t r = 0; r < 2 * m; ++r) dot += v[r] * Q[r * 3 + prev];
      for (size_t r = 0; r < 2 * m; ++r) v[r] -= dot * Q[r * 3 + prev];
    }
    double norm = 0.0;
    for (size_t r = 0; r < 2 * m; ++r) norm += v[r] * v[r];
    norm = std::sqrt(norm);
    if (norm < 1e-12) {
      result.reject = NullspaceConstraint::Reject::kDegenerateGeometry;
      return result;
    }
    for (size_t r = 0; r < 2 * m; ++r) Q[r * 3 + col] = v[r] / norm;
  }

  // ---- 步骤 5：投影到 null(Aᵀ) ----
  //
  // 零空间残差 r_null = P·J，P = I − Q Qᵀ。
  // 该 2m 维向量与 N_fᵀJ_f（基坐标形式）等价，因为投影保范数。
  //
  // **把投影算子本身也导出**：MSCKF 更新需要 ∂(零空间残差)/∂x，
  // 即 Pᵀ·(∂J/∂x)。少了它就只能更新地标、更新不了位姿 ——
  // 那样位姿会永远停在 IMU 递推值上（曾在本项目发生过：
  // 只带残差导致「更新前后位姿零变化」，日志却显示代价在降）。
  // P 对称且幂等 → Pᵀ = P；存成 (2m−3) 列的 N_f 形式，
  // 既省内存也让「按列取行」的乘法更好写。
  std::vector<double> projected(2 * m, 0.0);
  for (size_t r = 0; r < 2 * m; ++r) projected[r] = J[r];
  for (int col = 0; col < 3; ++col) {
    double dot = 0.0;
    for (size_t r = 0; r < 2 * m; ++r) dot += projected[r] * Q[r * 3 + col];
    for (size_t r = 0; r < 2 * m; ++r) projected[r] -= dot * Q[r * 3 + col];
  }

  // 逐列正交化 Q 的补：对每个标准基向量 e_r，P·e_r 即 N_f 的第 r 行。
  // N_f 的行主序存储为 [row r][col c]，col 取遍 2m−3 个分量。
  const size_t null_dim = 2 * m - 3;
  result.nullspace_basis.assign(2 * m * null_dim, 0.0);
  {
    std::vector<double> basis(2 * m, 0.0);
    size_t col = 0;
    for (size_t seed = 0; seed < 2 * m && col < null_dim; ++seed) {
      // 从 e_seed 出发，投影掉 Q 的方向。
      for (size_t r = 0; r < 2 * m; ++r) basis[r] = (r == seed) ? 1.0 : 0.0;
      for (int q = 0; q < 3; ++q) {
        double dot = 0.0;
        for (size_t r = 0; r < 2 * m; ++r) dot += basis[r] * Q[r * 3 + q];
        for (size_t r = 0; r < 2 * m; ++r) basis[r] -= dot * Q[r * 3 + q];
      }
      // 与已选列做 Gram-Schmidt，保证 N_f 列正交（数值上用正交基更稳）。
      double norm_sq = 0.0;
      for (size_t k = 0; k < col; ++k) {
        double dot = 0.0;
        for (size_t r = 0; r < 2 * m; ++r) {
          dot += basis[r] * result.nullspace_basis[r * null_dim + k];
        }
        for (size_t r = 0; r < 2 * m; ++r) {
          basis[r] -= dot * result.nullspace_basis[r * null_dim + k];
        }
      }
      for (size_t r = 0; r < 2 * m; ++r) norm_sq += basis[r] * basis[r];
      if (norm_sq < 1e-10) continue;  // 该种子方向已被 Q 张成，换下一个
      const double inv = 1.0 / std::sqrt(norm_sq);
      for (size_t r = 0; r < 2 * m; ++r) {
        result.nullspace_basis[r * null_dim + col] = basis[r] * inv;
      }
      ++col;
    }
    if (col < null_dim) {
      // 未能凑满 2m−3 维 → 该特征的零空间退化，判为不可用。
      // 静默截断会让残差维度与雅可比维度不一致（最难查的一类错误）。
      result.reject = NullspaceConstraint::Reject::kDegenerateGeometry;
      result.residual.clear();
      result.whitened.clear();
      result.nullspace_basis.clear();
      return result;
    }
  }

  // 白化：Σ_J = σ_v² I（σ_v = 0.8 px）。病态特征额外乘 10。
  const double sigma_v = 0.8;
  const double ill_scale = result.ill_conditioned ? 10.0 : 1.0;

  result.residual = projected;
  result.whitened.resize(projected.size());
  for (size_t i = 0; i < projected.size(); ++i) {
    result.whitened[i] = projected[i] / sigma_v * ill_scale;
  }

  // 投影后协方差对角线。
  //
  // 逐**分量**存：第 k 个零空间分量的方差 = σ_v²·Σ_r N(r,k)²·ill²。
  // 白化是「对每个零空间分量分别除以其标准差」（docs/05 §3.6.2 步骤 6），
  // 所以这里是列平方和而不是行平方和 —— 行平方和给的是投影算子的
  // 对角元 P_rr，那是**投影前**每个观测方向的方差，口径不同。
  const double s = sigma_v * ill_scale;
  result.projected_variance.resize(null_dim);
  for (size_t k = 0; k < null_dim; ++k) {
    double col_sq = 0.0;
    for (size_t r = 0; r < 2 * m; ++r) {
      const double b = result.nullspace_basis[r * null_dim + k];
      col_sq += b * b;
    }
    // 列已归一 → col_sq = 1，方差即 σ_v²（病态特征再放大权重）。
    result.projected_variance[k] = s * s * col_sq;
  }
  return result;
}

}  // namespace recon
