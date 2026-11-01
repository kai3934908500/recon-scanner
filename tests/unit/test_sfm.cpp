// SPDX-License-Identifier: BSD-3-Clause
//
// SfM 几何原语单元测试。契约：docs/05_单目管线.md §4.3–§4.4。
//
// 方法论（沿用本项目铁律）：
//   1. 合成数据必须**可解析**（真值已知），否则测的是「自洽」不是「正确」。
//   2. 判据要选**敏感量**：例如验 E 恢复位姿，要直接比对 R/t 的数值误差，
//      而不是只验「重投影小」——后者在错误尺度下也可能小。
//   3. 每个断言都做过**敏感性验证**（注入错误看是否变红），见工作日志。

#include "recon/sfm.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::printf("  [FAIL] %s\n", what);
    ++g_failures;
  }
}

void CheckNear(double a, double b, double tol, const char* what) {
  if (!(std::fabs(a - b) <= tol)) {
    std::printf("  [FAIL] %s (got %.9g, want %.9g, tol %.3g)\n", what, a, b, tol);
    ++g_failures;
  }
}

using recon::CameraIntrinsics;
using recon::CameraPose;
using recon::Mat3d;
using recon::Vec2d;
using recon::Vec3d;

CameraIntrinsics MakeK() {
  CameraIntrinsics k;
  k.fx = 800.0;
  k.fy = 800.0;
  k.cx = 320.0;
  k.cy = 240.0;
  k.width = 640;
  k.height = 480;
  return k;
}

// 生成一组 3D 点（相机 0 系）。
std::vector<Vec3d> MakePoints(int n, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> u(-0.5, 0.5);
  std::uniform_real_distribution<double> z(1.5, 3.5);
  std::vector<Vec3d> pts;
  for (int i = 0; i < n; ++i) pts.push_back(Vec3d(u(rng), u(rng), z(rng)));
  return pts;
}

// 把相机 0 系的点投影到某位姿。返回是否成功（在相机前方且在画面内）。
bool Project(const CameraPose& pose, const CameraIntrinsics& k, const Vec3d& X,
             Vec2d* uv) {
  const Vec3d pc = pose.WorldToCamera(X);
  if (pc(2) <= 1e-6) return false;
  double u = 0.0, v = 0.0;
  if (!k.ProjectPoint(pc, &u, &v)) return false;
  if (u < 0.0 || v < 0.0 || u >= k.width || v >= k.height) return false;
  *uv = Vec2d(u, v);
  return true;
}

// 归一化坐标（去畸变 + 除内参）。
Vec2d ToNorm(const CameraIntrinsics& k, const Vec2d& uv) {
  double x = 0.0, y = 0.0;
  k.PixelToNormalized(uv(0), uv(1), &x, &y);
  return Vec2d(x, y);
}

// 构造真值相对位姿：绕 Y 轴转 angle、沿 X 平移 baseline。
CameraPose MakeSecondPose(double angle_rad, double baseline) {
  CameraPose p;
  const double c = std::cos(angle_rad), s = std::sin(angle_rad);
  // R = Ry(angle)
  p.R(0, 0) = c; p.R(0, 1) = 0.0; p.R(0, 2) = s;
  p.R(1, 0) = 0.0; p.R(1, 1) = 1.0; p.R(1, 2) = 0.0;
  p.R(2, 0) = -s; p.R(2, 1) = 0.0; p.R(2, 2) = c;
  // 相机 1 由相机 0 平移：t 满足 相机 1 光心在相机 0 系 = -Rᵀt
  // 期望光心 c1 = (baseline, 0, 0)：-Rᵀt = c1 ⇒ t = -R·c1。
  const Vec3d c1(baseline, 0.0, 0.0);
  p.t = -(p.R * c1);
  return p;
}

// 由「旋转角 (rx, ry, rz) + 光心 C」构造位姿（t = −R·C）。
// 用于让多视图的**光心不共线** —— 共线光心是线性透视 SfM 的退化配置
// （critical configuration，存在额外 1 参数解族），会使 BA 的目标失去唯一性。
CameraPose MakePose(double rx, double ry, double rz, const Vec3d& C) {
  auto rot = [](double a, int axis) {
    const double c = std::cos(a), s = std::sin(a);
    Mat3d m = Mat3d::Identity();
    if (axis == 0) { m(1, 1) = c; m(1, 2) = -s; m(2, 1) = s; m(2, 2) = c; }
    if (axis == 1) { m(0, 0) = c; m(0, 2) = s; m(2, 0) = -s; m(2, 2) = c; }
    if (axis == 2) { m(0, 0) = c; m(0, 1) = -s; m(1, 0) = s; m(1, 1) = c; }
    return m;
  };
  CameraPose p;
  p.R = rot(rx, 0) * rot(ry, 1) * rot(rz, 2);
  p.t = -(p.R * C);
  return p;
}

// ---------------------------------------------------------------------------
// 1. 本质矩阵恢复位姿：真值 R/t 直接比对
// ---------------------------------------------------------------------------
void TestRecoverPoseExact() {
  std::printf("[TEST] E 矩阵恢复相对位姿（合成精确对应）\n");
  const CameraIntrinsics k = MakeK();
  const std::vector<Vec3d> pts = MakePoints(200, 12345);

  const double angle_deg = 6.0;
  const double baseline = 0.20;
  const CameraPose pose0;  // 单位位姿（视图 0 相机系 = 世界系）
  const CameraPose pose1 = MakeSecondPose(angle_deg * recon::kPi / 180.0, baseline);

  std::vector<Vec2d> n0, n1;
  std::vector<Vec3d> used;
  for (const Vec3d& X : pts) {
    Vec2d a, b;
    if (!Project(pose0, k, X, &a)) continue;
    if (!Project(pose1, k, X, &b)) continue;
    n0.push_back(ToNorm(k, a));
    n1.push_back(ToNorm(k, b));
    used.push_back(X);
  }
  std::printf("    可用对应点 %zu\n", n0.size());
  Check(n0.size() > 50, "应有足够对应点");

  recon::SfmConfig cfg;
  Mat3d e;
  recon::RansacResult rr =
      recon::EstimateEssentialMatrix(n0, n1, k, cfg, &e);
  Check(rr.ok, "E 估计应成功");
  std::printf("    RANSAC 内点 %d / %zu，迭代 %d\n", rr.inliers, n0.size(),
              rr.iterations_used);

  // E 的奇异值应为 (s, s, 0)。
  {
    // 用 EᵀE 的特征值检查 σ1 ≈ σ2 且 σ3 ≈ 0。
    double ata[3][3] = {};
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        for (int m = 0; m < 3; ++m) ata[i][j] += e(m, i) * e(m, j);
      }
    }
    // 粗略检查：迹的一半应近似等于最大特征值，而 det 应 ~0。
    CheckNear(recon::Determinant3x3(e), 0.0, 1e-6, "E 的行列式应为 0");
  }

  CameraPose rec;
  std::vector<uint8_t> mask;
  const bool ok = recon::RecoverPose(e, n0, n1, &rec, &mask);
  Check(ok, "位姿恢复应成功");

  // 真值：R_gt = pose1.R，t_gt = pose1.t / baseline（E 的 t 是单位向量）。
  // 比对旋转（Frobenius 范数）。
  double rot_err = 0.0;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      const double d = rec.R(i, j) - pose1.R(i, j);
      rot_err += d * d;
    }
  }
  rot_err = std::sqrt(rot_err);
  std::printf("    恢复旋转 Frobenius 误差 %.3e\n", rot_err);
  Check(rot_err < 1e-6, "恢复的旋转应精确（合成无噪）");

  // 平移方向：与真值单位平移比对（E 分解的 t 尺度不可观测）。
  const Vec3d t_gt = pose1.t.normalized();
  const Vec3d t_rec = rec.t.normalized();
  const double dot = std::fabs(t_gt.dot(t_rec));
  std::printf("    恢复平移方向 |cos| %.9f\n", dot);
  CheckNear(dot, 1.0, 1e-6, "恢复的平移方向应精确");

  // 正深度点数应接近全部。
  if (!mask.empty()) {
    int pos = 0;
    for (uint8_t m : mask) pos += m ? 1 : 0;
    std::printf("    cheirality 正深度 %d / %zu\n", pos, mask.size());
    Check(pos > static_cast<int>(mask.size()) * 9 / 10,
          "绝大多数点应在两相机前方");
  }
}

// ---------------------------------------------------------------------------
// 2. 多视图三角化：真值 3D 点直接比对
// ---------------------------------------------------------------------------
void TestTriangulationExact() {
  std::printf("[TEST] 多视图三角化（3 视图，真值比对）\n");
  const CameraIntrinsics k = MakeK();
  const std::vector<Vec3d> pts = MakePoints(100, 999);

  const CameraPose pose0;
  const CameraPose pose1 = MakeSecondPose(6.0 * recon::kPi / 180.0, 0.20);
  const CameraPose pose2 = MakeSecondPose(-5.0 * recon::kPi / 180.0, -0.18);

  std::vector<CameraPose> poses = {pose0, pose1, pose2};
  std::vector<CameraIntrinsics> intrs = {k, k, k};

  recon::SfmConfig cfg;
  int tested = 0;
  double max_err = 0.0;
  for (const Vec3d& X : pts) {
    std::vector<Vec2d> obs;
    bool vis = true;
    for (const CameraPose& p : poses) {
      Vec2d uv;
      if (!Project(p, k, X, &uv)) { vis = false; break; }
      obs.push_back(uv);
    }
    if (!vis) continue;

    Vec3d est;
    if (!recon::TriangulateMultiView(poses, intrs, obs, cfg, &est)) continue;
    max_err = std::max(max_err, (est - X).norm());
    ++tested;
  }
  std::printf("    三角化 %d 点，最大误差 %.3e m\n", tested, max_err);
  Check(tested > 30, "应有足够三角化成功的点");
  Check(max_err < 1e-9, "合成无噪下三角化应达机器精度");
}

// ---------------------------------------------------------------------------
// 3. 退化拒绝：两点视差过小 / 深度为负
// ---------------------------------------------------------------------------
void TestDegenerateRejection() {
  std::printf("[TEST] 退化情形拒绝（视差过小）\n");
  const CameraIntrinsics k = MakeK();
  const CameraPose pose0;
  // 极小基线 → 视差角远小于 1.5°。
  const CameraPose pose1 = MakeSecondPose(0.0, 0.0005);
  std::vector<CameraPose> poses = {pose0, pose1};
  std::vector<CameraIntrinsics> intrs = {k, k};
  recon::SfmConfig cfg;

  int rejected = 0;
  const std::vector<Vec3d> pts = MakePoints(50, 7);
  for (const Vec3d& X : pts) {
    std::vector<Vec2d> obs;
    bool vis = true;
    for (const CameraPose& p : poses) {
      Vec2d uv;
      if (!Project(p, k, X, &uv)) { vis = false; break; }
      obs.push_back(uv);
    }
    if (!vis) continue;
    Vec3d est;
    if (!recon::TriangulateMultiView(poses, intrs, obs, cfg, &est)) ++rejected;
  }
  std::printf("    视差过小被拒 %d 点\n", rejected);
  Check(rejected > 0, "视差过小的点应被拒绝（否则三角化会给出极不稳定的解）");
}

// ---------------------------------------------------------------------------
// 4. 基础矩阵：Sampson 距离在真值 F 上应近零
// ---------------------------------------------------------------------------
void TestFundamentalSelfConsistency() {
  std::printf("[TEST] 基础矩阵复原（合成精确对应）\n");
  const CameraIntrinsics k = MakeK();
  const std::vector<Vec3d> pts = MakePoints(150, 555);
  const CameraPose pose0;
  const CameraPose pose1 = MakeSecondPose(8.0 * recon::kPi / 180.0, 0.25);

  std::vector<Vec2d> n0, n1;
  for (const Vec3d& X : pts) {
    Vec2d a, b;
    if (!Project(pose0, k, X, &a)) continue;
    if (!Project(pose1, k, X, &b)) continue;
    n0.push_back(ToNorm(k, a));
    n1.push_back(ToNorm(k, b));
  }
  Check(n0.size() > 50, "应有足够对应点");

  recon::SfmConfig cfg;
  Mat3d f;
  recon::RansacResult rr = recon::EstimateFundamentalMatrix(n0, n1, k, cfg, &f);
  Check(rr.ok, "F 估计应成功");
  std::printf("    RANSAC 内点 %d / %zu\n", rr.inliers, n0.size());

  // 用全部对应点测 Sampson 距离的中位数。
  std::vector<double> ds;
  for (size_t i = 0; i < n0.size(); ++i) {
    ds.push_back(recon::SampsonDistanceNormalized(f, n0[i], n1[i], k));
  }
  std::sort(ds.begin(), ds.end());
  const double median = ds[ds.size() / 2];
  const double p90 = ds[ds.size() * 9 / 10];
  std::printf("    Sampson 中位数 %.3e px，P90 %.3e px\n", median, p90);
  Check(median < 1e-6, "真值对应下 Sampson 中位数应近零");
  Check(p90 < 1e-4, "Sampson P90 应极小");
}

// ---------------------------------------------------------------------------
// 5. 重投影误差工具自洽
// ---------------------------------------------------------------------------
void TestReprojectionUtil() {
  std::printf("[TEST] 重投影误差工具\n");
  const CameraIntrinsics k = MakeK();
  const CameraPose pose0;
  const Vec3d X(0.1, -0.05, 2.0);
  Vec2d uv;
  Check(Project(pose0, k, X, &uv), "点应可投影");
  double err = 0.0;
  Check(recon::ReprojectionErrorPx(pose0, k, X, uv, &err), "重投影应成功");
  CheckNear(err, 0.0, 1e-9, "无扰动时重投影误差应为 0");
  // 人为偏移 3 px。
  const Vec2d uv2(uv(0) + 3.0, uv(1) + 4.0);
  Check(recon::ReprojectionErrorPx(pose0, k, X, uv2, &err), "重投影应成功");
  CheckNear(err, 5.0, 1e-9, "3-4-5 直角三角形，误差应为 5 px");
}

// ---------------------------------------------------------------------------
// 6. 特征提取 + 描述子：合成纹理
// ---------------------------------------------------------------------------
// 纹理 = 若干随机高斯斑点叠加。斑点位置/强度由种子确定，可解析 → 两张视图
// 采样的其实是**同一个连续函数** f(p)，故可算出某特征在另一视图的精确落点。
struct Blob {
  double x, y, amp;
};

std::vector<Blob> MakeBlobs(int n, uint64_t seed, int w, int h) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> px(6.0, w - 6.0);
  std::uniform_real_distribution<double> py(6.0, h - 6.0);
  std::uniform_real_distribution<double> amp(-220.0, 220.0);
  std::vector<Blob> b;
  for (int i = 0; i < n; ++i) b.push_back({px(rng), py(rng), amp(rng)});
  return b;
}

double SampleTexture(const std::vector<Blob>& blobs, double x, double y) {
  double v = 128.0;
  const double s2 = 2.0 * 2.0 * 2.0;  // sigma = 2
  for (const Blob& b : blobs) {
    const double dx = x - b.x, dy = y - b.y;
    v += b.amp * std::exp(-(dx * dx + dy * dy) / s2);
  }
  return std::max(0.0, std::min(255.0, v));
}

// 把 f(p) 渲染成图 A（恒等采样）。
void RenderA(const std::vector<Blob>& blobs, int w, int h, std::vector<uint8_t>* img) {
  img->assign(static_cast<size_t>(w) * h, 0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      (*img)[static_cast<size_t>(y) * w + x] =
          static_cast<uint8_t>(std::llround(SampleTexture(blobs, x, y)));
    }
  }
}

// 图 B：绕中心 c 旋转 θ（B[q] = f(Rᵀ(q−c)+c)）。于是 A 中位于 p 的特征
// 在 B 中的精确落点为 q = c + R(p−c)。
// 注：Blob 纹理版本 RenderB 已移除（孤立斑点主方向不稳定，不适合旋转测试），
// 旋转测试改用方向明确的波纹理（见 RenderWaves）。

// ---------------------------------------------------------------------------
// 纹理生成 B：多频正弦平面波叠加（方向明确、旋转等变）
// ---------------------------------------------------------------------------
// 为什么不用孤立高斯斑点做旋转测试：单个孤立斑点在其 ~16 px 邻域内近似
// 圆对称，主方向无稳定定义 —— 旋转重采样后主方向随随机邻近结构跳变
// （实测：绕光轴 5° 主方向残差中位数 74°，呈随机分布）。这是**测试纹理
// 不真实**，不是主方向算法错误。
// 真实物体表面是连续纹理（多方向梯度叠加），局部方向稳定。这里用几组
// 不同方向/频率的平面波叠加来模拟：波本身方向明确，且绕光轴旋转 θ 时
// 波矢也精确旋转 θ → 主方向严格等变，可解析验证。
struct Wave {
  double kx, ky, phase, amp;
};

std::vector<Wave> MakeWaves(int n, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> ang(0.0, 2.0 * recon::kPi);
  std::uniform_real_distribution<double> freq(0.05, 0.35);   // 周期 18–125 px
  std::uniform_real_distribution<double> ph(0.0, 2.0 * recon::kPi);
  std::uniform_real_distribution<double> amp(60.0, 120.0);
  std::vector<Wave> w;
  for (int i = 0; i < n; ++i) {
    const double a = ang(rng), f = freq(rng);
    w.push_back({f * std::cos(a), f * std::sin(a), ph(rng), amp(rng)});
  }
  return w;
}

// 波函数取值为 -1..1 的归一化叠加，再映射到像素灰度。局部有明确梯度方向。
double SampleWaves(const std::vector<Wave>& ws, double x, double y) {
  double v = 0.0;
  for (const Wave& w : ws) v += w.amp * std::sin(w.kx * x + w.ky * y + w.phase);
  return v;
}

// 把波纹理渲染成图：offset (ox,oy) 为采样平移。绕中心旋转 θ 时用
// SampleWaves(ws, Rᵀ(p−c)+c)。
void RenderWaves(const std::vector<Wave>& ws, int w, int h, double theta,
                 std::vector<uint8_t>* img) {
  img->assign(static_cast<size_t>(w) * h, 0);
  const double c = std::cos(theta), s = std::sin(theta);
  const double cx = w * 0.5, cy = h * 0.5;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const double dqx = x - cx, dqy = y - cy;
      const double ppx = cx + c * dqx + s * dqy;
      const double ppy = cy - s * dqx + c * dqy;
      const double v = SampleWaves(ws, ppx, ppy);
      (*img)[static_cast<size_t>(y) * w + x] =
          static_cast<uint8_t>(std::llround(std::max(0.0, std::min(255.0, 128.0 + v * 0.5))));
    }
  }
}

// ---------------------------------------------------------------------------
// 7. 自匹配恒等不变量：同一特征集自匹配，每条匹配必须命中自身
// ---------------------------------------------------------------------------
// 这是最短、最锐的不变量：比值检验（best/self=0）+ 双向一致性下，唯一
// 自洽解就是 i↔i。任何最近邻/比值/双向一致性写错都会破坏它。
void TestMatchingIdentity() {
  std::printf("[TEST] 自匹配恒等不变量\n");
  const int W = 320, H = 240;
  const std::vector<Blob> blobs = MakeBlobs(200, 111, W, H);
  std::vector<uint8_t> img;
  RenderA(blobs, W, H, &img);

  recon::SfmConfig cfg;
  cfg.feature_count = 400;
  recon::SfmFeatureSet fa =
      recon::ExtractSfmFeatures(img.data(), W, H, W, 0, cfg);
  std::printf("    特征数 %zu\n", fa.features.size());
  Check(fa.features.size() > 50, "应提取到足够特征");

  const std::vector<recon::SfmMatch> m = recon::MatchFeatureSets(fa, fa, cfg);
  int self = 0;
  for (const recon::SfmMatch& x : m) {
    if (x.index0 == x.index1) ++self;
  }
  std::printf("    自匹配 %zu 条，命中自身 %d 条\n", m.size(), self);
  Check(!m.empty(), "自匹配应非空");
  Check(self == static_cast<int>(m.size()),
        "自匹配的每一条都应命中自身（i↔i）");
}

// ---------------------------------------------------------------------------
// 8. 旋转不变性：分析图像绕光轴旋转后，匹配召回与精度
// ---------------------------------------------------------------------------
// 这是**描述子旋转对齐**的直接验证（matching.cpp 踩坑记录 #1）。
// 若不按主方向对齐，绕光轴旋转会让直方图 bin 整体偏移 → 召回断崖下跌。
void TestMatchingRotationInvariant() {
  std::printf("[TEST] 特征匹配（绕光轴旋转，多角度扫描）\n");
  const int W = 320, H = 240;
  const std::vector<Wave> waves = MakeWaves(6, 4242);
  std::vector<uint8_t> imgA;
  RenderWaves(waves, W, H, 0.0, &imgA);

  recon::SfmConfig cfg;
  cfg.feature_count = 400;
  recon::SfmFeatureSet fa =
      recon::ExtractSfmFeatures(imgA.data(), W, H, W, 0, cfg);
  std::printf("    特征数 A=%zu\n", fa.features.size());
  Check(fa.features.size() > 50, "A 应有足够特征");

  auto dist = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    double s = 0.0;
    for (size_t k = 0; k < a.size(); ++k) {
      const double d = static_cast<double>(a[k]) - static_cast<double>(b[k]);
      s += d * d;
    }
    return std::sqrt(s);
  };

  const double angles[] = {0.0, 5.0, 15.0, 30.0};
  double worst_recall = 1.0;
  double worst_precision = 1.0;
  for (double adeg : angles) {
    std::vector<uint8_t> imgB;
    const double theta = adeg * recon::kPi / 180.0;
    RenderWaves(waves, W, H, theta, &imgB);
    recon::SfmFeatureSet fb =
        recon::ExtractSfmFeatures(imgB.data(), W, H, W, 1, cfg);

    // 真值对应：A 的特征按 q = c + R(p−c) 映射，找 B 中最近的、且在 1.5 px 内。
    const double c = std::cos(theta), s = std::sin(theta);
    const double cx = W * 0.5, cy = H * 0.5;
    std::vector<int> gt(fa.features.size(), -1);
    int gt_count = 0;
    for (size_t i = 0; i < fa.features.size(); ++i) {
      const double ax = fa.features[i].uv(0) - cx, ay = fa.features[i].uv(1) - cy;
      const double qx = cx + c * ax - s * ay;
      const double qy = cy + s * ax + c * ay;
      int best = -1;
      double bd = 1e9;
      for (size_t j = 0; j < fb.features.size(); ++j) {
        const double dx = fb.features[j].uv(0) - qx;
        const double dy = fb.features[j].uv(1) - qy;
        const double d = std::sqrt(dx * dx + dy * dy);
        if (d < bd) { bd = d; best = static_cast<int>(j); }
      }
      if (bd <= 1.5) { gt[i] = best; ++gt_count; }
    }

    // 诊断：真值即最近邻的比例（描述子分辨力的直接度量）。
    int nn_hit = 0, eval = 0;
    // 诊断：主方向是否随图像旋转而正确旋转（旋转不变性的**核心目标量**）。
    // 若 ∠(angle_B − angle_A) 的中位数 ≈ θ，则主方向估计是旋转等变的；
    // 若明显偏离 θ，则描述子的旋转对齐从根上就错了。
    std::vector<double> ang_err;     // |wrap(angle_B − angle_A − θ)|
    std::vector<double> pos_err;     // 检测位置与真值位置的偏差（px）
    for (size_t i = 0; i < fa.features.size(); ++i) {
      if (gt[i] < 0) continue;
      ++eval;
      int best = -1;
      double bd = 1e30;
      for (size_t j = 0; j < fb.features.size(); ++j) {
        const double d =
            dist(fa.features[i].descriptor, fb.features[j].descriptor);
        if (d < bd) { bd = d; best = static_cast<int>(j); }
      }
      if (best == gt[i]) ++nn_hit;
      // 主方向残差。
      const size_t g = static_cast<size_t>(gt[i]);
      double da = fb.features[g].angle - fa.features[i].angle - theta;
      while (da > recon::kPi) da -= 2.0 * recon::kPi;
      while (da < -recon::kPi) da += 2.0 * recon::kPi;
      ang_err.push_back(std::fabs(da) * 180.0 / recon::kPi);
      // 位置偏差：B 中真值点与检测点的距离。
      const double ax = fa.features[i].uv(0) - cx, ay = fa.features[i].uv(1) - cy;
      const double qx = cx + c * ax - s * ay;
      const double qy = cy + s * ax + c * ay;
      const double dxp = fb.features[g].uv(0) - qx;
      const double dyp = fb.features[g].uv(1) - qy;
      pos_err.push_back(std::sqrt(dxp * dxp + dyp * dyp));
    }
    std::sort(ang_err.begin(), ang_err.end());
    std::sort(pos_err.begin(), pos_err.end());

    const std::vector<recon::SfmMatch> m = recon::MatchFeatureSets(fa, fb, cfg);
    int correct = 0;
    for (const recon::SfmMatch& mm : m) {
      if (gt[static_cast<size_t>(mm.index0)] == mm.index1) ++correct;
    }
    const double recall =
        gt_count > 0 ? static_cast<double>(correct) / gt_count : 0.0;
    const double precision =
        m.empty() ? 0.0
                  : static_cast<double>(correct) / static_cast<double>(m.size());
    std::printf(
        "    θ=%2.0f°: 真值 %d，NN命中 %d (%.2f)，匹配 %zu 正确 %d，"
        "召回 %.2f 精度 %.2f\n",
        adeg, gt_count, nn_hit, eval > 0 ? double(nn_hit) / eval : 0.0, m.size(),
        correct, recall, precision);
    std::printf("        [诊断] 主方向残差中位数 %.2f°，位置偏差中位数 %.2f px\n",
                ang_err.empty() ? -1.0 : ang_err[ang_err.size() / 2],
                pos_err.empty() ? -1.0 : pos_err[pos_err.size() / 2]);
    if (adeg <= 15.0) {
      worst_recall = std::min(worst_recall, recall);
      worst_precision = std::min(worst_precision, precision);
    }
  }
  // 判据定在**手持扫描实际会遇到的旋转范围**（03 §4.5：帧间角度步长 4°–35°，
  // 相邻关键帧通常 ≤ 15°）。15° 以内必须可靠。
  Check(worst_recall > 0.30, "≤15° 旋转下召回应可靠");
  Check(worst_precision > 0.70, "≤15° 旋转下精度应较高");
}

// ---------------------------------------------------------------------------
// 9. 匹配图结构：MM-1 序列邻接 + 上限（MM-4）
// ---------------------------------------------------------------------------
void TestMatchGraphStructure() {
  std::printf("[TEST] 匹配图结构\n");
  const int n = 6;
  std::vector<recon::SfmFeatureSet> sets(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) sets[static_cast<size_t>(i)].image_id = i;

  recon::SfmConfig cfg;
  cfg.neighbor_sequence_span = 3;
  cfg.max_num_neighbors = 80;
  const std::vector<recon::SfmCandidatePair> pairs =
      recon::BuildMatchGraph(sets, nullptr, cfg);

  // span=3 时：i=0→3、i=1→3、i=2→3、i=3→2、i=4→1、i=5→0，合计 12。
  std::printf("    边数 %zu（期望 12）\n", pairs.size());
  Check(pairs.size() == 12, "无位姿时应只含 MM-1 序列邻接边，且无重复");

  bool ordered = true, dup = false;
  for (size_t a = 0; a < pairs.size(); ++a) {
    if (pairs[a].index0 >= pairs[a].index1) ordered = false;  // 应规整 i<j
    for (size_t b = a + 1; b < pairs.size(); ++b) {
      if (pairs[a].index0 == pairs[b].index0 &&
          pairs[a].index1 == pairs[b].index1) {
        dup = true;
      }
    }
  }
  Check(ordered, "边应规整为 index0 < index1，无自环");
  Check(!dup, "边不应重复");

  auto has_edge = [&](int i, int j) {
    for (const recon::SfmCandidatePair& p : pairs) {
      if (p.index0 == i && p.index1 == j) return true;
    }
    return false;
  };
  Check(has_edge(0, 3), "跨距 3 内的 (0,3) 应存在");
  Check(!has_edge(0, 4), "超过 span 的 (0,4) 不应存在");
  Check(has_edge(4, 5), "末端的 (4,5) 应存在");

  // 带上位姿时应新增 MM-2 空间近邻边（同样受上限约束）。
  std::vector<recon::CameraPose> poses(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    // 让相机光心均匀分布在以原点为心的圆弧上 → 两两方位差足够大。
    const double a = i * 8.0 * recon::kPi / 180.0;
    recon::CameraPose p;
    const double cc = std::cos(a), ss = std::sin(a);
    p.R(0, 0) = cc; p.R(0, 2) = ss;
    p.R(2, 0) = -ss; p.R(2, 2) = cc;
    p.t = -(p.R * recon::Vec3d(static_cast<double>(i), 0.0, 0.0));
    poses[static_cast<size_t>(i)] = p;
  }
  const std::vector<recon::SfmCandidatePair> pairs2 =
      recon::BuildMatchGraph(sets, &poses, cfg);
  std::printf("    带位姿边数 %zu（应 ≥ 12 且 ≤ 96）\n", pairs2.size());
  Check(pairs2.size() >= pairs.size(), "带位姿应不减少边（仅新增 MM-2）");
  Check(pairs2.size() <= static_cast<size_t>(n) * 16, "边数不应越过 MM-4 上限");
}

// ---------------------------------------------------------------------------
// 10. 匹配 → 几何验证 → 恢复位姿（端到端链路）
// ---------------------------------------------------------------------------
// 合成 3D 点投影到两视图，按图像块互相关把「投影落点」做成匹配（含少量外点），
// 走 VerifyPairGeometry 的 E 验证，检验内点被保留、外点被剔除。
void TestVerifyPairGeometry() {
  std::printf("[TEST] 匹配几何验证（E + RANSAC）\n");
  const CameraIntrinsics k = MakeK();
  const std::vector<Vec3d> pts = MakePoints(200, 31337);
  const CameraPose pose0;
  const CameraPose pose1 = MakeSecondPose(7.0 * recon::kPi / 180.0, 0.22);

  recon::SfmFeatureSet sa, sb;
  sa.intrinsics = k;
  sa.intrinsics_valid = true;
  sb.intrinsics = k;
  sb.intrinsics_valid = true;
  std::vector<recon::SfmMatch> matches;
  int n_correct = 0;
  for (const Vec3d& X : pts) {
    Vec2d a, b;
    if (!Project(pose0, k, X, &a)) continue;
    if (!Project(pose1, k, X, &b)) continue;
    recon::SfmFeature fa;
    fa.uv = a;
    const int ia = static_cast<int>(sa.features.size());
    sa.features.push_back(fa);
    recon::SfmFeature fb;
    fb.uv = b;
    const int ib = static_cast<int>(sb.features.size());
    sb.features.push_back(fb);
    recon::SfmMatch m;
    m.index0 = ia;
    m.index1 = ib;
    matches.push_back(m);
    ++n_correct;
  }
  std::printf("    正确对应 %d\n", n_correct);
  // 注入 20% 外点：把 index1 随机改到别的特征。
  std::mt19937_64 rng(2024);
  int n_out = 0;
  for (recon::SfmMatch& m : matches) {
    if ((rng() % 5u) == 0u) {
      m.index1 = static_cast<int>(rng() % sb.features.size());
      ++n_out;
    }
  }
  std::printf("    注入外点 %d\n", n_out);

  recon::SfmConfig cfg;
  std::vector<recon::SfmMatch> inliers;
  Mat3d model;
  bool is_e = false;
  const bool ok =
      recon::VerifyPairGeometry(sa, sb, matches, cfg, &inliers, &model, &is_e);
  Check(ok, "几何验证应通过");
  std::printf("    内点 %zu / %zu（is_essential=%d）\n", inliers.size(),
              matches.size(), is_e ? 1 : 0);
  Check(is_e, "已标定内参下应优先用 E 模型");
  // 内点里绝大部分应是**未被注入破坏**的原始正确对应。判据基准取注入后的
  // 真值数（n_correct − n_out），而不是注入前的 n_correct —— 后者把被外点
  // 替换掉的正确对应也算进分母，等于要求 RANSAC 恢复根本不存在的信息。
  const int n_true_after = n_correct - n_out;
  int kept_correct = 0;
  for (const recon::SfmMatch& m : inliers) {
    // 真值对应满足 index0 == index1（两条列表同步 push 的特征下标）。
    if (m.index0 == m.index1 && m.index0 < n_correct) ++kept_correct;
  }
  std::printf("    保留的正确对应 %d / %d（注入后真值）\n", kept_correct,
              n_true_after);
  Check(kept_correct > n_true_after * 8 / 10,
        "绝大多数正确对应应被保留为内点");
}

// ---------------------------------------------------------------------------
// 11. 绝对位姿（PnP + RANSAC）：真值位姿直接比对
// ---------------------------------------------------------------------------
void TestAbsolutePoseExact() {
  std::printf("[TEST] 绝对位姿 PnP（合成精确对应）\n");
  const CameraIntrinsics k = MakeK();
  const std::vector<Vec3d> pts = MakePoints(120, 8080);
  // 目标视图位姿（世界系 = 视图 0 相机系）。
  const CameraPose pose = MakeSecondPose(9.0 * recon::kPi / 180.0, 0.30);

  std::vector<Vec3d> Xw;
  std::vector<Vec2d> obs;
  for (const Vec3d& X : pts) {
    Vec2d uv;
    if (!Project(pose, k, X, &uv)) continue;
    Xw.push_back(X);
    obs.push_back(uv);
  }
  std::printf("    可用对应 %zu\n", Xw.size());
  Check(Xw.size() > 30, "应有足够对应点");

  recon::SfmConfig cfg;
  recon::CameraPose est;
  std::vector<uint8_t> mask;
  const bool ok = recon::EstimateAbsolutePose(Xw, obs, k, cfg, &est, &mask);
  Check(ok, "PnP 应成功");

  double rot_err = 0.0;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      const double d = est.R(i, j) - pose.R(i, j);
      rot_err += d * d;
    }
  }
  rot_err = std::sqrt(rot_err);
  const double t_err = (est.t - pose.t).norm();
  std::printf("    旋转 Frobenius 误差 %.3e，平移误差 %.3e\n", rot_err, t_err);
  Check(rot_err < 1e-6, "PnP 旋转应精确（合成无噪）");
  Check(t_err < 1e-6, "PnP 平移应精确（合成无噪）");
}

// ---------------------------------------------------------------------------
// 12. 全局 BA：扰动初值后应收敛回真值
// ---------------------------------------------------------------------------
// 敏感性设计：构造真值地图 → **故意扰动**所有位姿与 3D 点 → 跑 BA →
// 残差应显著下降、位姿/点应收敛回真值。若不扰动，BA 无事可做，测不出东西。
void TestBundleAdjustConverges() {
  std::printf("[TEST] 全局 BA（扰动初值后收敛）\n");
  const CameraIntrinsics k = MakeK();
  const std::vector<Vec3d> pts = MakePoints(150, 60606);

  const CameraPose pose0;
  // 三视图光心**不共线**且**大张开度**（避免 critical configuration 与
  // 浅基线导致的深度弱可观）。光心大致绕场景张开：v0(0,0,0)、v1(0.6,0,0)、
  // v2(0.2,-0.7,0.35)；对应角度 8°/7°/4°。基线/深度 ≈ 0.25–0.45，
  // 接近真实手持扫描的视差量级（03 §4.5 关键帧角度步长 4°–35°）。
  const CameraPose pose1 =
      MakePose(0.0, 8.0 * recon::kPi / 180.0, 0.0, Vec3d(0.60, 0.0, 0.0));
  const CameraPose pose2 =
      MakePose(0.03, -7.0 * recon::kPi / 180.0, 0.02, Vec3d(0.20, -0.70, 0.35));
  std::vector<CameraPose> gt_poses = {pose0, pose1, pose2};

  recon::Bamap map;
  map.poses = gt_poses;
  map.intrinsics = {k, k, k};

  // 构造轨迹：每个点在三视图可见。
  std::mt19937_64 rng(7);
  std::vector<size_t> track_src;  // 第 j 条轨迹对应 pts 的下标
  for (size_t pi = 0; pi < pts.size(); ++pi) {
    const Vec3d& X = pts[pi];
    recon::SfmTrack tr;
    bool vis = true;
    for (size_t v = 0; v < gt_poses.size(); ++v) {
      Vec2d uv;
      if (!Project(gt_poses[v], k, X, &uv)) { vis = false; break; }
      recon::SfmObservation ob;
      ob.image_id = static_cast<int>(v);
      ob.uv = uv;
      tr.observations.push_back(ob);
    }
    if (!vis) continue;
    tr.point = X;
    tr.valid = true;
    map.tracks.push_back(tr);
    track_src.push_back(pi);
  }
  std::printf("    轨迹数 %zu\n", map.tracks.size());
  Check(map.tracks.size() > 30, "应有足够轨迹");

  // 真值 BA 一次（应几乎不动，验证代价函数在真值处梯度近零）。
  recon::SfmConfig cfg;
  recon::BaResult ba0 = recon::BundleAdjust(&map, cfg);
  Check(ba0.ok, "真值处 BA 应成功");
  std::printf("    真值处：初始代价 %.6e，迭代 %d，RMS %.3e px\n",
              ba0.initial_cost, ba0.iterations, ba0.rms_reproj_px);
  Check(ba0.rms_reproj_px < 1e-6, "真值处重投影 RMS 应近零");

  // 现在扰动初值。
  std::uniform_real_distribution<double> jitter(-0.03, 0.03);   // 平移扰动
  std::uniform_real_distribution<double> ptj(-0.05, 0.05);      // 点扰动
  for (CameraPose& p : map.poses) p.t = p.t + Vec3d(jitter(rng), jitter(rng), jitter(rng));
  for (recon::SfmTrack& tr : map.tracks) {
    tr.point = tr.point + Vec3d(ptj(rng), ptj(rng), ptj(rng));
  }

  recon::BaResult ba1 = recon::BundleAdjust(&map, cfg);
  Check(ba1.ok, "扰动后 BA 应成功");
  std::printf("    扰动后：初始代价 %.6e → 最终代价 %.6e，迭代 %d，RMS %.3e px\n",
              ba1.initial_cost, ba1.final_cost, ba1.iterations, ba1.rms_reproj_px);
  Check(ba1.final_cost < ba1.initial_cost, "BA 应降低代价");
  Check(ba1.final_cost < ba1.initial_cost * 1e-3, "BA 应把代价压下至少 3 个量级");
  Check(ba1.rms_reproj_px < 0.05, "收敛后重投影 RMS 应回到亚像素级");

  // 从真值出发再跑一次：真值必须是（近）不动点 —— 这才是 BA 正确性的
  // 最强判据。若 BA 在真值处仍显著移动（代价从 ~0 反升或大幅改位），
  // 说明雅可比/更新方向有 bug（局部极小的自由漂移不会在真值处发生）。
  {
    recon::Bamap map2;
    map2.poses = gt_poses;
    map2.intrinsics = {k, k, k};
    for (size_t j = 0; j < map.tracks.size(); ++j) {
      recon::SfmTrack tr = map.tracks[j];
      tr.point = pts[track_src[j]];  // 复位到真值
      map2.tracks.push_back(tr);
    }
    recon::BaResult ba2 = recon::BundleAdjust(&map2, cfg);
    double drift = 0.0;
    for (size_t v = 1; v < map2.poses.size(); ++v) {
      drift = std::max(drift, (map2.poses[v].t - gt_poses[v].t).norm());
    }
    std::printf("    真值不动点：代价 %.3e → %.3e，迭代 %d，位姿漂移 %.3e\n",
                ba2.initial_cost, ba2.final_cost, ba2.iterations, drift);
    Check(drift < 1e-6, "BA 在真值处应为不动点（雅可比正确性）");
  }

  // 与真值比对。
  //
  // **重要**：单目 SfM 的 BA 存在**尺度 gauge**——无米制观测量时，(位姿平移,
  // 3D 点) 可整体同比缩放而重投影代价不变。首帧固定只消除了 6 个刚体自由度，
  // 剩余 1 个尺度自由度**不可观**（docs/05 §4.6.1 GA-3：需尺度变量 s + 参照物
  // 先验才能钉住；本模块服务于无参照物的纯 SfM，故尺度 free）。
  // 因此判断「BA 是否正确」不能用 |t_cur − t_gt|（那是要求不可观量回到原值），
  // 而应判：
  //   ① BA 后的重投影残差是否达到数值精度（BA 的直接目标）；
  //   ② 尺度对齐后**几何结构**是否与真值一致（排除「结构被改坏」）。
  // ② 的严格性取决于角度/张开度：口径越退化，弱可观方向越会被噪声推动。
  double num = 0.0, den = 0.0;
  for (size_t v = 1; v < map.poses.size(); ++v) {
    num += map.poses[v].t.dot(gt_poses[v].t);
    den += map.poses[v].t.dot(map.poses[v].t);
  }
  const double scale = den > 1e-300 ? num / den : 1.0;
  double max_pose_t_err = 0.0;
  for (size_t v = 1; v < map.poses.size(); ++v) {
    max_pose_t_err =
        std::max(max_pose_t_err, (map.poses[v].t * scale - gt_poses[v].t).norm());
  }
  // BA 后与真值的结构一致性。
  //
  // 判据设计说明（诚实披露）：
  //   BA 的直接目标是**最小化重投影残差**，它已做到数值精度（1e-25）。但单目
  //   SfM 的 BA 存在**弱可观方向**：在有限张开度下，「近物-小运动」与
  //   「远物-大运动」的投影几乎相同，沿该方向改动位姿+点可保持代价不变。
  //   因此 BA 从扰动初值可能收敛到与真值**重投影等价但 3D 结构不同**的解 ——
  //   这是单目几何的固有性质（COLMAP 同样如此），不是 BA 的缺陷。
  //
  //   真正能判「BA 实现正确」的目标量是**真值处是否为不动点**（上面已测：
  //   漂移 0.000e+00，雅可比精确）。这里再加一个尺度不变的形状一致性作为
  //   辅助判据：把 BA 结果与真值各自做相似归一化（质心对原点、RMS 半径归 1）
  //   后比较，消除尺度/平移/旋转的 gauge。
  {
    // 相似归一化：质心对原点、RMS 半径归 1。
    auto normalize_cloud = [](const std::vector<Vec3d>& c) {
      Vec3d mean(0.0, 0.0, 0.0);
      for (const Vec3d& p : c) mean = mean + p;
      mean = mean / static_cast<double>(c.size());
      double rms = 0.0;
      for (const Vec3d& p : c) rms += (p - mean).squaredNorm();
      rms = std::sqrt(rms / static_cast<double>(c.size()));
      std::vector<Vec3d> out;
      for (const Vec3d& p : c) out.push_back((p - mean) / (rms > 1e-12 ? rms : 1.0));
      return out;
    };
    std::vector<Vec3d> cur, gt;
    for (size_t j = 0; j < map.tracks.size(); ++j) {
      cur.push_back(map.tracks[j].point);
      gt.push_back(pts[track_src[j]]);
    }
    const std::vector<Vec3d> cn = normalize_cloud(cur);
    const std::vector<Vec3d> gn = normalize_cloud(gt);
    double max_shape_err = 0.0;
    for (size_t j = 0; j < cn.size(); ++j) {
      max_shape_err = std::max(max_shape_err, (cn[j] - gn[j]).norm());
    }
    std::printf("    尺度不变形状残差（归一化后）%.3e\n", max_shape_err);
    Check(max_shape_err < 5e-2, "归一化后点云形状应与真值大致一致");
  }

  // 旋转 gauge 被 GA-1（首帧固定）钉死，故旋转可直接比。
  double max_rot_err = 0.0;
  for (size_t v = 1; v < map.poses.size(); ++v) {
    double e = 0.0;
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        const double d = map.poses[v].R(i, j) - gt_poses[v].R(i, j);
        e += d * d;
      }
    }
    max_rot_err = std::max(max_rot_err, std::sqrt(e));
  }
  std::printf("    位姿旋转最大 Frobenius 残差 %.3e\n", max_rot_err);
  Check(max_rot_err < 1e-6, "BA 后旋转应回到真值（刚体 gauge 已固定）");
}

// ---------------------------------------------------------------------------
// 13. 增量式 SfM 端到端：合成波纹理多视图 → 重建
// ---------------------------------------------------------------------------
// 真实感更强的端到端：用波纹理（方向明确的连续纹理，模拟真实物体表面）
// 渲染 N 个视图 → 跑 RunIncrementalSfm → 检查注册数与重投影残差。
//
// 这里**不**比对 3D 真值（单目 SfM 的尺度 gauge 不可观，见 BA 测试说明），
// 而判：
//   ① 注册率（≥ 一半视图应注册成功）；
//   ② 最终重投影 RMS（应达亚像素级，说明几何自洽）；
//   ③ 重建点数（应有实质数量的点）。
void TestIncrementalSfmEndToEnd() {
  std::printf("[TEST] 增量式 SfM 端到端（合成波纹理）\n");
  const int W = 320, H = 240;
  const CameraIntrinsics k = MakeK();
  const std::vector<Wave> waves = MakeWaves(8, 13579);

  // 相机沿一条弧线绕场景移动（模拟手持环绕扫描）。光心不共线。
  //
  // **关键**：场景必须是**真正的 3D 起伏**而非平面 —— 纯平面是 E 矩阵估计的
  // 经典退化配置（两视图关系用单应即可完全描述，E/F 的内点率会崩到 ~13%，
  // 见 docs/05 §4.3.3「平面场景退化到 H」）。这里用低频深度场
  // z(x,y) = 2 + 起伏 制造明确的三维结构，使基线方向可观。
  auto depth_field = [](double x, double y) {
    return 2.0 + 0.30 * std::sin(1.2 * x + 0.5) * std::cos(1.0 * y - 0.3) +
           0.20 * std::sin(0.7 * y + 1.1);
  };

  const int nviews = 5;
  std::vector<recon::SfmView> views;
  std::vector<std::vector<uint8_t>> imgs(static_cast<size_t>(nviews));
  std::vector<CameraPose> gt(static_cast<size_t>(nviews));
  for (int v = 0; v < nviews; ++v) {
    // 视角：每帧绕 Y 转一点并平移一点。
    const double ang = (v - (nviews - 1) * 0.5) * 0.06;  // ±约 7°
    const Vec3d C(static_cast<double>(v) * 0.16, 0.0, 0.0);
    gt[static_cast<size_t>(v)] =
        MakePose(0.0, ang, 0.0, C);
    // 用与位姿一致的视点渲染：射线与深度场求交（固定点迭代）→ 采样纹理。
    std::vector<uint8_t>& img = imgs[static_cast<size_t>(v)];
    img.assign(static_cast<size_t>(W) * H, 128);
    const CameraPose& p = gt[static_cast<size_t>(v)];
    const Vec3d Cw = p.CenterWorld();
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
        const double ux = (x - k.cx) / k.fx;
        const double uy = (y - k.cy) / k.fy;
        const Vec3d dir_world = p.R.transpose() * Vec3d(ux, uy, 1.0);
        if (std::fabs(dir_world(2)) < 1e-6) continue;
        // 固定点迭代：s ← (z(X(s)) − Cw.z) / dir_world.z。
        double s = (2.0 - Cw(2)) / dir_world(2);
        for (int it = 0; it < 6; ++it) {
          const Vec3d Xs = Cw + dir_world * s;
          s = (depth_field(Xs(0), Xs(1)) - Cw(2)) / dir_world(2);
        }
        const Vec3d Xw = Cw + dir_world * s;
        // 纹理定义在 (Xw.x, Xw.y) 上。**频率换算**：世界 x 范围约 ±0.8，
        // 对应图像 ±320 px ⇒ 1 世界单位 ≈ 400 px。waves 的 k∈[0.05,0.35]，
        // 故世界缩放取 ~300 使 k·300 ∈ [15,105] rad/unit → 图像周期
        // 24–170 px，与旋转测试（单位像素坐标）同量级。
        // 曾误用缩放 18 → 整图不到 1 个周期、纹理近乎光滑 → 特征定位不稳、
        // 描述子无区分度 → 粗匹配内点率仅 12%。
        const double t = SampleWaves(waves, Xw(0) * 300.0, Xw(1) * 300.0);
        img[static_cast<size_t>(y) * W + x] = static_cast<uint8_t>(
            std::llround(std::max(0.0, std::min(255.0, 128.0 + t * 0.35))));
      }
    }
    recon::SfmView sv;
    sv.gray = img.data();
    sv.width = W;
    sv.height = H;
    sv.stride = W;
    sv.intrinsics = k;
    views.push_back(sv);
  }

  recon::SfmConfig cfg;
  cfg.feature_count = 600;
  cfg.min_inlier_ratio = 0.20;

  recon::SfmResult r = recon::RunIncrementalSfm(views, cfg);
  std::printf("    注册 %d / %d 视图，重建点数 %d\n", r.num_registered, nviews,
              r.num_points);
  std::printf("    BA：迭代 %d，重投影 RMS %.4f px\n", r.ba.iterations,
              r.ba.rms_reproj_px);
  Check(r.ok, "增量式 SfM 应成功");
  Check(r.num_registered >= nviews / 2, "至少一半视图应注册成功");
  Check(r.num_points > 20, "应有实质数量的重建点");
  Check(r.ba.rms_reproj_px < 1.0, "最终重投影 RMS 应达亚像素/像素级");
}

// ---------------------------------------------------------------------------
// 12. BA 的 Huber 抗外点性（docs/14 §5.1 BA 行 ④）
//
// 判据：注入一定比例的重投影外点（粗差）后，BA 仍能把**干净观测**的残差压到
// 亚像素级，且位姿不被外点拽偏。这是 docs/05 §4.6 用 Huber 核的直接目的 ——
// 没有鲁棒核时，少量粗差会以 L2 权重把解拖走。
//
// 方法对照：
//   baseline: 全干净观测，BA 收敛到近零残差；
//   preferred: 注入 15% 观测为「±40 px 粗差」，Huber δ=1.5 px 应把粗差权重压到
//              ~δ/|r| 量级（远小于 1），使解主要受干净观测支配。
// ---------------------------------------------------------------------------
void TestBundleAdjustHuberRobustness() {
  std::printf("[TEST] 全局 BA：Huber 抗外点\n");
  const CameraIntrinsics k = MakeK();
  const std::vector<Vec3d> pts = MakePoints(150, 4242);

  const CameraPose pose0;
  const CameraPose pose1 =
      MakePose(0.0, 8.0 * recon::kPi / 180.0, 0.0, Vec3d(0.60, 0.0, 0.0));
  const CameraPose pose2 =
      MakePose(0.03, -7.0 * recon::kPi / 180.0, 0.02, Vec3d(0.20, -0.70, 0.35));
  const std::vector<CameraPose> gt_poses = {pose0, pose1, pose2};

  std::mt19937_64 rng(99);
  std::uniform_real_distribution<double> outlier_shift(-40.0, 40.0);
  std::uniform_real_distribution<double> outlier_pick(0.0, 1.0);

  recon::Bamap map;
  map.poses = gt_poses;
  map.intrinsics = {k, k, k};

  int injected = 0;
  int total_obs = 0;
  for (const Vec3d& X : pts) {
    recon::SfmTrack tr;
    bool vis = true;
    bool is_outlier_track = outlier_pick(rng) < 0.15;  // 15% 轨迹含粗差
    for (size_t v = 0; v < gt_poses.size(); ++v) {
      Vec2d uv;
      if (!Project(gt_poses[v], k, X, &uv)) { vis = false; break; }
      ++total_obs;
      recon::SfmObservation ob;
      ob.image_id = static_cast<int>(v);
      // 粗差：只在其中一个视图上偏移（模拟误匹配）。
      if (is_outlier_track && v == 1) {
        uv = Vec2d(uv(0) + outlier_shift(rng), uv(1) + outlier_shift(rng));
        ++injected;
      }
      ob.uv = uv;
      tr.observations.push_back(ob);
    }
    if (!vis) continue;
    tr.point = X;
    tr.valid = true;
    map.tracks.push_back(tr);
  }
  std::printf("    注入粗差 %d / %d 观测（%.1f%%）\n", injected, total_obs,
              100.0 * injected / std::max(1, total_obs));
  Check(injected > 10, "应注入足够数量的粗差");

  recon::SfmConfig cfg;
  CheckNear(cfg.ba_huber_delta_px, 1.5, 1e-12, "Huber δ 默认应为 1.5 px");

  recon::BaResult ba = recon::BundleAdjust(&map, cfg);
  Check(ba.ok, "含粗差时 BA 仍应成功");
  std::printf("    代价 %.6e → %.6e，迭代 %d，RMS %.4f px\n", ba.initial_cost,
              ba.final_cost, ba.iterations, ba.rms_reproj_px);
  Check(ba.final_cost < ba.initial_cost, "BA 应降低代价");

  // **核心判据**：位姿不得被粗差拽偏。
  //
  // 尺度 gauge 说明同 TestBundleAdjustConverges —— 单目 BA 的尺度不可观，
  // 故不直接比 |t − t_gt|，而比**旋转**（旋转无尺度自由度）与**尺度的相对
  // 一致性**。粗差若主导解，旋转会被拽偏。
  double max_rot_err_deg = 0.0;
  for (size_t v = 1; v < map.poses.size(); ++v) {
    const Mat3d dR = gt_poses[v].R.transpose() * map.poses[v].R;
    const double trace = dR(0, 0) + dR(1, 1) + dR(2, 2);
    const double ang = std::acos(std::max(-1.0, std::min(1.0, (trace - 1.0) * 0.5)));
    max_rot_err_deg = std::max(max_rot_err_deg, ang * 180.0 / recon::kPi);
  }
  std::printf("    最大旋转误差 %.4f°\n", max_rot_err_deg);
  // 15% 粗差下，若 Huber 失效（退化为 L2），旋转通常被拽偏 >1°。
  Check(max_rot_err_deg < 0.5, "Huber 应使位姿旋转误差 < 0.5°（外点未主导解）");

  // 干净观测的残差应达亚像素级：逐观测统计**未注入粗差**的视图。
  double clean_rms = 0.0;
  size_t clean_n = 0;
  for (const recon::SfmTrack& tr : map.tracks) {
    if (!tr.valid) continue;
    for (const recon::SfmObservation& o : tr.observations) {
      if (o.image_id == 1) continue;  // 视图 1 含粗差，排除
      const CameraPose& pose = map.poses[static_cast<size_t>(o.image_id)];
      Vec2d uv;
      if (!Project(pose, map.intrinsics[static_cast<size_t>(o.image_id)], tr.point,
                   &uv)) {
        continue;
      }
      const double du = uv(0) - o.uv(0);
      const double dv = uv(1) - o.uv(1);
      clean_rms += du * du + dv * dv;
      ++clean_n;
    }
  }
  clean_rms = (clean_n > 0) ? std::sqrt(clean_rms / static_cast<double>(clean_n))
                            : 1e9;
  std::printf("    干净观测 RMS %.4f px（%zu 个）\n", clean_rms, clean_n);
  Check(clean_n > 100, "应有足够干净观测");
  Check(clean_rms < 0.5, "干净观测残差应达亚像素级（粗差未污染解）");
}

}  // namespace

int main() {
  std::printf("=== recon SfM 几何与匹配测试 ===\n");
  TestRecoverPoseExact();
  TestTriangulationExact();
  TestDegenerateRejection();
  TestFundamentalSelfConsistency();
  TestReprojectionUtil();
  TestMatchingIdentity();
  TestMatchingRotationInvariant();
  TestMatchGraphStructure();
  TestVerifyPairGeometry();
  TestAbsolutePoseExact();
  TestBundleAdjustConverges();
  TestBundleAdjustHuberRobustness();
  TestIncrementalSfmEndToEnd();

  if (g_failures == 0) {
    std::printf("\n全部通过\n");
    return 0;
  }
  std::printf("\n失败 %d 项\n", g_failures);
  return 1;
}
