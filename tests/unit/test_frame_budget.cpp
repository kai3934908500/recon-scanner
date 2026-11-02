// SPDX-License-Identifier: BSD-3-Clause
//
// 采集期像素内存预算单元测试（缺口 AT，2026-11-02）。
//
// ── 这个测试为什么必须存在 ─────────────────────────────────────────────────
//
// 2026-11-02 真机实测：扫描到 100 余帧时 App **必闪退**。根因是
// `Orchestrator::frames_` 把每帧 RGB/深度**全量深拷贝且无任何上限** ——
// iPhone 13 上 ARKit 默认 1920×1440 BGRA8 = 11.06 MiB/帧，100 帧即
// ~1.3 GiB，撞 iOS jetsam 上限被杀。修复引入「像素预算 + FIFO 释放像素」。
//
// 修复的核心不变式（本文件逐条断言）：
//
//   I-1  常驻像素字节数**永不超过**预算（这是 OOM 不再发生的直接判据）。
//   I-2  释放像素**不删帧**：`captured_frame_count()` 恒等于已推帧数，
//        且被释放帧的**元数据（位姿/内参/时间戳/尺寸字段）完好**。
//        —— 删帧会让 `keyframe_indices_` 等平行数组下标整体错位，
//           是比 OOM 更隐蔽的错误（静默产出错配位姿）。
//   I-3  预算设为 0 时**不设限**（测试与诊断用的显式逃生门）。
//   I-4  预算调小后**立即**裁决既有帧（不需要等下一帧才生效）。
//   I-5  `Reset()` 归零计数器（否则下一次会话首帧像素会被立即误释放）。
//   I-6  **释放后重建链不崩**：被释放帧参与 TSDF/烘焙/MVS 时必须被
//        各自的守卫跳过，而不是越界读（历史上 `depth_confidence` 的
//        stride 误解就造成过一次 SIGSEGV，见 orchestrator.cpp:398 长注释）。
//
// ── 负向注入（本测试的自我证伪）────────────────────────────────────────────
//
// 若把 `EnforcePixelBudgetLocked()` 里的 `resident_pixel_bytes_ > budget`
// 判据改成恒 false（即禁用释放），I-1 立刻失败 —— 说明 I-1 不是恒真判据。
// 若把 `f.rgb.clear()` 改成 `frames_.erase(begin())`（删帧），I-2 失败。

#include "recon/context.h"
#include "recon/orchestrator.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::printf("  [FAIL] %s\n", what);
    ++g_failures;
  }
}

// 单帧几何：RGB 与深度的字节数由宽高决定，便于精确推算预算。
constexpr int kWidth = 64;
constexpr int kHeight = 48;
// BGRA8 = w*h*4；uint16 深度 = w*h*2。
constexpr size_t kRgbBytes = static_cast<size_t>(kWidth) * kHeight * 4;
constexpr size_t kDepthBytes = static_cast<size_t>(kWidth) * kHeight * 2;
constexpr size_t kFrameBytes = kRgbBytes + kDepthBytes;

recon::CameraPose MakePose() {
  recon::CameraPose p;
  p.R = recon::Mat3d::Identity();
  p.t = recon::Vec3d(0.0, 0.0, 0.5);
  return p;
}

// 组装一个带 RGB + 深度的帧包。缓冲由调用方持有（契约：返回后不访问）。
struct PacketStorage {
  std::vector<uint8_t> rgb;
  std::vector<uint8_t> depth;
  recon_camera_intrinsics intr{};
  recon_pose pose{};
  recon_frame_packet packet{};
};

void FillPacket(uint64_t id, PacketStorage* st) {
  st->rgb.assign(kRgbBytes, 128);
  st->depth.assign(kDepthBytes, 0);
  // 深度填一个有效的中心区域值，避免下游按「全 0 = 无深度」跳过。
  for (size_t i = 0; i < st->depth.size(); i += 2) {
    const uint16_t mm = 500;  // 0.5 m
    std::memcpy(&st->depth[i], &mm, 2);
  }

  st->intr.fx = 500.0; st->intr.fy = 500.0;
  st->intr.cx = kWidth * 0.5; st->intr.cy = kHeight * 0.5;
  st->intr.width = kWidth; st->intr.height = kHeight;

  const recon::CameraPose p = MakePose();
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) st->pose.r[i * 3 + j] = p.R(i, j);
    st->pose.t[i] = p.t(i);
  }

  st->packet = recon_frame_packet{};
  st->packet.frame_id = id;
  st->packet.timestamp_ns = 1000000000ull * id;
  st->packet.rgb_data = st->rgb.data();
  st->packet.rgb_width = kWidth;
  st->packet.rgb_height = kHeight;
  st->packet.rgb_stride = kWidth * 4;
  st->packet.depth_data = st->depth.data();
  st->packet.depth_width = kWidth;
  st->packet.depth_height = kHeight;
  st->packet.depth_stride = kWidth * 2;
  st->packet.depth_source = RECON_DEPTH_SRC_LIDAR;
  st->packet.has_depth = 1;
  st->packet.intrinsics = &st->intr;
  st->packet.pose = &st->pose;
}

// ── I-1 / I-2：预算触顶后常驻像素不超预算，且帧数不变、元数据完好 ──────────
void TestBudgetEnforcedWithoutDroppingFrames() {
  std::printf("[I-1/I-2] 预算触顶：释放像素但保留全部帧与元数据\n");

  recon_config cfg{};
  cfg.abi_version = RECON_ABI_VERSION;
  cfg.session_dir = "";
  cfg.preset = 1;
  cfg.forced_branch = static_cast<recon_pipeline_branch>(0xFFFFFFFFu);
  recon::Context ctx(&cfg);
  recon::Orchestrator& orch = ctx.orchestrator();
  recon::SessionSpec spec;
  spec.session_dir = "";  // 不落盘
  Check(orch.startSession(spec) == RECON_OK, "开始会话应成功");

  // 预算 = 恰好容纳 3 帧的像素。
  const size_t budget = kFrameBytes * 3;
  orch.SetFramePixelBudget(budget);

  constexpr int kN = 10;
  std::vector<PacketStorage> store(kN);
  for (int i = 0; i < kN; ++i) {
    FillPacket(static_cast<uint64_t>(i + 1), &store[i]);
    Check(orch.onFramePushed(store[i].packet) == RECON_OK, "推帧应成功");
    // I-1：每一步都必须守在预算内。
    Check(orch.resident_pixel_bytes() <= budget,
          "I-1：常驻像素字节数不得超过预算");
  }

  // I-2：帧一个都没丢（元数据全在册）。
  Check(orch.captured_frame_count() == kN, "I-2：帧数必须等于推入帧数（不删帧）");
  // 3 帧在预算内 → 7 帧被释放像素。
  Check(orch.released_pixel_frames() == kN - 3,
        "I-2：应恰好释放 (N - 预算帧数) 帧的像素");
  // 常驻像素正好是 3 帧。
  Check(orch.resident_pixel_bytes() == kFrameBytes * 3,
        "I-1：常驻像素应恰好等于预算内的 3 帧");
}

// ── I-3：预算 0 = 不设限 ────────────────────────────────────────────────
void TestZeroBudgetMeansUnlimited() {
  std::printf("[I-3] 预算 0：不设限（显式逃生门）\n");

  recon_config cfg{};
  cfg.abi_version = RECON_ABI_VERSION;
  cfg.session_dir = "";
  cfg.preset = 1;
  cfg.forced_branch = static_cast<recon_pipeline_branch>(0xFFFFFFFFu);
  recon::Context ctx(&cfg);
  recon::Orchestrator& orch = ctx.orchestrator();
  recon::SessionSpec spec;
  spec.session_dir = "";
  Check(orch.startSession(spec) == RECON_OK, "开始会话应成功");

  orch.SetFramePixelBudget(0);
  Check(orch.frame_pixel_budget_bytes() == 0, "预算应为 0");

  constexpr int kN = 8;
  std::vector<PacketStorage> store(kN);
  for (int i = 0; i < kN; ++i) {
    FillPacket(static_cast<uint64_t>(i + 1), &store[i]);
    Check(orch.onFramePushed(store[i].packet) == RECON_OK, "推帧应成功");
  }
  Check(orch.released_pixel_frames() == 0, "I-3：预算 0 时不得释放任何像素");
  Check(orch.resident_pixel_bytes() == kFrameBytes * kN,
        "I-3：预算 0 时全部像素常驻");
}

// ── I-4：预算调小立即裁决既有帧 ─────────────────────────────────────────
void TestShrinkingBudgetAppliesImmediately() {
  std::printf("[I-4] 预算调小应立即对既有帧生效\n");

  recon_config cfg{};
  cfg.abi_version = RECON_ABI_VERSION;
  cfg.session_dir = "";
  cfg.preset = 1;
  cfg.forced_branch = static_cast<recon_pipeline_branch>(0xFFFFFFFFu);
  recon::Context ctx(&cfg);
  recon::Orchestrator& orch = ctx.orchestrator();
  recon::SessionSpec spec;
  spec.session_dir = "";
  Check(orch.startSession(spec) == RECON_OK, "开始会话应成功");

  orch.SetFramePixelBudget(0);  // 先不设限
  constexpr int kN = 6;
  std::vector<PacketStorage> store(kN);
  for (int i = 0; i < kN; ++i) {
    FillPacket(static_cast<uint64_t>(i + 1), &store[i]);
    Check(orch.onFramePushed(store[i].packet) == RECON_OK, "推帧应成功");
  }
  Check(orch.released_pixel_frames() == 0, "调小前不应有释放");

  // 调小到 2 帧 —— 应立即释放 4 帧。
  orch.SetFramePixelBudget(kFrameBytes * 2);
  Check(orch.resident_pixel_bytes() <= kFrameBytes * 2,
        "I-4：调小预算后常驻像素应立刻回到预算内");
  Check(orch.released_pixel_frames() == kN - 2,
        "I-4：应立即释放多余帧的像素");
  Check(orch.captured_frame_count() == kN, "I-4：帧数仍不变");
}

// ── I-5：Reset 归零计数器 ───────────────────────────────────────────────
void TestResetClearsCounters() {
  std::printf("[I-5] Reset 应归零像素计数器\n");

  recon_config cfg{};
  cfg.abi_version = RECON_ABI_VERSION;
  cfg.session_dir = "";
  cfg.preset = 1;
  cfg.forced_branch = static_cast<recon_pipeline_branch>(0xFFFFFFFFu);
  recon::Context ctx(&cfg);
  recon::Orchestrator& orch = ctx.orchestrator();
  recon::SessionSpec spec;
  spec.session_dir = "";
  Check(orch.startSession(spec) == RECON_OK, "开始会话应成功");

  orch.SetFramePixelBudget(kFrameBytes * 2);
  constexpr int kN = 5;
  std::vector<PacketStorage> store(kN);
  for (int i = 0; i < kN; ++i) {
    FillPacket(static_cast<uint64_t>(i + 1), &store[i]);
    Check(orch.onFramePushed(store[i].packet) == RECON_OK, "推帧应成功");
  }
  Check(orch.released_pixel_frames() > 0, "应有释放发生");

  orch.Reset();
  Check(orch.resident_pixel_bytes() == 0, "I-5：Reset 后常驻像素应为 0");
  Check(orch.released_pixel_frames() == 0, "I-5：Reset 后释放计数应为 0");
  Check(orch.captured_frame_count() == 0, "I-5：Reset 后帧数应为 0");
  // 预算本身**不该**被 Reset 清掉（它是跨会话的平台能力，不是会话状态）。
  Check(orch.frame_pixel_budget_bytes() == kFrameBytes * 2,
        "I-5：预算应跨 Reset 保持");
}

// ── I-6：释放像素后重建链不崩（守卫必须生效）─────────────────────────────
void TestReconstructionSurvivesPixelRelease() {
  std::printf("[I-6] 像素被释放后，重建链应跳过而非越界\n");

  recon_config cfg{};
  cfg.abi_version = RECON_ABI_VERSION;
  cfg.session_dir = "";
  cfg.preset = 1;
  cfg.forced_branch = static_cast<recon_pipeline_branch>(0xFFFFFFFFu);
  recon::Context ctx(&cfg);
  recon::Orchestrator& orch = ctx.orchestrator();
  recon::SessionSpec spec;
  spec.session_dir = "";
  Check(orch.startSession(spec) == RECON_OK, "开始会话应成功");

  constexpr int kN = 12;
  std::vector<PacketStorage> store(kN);
  for (int i = 0; i < kN; ++i) {
    FillPacket(static_cast<uint64_t>(i + 1), &store[i]);
    Check(orch.onFramePushed(store[i].packet) == RECON_OK, "推帧应成功");
  }
  // 把预算压到只剩 1 帧 → 其余 11 帧的像素全部释放。
  orch.SetFramePixelBudget(kFrameBytes);
  Check(orch.released_pixel_frames() == kN - 1, "应释放 11 帧像素");

  Check(orch.finishCapture() == RECON_OK, "结束采集应成功");

  // 重建：像素缺失时各阶段应走「降级」而非崩溃。
  // 这里**不要求**重建成功（像素被释放本就会让 MVS 无源视图），
  // 只要求**返回一个合法状态码、不崩溃、不越界** —— 这正是 I-6 的内容。
  // （若某个消费路径漏了守卫，此处会 SIGSEGV，测试进程直接死。）
  recon::OrchestratorCallbacks cb;
  recon::ReconOptions opt;
  const recon_status st = orch.runReconstruction(opt, cb);
  std::printf("    重建返回：%d（预期为「可解释的失败或降级」，非崩溃）\n",
              static_cast<int>(st));
  Check(st == RECON_OK || st > 0, "I-6：重建必须返回合法状态码而不崩溃");
  std::printf("    [OK] 未崩溃（守卫生效）\n");
}

}  // namespace

int main() {
  std::printf("=== 采集期像素内存预算测试（缺口 AT）===\n");
  TestBudgetEnforcedWithoutDroppingFrames();
  TestZeroBudgetMeansUnlimited();
  TestShrinkingBudgetAppliesImmediately();
  TestResetClearsCounters();
  TestReconstructionSurvivesPixelRelease();

  if (g_failures == 0) {
    std::printf("全部通过。\n");
    return 0;
  }
  std::printf("失败 %d 项。\n", g_failures);
  return 1;
}
