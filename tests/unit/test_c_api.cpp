// SPDX-License-Identifier: BSD-3-Clause
//
// 能力探测与 C ABI 单元测试。
//
// 验证「两条分支平级」的判定逻辑：有深度走 RGBD，无深度走 MONO，
// 且 MONO 分支不是降级态（见 docs/00 §4.5）。

#include "recon/context.h"
#include "recon/recon_c_api.h"

#include <cmath>
#include <cstdio>
#include <cstring>

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
    std::printf("  [FAIL] %s (got %.6f, want %.6f)\n", what, a, b);
    ++g_failures;
  }
}

// 可切换的平台探测桩。
recon::PlatformProbe g_stub;

recon::PlatformProbe StubProbe() { return g_stub; }

void TestBranchSelection() {
  std::printf("[TEST] 管线分支判定\n");
  recon::SetPlatformProbeProvider(&StubProbe);

  // 无深度 → MONO。
  g_stub = recon::PlatformProbe{};
  g_stub.has_camera = true;
  g_stub.has_imu = true;
  {
    recon::Context ctx(nullptr);
    ctx.ProbeCapability();
    Check(ctx.capability().branch == RECON_BRANCH_MONO, "无深度应走 MONO 分支");
    Check(ctx.capability().depth_tier == RECON_DEPTH_NONE, "深度等级应为 NONE");
  }

  // LiDAR → RGBD，深度等级 LIDAR。
  g_stub.has_lidar = true;
  {
    recon::Context ctx(nullptr);
    ctx.ProbeCapability();
    Check(ctx.capability().branch == RECON_BRANCH_RGBD, "LiDAR 应走 RGBD 分支");
    Check(ctx.capability().depth_tier == RECON_DEPTH_LIDAR, "深度等级应为 LIDAR");
  }

  // ToF → RGBD，深度等级 TOF。
  g_stub = recon::PlatformProbe{};
  g_stub.has_tof = true;
  {
    recon::Context ctx(nullptr);
    ctx.ProbeCapability();
    Check(ctx.capability().depth_tier == RECON_DEPTH_TOF, "深度等级应为 TOF");
    Check(ctx.capability().branch == RECON_BRANCH_RGBD, "ToF 应走 RGBD 分支");
  }

  // 运动推断深度 → RGBD，深度等级 ALGO。
  g_stub = recon::PlatformProbe{};
  g_stub.has_algo_depth = true;
  {
    recon::Context ctx(nullptr);
    ctx.ProbeCapability();
    Check(ctx.capability().depth_tier == RECON_DEPTH_ALGO, "深度等级应为 ALGO");
    Check(ctx.capability().branch == RECON_BRANCH_RGBD,
          "运动深度也应走 RGBD 分支（提供弱纹理区覆盖）");
  }
}

void TestCApiLifecycle() {
  std::printf("[TEST] C ABI 生命周期\n");
  recon::SetPlatformProbeProvider(&StubProbe);
  g_stub = recon::PlatformProbe{};
  g_stub.has_camera = true;

  void* ctx = nullptr;
  Check(recon_create(nullptr, &ctx) == RECON_OK, "create 应成功");
  Check(ctx != nullptr, "context 不应为空");
  Check(recon_abi_version() == RECON_ABI_VERSION, "ABI 版本应一致");

  recon_capability cap{};
  Check(recon_probe_capability(ctx, &cap) == RECON_OK, "探测应成功");
  Check(cap.has_camera == 1, "应识别到相机");

  // 非法参数。
  Check(recon_probe_capability(nullptr, &cap) == RECON_ERR_INVALID_ARGUMENT,
        "空 context 应返回 INVALID_ARGUMENT");
  Check(recon_create(nullptr, nullptr) == RECON_ERR_INVALID_ARGUMENT,
        "空 out 应返回 INVALID_ARGUMENT");

  recon_destroy(ctx);
}

void TestVersionMismatch() {
  std::printf("[TEST] ABI 版本校验\n");
  recon_config cfg{};
  cfg.abi_version = RECON_ABI_VERSION + 1;
  void* ctx = nullptr;
  Check(recon_create(&cfg, &ctx) == RECON_ERR_VERSION_MISMATCH,
        "版本不匹配应被拒绝");
  Check(ctx == nullptr, "失败时不应分配 context");
}

void TestParamThroughCApi() {
  std::printf("[TEST] 参数读写（C ABI）\n");
  void* ctx = nullptr;
  Check(recon_create(nullptr, &ctx) == RECON_OK, "create 应成功");

  Check(recon_set_param(ctx, recon::keys::kVioFeatureCount, 300, 0.0) == RECON_OK,
        "设置整数参数应成功");

  int32_t iv = 0;
  double dv = 0.0;
  Check(recon_get_param(ctx, recon::keys::kVioFeatureCount, &iv, &dv) == RECON_OK,
        "读取参数应成功");
  Check(iv == 300, "整数参数值应正确");

  Check(recon_set_param(ctx, "no.such.key", 1, 0.0) == RECON_ERR_INVALID_ARGUMENT,
        "未知键应报错");

  // 预设应用。
  Check(recon_apply_preset(ctx, 2) == RECON_OK, "应用高精度预设应成功");
  Check(recon_get_param(ctx, recon::keys::kMeshPoissonDepth, &iv, &dv) == RECON_OK,
        "读取泊松深度应成功");
  Check(iv == 14, "高精度档泊松深度应为 14");

  // 参数导出。
  char* json = nullptr;
  Check(recon_export_params(ctx, &json) == RECON_OK, "导出参数应成功");
  Check(json != nullptr && std::strlen(json) > 0, "导出内容不应为空");
  recon_free_string(json);

  recon_destroy(ctx);
}

void TestCancel() {
  std::printf("[TEST] 取消语义\n");
  void* ctx = nullptr;
  recon_create(nullptr, &ctx);

  Check(recon_is_cancelled(ctx) == 0, "初始不应为已取消");
  Check(recon_cancel(ctx) == RECON_OK, "取消应成功");
  Check(recon_is_cancelled(ctx) == 1, "取消后标志应为真");

  recon_destroy(ctx);
}

}  // namespace

int main() {
  std::printf("=== recon 能力探测与 C ABI 测试 ===\n");
  TestBranchSelection();
  TestCApiLifecycle();
  TestVersionMismatch();
  TestParamThroughCApi();
  TestCancel();

  if (g_failures == 0) {
    std::printf("\n全部通过\n");
    return 0;
  }
  std::printf("\n失败 %d 项\n", g_failures);
  return 1;
}
