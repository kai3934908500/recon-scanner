// SPDX-License-Identifier: BSD-3-Clause
//
// 轻量线性代数类型。
//
// 为什么不直接用 Eigen：
//   Eigen 是 MPL-2.0（文件级 copyleft，非传染），是本项目的白名单边界项 B-1。
//   核心几何算法（预积分、位姿、TSDF 坐标变换、尺度解算）只用到极小的
//   线性代数子集，自行实现可以：
//     1) 让这部分核心逻辑完全脱离许可边界；
//     2) 避免移动端 Eigen 的编译体积与 SIMD 开销；
//     3) 数值行为完全可控，便于精度调优。
//
//   Ceres（BA 求解器）仍然需要 Eigen，那是 unavoidable 的，见 docs/10 §4。
//
// 约定：向量列优先（column-major），与 Eigen 一致。

#ifndef RECON_CORE_LINALG_H_
#define RECON_CORE_LINALG_H_

#include <cmath>
#include <cstddef>
#include <initializer_list>

namespace recon {

// pi 常量。避免依赖 <cmath> 的 M_PI（非标准扩展，MSVC 不可用）。
constexpr double kPi = 3.14159265358979323846;

template <int N>
class Vec {
 public:
  Vec() { for (int i = 0; i < N; ++i) data_[i] = 0.0; }
  Vec(double s) { for (int i = 0; i < N; ++i) data_[i] = s; }
  Vec(double x, double y) {
    static_assert(N == 2, "双参构造仅适用于 2 维向量");
    data_[0] = x;
    data_[1] = y;
  }
  Vec(double x, double y, double z) {
    static_assert(N == 3, "三参构造仅适用于 3 维向量");
    data_[0] = x;
    data_[1] = y;
    data_[2] = z;
  }
  Vec(double x, double y, double z, double w) {
    static_assert(N == 4, "四参构造仅适用于 4 维向量");
    data_[0] = x;
    data_[1] = y;
    data_[2] = z;
    data_[3] = w;
  }

  double& operator()(int i) { return data_[i]; }
  const double& operator()(int i) const { return data_[i]; }
  double& operator[](int i) { return data_[i]; }
  const double& operator[](int i) const { return data_[i]; }

  static Vec Zero() { return Vec(); }
  static Vec Ones() { return Vec(1.0); }

  double norm() const {
    double s = 0.0;
    for (int i = 0; i < N; ++i) s += data_[i] * data_[i];
    return std::sqrt(s);
  }
  double squaredNorm() const {
    double s = 0.0;
    for (int i = 0; i < N; ++i) s += data_[i] * data_[i];
    return s;
  }
  Vec normalized() const {
    const double n = norm();
    if (n < 1e-15) return Vec();
    Vec out;
    for (int i = 0; i < N; ++i) out(i) = data_[i] / n;
    return out;
  }
  void normalize() {
    const double n = norm();
    if (n < 1e-15) return;
    for (int i = 0; i < N; ++i) data_[i] /= n;
  }

  Vec operator+(const Vec& o) const {
    Vec out;
    for (int i = 0; i < N; ++i) out(i) = data_[i] + o(i);
    return out;
  }
  Vec operator-(const Vec& o) const {
    Vec out;
    for (int i = 0; i < N; ++i) out(i) = data_[i] - o(i);
    return out;
  }
  Vec operator-() const {
    Vec out;
    for (int i = 0; i < N; ++i) out(i) = -data_[i];
    return out;
  }
  Vec operator*(double s) const {
    Vec out;
    for (int i = 0; i < N; ++i) out(i) = data_[i] * s;
    return out;
  }
  Vec operator/(double s) const {
    Vec out;
    for (int i = 0; i < N; ++i) out(i) = data_[i] / s;
    return out;
  }
  Vec& operator+=(const Vec& o) {
    for (int i = 0; i < N; ++i) data_[i] += o(i);
    return *this;
  }
  Vec& operator-=(const Vec& o) {
    for (int i = 0; i < N; ++i) data_[i] -= o(i);
    return *this;
  }
  Vec& operator*=(double s) {
    for (int i = 0; i < N; ++i) data_[i] *= s;
    return *this;
  }
  Vec& operator/=(double s) {
    if (std::fabs(s) < 1e-300) return *this;
    for (int i = 0; i < N; ++i) data_[i] /= s;
    return *this;
  }

  // 左乘标量。便于书写向量与标量的组合表达式。
  friend Vec operator*(double s, const Vec& v) {
    Vec out;
    for (int i = 0; i < N; ++i) out(i) = s * v(i);
    return out;
  }

  double dot(const Vec& o) const {
    double s = 0.0;
    for (int i = 0; i < N; ++i) s += data_[i] * o(i);
    return s;
  }

  // 叉积。仅 N=3 有意义。
  Vec cross(const Vec& o) const {
    Vec out;
    if constexpr (N == 3) {
      out(0) = data_[1] * o(2) - data_[2] * o(1);
      out(1) = data_[2] * o(0) - data_[0] * o(2);
      out(2) = data_[0] * o(1) - data_[1] * o(0);
    }
    return out;
  }

  bool isFinite() const {
    for (int i = 0; i < N; ++i) {
      if (!std::isfinite(data_[i])) return false;
    }
    return true;
  }

  double* data() { return data_; }
  const double* data() const { return data_; }
  static constexpr int size() { return N; }

 private:
  double data_[N];
};

template <int R, int C>
class Mat {
 public:
  Mat() {
    for (int i = 0; i < R; ++i) {
      for (int j = 0; j < C; ++j) m_[i][j] = 0.0;
    }
  }

  static Mat Zero() { return Mat(); }
  static Mat Identity() {
    Mat out;
    for (int i = 0; i < R && i < C; ++i) out(i, i) = 1.0;
    return out;
  }

  double& operator()(int i, int j) { return m_[i][j]; }
  const double& operator()(int i, int j) const { return m_[i][j]; }

  Mat operator+(const Mat& o) const {
    Mat out;
    for (int i = 0; i < R; ++i) {
      for (int j = 0; j < C; ++j) out(i, j) = m_[i][j] + o(i, j);
    }
    return out;
  }
  Mat operator-(const Mat& o) const {
    Mat out;
    for (int i = 0; i < R; ++i) {
      for (int j = 0; j < C; ++j) out(i, j) = m_[i][j] - o(i, j);
    }
    return out;
  }
  // 左乘标量。便于书写 Rodrigues 公式这类表达式。
  friend Mat operator*(double s, const Mat& m) {
    Mat out;
    for (int i = 0; i < R; ++i) {
      for (int j = 0; j < C; ++j) out(i, j) = s * m(i, j);
    }
    return out;
  }
  Mat operator*(double s) const {
    Mat out;
    for (int i = 0; i < R; ++i) {
      for (int j = 0; j < C; ++j) out(i, j) = m_[i][j] * s;
    }
    return out;
  }
  Mat& operator+=(const Mat& o) {
    for (int i = 0; i < R; ++i) {
      for (int j = 0; j < C; ++j) m_[i][j] += o(i, j);
    }
    return *this;
  }
  Mat& operator*=(double s) {
    for (int i = 0; i < R; ++i) {
      for (int j = 0; j < C; ++j) m_[i][j] *= s;
    }
    return *this;
  }

  // 矩阵乘。列优先存储，因此 (A*B)(i,j) = sum_k A(i,k)*B(k,j)。
  template <int K>
  Mat<R, K> operator*(const Mat<C, K>& o) const {
    Mat<R, K> out;
    for (int i = 0; i < R; ++i) {
      for (int j = 0; j < K; ++j) {
        double s = 0.0;
        for (int k = 0; k < C; ++k) s += m_[i][k] * o(k, j);
        out(i, j) = s;
      }
    }
    return out;
  }

  // 矩阵乘向量。
  Vec<C> operator*(const Vec<C>& v) const {
    Vec<C> out;
    for (int i = 0; i < R; ++i) {
      double s = 0.0;
      for (int j = 0; j < C; ++j) s += m_[i][j] * v(j);
      out(i) = s;
    }
    return out;
  }

  Mat<C, R> transpose() const {
    Mat<C, R> out;
    for (int i = 0; i < R; ++i) {
      for (int j = 0; j < C; ++j) out(j, i) = m_[i][j];
    }
    return out;
  }

  double trace() const {
    double s = 0.0;
    for (int i = 0; i < R && i < C; ++i) s += m_[i][i];
    return s;
  }

  bool isFinite() const {
    for (int i = 0; i < R; ++i) {
      for (int j = 0; j < C; ++j) {
        if (!std::isfinite(m_[i][j])) return false;
      }
    }
    return true;
  }

  double* data() { return &m_[0][0]; }
  const double* data() const { return &m_[0][0]; }

 private:
  double m_[R][C];
};

using Vec2d = Vec<2>;
using Vec3d = Vec<3>;
using Vec4d = Vec<4>;
using Mat3d = Mat<3, 3>;
using Mat4d = Mat<4, 4>;
using Mat9d = Mat<9, 9>;

// 向量外积 a * b^T。
inline Mat3d Outer(const Vec3d& a, const Vec3d& b) {
  Mat3d out;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) out(i, j) = a(i) * b(j);
  }
  return out;
}

// 3x3 求逆（伴随矩阵法）。返回 false 表示奇异。
bool Inverse3x3(const Mat3d& m, Mat3d* out);

// 3x3 行列式。
double Determinant3x3(const Mat3d& m);

}  // namespace recon

#endif  // RECON_CORE_LINALG_H_
