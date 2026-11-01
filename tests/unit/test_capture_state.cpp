// SPDX-License-Identifier: BSD-3-Clause
//
// 采集状态机单元测试（docs/09 §4.1/§4.2；docs/14 §5.1 平台适配层）。
//
// 逐条对应 docs/09 §4.2 的迁移表 T-01..T-30：
//   ① 每条合法迁移的「下一状态」与文档一致；
//   ② 非法 (state, event) 组合被拒绝且状态不变；
//   ③ T-14 切后台的「暂停」变体语义；
//   ④ 终态 Cancelled 无出边；
//   ⑤ T-24 按失败阶段路由重试；
//   ⑥ 正常主流程 Idle→…→Completed 全链路。

#include "recon_platform/capture_state.h"

#include <cstdio>
#include <string>

using recon::platform::CaptureEvent;
using recon::platform::CaptureState;
using recon::platform::CaptureStateMachine;
using recon::platform::FailureStage;

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::printf("  [FAIL] %s\n", what);
    ++g_failures;
  }
}

void CheckState(CaptureStateMachine& m, CaptureState want, const char* what) {
  if (m.state() != want) {
    std::printf("  [FAIL] %s (got %s, want %s)\n", what,
                CaptureStateMachine::StateName(m.state()),
                CaptureStateMachine::StateName(want));
    ++g_failures;
  }
}

// 应用事件并断言「被接受」与「下一状态」。
void Expect(CaptureStateMachine& m, CaptureEvent ev, CaptureState want,
            const char* rule, const char* what) {
  const auto r = m.Apply(ev);
  if (!r.accepted) {
    std::printf("  [FAIL] %s: 迁移被拒（应接受，规则 %s）\n", what, rule);
    ++g_failures;
    return;
  }
  if (m.state() != want) {
    std::printf("  [FAIL] %s: 状态 %s，应为 %s（规则 %s）\n", what,
                CaptureStateMachine::StateName(m.state()),
                CaptureStateMachine::StateName(want), rule);
    ++g_failures;
    return;
  }
  if (r.rule == nullptr || std::string(r.rule) != rule) {
    std::printf("  [FAIL] %s: 规则编号为 %s，应为 %s\n", what,
                r.rule ? r.rule : "(null)", rule);
    ++g_failures;
  }
}

void ExpectReject(CaptureStateMachine& m, CaptureEvent ev, const char* what) {
  const CaptureState before = m.state();
  const auto r = m.Apply(ev);
  if (r.accepted) {
    std::printf("  [FAIL] %s: 非法迁移被接受（%s → %s）\n", what,
                CaptureStateMachine::StateName(before),
                CaptureStateMachine::StateName(m.state()));
    ++g_failures;
    return;
  }
  if (m.state() != before) {
    std::printf("  [FAIL] %s: 非法迁移改变了状态\n", what);
    ++g_failures;
  }
}

// ------------------------------------------------------------------------- //
// ① 主流程全链路：Idle → CapabilityProbe → RequestingPermission →
//    Previewing → Capturing → Reviewing → Processing → Completed
// ------------------------------------------------------------------------- //
void TestHappyPath() {
  std::printf("[TEST] 主流程全链路\n");
  CaptureStateMachine m;
  CheckState(m, CaptureState::kIdle, "初始为 Idle");

  Expect(m, CaptureEvent::kStartScan, CaptureState::kCapabilityProbe, "T-01", "开始扫描");
  Expect(m, CaptureEvent::kProbeComplete, CaptureState::kRequestingPermission, "T-02", "探测完成");
  Expect(m, CaptureEvent::kPermissionsGranted, CaptureState::kPreviewing, "T-05", "权限授予");
  Expect(m, CaptureEvent::kUserStart, CaptureState::kCapturing, "T-09", "用户点开始");
  Expect(m, CaptureEvent::kFinishScan, CaptureState::kReviewing, "T-11", "完成扫描");
  Expect(m, CaptureEvent::kUserConfirmReconstruct, CaptureState::kProcessing, "T-17", "确认重建");
  Expect(m, CaptureEvent::kReconstructionSucceeded, CaptureState::kCompleted, "T-20", "重建成功");
}

// ------------------------------------------------------------------------- //
// ② 权限分支：T-03 / T-04 / T-06 / T-07 / T-08
// ------------------------------------------------------------------------- //
void TestPermissionBranches() {
  std::printf("[TEST] 权限分支\n");

  {  // T-03 相机不可用
    CaptureStateMachine m;
    m.Apply(CaptureEvent::kStartScan);
    Expect(m, CaptureEvent::kProbeCameraUnavailable, CaptureState::kFailed, "T-03", "相机不可用");
    Check(m.failure_stage() == FailureStage::kCamera, "T-03 记录失败阶段=相机");
  }
  {  // T-04 内部异常：保持探测态（调用方决定降级重试）
    CaptureStateMachine m;
    m.Apply(CaptureEvent::kStartScan);
    Expect(m, CaptureEvent::kProbeInternalError, CaptureState::kCapabilityProbe, "T-04", "探测内部异常");
  }
  {  // T-06 相机权限被拒
    CaptureStateMachine m;
    m.Apply(CaptureEvent::kStartScan);
    m.Apply(CaptureEvent::kProbeComplete);
    Expect(m, CaptureEvent::kCameraPermissionDenied, CaptureState::kFailed, "T-06", "相机权限被拒");
    Check(m.failure_stage() == FailureStage::kCamera, "T-06 记录失败阶段=相机");
  }
  {  // T-07 运动权限被拒：继续进 Previewing
    CaptureStateMachine m;
    m.Apply(CaptureEvent::kStartScan);
    m.Apply(CaptureEvent::kProbeComplete);
    Expect(m, CaptureEvent::kMotionPermissionDenied, CaptureState::kPreviewing, "T-07", "运动权限被拒");
  }
  {  // T-08 相册权限被拒：状态不变
    CaptureStateMachine m;
    m.Apply(CaptureEvent::kStartScan);
    m.Apply(CaptureEvent::kProbeComplete);
    Expect(m, CaptureEvent::kPhotoPermissionDenied, CaptureState::kRequestingPermission, "T-08", "相册权限被拒");
  }
}

// ------------------------------------------------------------------------- //
// ③ 取消路径：T-10 / T-12 / T-19
// ------------------------------------------------------------------------- //
void TestCancellationPaths() {
  std::printf("[TEST] 取消路径\n");
  {  // T-10 从预览取消
    CaptureStateMachine m;
    m.Apply(CaptureEvent::kStartScan);
    m.Apply(CaptureEvent::kProbeComplete);
    m.Apply(CaptureEvent::kPermissionsGranted);
    Expect(m, CaptureEvent::kUserCancelFromPreview, CaptureState::kCancelled, "T-10", "预览取消");
  }
  {  // T-12 采集期返回手势 → Reviewing（承载确认弹窗）
    CaptureStateMachine m;
    m.Apply(CaptureEvent::kStartScan);
    m.Apply(CaptureEvent::kProbeComplete);
    m.Apply(CaptureEvent::kPermissionsGranted);
    m.Apply(CaptureEvent::kUserStart);
    Expect(m, CaptureEvent::kCancelGesture, CaptureState::kReviewing, "T-12", "返回手势");
  }
  {  // T-19 Reviewing 期取消
    CaptureStateMachine m;
    m.Apply(CaptureEvent::kStartScan);
    m.Apply(CaptureEvent::kProbeComplete);
    m.Apply(CaptureEvent::kPermissionsGranted);
    m.Apply(CaptureEvent::kUserStart);
    m.Apply(CaptureEvent::kFinishScan);
    Expect(m, CaptureEvent::kReviewCancel, CaptureState::kCancelled, "T-19", "Reviewing 取消");
  }
  {  // T-13 帧数达上限 / 磁盘不足 → Reviewing
    CaptureStateMachine m;
    m.Apply(CaptureEvent::kStartScan);
    m.Apply(CaptureEvent::kProbeComplete);
    m.Apply(CaptureEvent::kPermissionsGranted);
    m.Apply(CaptureEvent::kUserStart);
    Expect(m, CaptureEvent::kFrameCapOrDiskLow, CaptureState::kReviewing, "T-13", "帧数上限/磁盘不足");
  }
  {  // T-15 相机被占用 → Failed（记录失败阶段=相机）
    CaptureStateMachine m;
    m.Apply(CaptureEvent::kStartScan);
    m.Apply(CaptureEvent::kProbeComplete);
    m.Apply(CaptureEvent::kPermissionsGranted);
    m.Apply(CaptureEvent::kUserStart);
    Expect(m, CaptureEvent::kCameraBusy, CaptureState::kFailed, "T-15", "相机被占用");
    Check(m.failure_stage() == FailureStage::kCamera, "T-15 记录失败阶段=相机");
  }
}

// ------------------------------------------------------------------------- //
// ④ T-14 暂停变体（状态不变 + paused 标志）
// ------------------------------------------------------------------------- //
void TestBackgroundPause() {
  std::printf("[TEST] 切后台暂停变体（T-14）\n");
  CaptureStateMachine m;
  m.Apply(CaptureEvent::kStartScan);
  m.Apply(CaptureEvent::kProbeComplete);
  m.Apply(CaptureEvent::kPermissionsGranted);
  m.Apply(CaptureEvent::kUserStart);
  Check(!m.paused(), "采集期默认不暂停");

  Expect(m, CaptureEvent::kAppBackgrounded, CaptureState::kCapturing, "T-14", "切后台");
  Check(m.paused(), "T-14 置 paused=true");

  // 回前台：清暂停（docs/09 §4.2 无独立事件，状态不变）
  m.clear_paused();
  Check(!m.paused(), "回前台 clear_paused");
  CheckState(m, CaptureState::kCapturing, "回前台后仍是 Capturing");
}

// ------------------------------------------------------------------------- //
// ⑤ T-24 按失败阶段路由重试
// ------------------------------------------------------------------------- //
void TestRetryRouting() {
  std::printf("[TEST] T-24 按失败阶段路由\n");
  {  // 相机失败 → RequestingPermission
    CaptureStateMachine m;
    m.Apply(CaptureEvent::kStartScan);
    m.Apply(CaptureEvent::kProbeCameraUnavailable);  // 失败阶段=相机
    Expect(m, CaptureEvent::kRetry, CaptureState::kRequestingPermission, "T-24", "相机重试");
  }
  {  // 重建失败 → Processing
    CaptureStateMachine m;
    m.Apply(CaptureEvent::kStartScan);
    m.Apply(CaptureEvent::kProbeComplete);
    m.Apply(CaptureEvent::kPermissionsGranted);
    m.Apply(CaptureEvent::kUserStart);
    m.Apply(CaptureEvent::kFinishScan);
    m.Apply(CaptureEvent::kUserConfirmReconstruct);
    m.Apply(CaptureEvent::kReconstructionFailed);  // 失败阶段=重建
    Expect(m, CaptureEvent::kRetry, CaptureState::kProcessing, "T-24", "重建重试");
  }
  {  // 标定失败 → Previewing（由调用方显式设置阶段）
    CaptureStateMachine m;
    m.Apply(CaptureEvent::kStartScan);
    m.Apply(CaptureEvent::kProbeCameraUnavailable);  // T-03 合法进入 Failed
    m.set_failure_stage(FailureStage::kCalibration);
    Expect(m, CaptureEvent::kRetry, CaptureState::kPreviewing, "T-24", "标定重试");
  }
  {  // T-25 回首页
    CaptureStateMachine m;
    m.Apply(CaptureEvent::kStartScan);
    m.Apply(CaptureEvent::kProbeCameraUnavailable);
    Expect(m, CaptureEvent::kBackToHome, CaptureState::kIdle, "T-25", "回首页");
    Check(m.failure_stage() == FailureStage::kNone, "T-25 清失败阶段");
  }
}

// ------------------------------------------------------------------------- //
// ⑥ 终态与无副作用事件
// ------------------------------------------------------------------------- //
void TestTerminalAndNoOps() {
  std::printf("[TEST] 终态与无副作用事件\n");
  {  // Cancelled 是终态，任何事件都拒绝
    CaptureStateMachine m;
    m.Apply(CaptureEvent::kStartScan);
    m.Apply(CaptureEvent::kProbeCameraUnavailable);
    m.Apply(CaptureEvent::kRetry);
    m.Apply(CaptureEvent::kCameraPermissionDenied);
    CheckState(m, CaptureState::kFailed, "先行至 Failed");
    // 构造一个 Cancelled 态
    CaptureStateMachine c;
    c.Apply(CaptureEvent::kStartScan);
    c.Apply(CaptureEvent::kProbeComplete);
    c.Apply(CaptureEvent::kPermissionsGranted);
    c.Apply(CaptureEvent::kUserCancelFromPreview);
    CheckState(c, CaptureState::kCancelled, "到达 Cancelled");
    ExpectReject(c, CaptureEvent::kStartScan, "Cancelled 无出边");
    ExpectReject(c, CaptureEvent::kRetry, "Cancelled 拒绝重试");
  }
  {  // T-26 导出模型不变更状态；T-28 设置不变更状态；T-29 参数不变更
    CaptureStateMachine m;
    m.Apply(CaptureEvent::kStartScan);
    m.Apply(CaptureEvent::kProbeComplete);
    m.Apply(CaptureEvent::kPermissionsGranted);
    m.Apply(CaptureEvent::kUserStart);
    m.Apply(CaptureEvent::kFinishScan);
    m.Apply(CaptureEvent::kUserConfirmReconstruct);
    m.Apply(CaptureEvent::kReconstructionSucceeded);  // Completed
    Expect(m, CaptureEvent::kExportModel, CaptureState::kCompleted, "T-26", "导出模型");
    Expect(m, CaptureEvent::kOpenSettings, CaptureState::kCompleted, "T-28", "设置");
    Expect(m, CaptureEvent::kRecapture, CaptureState::kPreviewing, "T-27", "重新采集");
  }
  {  // T-16 自检 severe 未通过：保持 Reviewing；T-18 继续扫描
    CaptureStateMachine m;
    m.Apply(CaptureEvent::kStartScan);
    m.Apply(CaptureEvent::kProbeComplete);
    m.Apply(CaptureEvent::kPermissionsGranted);
    m.Apply(CaptureEvent::kUserStart);
    m.Apply(CaptureEvent::kFinishScan);
    Expect(m, CaptureEvent::kSelfCheckSevereFail, CaptureState::kReviewing, "T-16", "severe 未过");
    Expect(m, CaptureEvent::kUserContinueScan, CaptureState::kCapturing, "T-18", "继续扫描");
  }
  {  // T-30 Completed → 改 C 类参数重建 → Processing
    CaptureStateMachine m;
    m.Apply(CaptureEvent::kStartScan);
    m.Apply(CaptureEvent::kProbeComplete);
    m.Apply(CaptureEvent::kPermissionsGranted);
    m.Apply(CaptureEvent::kUserStart);
    m.Apply(CaptureEvent::kFinishScan);
    m.Apply(CaptureEvent::kUserConfirmReconstruct);
    m.Apply(CaptureEvent::kReconstructionSucceeded);
    Expect(m, CaptureEvent::kModifyClassCAndRebuild, CaptureState::kProcessing, "T-30", "改 C 类重建");
  }
}

// ------------------------------------------------------------------------- //
// ⑦ 非法迁移：Idle 收到 Capturing 专属事件应被拒
// ------------------------------------------------------------------------- //
void TestIllegalTransitions() {
  std::printf("[TEST] 非法迁移拒绝\n");
  CaptureStateMachine m;  // Idle
  ExpectReject(m, CaptureEvent::kFinishScan, "Idle 收到 kFinishScan");
  ExpectReject(m, CaptureEvent::kReconstructionSucceeded, "Idle 收到 kReconstructionSucceeded");
  ExpectReject(m, CaptureEvent::kUserConfirmReconstruct, "Idle 收到 kUserConfirmReconstruct");
  CheckState(m, CaptureState::kIdle, "非法迁移后仍在 Idle");
}

}  // namespace

int main() {
  std::printf("=== 采集状态机单元测试（docs/09 §4.1/§4.2）===\n");
  TestHappyPath();
  TestPermissionBranches();
  TestCancellationPaths();
  TestBackgroundPause();
  TestRetryRouting();
  TestTerminalAndNoOps();
  TestIllegalTransitions();

  if (g_failures == 0) {
    std::printf("全部通过。\n");
    return 0;
  }
  std::printf("失败 %d 项。\n", g_failures);
  return 1;
}
