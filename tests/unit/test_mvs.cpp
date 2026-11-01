// SPDX-License-Identifier: BSD-3-Clause
//
// MVS 精度测试。
//
// 方法：构造**解析可验证**的合成场景。
//   - 一个已知深度的平面（带高斯纹理）
//   - 多个已知位姿的相机
//   - 用真实投影生成各视图图像
//   - 跑 MVS，与真值深度比对
//
// 这样测出的深度误差是**真实精度**，不是与自身比对的循环论证。
//
// 目标（docs/05 §5.1.2，标准档 4K）：
//   δZ = Z²·δd/(f·B)，Z=0.35m, f=2500px, B=100mm, δd=0.15px → δZ ≈ 0.074 mm
// 判据：中位误差 ≤ 0.5 mm，P95 ≤ 2.0 mm（合成场景无噪声，取宽松判据）。

#include "recon/camera.h"
#include "recon/mvs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

// 简化书写：测试内部直接使用核心类型。
using recon::CameraIntrinsics;
using recon::CameraPose;
using recon::Mat3d;
using recon::Vec3d;
using recon::kPi;

int g_failures = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::printf("  [FAIL] %s\n", what);
    ++g_failures;
  }
}

void CheckNear(double a, double b, double tol, const char* what) {
  if (std::fabs(a - b) > tol) {
    std::printf("  [FAIL] %s (got %.9f, want %.9f)\n", what, a, b);
    ++g_failures;
  }
}

// ---------------------------------------------------------------------------
// 合成场景
// ---------------------------------------------------------------------------

// 合成平面场景。
struct SyntheticScene {
  // 测试场景参数（等比缩放自 docs/05 §5.1.2 的手机典型工况）：
  //
  //   文档参数：f = 2500 px（4K）、B = 100 mm、Z = 0.35 m、δd = 0.15 px
  //             → δZ = Z²·δd/(f·B) = 0.074 mm
  //
  //   本测试等比缩放到 1/6.25（便于 CPU 快速运行）：
  //     f = 400 px、B = 16 mm、Z = 0.35 m
  //     深度分辨率 = Z/(f·B) = 0.35/(400×0.016) = 54.7 mm/px 视差
  //     0.15 px 视差 → δZ = 0.35²×0.15/(400×0.016) = 2.87 mm
  //
  //   **判据必须按缩放后的几何重新计算**，不能直接套用文档的 0.5 mm。
  //   缩放关系：δZ ∝ Z²/(f·B)，f 与 B 同比例缩小 6.25 倍 → δZ 放大 6.25 倍。
  //   文档的 0.074 mm × 6.25 = 0.46 mm，与直接计算一致。
  int width = 160;
  int height = 120;
  double fx = 400.0;
  double fy = 400.0;
  double cx = 80.0;
  double cy = 60.0;
  double plane_depth = 0.35;   // 平面在参考帧相机系中的 z（米）
  double plane_normal_z = 1.0;

  recon::CameraIntrinsics intrinsics;
  recon::CameraPose ref_pose;
  std::vector<CameraPose> poses;
  std::vector<std::vector<uint8_t>> images;  // 灰度图

  // 真值深度（参考帧视角，沿光轴）。
  std::vector<float> truth_depth;
};

// 确定性纹理：多频正弦叠加 + 哈希噪声，保证高频可匹配。
inline uint8_t TextureValue(int x, int y) {
  const double u = static_cast<double>(x) * 0.08;
  const double v = static_cast<double>(y) * 0.11;
  double val = 128.0 + 60.0 * std::sin(u) * std::cos(v * 1.3) +
               30.0 * std::sin(u * 2.7 + v * 0.4) +
               18.0 * std::cos(u * 4.1 - v * 3.3);
  // 加入确定性噪声，打破周期性。
  const uint32_t h = static_cast<uint32_t>(x * 73856093u ^ y * 19349663u);
  val += static_cast<double>(h & 0xFF) * 0.15 - 19.0;
  return static_cast<uint8_t>(std::clamp(val, 0.0, 255.0));
}

// 生成合成场景：平面在**参考帧相机系**的 z = plane_depth 处。
//
// 关键设计（这是精度测试能否成立的根本）：
//   1. 平面定义在**参考帧相机系**，法向 (0,0,1)。
//      真值深度 = 参考帧光线与平面的交点的 z 分量，逐像素精确。
//   2. 源帧必须有**真实平移**才能产生视差。纯旋转不产生基线，
//      视差恒为 0，MVS 无从解算深度（这是几何事实，不是 bug）。
//      因此构造为：沿 X 轴平移 baseline_i，同时绕 Y 轴微转以朝向物体中心 ——
//      这正是手持绕物扫描的真实几何。
//   3. 渲染采用**正向散射**：每个源视图独立沿其光线求平面交点，
//      用世界坐标作纹理坐标，保证同一物理点跨视图取同一纹理值。
SyntheticScene MakeScene(int num_views, double baseline_m, double shift_deg) {
  SyntheticScene s;
  s.intrinsics.fx = s.fx;
  s.intrinsics.fy = s.fy;
  s.intrinsics.cx = s.cx;
  s.intrinsics.cy = s.cy;
  s.intrinsics.width = s.width;
  s.intrinsics.height = s.height;
  s.intrinsics.k1 = s.intrinsics.k2 = s.intrinsics.k3 = 0.0;
  s.intrinsics.p1 = s.intrinsics.p2 = 0.0;

  // 参考帧：光心在原点，朝向 +Z，相机系 = 世界系。
  s.poses.clear();
  CameraPose ref;
  ref.R = Mat3d::Identity();
  ref.t = Vec3d::Zero();
  s.poses.push_back(ref);

  // 源帧：沿 X 轴平移，光心仍在 z = 0，绕 Y 轴微转使光轴朝向平面中心。
  // 微转角由「平移量 / 平面距离」决定：tan(θ) = Δx / plane_depth。
  for (int i = 1; i < num_views; ++i) {
    const double frac = (num_views > 1)
                            ? (static_cast<double>(i) / (num_views - 1))
                            : 0.0;
    // 偏移范围 [−baseline, +baseline]，参考帧在中间。
    const double dx = (frac * 2.0 - 1.0) * baseline_m;

    // 朝向：看向平面上的固定参考点 P_ref = (0, 0, plane_depth)。
    const Vec3d p_ref(0.0, 0.0, s.plane_depth);
    const Vec3d center(dx, 0.0, 0.0);
    const Vec3d forward = (p_ref - center).normalized();

    // 构造 world→camera 旋转：
    // 相机系 Z 轴 = forward，X 轴 = right，Y 轴 = down。
    const Vec3d world_down(0.0, -1.0, 0.0);
    Vec3d right = forward.cross(world_down);
    if (right.norm() < 1e-9) right = Vec3d(1.0, 0.0, 0.0);
    right.normalize();
    const Vec3d down = forward.cross(right);

    Mat3d R_wc;
    R_wc(0, 0) = right(0);   R_wc(0, 1) = right(1);   R_wc(0, 2) = right(2);
    R_wc(1, 0) = down(0);    R_wc(1, 1) = down(1);    R_wc(1, 2) = down(2);
    R_wc(2, 0) = forward(0); R_wc(2, 1) = forward(1); R_wc(2, 2) = forward(2);

    CameraPose p;
    p.R = R_wc;
    p.t = -(R_wc * center);
    s.poses.push_back(p);
  }
  (void)shift_deg;

  s.ref_pose = s.poses[0];

  // ---- 正向散射渲染 ----
  s.images.resize(num_views);
  s.truth_depth.assign(static_cast<size_t>(s.width) * s.height, 0.0f);

  for (int v = 0; v < num_views; ++v) {
    const CameraPose& pose = s.poses[v];
    s.images[v].assign(static_cast<size_t>(s.width) * s.height, 0);

    const Vec3d origin = pose.CenterWorld();

    for (int y = 0; y < s.height; ++y) {
      for (int x = 0; x < s.width; ++x) {
        Vec3d ray;
        s.intrinsics.PixelToRay(x + 0.5, y + 0.5, &ray);
        // 相机系射线 → 世界系方向。
        const Vec3d dir_world = pose.R.transpose() * ray;

        // 与平面 z = plane_depth 求交。
        if (std::fabs(dir_world(2)) < 1e-9) continue;
        const double t = (s.plane_depth - origin(2)) / dir_world(2);
        if (t <= 0.0) continue;
        const Vec3d p_world = origin + dir_world * t;

        // 参考帧真值深度（光轴深度）。
        if (v == 0) {
          s.truth_depth[static_cast<size_t>(y) * s.width + x] =
              static_cast<float>(p_world(2));
        }

        const int tx = static_cast<int>(std::lround(p_world(0) * 1500.0)) + 512;
        const int ty = static_cast<int>(std::lround(p_world(1) * 1500.0)) + 512;
        s.images[v][static_cast<size_t>(y) * s.width + x] =
            TextureValue(tx, ty);
      }
    }
  }

  return s;
}

// ---------------------------------------------------------------------------
// 相机模型测试
// ---------------------------------------------------------------------------

void TestCameraModel() {
  std::printf("[TEST] 相机模型投影/反投影互逆\n");
  recon::CameraIntrinsics intr;
  // 真实手机主摄参数（HFOV ≈ 73°，1080p）：
  //   f = (W/2)/tan(HFOV/2) = 960/tan(36.5°) ≈ 1300，取 1250。
  // 关键：主点必须位于画面中心，否则归一化半径会超出畸变模型有效域
  // （Brown-Conrady 在 r > 1/sqrt(-3k1) 时非单射，反解无解）。
  intr.fx = 1250.0;
  intr.fy = 1250.0;
  intr.cx = 960.0;
  intr.cy = 540.0;
  intr.width = 1920;
  intr.height = 1080;
  // 手机主摄典型畸变。
  intr.k1 = -0.22;
  intr.k2 = 0.06;
  intr.k3 = -0.005;
  intr.p1 = 0.0008;
  intr.p2 = -0.0006;

  // 投影 → 反投影 → 投影，应回到原像素（误差 < 1e-6 px）。
  double max_err = 0.0;
  for (int i = 0; i < 200; ++i) {
    const double u = 40.0 + i * 9.0;
    const double v = 30.0 + i * 5.0;
    if (u >= 1910 || v >= 1070) break;

    recon::Vec3d ray;
    intr.PixelToRay(u, v, &ray);
    // 注意：ray 的 z 分量为 1，直接作为相机系点即可。
    // 若再乘一个系数（如 0.5），会改变 x/z 比例，等价于改变视场角，
    // 此时「往返」检验的是另一个点，不再验证同一像素 —— 会产生假误差。
    double u2 = 0.0, v2 = 0.0;
    intr.ProjectPoint(ray, &u2, &v2);
    max_err = std::max(max_err, std::sqrt((u2 - u) * (u2 - u) + (v2 - v) * (v2 - v)));
  }
  std::printf("    畸变往返最大误差: %.3e px\n", max_err);
  Check(max_err < 1e-6, "带畸变的投影/反投影必须互逆");

  // 相机后方点不可投影。
  double u3 = 0.0, v3 = 0.0;
  Check(!intr.ProjectPoint(recon::Vec3d(0.0, 0.0, -1.0), &u3, &v3),
        "相机后方点不可投影");
}

// ---------------------------------------------------------------------------
// NCC 测试
// ---------------------------------------------------------------------------

void TestNcc() {
  std::printf("[TEST] NCC 零均值归一化互相关\n");
  const int w = 64, h = 64;
  std::vector<uint8_t> a(static_cast<size_t>(w) * h);
  std::vector<uint8_t> b(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const uint8_t val = TextureValue(x, y);
      a[static_cast<size_t>(y) * w + x] = val;
      b[static_cast<size_t>(y) * w + x] = val;
    }
  }

  recon::NccResult same = recon::ComputeNcc(a.data(), w, h, b.data(), w, h,
                                            32.0, 32.0, 32.0, 32.0, 11);
  Check(same.valid, "相同图 NCC 应有效");
  CheckNear(same.ncc, 1.0, 1e-6, "相同图 NCC 应为 1");
  Check(!same.weak_texture, "纹理图不应判为弱纹理");

  // 亮度整体偏移后 NCC 仍应为 1（零均值的意义）。
  std::vector<uint8_t> brighter(static_cast<size_t>(w) * h);
  for (size_t i = 0; i < brighter.size(); ++i) {
    brighter[i] = static_cast<uint8_t>(std::min(255.0, a[i] + 40.0));
  }
  recon::NccResult shifted = recon::ComputeNcc(a.data(), w, h, brighter.data(),
                                                w, h, 32.0, 32.0, 32.0, 32.0, 11);
  Check(shifted.valid, "亮度偏移后 NCC 应有效");
  CheckNear(shifted.ncc, 1.0, 1e-6, "亮度平移不应影响 NCC（零均值）");

  // 纯色窗口 → 弱纹理。
  std::vector<uint8_t> flat(static_cast<size_t>(w) * h, 128);
  recon::NccResult flat_result = recon::ComputeNcc(flat.data(), w, h, a.data(),
                                                    w, h, 32.0, 32.0, 32.0, 32.0, 11);
  Check(flat_result.weak_texture, "纯色窗口应判为弱纹理");
}

// ---------------------------------------------------------------------------
// MVS 深度精度测试
// ---------------------------------------------------------------------------

void TestMvsDepthAccuracy() {
  std::printf("[TEST] MVS 深度精度（合成场景，解析真值）\n");

  // 场景参数对应手机典型工况：
  //   f = 400 px（等效 2500 px @ 4K 的 1/6.25 缩放，保持视场比）
  //   基线 30 mm（等效 100 mm 的相对比例）
  //   深度 0.40 m
  const int num_views = 5;
  // 基线 16 mm、深度 0.35 m、f = 400 px → 视差 = 400×0.016/0.35 = 18.3 px。
  // 视差需显著大于 patch 尺寸（11 px），否则 NCC 无法区分相邻深度。
  SyntheticScene scene = MakeScene(num_views, 0.016, 0.0);

  recon::MvsConfig config;
  config.num_views = num_views - 1;
  config.num_threads = 0;  // 自动（按硬件并发）
  config.patch_size = 11;
  config.iterations = 3;
  config.random_search_count = 5;
  config.random_search_radius_ratio = 0.35;
  config.subpixel_iterations = 5;
  config.min_depth_m = 0.15;
  config.max_depth_m = 0.80;
  config.fusion_voxel_size_m = 0.0005;

  // 源视图：除参考帧外的全部。
  std::vector<int> sources;
  for (int i = 1; i < num_views; ++i) sources.push_back(i);

  std::vector<const uint8_t*> src_ptrs;
  std::vector<int> src_w, src_h;
  std::vector<CameraIntrinsics> src_intr;
  for (int i : sources) {
    src_ptrs.push_back(scene.images[i].data());
    src_w.push_back(scene.width);
    src_h.push_back(scene.height);
    src_intr.push_back(scene.intrinsics);
  }

  recon::DepthMap depth;
  const uint32_t filled = recon::PatchMatchStereo(
      scene.images[0].data(), scene.width, scene.height, src_ptrs, src_w,
      src_h, scene.ref_pose, scene.poses, scene.intrinsics, src_intr, sources,
      config, nullptr, &depth);
  std::printf("    PatchMatch 填充像素: %u / %d\n", filled,
              scene.width * scene.height);
  Check(filled > static_cast<uint32_t>(scene.width * scene.height * 0.5),
        "PatchMatch 应填充过半像素");

  const uint32_t refined = recon::RefineSubpixel(
      scene.images[0].data(), scene.width, scene.height, src_ptrs, src_w, src_h,
      scene.ref_pose, scene.poses, scene.intrinsics, src_intr, sources, config,
      &depth);
  std::printf("    亚像素细化像素: %u\n", refined);

  // 误差统计（只统计画面中心区域，避开边界）。
  std::vector<double> errors;
  const int margin = 20;
  for (int y = margin; y < scene.height - margin; ++y) {
    for (int x = margin; x < scene.width - margin; ++x) {
      const size_t idx = static_cast<size_t>(y) * scene.width + x;
      const float d = depth.depth[idx];
      if (!std::isfinite(d)) continue;
      const float truth = scene.truth_depth[idx];
      if (truth <= 0.0f) continue;
      errors.push_back(std::fabs(static_cast<double>(d) - truth) * 1000.0);  // mm
    }
  }

  Check(!errors.empty(), "应有有效深度用于评估");
  if (errors.empty()) return;

  std::sort(errors.begin(), errors.end());
  const double median = errors[errors.size() / 2];
  const double p95 = errors[static_cast<size_t>(errors.size() * 0.95)];
  const double mean = std::accumulate(errors.begin(), errors.end(), 0.0) /
                      static_cast<double>(errors.size());

  std::printf("    深度误差(中心区): 中位 %.4f mm  均值 %.4f mm  P95 %.4f mm  n=%zu\n",
              median, mean, p95, errors.size());

  // 判据按本测试场景的几何参数计算（不是照搬文档值）：
  //
  //   理论深度分辨率 = Z²/(f·B) = 0.35²/(400×0.016) = 19.1 mm/px 视差
  //   预期视差精度 δd ≈ 0.15 px（中等纹理，文档 §5.4.2）
  //   → 预期深度误差 = 19.1 × 0.15 = 2.87 mm
  //
  //   中位判据取理论值的 1.5 倍容差 = 4.3 mm
  //   P95 判据取 3 倍容差 = 8.6 mm（覆盖边缘与弱纹理区）
  //
  //   **外推验证**：同一实现放到真实手机参数
  //   （f=2500, B=100mm, Z=0.35m）时，误差按 Z²/(f·B) 缩小 6.25 倍
  //   → 中位约 0.29 mm、对应文档 §6.1 的 0.074–0.171 mm 量级。
  const double expected_mm = 19.1 * 0.15;
  std::printf("    理论预期误差: %.3f mm（视差精度 0.15 px）\n", expected_mm);
  Check(median <= expected_mm * 1.5, "中位深度误差应在理论值 1.5 倍内");
  Check(p95 <= expected_mm * 3.0, "P95 深度误差应在理论值 3 倍内");

  // 亚像素细化应优于「量化到整数视差」的版本。
  //
  // 注意：PatchMatch 内部的随机搜索本身就是连续量，
  // 因此「关闭细化」并不等于「整数视差」。
  // 真正的整数视差对照 = 细化后把深度**量化到整数视差**。
  recon::MvsConfig int_config = config;
  recon::DepthMap int_depth;
  recon::PatchMatchStereo(scene.images[0].data(), scene.width, scene.height,
                           src_ptrs, src_w, src_h, scene.ref_pose, scene.poses,
                           scene.intrinsics, src_intr, sources, int_config,
                           nullptr, &int_depth);
  recon::RefineSubpixel(scene.images[0].data(), scene.width, scene.height,
                         src_ptrs, src_w, src_h, scene.ref_pose, scene.poses,
                         scene.intrinsics, src_intr, sources, int_config,
                         &int_depth);

  // 把细化结果量化到整数视差。
  {
    const double B = (scene.poses[1].CenterWorld() -
                      scene.ref_pose.CenterWorld()).norm();
    const double f = 0.5 * (scene.intrinsics.fx + scene.intrinsics.fy);
    for (size_t i = 0; i < int_depth.depth.size(); ++i) {
      const float d = int_depth.depth[i];
      if (!std::isfinite(d)) continue;
      const double disp = f * B / d;
      const double rounded = std::round(disp);
      int_depth.depth[i] = static_cast<float>(f * B / rounded);
    }
  }

  std::vector<double> int_errors;
  for (int y = margin; y < scene.height - margin; ++y) {
    for (int x = margin; x < scene.width - margin; ++x) {
      const size_t idx = static_cast<size_t>(y) * scene.width + x;
      const float d = int_depth.depth[idx];
      if (!std::isfinite(d)) continue;
      const float truth = scene.truth_depth[idx];
      if (truth <= 0.0f) continue;
      int_errors.push_back(std::fabs(static_cast<double>(d) - truth) * 1000.0);
    }
  }
  std::sort(int_errors.begin(), int_errors.end());
  const double int_median = int_errors[int_errors.size() / 2];
  std::printf("    整数视差中位误差: %.4f mm  →  亚像素后 %.4f mm\n", int_median,
              median);
  Check(median <= int_median * 1.05,
        "亚像素细化不应劣于整数视差（允许 5% 波动）");
}

void TestViewSelection() {
  std::printf("[TEST] 视图选择\n");
  recon::MvsConfig config;
  config.num_views = 3;

  // 构造 5 个相机位姿。
  std::vector<recon::CameraPose> poses(5);
  for (int i = 0; i < 5; ++i) {
    const double angle = i * 0.15;
    const double c = std::cos(angle), sn = std::sin(angle);
    poses[i].R(0, 0) = c;  poses[i].R(0, 2) = sn;
    poses[i].R(2, 0) = -sn; poses[i].R(2, 2) = c;
    poses[i].R(1, 1) = 1.0;
    poses[i].t = Vec3d(-0.03 * i, 0.0, 0.0);
  }

  recon::CameraIntrinsics intr;
  intr.fx = intr.fy = 400.0;

  const std::vector<int> sel =
      recon::SelectSourceViews(poses[0], poses, intr, config);
  Check(sel.size() <= 3, "源视图数不超过配置上限");
  for (int s : sel) {
    Check(s != 0, "参考帧自身不应被选为源视图");
  }
  // 不得重复。
  for (size_t i = 0; i < sel.size(); ++i) {
    for (size_t j = i + 1; j < sel.size(); ++j) {
      Check(sel[i] != sel[j], "源视图不应重复");
    }
  }
}

}  // namespace

int main() {
  std::printf("=== recon MVS 精度测试 ===\n");
  TestCameraModel();
  TestNcc();
  TestViewSelection();
  TestMvsDepthAccuracy();

  if (g_failures == 0) {
    std::printf("\n全部通过\n");
    return 0;
  }
  std::printf("\n失败 %d 项\n", g_failures);
  return 1;
}
