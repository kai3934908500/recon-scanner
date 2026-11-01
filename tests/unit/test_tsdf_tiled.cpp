// SPDX-License-Identifier: BSD-3-Clause
//
// 分块 TSDF 一致性测试（docs/13 §3.2.1；docs/14 §5.1 TSDF 行 ③④⑤）。
//
// 目标判据（docs/14 §5.1.1 登记的三项缺口）：
//   ③ 分块融合与单块融合的**结果一致**（差 < 1e-6）；
//   ④ 重叠区加权平均正确（本实现用全局索引精确划分，等价判据为「跨块邻域查询
//      正确」——即相邻块边界的表面点能被正确检出，与单块结果一致）；
//   ⑤ **体素边长未被自动修改**（黑名单 B-1）。
//
// 方法：同一 TsdfConfig + 同一合成深度帧，分别喂给 TsdfVolume（单块）与
//       TsdfTiledVolume（分块，block_voxels 取多个值），逐点比对位置/法向。

#include "recon/tsdf_fusion.h"

#include <cmath>
#include <cstdio>
#include <string>
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
    std::printf("  [FAIL] %s (got %.9f, want %.9f, tol %.1e)\n", what, a, b, tol);
    ++g_failures;
  }
}

// 构造一个倾斜平面墙的深度帧，使表面穿过多个块，检验跨块邻域。
struct Scene {
  int w = 160, h = 160;
  std::vector<uint16_t> depth;
  std::vector<uint8_t> conf;
  recon::Mat4d w2c;

  Scene() {
    depth.assign(static_cast<size_t>(w) * h, 0);
    conf.assign(static_cast<size_t>(w) * h, 220);
    w2c = recon::Mat4d::Identity();
    // 平面 z = 0.5 + 0.05·(x_norm)（沿 x 倾斜），x_norm ∈ [-1,1]。
    for (int v = 0; v < h; ++v) {
      for (int u = 0; u < w; ++u) {
        const double x_norm = (2.0 * u / (w - 1)) - 1.0;
        const double z = 0.5 + 0.05 * x_norm;
        depth[static_cast<size_t>(v) * w + u] =
            static_cast<uint16_t>(std::lround(z * 1000.0));
      }
    }
  }
};

recon::TsdfConfig MakeConfig() {
  recon::TsdfConfig cfg;
  cfg.voxel_size_m = 0.004;
  cfg.truncation_m = 0.02;
  cfg.min_depth_m = 0.05;
  cfg.max_depth_m = 2.0;
  cfg.fx = 180.0;
  cfg.fy = 180.0;
  cfg.cx = 80.0;
  cfg.cy = 80.0;
  cfg.origin_x = -0.30;
  cfg.origin_y = -0.20;
  cfg.origin_z = 0.30;
  cfg.bbox_max_x = 0.30;
  cfg.bbox_max_y = 0.20;
  cfg.bbox_max_z = 0.70;
  cfg.min_confidence = 30;
  return cfg;
}

recon::Mat4d MakePose(double tx, double ty, double tz, double yaw) {
  const double c = std::cos(yaw), s = std::sin(yaw);
  recon::Mat4d m = recon::Mat4d::Identity();
  m(0, 0) = c; m(0, 2) = s;
  m(2, 0) = -s; m(2, 2) = c;
  m(0, 3) = tx; m(1, 3) = ty; m(2, 3) = tz;
  return m;
}

// 逐点比对两组表面点（位置 + 法向）。返回是否完全一致。
bool ComparePoints(const std::vector<recon::Vec3d>& pa,
                   const std::vector<recon::Vec3d>& na,
                   const std::vector<recon::Vec3d>& pb,
                   const std::vector<recon::Vec3d>& nb, const char* tag) {
  if (pa.size() != pb.size()) {
    std::printf("  [FAIL] %s: 点数不一致 (%zu vs %zu)\n", tag, pa.size(),
                pb.size());
    return false;
  }
  double max_dp = 0.0, max_dn = 0.0;
  for (size_t i = 0; i < pa.size(); ++i) {
    const double dpx = std::fabs(pa[i](0) - pb[i](0));
    const double dpy = std::fabs(pa[i](1) - pb[i](1));
    const double dpz = std::fabs(pa[i](2) - pb[i](2));
    max_dp = std::max(max_dp, std::max(dpx, std::max(dpy, dpz)));
    const double dnx = std::fabs(na[i](0) - nb[i](0));
    const double dny = std::fabs(na[i](1) - nb[i](1));
    const double dnz = std::fabs(na[i](2) - nb[i](2));
    max_dn = std::max(max_dn, std::max(dnx, std::max(dny, dnz)));
  }
  std::printf("  [INFO] %s: %zu 点, 最大位置差 %.3e, 最大法向差 %.3e\n", tag,
              pa.size(), max_dp, max_dn);
  bool ok = true;
  if (max_dp > 1e-6) {
    std::printf("  [FAIL] %s: 位置差超阈值 1e-6\n", tag);
    ok = false;
  }
  if (max_dn > 1e-6) {
    std::printf("  [FAIL] %s: 法向差超阈值 1e-6\n", tag);
    ok = false;
  }
  return ok;
}

// ③④ 分块与单块结果一致（多块尺寸 × 多视角）。
void TestTiledMatchesSingle() {
  std::printf("[TEST] 分块 TSDF 与单块结果一致\n");
  const Scene scene;
  const recon::TsdfConfig cfg = MakeConfig();

  // 单块参考：多帧融合。
  recon::TsdfVolume single(cfg);
  std::vector<recon::Mat4d> poses = {
      MakePose(0.0, 0.0, 0.0, 0.0),
      MakePose(0.03, 0.01, 0.0, 0.05),
      MakePose(-0.02, 0.0, 0.0, -0.04),
  };
  for (const auto& p : poses) {
    single.IntegrateDepthFrame(scene.depth.data(), scene.conf.data(), scene.w,
                               scene.h, p, 0.004f);
  }
  std::vector<recon::Vec3d> sp, sn;
  std::vector<float> sc;
  single.ExtractPoints(&sp, &sn, &sc);
  Check(!sp.empty(), "单块应提取到表面点");

  // 分块：block_voxels 取 8 / 16 / 32 / 64（其中 8 远小于网格尺寸，
  // 强制产生大量跨块邻域查询）。
  const int blocks[] = {8, 16, 32, 64};
  for (int bv : blocks) {
    recon::TsdfTiledVolume tiled(cfg, bv);
    for (const auto& p : poses) {
      tiled.IntegrateDepthFrame(scene.depth.data(), scene.conf.data(), scene.w,
                                scene.h, p, 0.004f);
    }
    std::vector<recon::Vec3d> tp, tn;
    std::vector<float> tc;
    tiled.ExtractPoints(&tp, &tn, &tc);

    char tag[64];
    std::snprintf(tag, sizeof(tag), "block_voxels=%d", bv);
    Check(ComparePoints(sp, sn, tp, tn, tag), "分块应与单块逐点一致");
  }
}

// ⑤ 体素边长不被分块修改（黑名单 B-1）。
void TestVoxelSizeUnchanged() {
  std::printf("[TEST] 分块不改变体素边长（黑名单 B-1）\n");
  recon::TsdfConfig cfg = MakeConfig();
  const double vs = cfg.voxel_size_m;
  const double trunc = cfg.truncation_m;
  recon::TsdfTiledVolume tiled(cfg, 16);
  // dims 由 voxel_size 与包围盒决定；若体素边长被改，dims 会随之变化。
  recon::TsdfConfig cfg2 = MakeConfig();
  recon::TsdfVolume single(cfg2);
  const int* d1 = tiled.dims();
  const int* d2 = single.dims();
  Check(d1[0] == d2[0] && d1[1] == d2[1] && d1[2] == d2[2],
        "分块与单块的网格尺寸必须相同（体素边长未变）");
  CheckNear(cfg.voxel_size_m, vs, 1e-15, "体素边长未被修改");
  CheckNear(cfg.truncation_m, trunc, 1e-15, "截断距离未被修改");
}

// 分块内存：常驻块数应远小于全局块数（证明分块确实省内存）。
void TestTiledMemoryReduction() {
  std::printf("[TEST] 分块内存占用\n");
  const Scene scene;
  recon::TsdfConfig cfg = MakeConfig();
  // 只为少量像素融合，使只有部分块被触达。
  std::vector<uint16_t> small(20 * 20, 0);
  std::vector<uint8_t> smallc(20 * 20, 220);
  for (size_t i = 0; i < small.size(); ++i) small[i] = 500;

  recon::TsdfTiledVolume tiled(cfg, 8);
  recon::Mat4d id = recon::Mat4d::Identity();
  tiled.IntegrateDepthFrame(small.data(), smallc.data(), 20, 20, id, 0.004f);

  const int* d = tiled.dims();
  const int gb = 8;
  const size_t total_blocks =
      static_cast<size_t>((d[0] + gb - 1) / gb) * ((d[1] + gb - 1) / gb) *
      ((d[2] + gb - 1) / gb);
  std::printf("  [INFO] 常驻块 %zu / 全局块 %zu\n", tiled.resident_blocks(),
              total_blocks);
  Check(tiled.resident_blocks() > 0, "应有块被触达");
  Check(tiled.resident_blocks() < total_blocks,
        "稀疏融合时不应触达全部块（证明分块省内存）");

  uint64_t bytes = 0;
  tiled.MemoryUsageBytes(&bytes);
  Check(bytes > 0, "分块内存占用应 > 0");
}

// 空输入与退化输入不崩溃。
void TestTiledEdgeCases() {
  std::printf("[TEST] 分块边界情形\n");
  recon::TsdfConfig cfg = MakeConfig();
  recon::TsdfTiledVolume tiled(cfg, 16);
  std::vector<recon::Vec3d> p, n;
  std::vector<float> c;
  tiled.ExtractPoints(&p, &n, &c);
  Check(p.empty(), "空体素应无表面点");
  const uint32_t r = tiled.IntegrateDepthFrame(nullptr, nullptr, 0, 0,
                                               recon::Mat4d::Identity(), 0.01f);
  Check(r == 0u, "空指针输入应返回 0");
  // block_voxels 过小（1）也不应崩溃。
  recon::TsdfTiledVolume tiny(cfg, 1);
  std::vector<uint16_t> d(9, 500);
  const uint32_t r2 =
      tiny.IntegrateDepthFrame(d.data(), nullptr, 3, 3,
                               recon::Mat4d::Identity(), 0.01f);
  Check(r2 == 9u, "block_voxels=1 时仍应融合全部像素");
}

}  // namespace

int main() {
  std::printf("=== 分块 TSDF 一致性测试 ===\n");
  TestTiledMatchesSingle();
  TestVoxelSizeUnchanged();
  TestTiledMemoryReduction();
  TestTiledEdgeCases();
  if (g_failures == 0) {
    std::printf("全部通过。\n");
    return 0;
  }
  std::printf("失败 %d 项。\n", g_failures);
  return 1;
}
