// SPDX-License-Identifier: BSD-3-Clause
//
// 内存保护与降级器单元测试（docs/13 §3.4 / §3.5 / §4；docs/14 §5.1 内存保护行）。
//
// 逐条对应 docs/13 §4.3 的「运行时断言测试」：
//   ① 水位触发的降级只改白名单；
//   ② 黑名单 B-1..B-10 任何键值不变；
//   ③ 迟滞恢复（阈值 − 0.08 且 60 s，每 60 s 至多一级）；
//   ④ spill 记录；
//   ⑤ blacklist_touched 恒空；
//   ⑥ 白名单按预期的乘数变化（从基线重算，不叠加）。

#include "recon/memory_guard.h"
#include "recon/params.h"

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
    std::printf("  [FAIL] %s (got %.6f, want %.6f)\n", what, a, b);
    ++g_failures;
  }
}

double GetParam(const recon::ParamStore& p, const std::string& key) {
  double v = -1e9;
  const_cast<recon::ParamStore&>(p).Get(key, &v);
  return v;
}

// 快照全部参数值，用于「黑名单键值不变」断言。
std::vector<double> Snapshot(const recon::ParamStore& p) {
  std::vector<double> out;
  for (const auto& s : p.specs()) out.push_back(GetParam(p, s.key));
  return out;
}

// ① 水位分级正确。
void TestLevelClassification() {
  std::printf("[TEST] 水位分级 0.60/0.75/0.85/0.92/1.00\n");
  recon::MemoryGuard g;
  const int64_t t = 1000;
  Check(g.Update(500.0, 1000.0, t) == recon::MemoryLevel::kNormal, "0.50 → L0");
  Check(g.Update(620.0, 1000.0, t) == recon::MemoryLevel::kWarning, "0.62 → 预警");
  Check(g.Update(780.0, 1000.0, t) == recon::MemoryLevel::kReduceParallel, "0.78 → L1");
  Check(g.Update(880.0, 1000.0, t) == recon::MemoryLevel::kStrongReduce, "0.88 → L2");
  Check(g.Update(950.0, 1000.0, t) == recon::MemoryLevel::kEmergency, "0.95 → L3");
  Check(g.Update(1010.0, 1000.0, t) == recon::MemoryLevel::kOom, "1.01 → 溢出");
  Check(g.ShouldAbortForOom(), "溢出时 ShouldAbortForOom");
  CheckNear(g.water_ratio(), 1.01, 1e-6, "water_ratio 记录正确");
}

// ② 降级只改白名单；黑名单键值必须逐一不变。
void TestWhitelistOnlyAndBlacklistUntouched() {
  std::printf("[TEST] 降级只改白名单，黑名单全部不变\n");
  recon::ParamStore p = recon::BuildDefaultParams();
  p.ApplyPreset(recon::Preset::kStandard);

  // 先把白名单里属于「当前档基线」的值记录下来。
  const auto before = Snapshot(p);
  std::vector<std::string> keys_before;
  for (const auto& s : p.specs()) keys_before.push_back(s.key);

  recon::MemoryGuard g;
  g.Update(950.0, 1000.0, 1000);  // 触发 L3
  g.ApplyToParams(&p);

  // 黑名单：任何键值都不得变化。
  for (const auto& bk : recon::MemoryGuard::BlacklistKeys()) {
    double v = -1e9;
    const bool exists = p.Get(bk, &v);
    if (!exists) continue;  // 未注册的键（如 B-7）本就无值
    // 与快照中的值比对。
    for (size_t i = 0; i < p.specs().size(); ++i) {
      if (p.specs()[i].key == bk) {
        CheckNear(v, before[i], 1e-12,
                  (std::string("黑名单键不得被降级改动: ") + bk).c_str());
        break;
      }
    }
  }

  // 白名单：至少有一部分确实变了（否则「降级」是空转）。
  int changed = 0;
  for (size_t i = 0; i < p.specs().size(); ++i) {
    const double now = GetParam(p, p.specs()[i].key);
    if (std::fabs(now - before[i]) > 1e-12) ++changed;
  }
  Check(changed > 0, "L3 降级必须至少改动一个白名单键");

  // 所有发生变化的键必须都在白名单内。
  const auto& wl = recon::MemoryGuard::WhitelistKeys();
  for (size_t i = 0; i < p.specs().size(); ++i) {
    const double now = GetParam(p, p.specs()[i].key);
    if (std::fabs(now - before[i]) <= 1e-12) continue;
    bool in_wl = false;
    for (const auto& w : wl) {
      if (p.specs()[i].key == w) { in_wl = true; break; }
    }
    Check(in_wl, (std::string("被改动的键必须在白名单内: ") + p.specs()[i].key).c_str());
  }
}

// ③ 从基线重算（不叠加）：多次触发 L3 后值必须稳定，不持续衰减。
void TestBaselineRecalcNotCompounding() {
  std::printf("[TEST] 从基线重算，不叠加衰减\n");
  recon::ParamStore p = recon::BuildDefaultParams();
  p.ApplyPreset(recon::Preset::kStandard);

  double baseline = 0.0;
  p.Get(recon::keys::kPerfGpuBatchSize, &baseline);

  recon::MemoryGuard g;
  g.Update(950.0, 1000.0, 1000);
  g.ApplyToParams(&p);
  const double after1 = GetParam(p, recon::keys::kPerfGpuBatchSize);

  g.Update(960.0, 1000.0, 1100);
  g.ApplyToParams(&p);
  const double after2 = GetParam(p, recon::keys::kPerfGpuBatchSize);

  CheckNear(after1, after2, 1e-9, "重复触发 L3 不得继续衰减（非叠加）");
  Check(after1 < baseline, "L3 必须低于基线");
  CheckNear(after1, baseline * 0.5, 1e-6, "L3 批大小 = 基线 ×0.5");
}

// ④ 迟滞恢复：需降到「当前级别下界 − 0.08」以下并持续 60 s，且每 60 s 至多一级。
void TestHysteresisRecovery() {
  std::printf("[TEST] 迟滞恢复（−0.08 且 60 s，逐级）\n");
  recon::MemoryGuard g;
  const int64_t t0 = 10000;
  g.Update(950.0, 1000.0, t0);  // → L3（门限 0.92）
  Check(g.level() == recon::MemoryLevel::kEmergency, "先到 L3");

  // 水位降到 0.90：高于 L3 门限（0.92−0.08=0.84）→ 不恢复。
  g.Update(900.0, 1000.0, t0 + 1000);
  Check(g.level() == recon::MemoryLevel::kEmergency, "0.90 高于 L3 门限(0.84)，不恢复");

  // 降到 0.70（< 0.84），但未满 60 s → 仍不恢复。
  g.Update(700.0, 1000.0, t0 + 2000);
  Check(g.level() == recon::MemoryLevel::kEmergency, "低水位但未满 60 s，不恢复");

  // 持续 61 s 后 → 恢复一级到 L2。
  g.Update(700.0, 1000.0, t0 + 2000 + 61000);
  Check(g.level() == recon::MemoryLevel::kStrongReduce, "满 60 s 后恢复一级到 L2");

  // 立即再满足条件 → 因 60 s 限速，不得再恢复。
  g.Update(700.0, 1000.0, t0 + 2000 + 62000);
  Check(g.level() == recon::MemoryLevel::kStrongReduce, "60 s 内不得再恢复一级");

  // 再过 61 s → L2 门限 0.85−0.08=0.77，水位 0.70 满足 → 恢复一级到 L1。
  g.Update(700.0, 1000.0, t0 + 2000 + 62000 + 61000);
  Check(g.level() == recon::MemoryLevel::kReduceParallel, "再过 60 s 恢复一级到 L1");

  // L1 门限 0.75−0.08=0.67，水位 0.70 不满足 → 停住（不冒进恢复）。
  g.Update(700.0, 1000.0, t0 + 2000 + 62000 + 61000 + 61000);
  Check(g.level() == recon::MemoryLevel::kReduceParallel,
        "水位高于 L1 门限(0.67)时停住不恢复");
}

// ⑤ 平台内存告警叠加（取最严，不平移水位级别）。
void TestPlatformWarning() {
  std::printf("[TEST] 平台内存告警按严重度叠加\n");
  recon::MemoryGuard g;
  g.Update(300.0, 1000.0, 1000);  // L0
  Check(g.effective_level() == recon::MemoryLevel::kNormal, "无告警时 L0");
  g.NotifyPlatformWarning(0, 2000);
  Check(g.effective_level() == recon::MemoryLevel::kStrongReduce, "低告警 → 有效 L2");
  Check(g.level() == recon::MemoryLevel::kNormal, "水位级别本身不变");
  g.ClearPlatformWarning(3000);
  Check(g.effective_level() == recon::MemoryLevel::kNormal, "告警解除后回到 L0");
}

// ⑥ 降级记录与 summary：reason 受控枚举、blacklist_touched 恒空、精度影响分级。
void TestDegradationRecording() {
  std::printf("[TEST] 降级记录 / summary / blacklist_touched 恒空\n");
  recon::MemoryGuard g;
  g.Update(950.0, 1000.0, 1000);
  g.AddDegradation("S2_poisson", recon::DegradeReason::kChunkedPoisson,
                   "poisson_chunk_count=4, D=11", 1200);
  g.AddDegradation("S9_bake", recon::DegradeReason::kMemoryHighWater,
                   "rss=1840MB, budget=2200MB", 1300);

  Check(g.degradations().size() == 2, "记录条数正确");
  Check(std::string(recon::DegradeReasonName(g.degradations()[0].reason)) ==
            "chunked_poisson",
        "reason 受控枚举名正确");
  Check(std::string(g.degradations()[0].precision_impact) == "seam_local",
        "分块泊松精度影响 = seam_local");
  Check(std::string(g.degradations()[1].precision_impact) == "none",
        "水位降级精度影响 = none");

  const auto s = g.summary(2000);
  Check(s.max_level == 3, "summary.max_level 取最严 = 3");
  Check(s.blacklist_touched.empty(), "blacklist_touched 必须恒为空");
  Check(std::string(s.precision_impact) == "seam_local", "summary 精度影响取最严");
  Check(s.duration_ms == 1000, "duration_ms = now − first_degrade");

  // 若发生黑名单触碰，summary 必须如实暴露（供 P0 断言）。
  g.RecordBlacklistTouch("rgbd_tsdf_voxel_size");
  Check(g.summary(2000).blacklist_touched.size() == 1,
        "黑名单触碰必须被记录并暴露");
}

// ⑦ 白名单与黑名单集合本身正确（不重叠、非空）。
void TestKeySetsDisjoint() {
  std::printf("[TEST] 白/黑名单集合不重叠\n");
  const auto& wl = recon::MemoryGuard::WhitelistKeys();
  const auto& bl = recon::MemoryGuard::BlacklistKeys();
  Check(!wl.empty() && !bl.empty(), "两个集合都非空");
  for (const auto& w : wl) {
    for (const auto& b : bl) {
      Check(w != b, (std::string("键不得同时在白名单与黑名单: ") + w).c_str());
    }
  }
}

// ⑧ 溢出：w ≥ 1.00 时 ShouldAbortForOom 为真（供上层抛 OUT_OF_MEMORY）。
void TestOomSignal() {
  std::printf("[TEST] 溢出信号\n");
  recon::MemoryGuard g;
  g.Update(999.0, 1000.0, 1000);
  Check(!g.ShouldAbortForOom(), "0.999 未溢出");
  g.Update(1000.0, 1000.0, 1100);
  Check(g.ShouldAbortForOom(), "1.00 溢出");
}

}  // namespace

int main() {
  std::printf("=== 内存保护与降级器单元测试 ===\n");
  TestLevelClassification();
  TestWhitelistOnlyAndBlacklistUntouched();
  TestBaselineRecalcNotCompounding();
  TestHysteresisRecovery();
  TestPlatformWarning();
  TestDegradationRecording();
  TestKeySetsDisjoint();
  TestOomSignal();

  if (g_failures == 0) {
    std::printf("全部通过。\n");
    return 0;
  }
  std::printf("失败 %d 项。\n", g_failures);
  return 1;
}
