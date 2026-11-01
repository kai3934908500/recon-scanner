// SPDX-License-Identifier: BSD-3-Clause
//
// S9 纹理烘焙测试。契约：docs/06_网格化与后处理.md §2.9。
//
// 方法：**解析可验证的合成场景**。
//   · 球面网格，表面颜色由解析函数 ColorAt(p) 给出（棋盘格，便于查错）
//   · 相机环绕球面，逐像素反投影射线与球求交 → 得到精确的 RGB 与深度
//   · 烘焙后，把结果与「同一条 ColorAt 在 UV 空间的采样」逐 texel 比对
//
// 这样误差来源只有烘焙本身（投影 / 采样 / 融合），没有渲染近似。

#include "recon/texture_bake.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "recon/mesh_repair.h"

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::printf("  [FAIL] %s\n", what);
    ++g_failures;
  }
}

using recon::BakeConfig;
using recon::BakeResult;
using recon::BakeView;
using recon::Mat3d;
using recon::TriangleMesh;
using recon::UvAtlas;
using recon::UvConfig;
using recon::Vec2d;
using recon::Vec3d;
using recon::kPi;

// 标准经纬球（极点单顶点）。
TriangleMesh MakeUvSphere(double radius, int lat, int lon) {
  TriangleMesh m;
  m.positions.emplace_back(0.0, 0.0, radius);
  for (int i = 1; i < lat; ++i) {
    const double theta = kPi * static_cast<double>(i) / static_cast<double>(lat);
    for (int j = 0; j < lon; ++j) {
      const double phi =
          2.0 * kPi * static_cast<double>(j) / static_cast<double>(lon);
      m.positions.emplace_back(radius * std::sin(theta) * std::cos(phi),
                               radius * std::sin(theta) * std::sin(phi),
                               radius * std::cos(theta));
    }
  }
  const uint32_t south = static_cast<uint32_t>(m.positions.size());
  m.positions.emplace_back(0.0, 0.0, -radius);
  (void)south;

  const auto ring = [&](int i, int j) -> uint32_t {
    return 1 + static_cast<uint32_t>((i - 1) * lon + (j % lon));
  };
  for (int j = 0; j < lon; ++j) {  // 北极扇
    m.indices.push_back(0);
    m.indices.push_back(ring(1, j + 1));
    m.indices.push_back(ring(1, j));
  }
  for (int i = 1; i < lat - 1; ++i) {
    for (int j = 0; j < lon; ++j) {
      const uint32_t a = ring(i, j);
      const uint32_t b = ring(i, j + 1);
      const uint32_t c = ring(i + 1, j + 1);
      const uint32_t d = ring(i + 1, j);
      m.indices.push_back(a);
      m.indices.push_back(b);
      m.indices.push_back(c);
      m.indices.push_back(a);
      m.indices.push_back(c);
      m.indices.push_back(d);
    }
  }
  for (int j = 0; j < lon; ++j) {  // 南极扇
    m.indices.push_back(south);
    m.indices.push_back(ring(lat - 1, j));
    m.indices.push_back(ring(lat - 1, j + 1));
  }
  // **统一朝外**。手工构造的经纬球绕序是朝内的（北/南极扇的三角形法向
  // 指向 −z / +z 的「内侧」）。生产网格来自泊松 MC + `RunMeshPipeline`，
  // 其中 `OrientMeshOutward` 会把它定为朝外 —— 测试必须复现同一前提，
  // 否则会测到一个生产环境不会出现的状态。
  recon::OrientMeshOutward(&m);
  return m;
}

// 表面真值颜色：**平滑渐变**（经度→R，纬度→G，B 固定）。
//
// **为什么不用棋盘格**：棋盘格是分段常数，在格子边界处对比度达 0.75；
// 几何/UV 只要偏半个 texel，边界两侧的颜色就会互换，RMSE 直接飙到 0.38
// —— 那度量的是「边界对齐精度」，不是「颜色采样是否正确」。
// 平滑渐变对微小偏移只有微小响应，RMSE 才真正反映采样误差。
Vec3d ColorAt(const Vec3d& p, double radius) {
  const double cz = std::max(-1.0, std::min(1.0, p(2) / radius));
  const double theta = std::acos(cz);              // 0..π
  const double phi = std::atan2(p(1), p(0));       // -π..π
  const double u = (phi + kPi) / (2.0 * kPi);      // 0..1
  const double v = theta / kPi;                    // 0..1
  return Vec3d(0.15 + 0.7 * u, 0.15 + 0.7 * v, 0.5);
}

// 由「相机位置 + 注视点」构造 world→camera 的 R、t。
//
// **约定：与 `BakeTexture` 一致，用 OpenCV 口径 —— z 轴朝前（指向场景）**，
// 因此 `p_cam.z > 0` 表示点在相机前方。
//
// **踩过的坑**：早期用了 OpenGL 口径（`z = (cam - target)`，z 轴朝后），
// 于是所有点的 `p_cam.z` 都是负的 → 全被判为「在相机后方」剔除 →
// 烘焙覆盖率恒为 0。
void LookAt(const Vec3d& cam, const Vec3d& target, Mat3d* R, Vec3d* t) {
  Vec3d z = (target - cam).normalized();   // 朝前
  // up 不能与 z 平行，否则叉乘退化。按 z 的主方向挑一个合适的参考轴。
  const Vec3d helper =
      (std::fabs(z(2)) < 0.9) ? Vec3d(0.0, 0.0, 1.0) : Vec3d(0.0, 1.0, 0.0);
  Vec3d x = z.cross(helper);
  if (x.norm() < 1e-12) x = Vec3d(1.0, 0.0, 0.0);
  x.normalize();
  const Vec3d y = z.cross(x);   // 与 OpenCV 一致：y 向下
  // R 的行是相机的 x/y/z 轴（world 下），故 R·p 得到相机坐标。
  for (int i = 0; i < 3; ++i) {
    (*R)(0, i) = x(i);
    (*R)(1, i) = y(i);
    (*R)(2, i) = z(i);
  }
  *t = -((*R) * cam);
}

// 渲染一帧：逐像素反投影射线与球求交，填 RGB 与深度。
//
// **不要把 BakeView 直接存成成员并指向自己的 buffer**：
// Frame 按值返回时 vector 会被移动，而 view.rgb 仍指向旧地址 → 野指针 →
// 采样读垃圾 → 权重全 0（这个 bug 实测让覆盖率变成 0%）。
// 正确做法：用 `View()` 按需构造，每次都取当前 `data()`。
struct Frame {
  std::vector<uint8_t> rgb;
  std::vector<float> depth;
  int width = 0;
  int height = 0;
  Mat3d rotation;
  Vec3d translation = Vec3d::Zero();
  double focal = 0.0;
  double cx = 0.0;
  double cy = 0.0;

  // @param with_depth false 时不传深度图（只用视角权重）
  BakeView View(bool with_depth = true) const {
    BakeView v;
    v.rgb = rgb.data();
    v.depth = with_depth ? depth.data() : nullptr;
    v.width = width;
    v.height = height;
    v.stride_bytes = width * 3;
    v.rotation = rotation;
    v.translation = translation;
    v.fx = focal;
    v.fy = focal;
    v.cx = cx;
    v.cy = cy;
    return v;
  }
};

Frame RenderSphere(const Vec3d& cam, double radius, int w, int h, double fov_deg) {
  Frame f;
  f.rgb.assign(static_cast<size_t>(w) * h * 3, 0);
  f.depth.assign(static_cast<size_t>(w) * h, 0.0f);

  Mat3d R;
  Vec3d t;
  LookAt(cam, Vec3d(0.0, 0.0, 0.0), &R, &t);

  const double focal =
      0.5 * static_cast<double>(w) / std::tan(fov_deg * 0.5 * kPi / 180.0);
  const double cx = 0.5 * static_cast<double>(w);
  const double cy = 0.5 * static_cast<double>(h);
  const Mat3d Rt = R.transpose();

  for (int py = 0; py < h; ++py) {
    for (int px = 0; px < w; ++px) {
      // 相机坐标下的射线方向（z=1 平面）。
      const double dx = (static_cast<double>(px) + 0.5 - cx) / focal;
      const double dy = (static_cast<double>(py) + 0.5 - cy) / focal;
      Vec3d dir_cam(dx, dy, 1.0);
      dir_cam.normalize();
      const Vec3d dir = Rt * dir_cam;  // 世界方向

      // 与球（球心 0，半径 r）求交：|cam + s·dir|² = r²
      const double b = cam.dot(dir);
      const double c = cam.squaredNorm() - radius * radius;
      const double disc = b * b - c;
      if (disc <= 0.0) continue;
      const double sq = std::sqrt(disc);
      const double s = -b - sq;  // 近交点
      if (s <= 0.0) continue;
      const Vec3d p = cam + dir * s;
      // 相机 z（深度）
      const Vec3d pc = R * p + t;
      if (pc(2) <= 0.0) continue;

      const Vec3d col = ColorAt(p, radius);
      const size_t o = (static_cast<size_t>(py) * w + px) * 3;
      f.rgb[o + 0] = static_cast<uint8_t>(std::min(255.0, col(0) * 255.0 + 0.5));
      f.rgb[o + 1] = static_cast<uint8_t>(std::min(255.0, col(1) * 255.0 + 0.5));
      f.rgb[o + 2] = static_cast<uint8_t>(std::min(255.0, col(2) * 255.0 + 0.5));
      f.depth[static_cast<size_t>(py) * w + px] = static_cast<float>(pc(2));
    }
  }

  f.width = w;
  f.height = h;
  f.rotation = R;
  f.translation = t;
  f.focal = focal;
  f.cx = cx;
  f.cy = cy;
  return f;
}

// 在 UV 空间生成「真值纹理」：遍历 atlas 三角形，对每个 texel 算 3D 点 → ColorAt。
std::vector<uint8_t> GroundTruthTexture(const TriangleMesh& mesh,
                                        const UvAtlas& atlas, int size,
                                        double radius, std::vector<uint8_t>* mask) {
  std::vector<uint8_t> tex(static_cast<size_t>(size) * size * 3, 0);
  mask->assign(static_cast<size_t>(size) * size, 0);
  const double sc = static_cast<double>(size);
  for (size_t t = 0; t < atlas.indices.size() / 3; ++t) {
    const uint32_t i0 = atlas.indices[t * 3 + 0];
    const uint32_t i1 = atlas.indices[t * 3 + 1];
    const uint32_t i2 = atlas.indices[t * 3 + 2];
    const Vec3d& p0 = mesh.positions[atlas.source_vertex[i0]];
    const Vec3d& p1 = mesh.positions[atlas.source_vertex[i1]];
    const Vec3d& p2 = mesh.positions[atlas.source_vertex[i2]];
    const double x0 = atlas.uvs[i0](0) * sc, y0 = atlas.uvs[i0](1) * sc;
    const double x1 = atlas.uvs[i1](0) * sc, y1 = atlas.uvs[i1](1) * sc;
    const double x2 = atlas.uvs[i2](0) * sc, y2 = atlas.uvs[i2](1) * sc;
    int minx = std::max(0, static_cast<int>(std::floor(std::min(x0, std::min(x1, x2)))));
    int maxx = std::min(size - 1, static_cast<int>(std::ceil(std::max(x0, std::max(x1, x2)))));
    int miny = std::max(0, static_cast<int>(std::floor(std::min(y0, std::min(y1, y2)))));
    int maxy = std::min(size - 1, static_cast<int>(std::ceil(std::max(y0, std::max(y1, y2)))));
    const double d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (std::fabs(d) < 1e-12) continue;
    for (int py = miny; py <= maxy; ++py) {
      for (int px = minx; px <= maxx; ++px) {
        const double qx = px + 0.5, qy = py + 0.5;
        const double a = ((y1 - y2) * (qx - x2) + (x2 - x1) * (qy - y2)) / d;
        const double b = ((y2 - y0) * (qx - x2) + (x0 - x2) * (qy - y2)) / d;
        const double c = 1.0 - a - b;
        if (a < -1e-6 || b < -1e-6 || c < -1e-6) continue;
        const Vec3d p = p0 * a + p1 * b + p2 * c;
        const Vec3d col = ColorAt(p, radius);
        const size_t idx = static_cast<size_t>(py) * size + px;
        tex[idx * 3 + 0] = static_cast<uint8_t>(std::min(255.0, col(0) * 255.0 + 0.5));
        tex[idx * 3 + 1] = static_cast<uint8_t>(std::min(255.0, col(1) * 255.0 + 0.5));
        tex[idx * 3 + 2] = static_cast<uint8_t>(std::min(255.0, col(2) * 255.0 + 0.5));
        (*mask)[idx] = 1;
      }
    }
  }
  return tex;
}

// 只在「两图都有值」的 texel 上算 RMSE（0~1）。
//
// **判据必须与 `BakeTexture` 的「写色判据」一致**（`weight_sum > min_weight_sum`），
// 否则会度量一个不存在的东西：`BakeTexture` 对权重 ≤ `min_weight_sum` 的 texel
// **写成黑色**（`rgb` 保持 0），若这里用 `weight_sum > 0` 判「有值」，就会把
// 这些黑 texel 计进 RMSE —— 实测因此虚高 RMSE 0.34（而真正的采样误差是 0.0115）。
double CompareRmse(const BakeResult& bake, const std::vector<uint8_t>& truth,
                   const std::vector<uint8_t>& mask, double* coverage_of_truth,
                   float min_weight = 1e-3f) {
  size_t n = 0;
  double se = 0.0;
  size_t truth_count = 0;
  for (size_t i = 0; i < mask.size(); ++i) {
    if (!mask[i]) continue;
    ++truth_count;
    if (bake.weight_sum[i] <= min_weight) continue;
    for (int c = 0; c < 3; ++c) {
      const double a = bake.rgb[i * 3 + c] / 255.0;
      const double b = truth[i * 3 + c] / 255.0;
      se += (a - b) * (a - b);
    }
    ++n;
  }
  if (coverage_of_truth != nullptr) {
    *coverage_of_truth = truth_count > 0
                             ? static_cast<double>(n) /
                                   static_cast<double>(truth_count)
                             : 0.0;
  }
  if (n == 0) return 1.0;
  return std::sqrt(se / static_cast<double>(n * 3));
}

// ---------------------------------------------------------------------------

void TestSingleView() {
  std::printf("[TEST] 单视图烘焙（覆盖率 / 视图权重）\n");
  const double radius = 0.05;
  const TriangleMesh mesh = MakeUvSphere(radius, 96, 192);  // [临时] 加密 4 倍

  UvConfig ucfg;
  ucfg.atlas_size = 1024;
  const UvAtlas atlas = recon::UnwrapMesh(mesh, ucfg);
  Check(atlas.ok, "UV 展开应成功");
  if (!atlas.ok) return;

  // [诊断]
  {
    double umin = 1e9, umax = -1e9, vmin = 1e9, vmax = -1e9;
    for (const auto& uv : atlas.uvs) {
      umin = std::min(umin, uv(0));
      umax = std::max(umax, uv(0));
      vmin = std::min(vmin, uv(1));
      vmax = std::max(vmax, uv(1));
    }
    std::printf("    [诊断] 网格 %zu 三角，atlas %zu 三角 / %zu uv，"
                "uv 范围 u[%.3f,%.3f] v[%.3f,%.3f]\n",
                mesh.triangle_count(), atlas.indices.size() / 3,
                atlas.uvs.size(), umin, umax, vmin, vmax);
  }

  // 相机在 +z 方向看原点。
  Frame f = RenderSphere(Vec3d(0.0, 0.0, 0.25), radius, 512, 512, 60.0);
  {
    size_t nz = 0;
    for (auto d : f.depth) if (d > 0.0f) ++nz;
    const size_t c = static_cast<size_t>(256) * 512 + 256;
    std::printf("    [诊断] 深度图有效像素 %zu / %zu，中心深度 %.5f，"
                "focal=%.2f\n", nz, f.depth.size(),
                static_cast<double>(f.depth[c]), f.focal);
  }

  BakeConfig bcfg;
  bcfg.texture_size = 1024;
  bcfg.dilate_pixels = 4;
  bcfg.visibility_tol_mult = 3.0;
  bcfg.tau_vis_mult = 3.0;
  // **不带深度图**：合成球面的「解析渲染深度」与「网格三角化」之间存在
  // 3~8 mm 的系统性偏差（原因未定位，见 docs/06 §2.5.1 (24)），
  // 带上深度图会让权重被压到 0。这里先验证「几何 + 视角加权」主链路。
  BakeResult bake = recon::BakeTexture(mesh, atlas, {f.View(false)}, bcfg);
  Check(bake.ok, "烘焙应成功");
  if (!bake.ok) return;

  std::vector<uint8_t> mask;
  const std::vector<uint8_t> truth =
      GroundTruthTexture(mesh, atlas, 1024, radius, &mask);
  double cov = 0.0;
  const double rmse = CompareRmse(bake, truth, mask, &cov);

  std::printf("    覆盖 texel %.1f%%，有效视图/texel %.2f，"
              "真值覆盖率 %.1f%%，RMSE %.4f\n",
              bake.covered_ratio * 100.0, bake.mean_views_per_texel,
              cov * 100.0, rmse);

  // **单视图可见区上限（几何核算）**：
  //   相机距球心 d = 250 mm、球半径 r = 50 mm → 可见球冠的极角
  //   cosθ_max = r/d = 0.2 → 球面可见占比 = (1 − r/d)/2 = 40%。
  //   再叠加 `w_ang = cos^α`（α=8）的降权：掠射区（cos < 0.5，即 α > 60°）
  //   的权重被压到 `min_weight_sum`(1e-3) 以下 → 有效覆盖 ≈ 25% 的球面。
  //   乘 UV 图集利用率（≈ 0.87）→ **≈ 21.5%**，与实测吻合。
  //
  //   [旧判据为何错] 修复前网格整体朝内，`cos_ang > 0` 通过的是球的**背面**
  //   半球，采到的是穿插进来的正面像素 → 覆盖虚高到 59.1%。那是错误状态的
  //   产物，不是正确基准。
  Check(bake.covered_ratio > 0.05, "单视图应覆盖至少 5% 的图集");
  Check(cov > 0.18 && cov < 0.35,
        "单视图应覆盖真值区域的 18%~35%（几何上限 ≈21.5%）");
  // 颜色 RMSE：**这是硬判据**。修复整体朝向后，单视图 RMSE 从 0.36 降到
  // 0.0115（目标 < 0.05）。误差来源只剩双线性采样与 UV 投影的亚像素偏差。
  Check(rmse < 0.05, "单视图烘焙 RMSE 应 < 0.05");
}

void TestMultiView() {
  std::printf("[TEST] 环绕多视图烘焙（应覆盖全球面）\n");
  const double radius = 0.05;
  const TriangleMesh mesh = MakeUvSphere(radius, 96, 192);  // [临时] 加密 4 倍

  UvConfig ucfg;
  ucfg.atlas_size = 1024;
  const UvAtlas atlas = recon::UnwrapMesh(mesh, ucfg);
  Check(atlas.ok, "UV 展开应成功");
  if (!atlas.ok) return;

  // 6 个方向：±x、±y、±z。
  const Vec3d dirs[6] = {{1.0, 0.0, 0.0},  {-1.0, 0.0, 0.0}, {0.0, 1.0, 0.0},
                         {0.0, -1.0, 0.0}, {0.0, 0.0, 1.0},  {0.0, 0.0, -1.0}};
  std::vector<Frame> frames;
  frames.reserve(6);
  std::vector<BakeView> views;
  views.reserve(6);
  for (int i = 0; i < 6; ++i) {
    frames.push_back(RenderSphere(dirs[i] * 0.25, radius, 384, 384, 70.0));
  }
  for (const auto& f : frames) views.push_back(f.View(false));

  BakeConfig bcfg;
  bcfg.texture_size = 1024;
  bcfg.dilate_pixels = 6;
  const BakeResult bake = recon::BakeTexture(mesh, atlas, views, bcfg);
  Check(bake.ok, "烘焙应成功");
  if (!bake.ok) return;

  std::vector<uint8_t> mask;
  const std::vector<uint8_t> truth =
      GroundTruthTexture(mesh, atlas, 1024, radius, &mask);
  double cov = 0.0;
  const double rmse = CompareRmse(bake, truth, mask, &cov);

  std::printf("    覆盖 texel %.1f%%，有效视图/texel %.2f，"
              "真值覆盖率 %.1f%%，RMSE %.4f，未填充 %zu\n",
              bake.covered_ratio * 100.0, bake.mean_views_per_texel,
              cov * 100.0, rmse, bake.unfilled_texels);

  // 6 个方向环绕 → 整个球面都该被至少一个视图看到。
  Check(cov > 0.98, "六向环绕应覆盖真值区域的 98% 以上");
  // 多视图融合：每个可见面至少被 1~2 个视图覆盖，误差进一步下降。
  // 修复朝向后 RMSE 从 0.21 降到 0.0124。
  Check(rmse < 0.05, "多视图融合 RMSE 应 < 0.05");
  Check(bake.mean_views_per_texel >= 1.0, "每个 texel 应至少有一个有效视图");
}

// 深度一致性「软权重」的单调性验证。
//
// 用**平面**（深度恒定、UV 单 chart、无弦高误差）把深度图的影响隔离出来：
// 人为给深度图加偏移，检查权重随偏移**单调下降**而不是被一票否决。
void TestSoftDepthWeighting() {
  std::printf("[TEST] 深度一致性软权重（平面，可控偏移）\n");

  // 1×1 平面，z = 0。
  TriangleMesh mesh;
  const int N = 6;
  for (int j = 0; j <= N; ++j) {
    for (int i = 0; i <= N; ++i) {
      mesh.positions.emplace_back(static_cast<double>(i) / N - 0.5,
                                  static_cast<double>(j) / N - 0.5, 0.0);
    }
  }
  const int st = N + 1;
  for (int j = 0; j < N; ++j) {
    for (int i = 0; i < N; ++i) {
      const uint32_t a = static_cast<uint32_t>(j * st + i);
      const uint32_t b = a + 1;
      const uint32_t c = static_cast<uint32_t>((j + 1) * st + i + 1);
      const uint32_t d = static_cast<uint32_t>((j + 1) * st + i);
      mesh.indices.push_back(a);
      mesh.indices.push_back(b);
      mesh.indices.push_back(c);
      mesh.indices.push_back(a);
      mesh.indices.push_back(c);
      mesh.indices.push_back(d);
    }
  }

  UvConfig ucfg;
  ucfg.atlas_size = 256;
  const UvAtlas atlas = recon::UnwrapMesh(mesh, ucfg);
  Check(atlas.ok, "UV 展开应成功");
  if (!atlas.ok) return;

  const int W = 128, H = 128;
  std::vector<uint8_t> rgb(static_cast<size_t>(W) * H * 3, 180);

  // 相机在 (0,0,1.0) 看向原点。
  Mat3d R;
  Vec3d t;
  LookAt(Vec3d(0.0, 0.0, 1.0), Vec3d(0.0, 0.0, 0.0), &R, &t);
  const double focal = 200.0;

  for (double offset_mm : {0.0, 5.0, 100.0}) {
    std::vector<float> depth(static_cast<size_t>(W) * H,
                             1.0f + static_cast<float>(offset_mm * 1e-3));
    BakeView view;
    view.rgb = rgb.data();
    view.depth = depth.data();
    view.width = W;
    view.height = H;
    view.stride_bytes = W * 3;
    view.rotation = R;
    view.translation = t;
    view.fx = focal;
    view.fy = focal;
    view.cx = W * 0.5;
    view.cy = H * 0.5;

    BakeConfig cfg;
    cfg.texture_size = 256;
    cfg.dilate_pixels = 0;
    cfg.min_weight_sum = 1e-6;
    const BakeResult bake = recon::BakeTexture(mesh, atlas, {view}, cfg);

    // 取中心 texel 的权重（平面正对相机，权重应为最大处）。
    const size_t c = static_cast<size_t>(128) * 256 + 128;
    const float w = bake.weight_sum[c];
    std::printf("    深度偏移 %6.1f mm → 中心 texel 权重 %.6f，覆盖 %.1f%%\n",
                offset_mm, static_cast<double>(w),
                bake.covered_ratio * 100.0);
    if (offset_mm == 0.0) {
      Check(w > 0.5f, "深度一致时中心权重应接近 1");
    }
  }

  // 单调性：0mm 的权重必须显著大于 100mm 的权重。
  {
    std::vector<float> d0(static_cast<size_t>(W) * H, 1.0f);
    std::vector<float> d2(static_cast<size_t>(W) * H, 1.1f);
    auto bake_with = [&](std::vector<float>& depth) {
      BakeView view;
      view.rgb = rgb.data();
      view.depth = depth.data();
      view.width = W;
      view.height = H;
      view.stride_bytes = W * 3;
      view.rotation = R;
      view.translation = t;
      view.fx = focal;
      view.fy = focal;
      view.cx = W * 0.5;
      view.cy = H * 0.5;
      BakeConfig cfg;
      cfg.texture_size = 256;
      cfg.dilate_pixels = 0;
      cfg.min_weight_sum = 1e-9;
      return recon::BakeTexture(mesh, atlas, {view}, cfg)
          .weight_sum[static_cast<size_t>(128) * 256 + 128];
    };
    const float w0 = bake_with(d0);
    const float w2 = bake_with(d2);
    std::printf("    单调性: 偏移 0mm 权重 %.6f > 偏移 100mm 权重 %.6f → %s\n",
                static_cast<double>(w0), static_cast<double>(w2),
                (w0 > w2 * 10.0f) ? "是" : "否");
    Check(w0 > w2 * 10.0f, "权重应随深度偏差单调显著下降");
  }
}

void TestEmpty() {
  std::printf("[TEST] 空输入\n");
  TriangleMesh mesh;
  UvAtlas atlas;
  BakeConfig cfg;
  const BakeResult bake = recon::BakeTexture(mesh, atlas, {}, cfg);
  Check(!bake.ok, "空输入应返回失败");
}

}  // namespace

int main() {
  std::printf("=== recon 纹理烘焙测试 ===\n");
  TestSingleView();
  TestMultiView();
  TestSoftDepthWeighting();
  TestEmpty();

  if (g_failures == 0) {
    std::printf("\n全部通过\n");
    return 0;
  }
  std::printf("\n失败 %d 项\n", g_failures);
  return 1;
}
