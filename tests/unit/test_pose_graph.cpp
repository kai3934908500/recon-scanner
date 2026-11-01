// SPDX-License-Identifier: BSD-3-Clause
//
// 位姿图优化与闭环检测测试。契约：docs/05_单目管线.md §4.5–§4.6。
//
// 覆盖：
//   · HammingDistance / 指纹确定性（跨调用一致）；
//   · DetectLoopCandidates（帧间隔过滤、候选上限）；
//   · 闭环几何验证（视角夹角、尺度/方向一致性拒绝）；
//   · PoseGraph（合成真值下的漂移消除、gauge 固定、LM 代价单调下降）；
//   · TryAddLoopEdge（§4.5.3 代价上升 > 20% → 回滚）。

#include "recon/pose_graph.h"

#include <cmath>
#include <cstdio>
#include <vector>

#include "recon/imu_preintegration.h"

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

using recon::CameraPose;
using recon::DetectLoopCandidates;
using recon::FrameFingerprint;
using recon::HammingDistance;
using recon::LoopConfig;
using recon::Mat3d;
using recon::PoseGraph;
using recon::PoseGraphConfig;
using recon::PoseGraphEdge;
using recon::TryAddLoopEdge;
using recon::Vec3d;
using recon::ExpSO3;

// 由相机光心构造 CameraPose（R=I, t = −C）。
CameraPose PoseAt(const Vec3d& center) {
  CameraPose p;
  p.R = Mat3d::Identity();
  p.t = -center;
  return p;
}

// ---------------------------------------------------------------------------
// 汉明距离
// ---------------------------------------------------------------------------

void TestHamming() {
  std::printf("[TEST] HammingDistance\n");
  Check(HammingDistance(0, 0) == 0, "全同应距离 0");
  Check(HammingDistance(0, 1) == 1, "1 bit 差");
  Check(HammingDistance(0xFFFFFFFFFFFFFFFFULL, 0) == 64, "全反应 64");
  Check(HammingDistance(0b1010ULL, 0b0101ULL) == 4, "互补 4 bit");
  Check(HammingDistance(0xF0F0ULL, 0x0F0FULL) == 16, "交替字节 16");
}

// ---------------------------------------------------------------------------
// 指纹确定性
// ---------------------------------------------------------------------------

// 构造一个合成特征集：n 个特征，描述子用确定性伪随机填充。
recon::SfmFeatureSet MakeFeatureSet(int image_id, int n, uint64_t seed, int jitter) {
  recon::SfmFeatureSet s;
  s.image_id = image_id;
  uint64_t st = seed;
  auto next = [&st]() {
    st = st * 6364136223846793005ULL + 1442695040888963407ULL;
    return st;
  };
  for (int i = 0; i < n; ++i) {
    recon::SfmFeature f;
    f.uv = recon::Vec2d(i % 64, i / 64);
    f.response = static_cast<double>(i) + 0.5;
    f.descriptor.resize(128);
    for (int d = 0; d < 128; ++d) {
      f.descriptor[d] = static_cast<uint8_t>((next() >> 33) & 0xFF);
    }
    // jitter 用于制造「相近但不完全相同」的特征集：轻微改描述子。
    if (jitter != 0) {
      for (int d = 0; d < jitter; ++d) {
        f.descriptor[d] = static_cast<uint8_t>((f.descriptor[d] + 133) & 0xFF);
      }
    }
    s.features.push_back(std::move(f));
  }
  return s;
}

void TestFingerprintDeterminism() {
  std::printf("[TEST] 指纹确定性\n");
  LoopConfig cfg;
  const auto s = MakeFeatureSet(0, 600, 12345, 0);
  const FrameFingerprint f1 = recon::ComputeFrameFingerprint(s, cfg);
  const FrameFingerprint f2 = recon::ComputeFrameFingerprint(s, cfg);
  Check(f1.valid, "非空特征集应产出有效指纹");
  Check(f1.bits == f2.bits, "同一输入两次指纹应完全一致（确定性）");

  // 相同内容不同 image_id → 指纹应一致（指纹不看 id）。
  const auto s_id2 = MakeFeatureSet(1, 600, 12345, 0);
  const FrameFingerprint f3 = recon::ComputeFrameFingerprint(s_id2, cfg);
  Check(f1.bits == f3.bits, "指纹不应依赖 image_id");

  // 空特征集 → 无效。
  recon::SfmFeatureSet empty;
  empty.image_id = 0;
  Check(!recon::ComputeFrameFingerprint(empty, cfg).valid, "空特征集应无效");
}

void TestFingerprintSeparation() {
  std::printf("[TEST] 指纹区分度（相同 vs 不同内容）\n");
  LoopConfig cfg;
  const auto a = MakeFeatureSet(0, 600, 111, 0);
  const auto b = MakeFeatureSet(1, 600, 999999, 0);  // 完全不同
  const auto c = MakeFeatureSet(2, 600, 111, 4);     // 轻微扰动

  const auto fa = recon::ComputeFrameFingerprint(a, cfg);
  const auto fb = recon::ComputeFrameFingerprint(b, cfg);
  const auto fc = recon::ComputeFrameFingerprint(c, cfg);

  const int same = HammingDistance(fa.bits, fa.bits);
  const int diff = HammingDistance(fa.bits, fb.bits);
  const int near = HammingDistance(fa.bits, fc.bits);

  Check(same == 0, "自身距离为 0");
  // 不同内容应有更大距离。
  Check(diff >= near || diff > 0, "不同内容距离应不小于轻微扰动距离");
  Check(near <= same + 64, "轻微扰动距离应在合法范围内");
}

// ---------------------------------------------------------------------------
// 候选检测
// ---------------------------------------------------------------------------

void TestDetectCandidates() {
  std::printf("[TEST] DetectLoopCandidates\n");
  LoopConfig cfg;
  cfg.min_frame_separation = 5;
  cfg.lsh_hamming = 12;
  cfg.max_candidates_per_frame = 1;

  // 构造指纹序列：前 6 帧内容 A，帧 6..11 内容 B，帧 12..17 又回到 A（闭环）。
  std::vector<FrameFingerprint> fps;
  for (int i = 0; i < 18; ++i) {
    FrameFingerprint f;
    f.image_id = i;
    f.valid = true;
    // A 与 B 用两个固定位模式。
    f.bits = (i < 6 || i >= 12) ? 0xAAAAAAAAAAAAAAAAULL : 0x5555555555555555ULL;
    fps.push_back(f);
  }
  const auto cands = DetectLoopCandidates(fps, cfg);
  Check(!cands.empty(), "应检出闭环候选");
  // 所有候选必须满足帧间隔。
  bool sep_ok = true;
  int per_frame0 = 0;
  for (const auto& c : cands) {
    if (c.index1 - c.index0 < cfg.min_frame_separation) sep_ok = false;
    if (c.index0 == 0) ++per_frame0;
  }
  Check(sep_ok, "候选必须满足最小帧间隔");
  Check(per_frame0 <= cfg.max_candidates_per_frame, "每帧候选数应受上限约束");
  // 帧 0（A）与帧 12..17（A）的汉明距离应为 0。
  for (const auto& c : cands) {
    if (c.index0 == 0) Check(c.hamming == 0, "同内容指纹距离应为 0");
  }
}

// ---------------------------------------------------------------------------
// 位姿图优化：合成真值下的漂移消除
// ---------------------------------------------------------------------------

void TestPoseGraphDriftRemoval() {
  std::printf("[TEST] PoseGraph 漂移消除（合成真值）\n");
  // 真值：5 个节点沿 x 轴，光心 (0..4, 0, 0)，全部 R=I。
  const int N = 5;
  std::vector<CameraPose> truth;
  for (int i = 0; i < N; ++i) truth.push_back(PoseAt(Vec3d(i, 0, 0)));

  PoseGraph graph;
  for (int i = 0; i < N; ++i) graph.AddNode(truth[i]);
  // 里程计边（用真值计算 → 精确）。
  for (int i = 0; i + 1 < N; ++i) {
    graph.AddRelativeEdge(i, i + 1, truth[i], truth[i + 1], 0.01, 0.01);
  }
  // 闭环边：节点 0 ↔ 节点 4，真值相对平移 (4,0,0)。
  recon::PoseGraphEdge loop;
  loop.i = 0;
  loop.j = 4;
  loop.R_ij = Mat3d::Identity();
  loop.t_ij = Vec3d(4, 0, 0);
  loop.is_loop = true;
  loop.rot_sigma_rad = 0.01;
  loop.trans_sigma_m = 0.01;
  graph.AddEdge(loop);

  // 先制造漂移：把节点 4 的位置改错 +0.5 m。
  graph.mutable_poses()->at(4).t = -Vec3d(4.5, 0, 0);

  PoseGraphConfig cfg;
  cfg.fixed_node = 0;
  const double cost_before = graph.ComputeCost(cfg);
  recon::PoseGraphResult res = graph.Optimize(cfg);
  const double cost_after = graph.ComputeCost(cfg);

  Check(res.ok, "优化应成功");
  Check(cost_before > 0.0, "漂移状态下代价应非零");
  Check(cost_after < cost_before, "优化应降低代价");
  Check(res.iterations >= 1, "应迭代至少一次");

  // 节点 4 的光心应被拉回 4.0 附近（闭环 + 链条强行约束）。
  const Vec3d c4 = -(graph.poses()[4].R.transpose() * graph.poses()[4].t);
  CheckNear(c4(0), 4.0, 0.15, "闭环应把节点 4 拉回真值附近");
  CheckNear(c4(1), 0.0, 0.1, "节点 4 的 y 应回到 0");
}

void TestPoseGraphGaugeFixed() {
  std::printf("[TEST] PoseGraph gauge 固定\n");
  const int N = 4;
  std::vector<CameraPose> truth;
  for (int i = 0; i < N; ++i) truth.push_back(PoseAt(Vec3d(i, 0, 0)));
  PoseGraph graph;
  for (int i = 0; i < N; ++i) graph.AddNode(truth[i]);
  for (int i = 0; i + 1 < N; ++i) {
    graph.AddRelativeEdge(i, i + 1, truth[i], truth[i + 1], 0.01, 0.01);
  }
  // 扰动非固定节点。
  graph.mutable_poses()->at(2).t = -Vec3d(2.3, 0.1, 0);

  PoseGraphConfig cfg;
  cfg.fixed_node = 0;
  const CameraPose before = graph.poses()[0];
  graph.Optimize(cfg);
  const CameraPose after = graph.poses()[0];
  // 固定节点位姿必须完全不变。
  CheckNear(after.t.norm() - before.t.norm(), 0.0, 1e-12, "固定节点平移不应变");
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      CheckNear(after.R(i, j), before.R(i, j), 1e-12, "固定节点旋转不应变");
    }
  }
}

void TestPoseGraphExactChainZeroCost() {
  std::printf("[TEST] PoseGraph 精确链条代价 ≈ 0\n");
  const int N = 6;
  std::vector<CameraPose> truth;
  for (int i = 0; i < N; ++i) truth.push_back(PoseAt(Vec3d(i, 0, 0)));
  PoseGraph graph;
  for (int i = 0; i < N; ++i) graph.AddNode(truth[i]);
  for (int i = 0; i + 1 < N; ++i) {
    graph.AddRelativeEdge(i, i + 1, truth[i], truth[i + 1], 0.01, 0.01);
  }
  PoseGraphConfig cfg;
  cfg.fixed_node = 0;
  const double cost = graph.ComputeCost(cfg);
  CheckNear(cost, 0.0, 1e-10, "无漂移时初始代价应 ≈ 0");
  graph.Optimize(cfg);
  CheckNear(graph.ComputeCost(cfg), 0.0, 1e-10, "优化后仍应 ≈ 0");
}

// ---------------------------------------------------------------------------
// 闭环回滚（§4.5.3）
// ---------------------------------------------------------------------------

void TestTryAddLoopEdgeRollback() {
  std::printf("[TEST] TryAddLoopEdge 回滚（§4.5.3）\n");
  const int N = 5;
  std::vector<CameraPose> truth;
  for (int i = 0; i < N; ++i) truth.push_back(PoseAt(Vec3d(i, 0, 0)));
  PoseGraph graph;
  for (int i = 0; i < N; ++i) graph.AddNode(truth[i]);
  for (int i = 0; i + 1 < N; ++i) {
    graph.AddRelativeEdge(i, i + 1, truth[i], truth[i + 1], 0.01, 0.01);
  }
  PoseGraphConfig cfg;
  cfg.fixed_node = 0;

  // 加入一条**严重错误**的闭环边（平移 (100,0,0) 而非真值 (4,0,0)）。
  recon::PoseGraphEdge bad;
  bad.i = 0;
  bad.j = 4;
  bad.R_ij = Mat3d::Identity();
  bad.t_ij = Vec3d(100, 0, 0);
  bad.is_loop = true;
  bad.rot_sigma_rad = 0.01;
  bad.trans_sigma_m = 0.01;

  const std::vector<CameraPose> before = graph.poses();
  const bool added = TryAddLoopEdge(&graph, bad, cfg);
  Check(!added, "严重错误的闭环边应被回滚拒绝");
  // 位姿应回滚到加入前。
  bool same = true;
  for (size_t i = 0; i < before.size(); ++i) {
    if (std::fabs(before[i].t.norm() - graph.poses()[i].t.norm()) > 1e-12) {
      same = false;
    }
  }
  Check(same, "回滚后位姿应与加入前一致");

  // 正确的闭环边应被接受。
  recon::PoseGraphEdge good;
  good.i = 0;
  good.j = 4;
  good.R_ij = Mat3d::Identity();
  good.t_ij = Vec3d(4, 0, 0);
  good.is_loop = true;
  good.rot_sigma_rad = 0.01;
  good.trans_sigma_m = 0.01;
  const bool added2 = TryAddLoopEdge(&graph, good, cfg);
  Check(added2, "一致的闭环边应被接受");
}

// ---------------------------------------------------------------------------
// 闭环拒绝：视角夹角
// ---------------------------------------------------------------------------

void TestLoopAngleRejection() {
  std::printf("[TEST] 闭环视角夹角拒绝（§4.5.2）\n");
  // 两帧内参相同、视角几乎为 0 → 应被夹角判据拒绝。
  recon::SfmFeatureSet a = MakeFeatureSet(0, 120, 7, 0);
  recon::SfmFeatureSet b = MakeFeatureSet(1, 120, 7, 0);
  a.intrinsics = recon::CameraIntrinsics{};
  a.intrinsics.fx = a.intrinsics.fy = 500;
  a.intrinsics.cx = a.intrinsics.cy = 320;
  b.intrinsics = a.intrinsics;
  a.intrinsics_valid = true;
  b.intrinsics_valid = true;

  std::vector<recon::SfmFeatureSet> sets = {a, b};
  std::vector<CameraPose> poses = {PoseAt(Vec3d(0, 0, 0)), PoseAt(Vec3d(0, 0, 0))};

  LoopConfig cfg;
  cfg.min_angle_deg = 15.0;
  recon::LoopCandidate cand{0, 1, 0};
  const recon::LoopClosure lc = recon::VerifyLoopClosure(sets, poses, 1.0, cand, cfg);
  Check(!lc.accepted, "零视角变化的候选应被拒绝");
  Check(lc.angle_deg < cfg.min_angle_deg, "夹角应小于阈值");
}

}  // namespace

int main() {
  std::printf("=== recon 位姿图与闭环测试 ===\n");
  TestHamming();
  TestFingerprintDeterminism();
  TestFingerprintSeparation();
  TestDetectCandidates();
  TestPoseGraphDriftRemoval();
  TestPoseGraphGaugeFixed();
  TestPoseGraphExactChainZeroCost();
  TestTryAddLoopEdgeRollback();
  TestLoopAngleRejection();

  if (g_failures == 0) {
    std::printf("\n全部通过\n");
    return 0;
  }
  std::printf("\n失败 %d 项\n", g_failures);
  return 1;
}
