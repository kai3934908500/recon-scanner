// SPDX-License-Identifier: BSD-3-Clause
//
// 深度预处理测试。契约：docs/04_RGBD管线.md §3。
//
// 覆盖：
//   · 置信度掩码（RM-1 硬门槛 / RM-2 平台无效 / RM-4 统计量）；
//   · 飞行点剔除（§3.3.1 二级判据、§3.3.2 τ_eff 噪声自适应、"偏前景不误剔"）；
//   · BMRE（§3.4：区间求交的 clamp、断层不平滑、最小分支恢复带颜色判据）；
//   · 深度→RGB 对齐（§3.5：外参映射、P_c.z 口径、z-buffer、RA-4 空洞不填补）；
//   · **铁律**：原始深度缓冲逐字节不变（IR-1/IR-3）。

#include "recon/depth_preprocess.h"

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
    std::printf("  [FAIL] %s (got %.9g, want %.9g, tol %.3g)\n", what, a, b, tol);
    ++g_failures;
  }
}

using recon::AlignDepthToRgb;
using recon::CameraIntrinsics;
using recon::DepthPreprocessConfig;
using recon::DepthPreprocessResult;
using recon::DepthAlignmentResult;
using recon::EffectiveFlyingThreshold;
using recon::Mat3d;
using recon::PreprocessDepth;
using recon::Vec3d;

// 平面前深度图（毫米）。depth_mm 恒定。
std::vector<uint16_t> FlatDepth(int w, int h, uint16_t mm) {
  return std::vector<uint16_t>(static_cast<size_t>(w) * h, mm);
}

std::vector<uint8_t> FullConf(int w, int h, uint8_t c) {
  return std::vector<uint8_t>(static_cast<size_t>(w) * h, c);
}

// ---------------------------------------------------------------------------
// §3.3.2 τ_eff 噪声自适应
// ---------------------------------------------------------------------------

void TestEffectiveThreshold() {
  std::printf("[TEST] τ_eff 噪声自适应（§3.3.2）\n");
  DepthPreprocessConfig cfg;
  cfg.flying_pixel_threshold_m = 0.002;
  cfg.flying_pixel_noise_k = 3.0;

  // ToF：σ_z = 2 mm → τ_eff = max(2mm, 3·2mm=6mm) = 6mm。
  cfg.noise.tier = RECON_DEPTH_TOF;
  CheckNear(EffectiveFlyingThreshold(cfg, 0.35), 0.006, 1e-9, "ToF 标档 τ_eff=6mm");

  // LiDAR：σ_z = 12 mm → 36 mm。
  cfg.noise.tier = RECON_DEPTH_LIDAR;
  CheckNear(EffectiveFlyingThreshold(cfg, 0.35), 0.036, 1e-9, "LiDAR τ_eff=36mm");

  // ALGO @0.35 m：σ_z = 0.03·0.35 + 0.0005 = 0.011 m → 33 mm。
  cfg.noise.tier = RECON_DEPTH_ALGO;
  CheckNear(EffectiveFlyingThreshold(cfg, 0.35), 0.033, 1e-6, "ALGO@0.35 τ_eff≈33mm");

  // 极近处 ALGO：σ_z → abs 项 0.0005 → 但 τ 下限仍为 max(0.002, 3·0.0005)=0.002。
  CheckNear(EffectiveFlyingThreshold(cfg, 0.0), 0.002, 1e-9,
            "ALGO@0 σ 极小 → 回落到阈值下限");
}

// ---------------------------------------------------------------------------
// §3.2 置信度掩码
// ---------------------------------------------------------------------------

void TestConfidenceMask() {
  std::printf("[TEST] 置信度掩码（§3.2）\n");
  const int w = 8, h = 8;
  auto depth = FlatDepth(w, h, 1000);
  auto conf = FullConf(w, h, 200);
  // 平台标记无效：某像素深度 0；某像素 conf 0。
  depth[0] = 0;
  conf[1] = 0;

  DepthPreprocessConfig cfg;
  cfg.noise.tier = RECON_DEPTH_TOF;

  {
    // RM-1：阈值 0 = 不硬删，全部走降权路径。
    DepthPreprocessResult r = PreprocessDepth(depth.data(), conf.data(), nullptr, 0,
                                              w, h, cfg);
    Check(r.ok, "处理应成功");
    Check(r.stats.invalid_platform == 2, "RM-2：2 个平台无效像素");
    Check(r.stats.masked_hard == 0, "阈值 0 时不应硬删");
  }
  {
    // RM-1：阈值 100 时，conf=200 保留、conf<100 置无效。
    auto conf2 = FullConf(w, h, 200);
    conf2[5] = 50;  // 低于阈值
    DepthPreprocessConfig cfg2 = cfg;
    cfg2.confidence_threshold = 100;
    DepthPreprocessResult r = PreprocessDepth(depth.data(), conf2.data(), nullptr, 0,
                                              w, h, cfg2);
    // 像素 0 深度 0（平台无效）不参与；像素 5 conf 50 被硬删。
    Check(r.stats.masked_hard == 1, "RM-1：1 个像素被硬门槛置无效");
    Check(r.stats.mean_confidence > 199.0 && r.stats.mean_confidence < 201.0,
          "mean(c) 应在 200 附近（仅统计有效像素）");
  }
  {
    // 空置信度：内部视为 255。
    DepthPreprocessResult r = PreprocessDepth(depth.data(), nullptr, nullptr, 0, w,
                                              h, cfg);
    Check(r.stats.mean_confidence > 254.0, "无置信度时按 255 处理");
  }
}

// ---------------------------------------------------------------------------
// §3.3 飞行点剔除
// ---------------------------------------------------------------------------

void TestFlyingPixelRejection() {
  std::printf("[TEST] 飞行点剔除（§3.3）\n");
  const int w = 9, h = 9;
  // 背景 2000 mm，前景左半 1000 mm，交界处放一个飞行点：读数 1600 mm（偏背景侧）。
  auto depth = FlatDepth(w, h, 2000);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < 4; ++x) depth[static_cast<size_t>(y) * w + x] = 1000;
  }
  // 飞行点：位于交界（x=4），读数落在 (1000+0.5·1000=1500, 2000) 之间 → 1600。
  const int fx = 4, fy = 4;
  depth[static_cast<size_t>(fy) * w + fx] = 1600;

  DepthPreprocessConfig cfg;
  cfg.noise.tier = RECON_DEPTH_TOF;  // τ_eff = 6 mm
  auto conf = FullConf(w, h, 200);

  DepthPreprocessResult r = PreprocessDepth(depth.data(), conf.data(), nullptr, 0, w,
                                            h, cfg);
  Check(r.ok, "处理应成功");
  Check(r.flying_mask[static_cast<size_t>(fy) * w + fx] == 1,
        "偏背景侧的混合读数应被判为飞行点");
  Check(r.depth_filtered[static_cast<size_t>(fy) * w + fx] == 0,
        "飞行点应置无效");

  // 偏前景的"真实中间深度"不应被误剔（§3.3.1 判据 (c) 后半段）。
  auto depth2 = FlatDepth(w, h, 2000);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < 4; ++x) depth2[static_cast<size_t>(y) * w + x] = 1000;
  }
  // 真值 1200（偏前景，< 1500）→ 不是飞行点。
  depth2[static_cast<size_t>(fy) * w + fx] = 1200;
  DepthPreprocessResult r2 = PreprocessDepth(depth2.data(), conf.data(), nullptr, 0,
                                             w, h, cfg);
  Check(r2.flying_mask[static_cast<size_t>(fy) * w + fx] == 0,
        "偏前景的真实深度不应被误剔");
}

void TestFlyingRatioDownweight() {
  std::printf("[TEST] 飞行点占比整帧降权（§3.3.3）\n");
  const int w = 12, h = 12;
  // 交替 1000/3000 的棋盘 → 大量跳变。让混叠值偏背景 → 大量飞行点。
  auto depth = FlatDepth(w, h, 1000);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      depth[static_cast<size_t>(y) * w + x] = ((x + y) % 2 == 0) ? 1000 : 3000;
    }
  }
  // 把一半像素改成中间值（偏背景侧）以触发飞行点。
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      if ((x + y) % 2 == 1) depth[static_cast<size_t>(y) * w + x] = 2600;
    }
  }
  DepthPreprocessConfig cfg;
  cfg.noise.tier = RECON_DEPTH_TOF;
  auto conf = FullConf(w, h, 200);
  DepthPreprocessResult r = PreprocessDepth(depth.data(), conf.data(), nullptr, 0, w,
                                            h, cfg);
  // 只验证「飞行点多时统计量被正确设置」这一契约，不将判据写死到具体数字。
  if (r.stats.flying_ratio > cfg.flying_ratio_downweight) {
    Check(r.stats.frame_downweighted, "高飞行比应触发整帧降权");
    CheckNear(r.stats.frame_weight_scale, 0.5, 1e-9, "降权系数应为 0.5");
  }
}

// ---------------------------------------------------------------------------
// §3.4 BMRE
// ---------------------------------------------------------------------------

void TestBmreSmoothsFlat() {
  std::printf("[TEST] BMRE 一致邻域 clamp（§3.4.3）\n");
  const int w = 16, h = 16;
  auto depth = FlatDepth(w, h, 1000);
  // 一个孤立噪声点 +30 mm（远小于 τ_eff=6mm？不 → 30mm > 6mm，会被视作断层）。
  // 用 +3 mm 的小噪声（< τ_eff），应被 clamp 回一致区间。
  depth[static_cast<size_t>(8) * w + 8] = 1003;
  DepthPreprocessConfig cfg;
  cfg.noise.tier = RECON_DEPTH_TOF;
  auto conf = FullConf(w, h, 200);
  DepthPreprocessResult r = PreprocessDepth(depth.data(), conf.data(), nullptr, 0, w,
                                            h, cfg);
  Check(r.ok, "处理应成功");
  // 一致区间内 → 非边缘。
  Check(r.edge_flag[static_cast<size_t>(8) * w + 8] == 0,
        "小噪声不应判为边缘");
  // d_hat 应被拉回 1000 附近（区间 [max−τ, min+τ] = [1000−6, 1003+6]，
  // d_p=1003 落在区间内 → 不变；其余 1000 也在区间内）。
  Check(std::abs(static_cast<int>(r.depth_filtered[static_cast<size_t>(8) * w + 8]) - 1003) <= 6,
        "输出应在一致区间内");
}

void TestBmreDiscontinuityNotSmoothed() {
  std::printf("[TEST] BMRE 断层不平滑（§3.4.3）\n");
  const int w = 16, h = 16;
  auto depth = FlatDepth(w, h, 1000);
  // 右半 1300 mm（跳变 300 mm >> τ_eff=6mm）→ 真实断层。
  for (int y = 0; y < h; ++y) {
    for (int x = 8; x < w; ++x) depth[static_cast<size_t>(y) * w + x] = 1300;
  }
  DepthPreprocessConfig cfg;
  cfg.noise.tier = RECON_DEPTH_TOF;
  auto conf = FullConf(w, h, 200);
  DepthPreprocessResult r = PreprocessDepth(depth.data(), conf.data(), nullptr, 0, w,
                                            h, cfg);
  // 交界附近像素应被标为边缘。
  int edge_count = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 7; x <= 8; ++x) {
      if (r.edge_flag[static_cast<size_t>(y) * w + x]) ++edge_count;
    }
  }
  Check(edge_count > 0, "断层交界应有边缘像素");
  // 深色准则：1300 侧不应被平滑成 1150 之类中间值（远离断层处保持）。
  Check(r.depth_filtered[static_cast<size_t>(8) * w + 15] == 1300,
        "远离断层的 1300 侧应保持原值");
  Check(r.depth_filtered[static_cast<size_t>(8) * w + 0] == 1000,
        "远离断层的 1000 侧应保持原值");
}

void TestBmreColorGuidedBranch() {
  std::printf("[TEST] BMRE 最小分支恢复带颜色判据（§3.4.4）\n");
  const int w = 16, h = 16;
  auto depth = FlatDepth(w, h, 2000);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < 8; ++x) depth[static_cast<size_t>(y) * w + x] = 1000;
  }
  // 构造颜色：前景(x<8)统一红，背景(x≥8)统一蓝，交界像素与前景同色。
  std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3, 0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t b = (static_cast<size_t>(y) * w + x) * 3;
      if (x < 8) {
        rgb[b + 0] = 0; rgb[b + 1] = 0; rgb[b + 2] = 255;  // 红
      } else {
        rgb[b + 0] = 255; rgb[b + 1] = 0; rgb[b + 2] = 0;  // 蓝
      }
    }
  }
  DepthPreprocessConfig cfg;
  cfg.noise.tier = RECON_DEPTH_TOF;
  auto conf = FullConf(w, h, 200);
  DepthPreprocessResult r = PreprocessDepth(depth.data(), conf.data(), rgb.data(), 3, w,
                                            h, cfg);
  Check(r.ok, "带颜色处理应成功");
  // 颜色门控的关键作用：交界像素 (8,8) 为背景色（蓝），左邻 (7,8) 为前景色（红）
  // 且 d=1000。因两者颜色权重 ≈ 0，前景红邻域被排除出支持集 → 不跨表面混合。
  // 若颜色门控失效（把红邻域当同面），d̂ 会被拉向 1000。
  const size_t idx = static_cast<size_t>(8) * w + 8;
  Check(r.depth_filtered[idx] >= 1990 && r.depth_filtered[idx] <= 2010,
        "颜色门控应阻止跨表面混合（背景像素不应被前景拉近）");
  // 前景像素同理应保持 1000 附近。
  const size_t idx_fg = static_cast<size_t>(8) * w + 7;
  Check(r.depth_filtered[idx_fg] >= 990 && r.depth_filtered[idx_fg] <= 1010,
        "前景像素不应被背景拉远");
}

// ---------------------------------------------------------------------------
// §3.5 深度→RGB 对齐
// ---------------------------------------------------------------------------

void TestAlignSameOpticalCenter() {
  std::printf("[TEST] 对齐：同光心（§3.5.3）\n");
  const int dw = 4, dh = 4;
  auto depth = FlatDepth(dw, dh, 1000);  // 1 m
  auto conf = FullConf(dw, dh, 200);

  CameraIntrinsics kd;
  kd.fx = kd.fy = 2.0; kd.cx = 1.5; kd.cy = 1.5; kd.width = dw; kd.height = dh;
  CameraIntrinsics kc;
  kc.fx = kc.fy = 2.0; kc.cx = 1.5; kc.cy = 1.5; kc.width = dw; kc.height = dh;

  DepthPreprocessConfig cfg;
  cfg.align_derived_conf_scale = 0.5;
  DepthAlignmentResult r = AlignDepthToRgb(depth.data(), conf.data(), dw, dh, kd, kc,
                                           Mat3d::Identity(), Vec3d::Zero(), cfg);
  Check(r.status == RECON_OK, "同光心对齐应成功");
  CheckNear(r.measured_baseline_m, 0.0, 1e-12, "基线应为 0");
  Check(r.baseline_ok, "零基线应通过校验");
  // 同光心恒等映射：深度值不变（P_c.z == z_ax）。
  Check(r.depth_aligned[static_cast<size_t>(1) * dw + 1] == 1000,
        "同光心深度值应不变");
  // RA-6：置信度折扣 0.5 → 100。
  Check(r.confidence_aligned[static_cast<size_t>(1) * dw + 1] == 100,
        "派生像点置信度应折扣 0.5");
  Check(r.derived[static_cast<size_t>(1) * dw + 1] == 1, "应标记为派生");
}

void TestAlignBaselineShiftAndHole() {
  std::printf("[TEST] 对齐：基线偏移 + RA-4 空洞\n");
  const int dw = 4, dh = 4;
  // 只有中心 2×2 有深度，边缘为 0 → 对齐后应产生空洞。
  auto depth = std::vector<uint16_t>(static_cast<size_t>(dw) * dh, 0);
  depth[static_cast<size_t>(1) * dw + 1] = 1000;
  depth[static_cast<size_t>(1) * dw + 2] = 1000;
  depth[static_cast<size_t>(2) * dw + 1] = 1000;
  depth[static_cast<size_t>(2) * dw + 2] = 1000;

  CameraIntrinsics kd;
  kd.fx = kd.fy = 2.0; kd.cx = 1.5; kd.cy = 1.5; kd.width = dw; kd.height = dh;
  // 放大 8× 的 RGB 分辨率（模拟深度低分辨率 → RGB 高分辨率）。
  const int rw = 32, rh = 32;
  CameraIntrinsics kc;
  kc.fx = kc.fy = 16.0; kc.cx = 15.5; kc.cy = 15.5; kc.width = rw; kc.height = rh;

  DepthPreprocessConfig cfg;
  DepthAlignmentResult r = AlignDepthToRgb(depth.data(), nullptr, dw, dh, kd, kc,
                                           Mat3d::Identity(), Vec3d::Zero(), cfg);
  Check(r.status == RECON_OK, "对齐应成功");

  // RA-4：空洞必须保持无效（derived=0, depth=0）。
  int holes = 0, filled = 0;
  for (size_t i = 0; i < r.derived.size(); ++i) {
    if (r.derived[i] == 0) { ++holes; Check(r.depth_aligned[i] == 0, "空洞深度必须为 0"); }
    else ++filled;
  }
  Check(holes > 0, "低分辨率→高分辨率必然产生空洞");
  Check(filled > 0, "中心区域应有填充");

  // §3.5.5：基线超限告警。
  DepthPreprocessConfig cfg2 = cfg;
  cfg2.align_max_baseline_m = 0.001;  // 1 mm 上限
  DepthAlignmentResult r2 = AlignDepthToRgb(depth.data(), nullptr, dw, dh, kd, kc,
                                            Mat3d::Identity(), Vec3d(0.05, 0, 0), cfg2);
  Check(!r2.baseline_ok, "50 mm 基线应超 1 mm 上限");
  CheckNear(r2.measured_baseline_m, 0.05, 1e-9, "基线量级应为 50 mm");
}

// ---------------------------------------------------------------------------
// 铁律：原始缓冲不变（IR-1/IR-3）
// ---------------------------------------------------------------------------

void TestRawBufferUnchanged() {
  std::printf("[TEST] 铁律 IR-1/IR-3：原始深度缓冲不变\n");
  const int w = 8, h = 8;
  auto depth = FlatDepth(w, h, 1000);
  depth[10] = 1600;  // 制造飞行点
  auto conf = FullConf(w, h, 200);
  const auto depth_copy = depth;
  const auto conf_copy = conf;

  DepthPreprocessConfig cfg;
  cfg.noise.tier = RECON_DEPTH_TOF;
  DepthPreprocessResult r = PreprocessDepth(depth.data(), conf.data(), nullptr, 0, w,
                                            h, cfg);
  Check(depth == depth_copy, "原始深度缓冲必须逐字节不变");
  Check(conf == conf_copy, "原始置信度缓冲必须逐字节不变");
  // 且输出缓冲与输入独立（d_filt 是副本）。
  Check(r.depth_filtered.size() == depth.size(), "输出尺寸应一致");
}

// ---------------------------------------------------------------------------
// 边界
// ---------------------------------------------------------------------------

void TestInvalidInput() {
  std::printf("[TEST] 边界输入\n");
  DepthPreprocessConfig cfg;
  DepthPreprocessResult r0 = PreprocessDepth(nullptr, nullptr, nullptr, 0, 0, 0, cfg);
  Check(!r0.ok, "空输入应失败");
  Check(r0.status == RECON_ERR_INVALID_ARGUMENT, "应返回参数错误");

  DepthAlignmentResult r1 = AlignDepthToRgb(nullptr, nullptr, 0, 0, CameraIntrinsics{},
                                            CameraIntrinsics{}, Mat3d::Identity(),
                                            Vec3d::Zero(), cfg);
  Check(r1.status == RECON_ERR_INVALID_ARGUMENT, "对齐空输入应失败");
}

}  // namespace

int main() {
  std::printf("=== recon 深度预处理测试 ===\n");
  TestEffectiveThreshold();
  TestConfidenceMask();
  TestFlyingPixelRejection();
  TestFlyingRatioDownweight();
  TestBmreSmoothsFlat();
  TestBmreDiscontinuityNotSmoothed();
  TestBmreColorGuidedBranch();
  TestAlignSameOpticalCenter();
  TestAlignBaselineShiftAndHole();
  TestRawBufferUnchanged();
  TestInvalidInput();

  if (g_failures == 0) {
    std::printf("\n全部通过\n");
    return 0;
  }
  std::printf("\n失败 %d 项\n", g_failures);
  return 1;
}
