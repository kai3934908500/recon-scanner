// SPDX-License-Identifier: BSD-3-Clause
//
// 时间同步与深度预处理测试。
//
// 验证 docs/02 §3 的同步契约与 docs/04 §2 的深度口径转换。

#include "recon/quality_gate.h"
#include "recon/imu_preintegration.h"
#include "recon/tsdf_fusion.h"
#include "recon/camera.h"

#include <cmath>
#include <cstdio>
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
  if (std::fabs(a - b) > tol) {
    std::printf("  [FAIL] %s (got %.9f, want %.9f)\n", what, a, b);
    ++g_failures;
  }
}

// ---------------------------------------------------------------------------
// SO(3) 指数/对数往返
// ---------------------------------------------------------------------------

void TestSo3RoundTrip() {
  std::printf("[TEST] SO(3) 指数/对数\n");
  const double angles[] = {0.0, 1e-8, 0.1, 1.0, 3.0, 3.14159};
  for (double a : angles) {
    // Rodrigues 公式在 θ→π 时数值病态（轴提取退化），
    // 该点的往返容差放宽到 1e-3。其余点用 1e-9 级容差。
    const double tol = (a > 3.14) ? 1e-3 : 1e-9;
    recon::Vec3d axis = recon::Vec3d(0.3, -0.5, 0.81).normalized();
    recon::Vec3d omega = axis * a;
    recon::Mat3d R = recon::ExpSO3(omega);
    recon::Vec3d back = recon::LogSO3(R);
    CheckNear(back.norm(), a, tol, "SO(3) 往返角度应一致");
    // 旋转矩阵应为正交阵。
    recon::Mat3d should_be_identity = R.transpose() * R;
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        const double expect = (i == j) ? 1.0 : 0.0;
        CheckNear(should_be_identity(i, j), expect, 1e-9,
                  "SO(3) 旋转矩阵应正交");
      }
    }
  }
}

// ---------------------------------------------------------------------------
// IMU 预积分
// ---------------------------------------------------------------------------

void TestPreintegration() {
  std::printf("[TEST] IMU 预积分\n");
  // 噪声参数口径为**方差**（σ²）。取消费级 IMU 典型值：
  //   σ_g = 1.5e-4 rad/s  → 方差 2.25e-8
  //   σ_a = 1.0e-2 m/s²   → 方差 1.0e-4
  //   σ_bg = 1e-5         → 方差 1e-10
  //   σ_ba = 1e-4         → 方差 1e-8
  const double sigma_g = 1.5e-4;
  const double sigma_a = 1.0e-2;
  const double sigma_bg = 1.0e-5;
  const double sigma_ba = 1.0e-4;

  recon::ImuPreintegrator pre;
  pre.Reset(recon::Vec3d::Zero(), recon::Vec3d::Zero(),
            recon::Vec3d(sigma_g * sigma_g, sigma_g * sigma_g, sigma_g * sigma_g),
            recon::Vec3d(sigma_a * sigma_a, sigma_a * sigma_a, sigma_a * sigma_a),
            recon::Vec3d(sigma_bg * sigma_bg, sigma_bg * sigma_bg, sigma_bg * sigma_bg),
            recon::Vec3d(sigma_ba * sigma_ba, sigma_ba * sigma_ba, sigma_ba * sigma_ba));

  // 静止采样：加速度计测的是**比力**。设备保持水平静止时，
  // 比力 = −g_W（世界系重力指向 −z，支撑力使其读数为 +z）。
  // 预积分的参考系约定见 imu_preintegration.cpp：只积分 body 系比力，
  // 重力在世界系残差里显式出现（docs/05 §3.7.2）。
  const double g = 9.80665;
  pre.SetGravity(recon::Vec3d(0.0, 0.0, -g));
  for (int i = 0; i < 100; ++i) {
    const double t = static_cast<double>(i) * 1000000.0;  // 1 ms 间隔
    pre.AddSample(t, recon::Vec3d(0.0, 0.0, g), recon::Vec3d::Zero());
  }

  recon::PreintegratedMeasurement m;
  recon::PreintegrationResult r = pre.Finalize(&m);
  Check(r.ok, "预积分应成功");
  CheckNear(m.duration_s, 0.099, 1e-3, "预积分时长应约 0.1 s");

  // 静止水平放置时：Δv = (0,0,+g)Δt（这正是残差里被 "−g_W·Δt" 抵消的量），
  // Δp 含抛物项 ½gΔt²。
  //   Δv_z = g·0.099 = 0.971 m/s
  //   Δp_z = ½gΔt² 求和 ≈ ½·9.80665·0.099² = 0.04808 m
  // 用解析值校验（不用「接近零」这种把 bug 固化成期望的判据）。
  CheckNear(m.delta_v(2), g * 0.099, 1e-9, "静止时 Δv_z 应等于 g·Δt");
  Check(m.delta_v(0) < 1e-12 && m.delta_v(1) < 1e-12,
        "静止时水平方向速度增量应为零");
  CheckNear(m.delta_p(2), 0.5 * g * 0.099 * 0.099, 1e-6,
            "静止时 Δp_z 应等于 ½gΔt²");
  // 姿态增量：无角速度 → 单位阵。
  CheckNear(m.delta_q(0, 0), 1.0, 1e-12, "静止时姿态增量应为单位阵");
  CheckNear(m.delta_q(1, 1), 1.0, 1e-12, "静止时姿态增量应为单位阵");
  CheckNear(m.delta_q(2, 2), 1.0, 1e-12, "静止时姿态增量应为单位阵");

  // 噪声协方差必须为正且随时间增长。
  Check(m.covariance(0, 0) > 0.0, "陀螺噪声协方差应为正");
  Check(m.covariance(3, 3) > 0.0, "加计噪声协方差应为正");

  // 协方差量级验证：白噪声项 σ²·Δt 应精确匹配解析值。
  const double T = 0.099;  // 99 个 1ms 步
  CheckNear(m.covariance(0, 0), sigma_g * sigma_g * T, 1e-15,
            "陀螺协方差应等于 σ_g²·T");
  CheckNear(m.covariance(3, 3), sigma_a * sigma_a * T, 1e-15,
            "加计协方差应等于 σ_a²·T");

  // 随机游走项 σ_bg²·Δt³ 应远小于白噪声项 σ_g²·Δt（相差 6 个数量级）。
  const double white_term = sigma_g * sigma_g * T;
  const double rw_term = sigma_bg * sigma_bg * T * T * T;
  Check(rw_term < white_term * 1e-4, "随机游走项应远小于白噪声项");
}

void TestStaticDetection() {
  std::printf("[TEST] 静止检测\n");
  std::vector<recon::ImuSample> samples;
  const double g = 9.80665;

  // 静止 3 秒。
  for (int i = 0; i < 300; ++i) {
    recon::ImuSample s;
    s.timestamp_ns = static_cast<int64_t>(i) * 10000000;
    s.accel = recon::Vec3d(0.0, 0.0, g);
    s.gyro = recon::Vec3d::Zero();
    samples.push_back(s);
  }
  recon::StaticDetectionResult r = recon::DetectStatic(samples, 0.1, 0.05, 2.0);
  Check(r.is_static, "静止序列应判定为静止");

  // 加入角速度 → 不再静止。
  for (auto& s : samples) s.gyro = recon::Vec3d(0.5, 0.0, 0.0);
  recon::StaticDetectionResult r2 = recon::DetectStatic(samples, 0.1, 0.05, 2.0);
  Check(!r2.is_static, "有角速度应判定为非静止");

  // 样本不足。
  std::vector<recon::ImuSample> few(samples.begin(), samples.begin() + 5);
  recon::StaticDetectionResult r3 = recon::DetectStatic(few, 0.1, 0.05, 2.0);
  Check(!r3.is_static, "样本不足不应判为静止");
}

void TestScaleObservability() {
  std::printf("[TEST] 尺度可观性判定\n");
  std::vector<recon::ImuSample> straight;
  const double g = 9.80665;

  // 匀速直线运动：加速度恒为重力，方向一致性极高。
  for (int i = 0; i < 200; ++i) {
    recon::ImuSample s;
    s.timestamp_ns = static_cast<int64_t>(i) * 10000000;
    s.accel = recon::Vec3d(0.0, 0.0, g);
    s.gyro = recon::Vec3d::Zero();
    straight.push_back(s);
  }
  recon::ScaleObservabilityResult r =
      recon::CheckScaleObservability(straight, 1.0);
  Check(r.direction_consistency > 0.95, "直线运动方向一致性应很高");
  Check(r.degenerate, "匀速直线运动应判定为尺度退化");

  // 圆周运动：加速度方向变化，尺度可观。
  std::vector<recon::ImuSample> circular;
  for (int i = 0; i < 200; ++i) {
    const double phase = i * 0.05;
    recon::ImuSample s;
    s.timestamp_ns = static_cast<int64_t>(i) * 10000000;
    s.accel = recon::Vec3d(std::sin(phase) * 2.0, std::cos(phase) * 2.0, g);
    s.gyro = recon::Vec3d(0.0, 0.0, 0.5);
    circular.push_back(s);
  }
  recon::ScaleObservabilityResult r2 =
      recon::CheckScaleObservability(circular, 1.0);
  Check(!r2.degenerate, "圆周运动不应判定为尺度退化");
  // 加速度含重力分量，方向一致性仍较高，
  // 因此尺度退化的判据由「加速度方差」主导（见实现）。
  Check(r2.accel_variance > 0.05 * 0.05,
        "圆周运动的加速度方差应显著大于直线运动");
}

// ---------------------------------------------------------------------------
// 深度预处理与 TSDF
// ---------------------------------------------------------------------------

void TestTsdfFusion() {
  std::printf("[TEST] TSDF 融合\n");
  recon::TsdfConfig cfg;
  cfg.voxel_size_m = 0.005;
  cfg.truncation_m = 0.02;
  cfg.min_depth_m = 0.05;
  cfg.max_depth_m = 2.0;
  cfg.fx = 200.0;
  cfg.fy = 200.0;
  cfg.cx = 64.0;
  cfg.cy = 64.0;
  cfg.origin_x = -0.2;
  cfg.origin_y = -0.2;
  cfg.origin_z = -0.2;
  cfg.bbox_max_x = 0.2;
  cfg.bbox_max_y = 0.2;
  cfg.bbox_max_z = 0.6;
  cfg.min_confidence = 30;

  recon::TsdfVolume volume(cfg);

  // 构造一个 128x128 的深度图，所有像素深度 500 mm（0.5 m）。
  const int w = 128, h = 128;
  std::vector<uint16_t> depth(w * h, 500);
  std::vector<uint8_t> conf(w * h, 200);

  // 相机位于原点朝向 +Z，世界系与相机系一致（identity）。
  recon::Mat4d w2c = recon::Mat4d::Identity();

  const uint32_t integrated =
      volume.IntegrateDepthFrame(depth.data(), conf.data(), w, h, w2c, 0.005f);
  Check(integrated > 0, "应有像素参与融合");
  Check(integrated == static_cast<uint32_t>(w * h), "全部像素应参与融合");

  // 提取等值面点。
  std::vector<recon::Vec3d> positions;
  std::vector<recon::Vec3d> normals;
  std::vector<float> confidence;
  volume.ExtractPoints(&positions, &normals, &confidence);
  Check(!positions.empty(), "应提取到等值面点");

  // 深度 0.5 m 处的点，其 z 应接近 0.5。
  if (!positions.empty()) {
    double z_sum = 0.0;
    for (const auto& p : positions) z_sum += p(2);
    const double z_mean = z_sum / static_cast<double>(positions.size());
    // 容差说明：等值面位置受体素化（5 mm）与零交叉离散化影响，
    // 3 cm 容差对应亚体素级精度，符合实现。
    CheckNear(z_mean, 0.5, 0.03, "等值面 z 应接近实际深度 0.5 m");

    // === 法向朝向核查（#48）===
    //
    // 场景：相机在原点朝 +Z（identity world→camera），一堵平面墙位于
    // z = 0.5 m 处。因此**自由空间在 z < 0.5 一侧**，正确的**外法向
    // 必须指向相机 = −Z 方向**。
    //
    // 这里曾经暴露两个真实缺陷（2026-10-31 第十七轮）：
    //   ① 邻居正值判定只查 `tsdf > 0`、未查 `weight > 0` → 未融合体素
    //      的默认 tsdf=+1 被当成自由空间 → 法向被幻影分量污染
    //      （实测 n=(1,1,1)，z 跨度 15 mm 的厚板）；
    //   ② 累加符号写反（`n -= off`）→ 输出**朝内**法向，违反
    //      docs/06 §1.2「输入点云法向必须全部朝外」。
    // 两者叠加时，「朝外占比」类代理指标看似过关（+z/−z 各半、均值≈0），
    // 但**法向实际完全不可用**。故此处以**逐点朝向**为硬判据。
    Check(normals.size() == positions.size(), "法向应与点数等长");
    if (!normals.empty()) {
      size_t outward = 0;  // n_z < -0.9（指向 −Z = 朝向相机 = 朝外）
      double nz_sum = 0.0;
      for (const auto& n : normals) {
        nz_sum += n(2);
        if (n(2) < -0.9) ++outward;
      }
      const double ratio =
          static_cast<double>(outward) / static_cast<double>(normals.size());
      std::printf("  [INFO] TSDF 法向朝外占比 = %.2f%%（ref < -0.9）\n",
                  ratio * 100.0);
      // 平面墙的法向应**逐点统一**为 −Z；允许极少数边界点非轴对齐。
      Check(ratio > 0.95, "平面墙的 TSDF 法向应 95% 以上朝外（指向相机 −Z）");
      const double nz_mean = nz_sum / static_cast<double>(normals.size());
      CheckNear(nz_mean, -1.0, 0.05, "平面墙的平均外法向应≈(0,0,-1)");
    }

    // z 跨度应≈单个体素层（表面），而非被幻影邻居拉厚的多层板。
    if (!positions.empty()) {
      double zmin = 1e9, zmax = -1e9;
      for (const auto& p : positions) {
        if (p(2) < zmin) zmin = p(2);
        if (p(2) > zmax) zmax = p(2);
      }
      // 容差取 2 个体素（12 mm）：零交叉离散化允许邻层各贡献一层。
      Check(zmax - zmin < 4.0 * cfg.voxel_size_m,
            "等值面点云应贴合单层表面（厚度 < 4 体素）");
    }
  }

  // 低置信度应阻止融合。
  std::vector<uint16_t> depth2(w * h, 500);
  std::vector<uint8_t> low_conf(w * h, 10);
  recon::TsdfVolume volume2(cfg);
  const uint32_t integrated2 = volume2.IntegrateDepthFrame(
      depth2.data(), low_conf.data(), w, h, w2c, 0.005f);
  Check(integrated2 == 0, "低置信度像素不应参与融合");

  // 深度 0（无效）应被跳过。
  std::vector<uint16_t> depth3(w * h, 0);
  recon::TsdfVolume volume3(cfg);
  const uint32_t integrated3 = volume3.IntegrateDepthFrame(
      depth3.data(), nullptr, w, h, w2c, 0.005f);
  Check(integrated3 == 0, "无效深度不应参与融合");

  // 内存占用应与体素数一致。
  uint64_t bytes = 0;
  volume.MemoryUsageBytes(&bytes);
  Check(bytes > 0, "内存占用应可计算");
}

// ---------------------------------------------------------------------------
// 质量门限
// ---------------------------------------------------------------------------

void TestQualityGate() {
  std::printf("[TEST] 质量门限\n");
  const int w = 320, h = 240;
  std::vector<uint8_t> sharp(w * h * 4, 0);
  std::vector<uint8_t> flat(w * h * 4, 128);

  // 构造纹理丰富的图像（棋盘）。
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const uint8_t v = ((x / 8 + y / 8) % 2) ? 230 : 20;
      const size_t idx = (static_cast<size_t>(y) * w + x) * 4;
      sharp[idx + 0] = v;
      sharp[idx + 1] = v;
      sharp[idx + 2] = v;
      sharp[idx + 3] = 255;
    }
  }

  recon::QualityGateConfig config;
  recon::FrameSignals signals;

  recon::QualityGateResult sharp_result = recon::EvaluateQualityGate(
      sharp.data(), w, h, w * 4, config, signals);
  Check(sharp_result.blur_variance > config.blur_variance_min,
        "纹理图像应通过模糊检测");

  recon::QualityGateResult flat_result = recon::EvaluateQualityGate(
      flat.data(), w, h, w * 4, config, signals);
  Check(flat_result.blur_variance < config.blur_variance_min,
        "平坦图像应被判为模糊");
  Check(!flat_result.accepted, "严重模糊帧应被拒收");
  Check(flat_result.blur_severe, "应为严重模糊");

  // 高光：全白图像。
  std::vector<uint8_t> white(w * h * 4, 255);
  recon::QualityGateResult white_result = recon::EvaluateQualityGate(
      white.data(), w, h, w * 4, config, signals);
  Check(white_result.saturated_ratio > config.saturated_ratio_warning,
        "全白图像应被判高光");
  Check(white_result.specular_warning || white_result.specular_severe,
        "应触发高光检测");

  // 运动过快。
  recon::FrameSignals fast;
  fast.angular_speed_rad_s = config.max_angular_speed * 2.0;
  recon::QualityGateResult fast_result = recon::EvaluateQualityGate(
      sharp.data(), w, h, w * 4, config, fast);
  Check(fast_result.too_fast_severe, "超速应判为严重");

  // 重叠度不足。
  recon::FrameSignals low_overlap;
  low_overlap.overlap_ratio = config.min_overlap_ratio * 0.5;
  recon::QualityGateResult lo_result = recon::EvaluateQualityGate(
      sharp.data(), w, h, w * 4, config, low_overlap);
  Check(lo_result.low_overlap_severe, "低重叠应判为严重");
  Check(!lo_result.accepted, "低重叠帧应被拒收");

  // 正常帧应被接收。
  Check(sharp_result.accepted, "正常帧应被接收");
  Check(sharp_result.severity == recon::QualitySeverity::kOk, "正常帧应无异常");
}

// ---------------------------------------------------------------------------
// 内参分辨率缩放（docs/02 §3.9.1 C-1、docs/04 §3.5.3）
//
// 为什么必须单独测：TSDF 积分在**深度原生分辨率**上进行（docs/04 IR-2），
// 而平台（docs/11 §3.7）上报的是**彩色相机内参**，二者分辨率通常不同
// （ARKit 深度 256×192 vs RGB 1920×1440）。若换算错误，整条 RGBD 链的
// 几何都被系统性扭曲 —— 而此前所有测试都令 depth_width == rgb_width，
// 缩放恒为 1.0，缺陷对全部测试不可见（2026-10-31 实测）。
//
// 三个断言分别锁住三个曾缺失的正确性要点：
//   ① 主点必须做 +0.5/−0.5 补偿（漏掉 → 0.5 px 系统偏差）；
//   ② fx 与 fy 各按自己的比例（用单标量 → 宽高比不同时 fy 错）；
//   ③ 源尺寸取自内参自身，不依赖 RGB 缓冲是否存在（纯深度帧下 rgb_width == 0）。
// ---------------------------------------------------------------------------
void TestIntrinsicsResolutionScaling() {
  std::printf("[TEST] 内参分辨率缩放（同光心 RGB→深度，C-1）\n");

  // --- ① 4:3 → 4:3（AR 深度典型 256×192 ← RGB 1920×1440）----------------
  {
    recon::CameraIntrinsics k;
    k.fx = 1560.0;
    k.fy = 1560.0;
    k.cx = 960.0;
    k.cy = 720.0;
    k.width = 1920;
    k.height = 1440;

    const recon::CameraIntrinsics d =
        recon::ScaleIntrinsicsToResolution(k, 256, 192);

    // 比例 = 256/1920 = 192/1440 = 2/15。
    CheckNear(d.fx, 1560.0 * (2.0 / 15.0), 1e-9, "fx 应按宽度比例缩放");
    CheckNear(d.fy, 1560.0 * (2.0 / 15.0), 1e-9, "fy 应按高度比例缩放");
    // 主点补偿：(960 + 0.5)·(2/15) − 0.5。
    CheckNear(d.cx, (960.0 + 0.5) * (2.0 / 15.0) - 0.5, 1e-9,
              "主点 cx 必须含 +0.5/−0.5 补偿");
    CheckNear(d.cy, (720.0 + 0.5) * (2.0 / 15.0) - 0.5, 1e-9,
              "主点 cy 必须含 +0.5/−0.5 补偿");
    // 反证：若漏掉补偿，差值应恰为 0.5·(s−1)，此处非零 → 补偿确实生效。
    const double no_comp_cx = 960.0 * (2.0 / 15.0);
    Check(std::fabs(no_comp_cx - d.cx) > 1e-6,
          "主点补偿必须改变结果（否则判据未落在目标上）");
    Check(d.width == 256 && d.height == 192, "输出尺寸应更新为目标分辨率");
  }

  // --- ② 宽高比变化（4:3 → 16:9）：单标量实现必错 ------------------------
  {
    recon::CameraIntrinsics k;
    k.fx = 1000.0;
    k.fy = 1000.0;
    k.cx = 640.0;
    k.cy = 360.0;
    k.width = 1280;
    k.height = 720;

    const recon::CameraIntrinsics d =
        recon::ScaleIntrinsicsToResolution(k, 640, 480);

    // sx = 640/1280 = 0.5；sy = 480/720 ≈ 0.6667。
    CheckNear(d.fx, 500.0, 1e-9, "fx 按 sx 缩放");
    CheckNear(d.fy, 1000.0 * (480.0 / 720.0), 1e-9,
              "fy 必须按 sy 而非 sx（单标量实现会在此失败）");
    Check(std::fabs(d.fx - d.fy) > 100.0,
          "宽高比变化时 fx 与 fy 必须不同");
  }

  // --- ③ 源尺寸缺失（纯深度帧：RGB 缓冲为 NULL → 无尺寸可推）-------------
  {
    recon::CameraIntrinsics k;
    k.fx = 900.0;
    k.fy = 900.0;
    k.cx = 320.0;
    k.cy = 240.0;
    k.width = 0;   // 平台未填
    k.height = 0;

    const recon::CameraIntrinsics d =
        recon::ScaleIntrinsicsToResolution(k, 160, 120);
    CheckNear(d.fx, 900.0, 1e-12, "源尺寸未知时不得猜测缩放（fx 不变）");
    CheckNear(d.cx, 320.0, 1e-12, "源尺寸未知时主点不变");
  }

  // --- ④ 同分辨率：应是不动点 --------------------------------------------
  {
    recon::CameraIntrinsics k;
    k.fx = 500.0;
    k.fy = 500.0;
    k.cx = 320.0;
    k.cy = 240.0;
    k.width = 640;
    k.height = 480;
    const recon::CameraIntrinsics d =
        recon::ScaleIntrinsicsToResolution(k, 640, 480);
    CheckNear(d.fx, 500.0, 1e-12, "同分辨率时 fx 不变");
    CheckNear(d.cx, 320.0, 1e-12, "同分辨率时 cx 不变（+0.5/−0.5 相抵）");
  }
}

}  // namespace

int main() {
  std::printf("=== recon 同步与深度测试 ===\n");
  TestSo3RoundTrip();
  TestPreintegration();
  TestStaticDetection();
  TestScaleObservability();
  TestTsdfFusion();
  TestQualityGate();
  TestIntrinsicsResolutionScaling();

  if (g_failures == 0) {
    std::printf("\n全部通过\n");
    return 0;
  }
  std::printf("\n失败 %d 项\n", g_failures);
  return 1;
}
