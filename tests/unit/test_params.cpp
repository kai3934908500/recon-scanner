// SPDX-License-Identifier: BSD-3-Clause
//
// 参数系统单元测试。
//
// 重点验证 R-PREC-6：越界写入夹取到合法区间，**不回退到默认值**。

#include "recon/params.h"

#include <cmath>
#include <cstdio>
#include <string>

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

void TestRegistryIntegrity() {
  std::printf("[TEST] 参数注册表完整性\n");
  recon::ParamStore p = recon::BuildDefaultParams();

  // 键名唯一。
  const auto& specs = p.specs();
  for (size_t i = 0; i < specs.size(); ++i) {
    for (size_t j = i + 1; j < specs.size(); ++j) {
      Check(specs[i].key != specs[j].key, "参数键名必须唯一");
    }
  }

  // 每个参数的预设值必须在合法区间内。
  for (const auto& s : specs) {
    Check(s.min_value <= s.max_value, "min 必须 <= max");
    Check(s.default_value >= s.min_value && s.default_value <= s.max_value,
          "默认值必须在区间内");
    Check(s.fast_value >= s.min_value && s.fast_value <= s.max_value,
          "快速预设必须在区间内");
    Check(s.standard_value >= s.min_value && s.standard_value <= s.max_value,
          "标准预设必须在区间内");
    Check(s.high_precision_value >= s.min_value && s.high_precision_value <= s.max_value,
          "高精度预设必须在区间内");
  }

  // UI 可见性状态必须落在三个合法组合之一：是 / 专家 / 否。
  //
  // 为什么必须锁（2026-10-31 实测缺陷）：`user_adjustable` 与 `expert_only`
  // 语义互斥 —— 前者是「UI 常规面板暴露」，后者是「仅展开专家区才暴露」。
  // 二者同时为真没有合法含义，且会让 docs/09 §3.8.5「专家模式显示 10 个参数」
  // 的语义失效（因为参数在普通模式也已可见）。
  //
  // 实测曾恰好有 3 处落在非法组合 `(true, true)`：
  //   sfm_feature_edge_threshold / sfm_ransac_confidence /
  //   sfm_loop_similarity_scale_tolerance
  // 这三处的代码注释自写「专家项」、docs/08 第 177/183/392 行标「专家」，
  // 三个独立信号一致指向代码写错。已修正为 `(false, true)`。
  //
  // 本断言即「状态空间闸门」：将来任何人再写出 `(true, true)` 都会在此失败，
  // 而不是静默地让专家开关失效。
  for (const auto& s : specs) {
    Check(!(s.user_adjustable && s.expert_only),
          "user_adjustable 与 expert_only 不得同时为真（违反者为第一个失败项）");
  }
}

void TestClampNotFallback() {
  std::printf("[TEST] R-PREC-6：越界夹取而非回退\n");
  recon::ParamStore p = recon::BuildDefaultParams();

  // 高精度档下，把体素边长压到允许下界。
  p.ApplyPreset(recon::Preset::kHighPrecision);

  double voxel = 0.0;
  p.Get(recon::keys::kRgbdTsdfVoxelSize, &voxel);
  const double high_preset_voxel = voxel;

  // **从注册表读取合法区间**，不硬编码边界。
  //
  // 为什么：此前本测试硬编码 `0.005` / `0.0002`，而 docs/04 §7.1 与
  // docs/08 G6 的权威区间是 `[0.00025, 0.02]`。硬编码使本测试**绕过注册表**
  // 自证「正确」，区间实际漂移时它照样全绿 —— 这正是 `trim_confidence_quantile`
  // 被写反却未被单测发现的同类隐患。改为读规格即从根上消除。
  const recon::ParamSpec* spec = nullptr;
  for (const auto& s : p.specs()) {
    if (s.key == recon::keys::kRgbdTsdfVoxelSize) {
      spec = &s;
      break;
    }
  }
  Check(spec != nullptr, "体素边长参数应已注册");
  if (spec == nullptr) return;
  const double lo = spec->min_value;
  const double hi = spec->max_value;
  Check(lo < hi, "体素边长区间必须有效");

  // 写入超界值，应夹取到上界，而不是回退到预设值。
  Check(p.Set(recon::keys::kRgbdTsdfVoxelSize, 999.0), "Set 应成功");
  p.Get(recon::keys::kRgbdTsdfVoxelSize, &voxel);
  CheckNear(voxel, hi, 1e-9, "越界值应夹取到注册表上界");
  Check(std::fabs(voxel - high_preset_voxel) > 1e-9,
        "越界值不得回退到预设值");

  // 写入负值，应夹取到下界。
  Check(p.Set(recon::keys::kRgbdTsdfVoxelSize, -1.0), "Set 应成功");
  p.Get(recon::keys::kRgbdTsdfVoxelSize, &voxel);
  CheckNear(voxel, lo, 1e-9, "负值应夹取到注册表下界");

  // NaN 应被确定性处理（夹取到下界），不得产生 NaN。
  const double nan_val = std::nan("");
  Check(p.Set(recon::keys::kRgbdTsdfVoxelSize, nan_val), "NaN Set 应成功");
  p.Get(recon::keys::kRgbdTsdfVoxelSize, &voxel);
  Check(std::isfinite(voxel), "NaN 输入后参数必须有限");
}

void TestPresetOrdering() {
  std::printf("[TEST] 精度档位单调性\n");
  recon::ParamStore p = recon::BuildDefaultParams();

  // 关键精度参数：高精度档必须最激进。
  struct Case {
    const char* key;
    bool higher_is_better;
  };
  const Case cases[] = {
      {recon::keys::kVioFeatureCount, true},
      {recon::keys::kSfmBaMaxIterations, true},
      {recon::keys::kMeshPoissonDepth, true},
      {recon::keys::kMvsNumViews, true},
      {recon::keys::kMvsSubpixelIterations, true},
      {recon::keys::kRgbdTsdfVoxelSize, false},
      {recon::keys::kMvsFusionVoxelSizeM, false},
      {recon::keys::kCapIso, false},
  };

  for (const auto& c : cases) {
    p.ApplyPreset(recon::Preset::kFast);
    double fast_v = 0.0;
    p.Get(c.key, &fast_v);

    p.ApplyPreset(recon::Preset::kStandard);
    double std_v = 0.0;
    p.Get(c.key, &std_v);

    p.ApplyPreset(recon::Preset::kHighPrecision);
    double high_v = 0.0;
    p.Get(c.key, &high_v);

    if (c.higher_is_better) {
      Check(high_v >= std_v, "高精度档数值应 >= 标准档");
      Check(std_v >= fast_v, "标准档数值应 >= 快速档");
    } else {
      Check(high_v <= std_v, "高精度档数值应 <= 标准档");
      Check(std_v <= fast_v, "标准档数值应 <= 快速档");
    }
  }
}

void TestUserOverridePreserved() {
  std::printf("[TEST] 用户覆盖不被预设重置\n");
  recon::ParamStore p = recon::BuildDefaultParams();

  p.Set(recon::keys::kVioFeatureCount, 777);
  p.ApplyPreset(recon::Preset::kHighPrecision);

  double v = 0.0;
  p.Get(recon::keys::kVioFeatureCount, &v);
  CheckNear(v, 777.0, 1e-9, "用户覆盖值应被保留");

  // 重置后回到预设值。
  Check(p.ResetToPreset(recon::keys::kVioFeatureCount), "ResetToPreset 应成功");
  p.Get(recon::keys::kVioFeatureCount, &v);
  Check(v != 777.0, "重置后应回到预设值");
}

void TestBoolAndIntCoercion() {
  std::printf("[TEST] 类型强制\n");
  recon::ParamStore p = recon::BuildDefaultParams();

  p.Set(recon::keys::kSfmLoopDetectionEnabled, 0.9);
  double v = 0.0;
  p.Get(recon::keys::kSfmLoopDetectionEnabled, &v);
  CheckNear(v, 1.0, 1e-9, "布尔参数 0.9 应吸附到 1");

  p.Set(recon::keys::kSfmLoopDetectionEnabled, 0.2);
  p.Get(recon::keys::kSfmLoopDetectionEnabled, &v);
  CheckNear(v, 0.0, 1e-9, "布尔参数 0.2 应吸附到 0");

  p.Set(recon::keys::kSfmBaMaxIterations, 150.7);
  p.Get(recon::keys::kSfmBaMaxIterations, &v);
  CheckNear(v, 151.0, 1e-9, "整数参数应四舍五入");
}

void TestUnknownKey() {
  std::printf("[TEST] 未知键处理\n");
  recon::ParamStore p = recon::BuildDefaultParams();
  Check(!p.Set("nonexistent.key", 1.0), "未知键 Set 必须失败");
  double v = 0.0;
  Check(!p.Get("nonexistent.key", &v), "未知键 Get 必须失败");
}

void TestG6Contract() {
  std::printf("[TEST] G6 参数契约（docs/04 §7.1 ↔ 注册表）\n");
  recon::ParamStore p = recon::BuildDefaultParams();

  // 由 tools/gen_g6_params.py 从 docs/04 §7.1 生成。{键名, 期望默认值}。
  //
  // 为什么要有本测试：docs/04 §7.1 是 G6 的展开规格，注册表是它的实现。
  // 二者一旦漂移（漏注册 / 默认值抄错），此前无任何自动闸门 —— 只有人眼。
  // 本测试把「规格 → 注册表」变成可执行契约。
  struct Row {
    const char* key;
    double default_value;
  };
  static const Row kRows[] = {
#include "g6_contract.inc"
  };

  size_t covered = 0;
  // 用 `sizeof(kRows[0])` 而非 `sizeof(Row[0])`：后者 GCC 无法按表达式解析，
  // 会回退成类型名并算出 0 → `-Wdiv-by-zero`（零告警标准下不可接受）。
  const size_t kTotal = sizeof(kRows) / sizeof(kRows[0]);
  for (const auto& r : kRows) {
    // 经公有的 specs() 查规格（Find 为私有）。
    const recon::ParamSpec* s = nullptr;
    for (const auto& spec : p.specs()) {
      if (spec.key == r.key) {
        s = &spec;
        break;
      }
    }
    if (s == nullptr) {
      std::printf("  [FAIL] §7.1 键未注册: %s\n", r.key);
      ++g_failures;
      continue;
    }
    ++covered;
    CheckNear(s->default_value, r.default_value, 1e-12, r.key);
    Check(s->type == recon::ParamType::kInt ||
              s->type == recon::ParamType::kDouble,
          "G6 参数类型应为 int 或 double");
  }
  Check(covered == kTotal, "§7.1 全部键都必须已注册");
  std::printf("  §7.1 契约覆盖: %zu / %zu\n", covered, kTotal);
}

void TestJsonExport() {
  recon::ParamStore p = recon::BuildDefaultParams();
  p.ApplyPreset(recon::Preset::kHighPrecision);
  p.Set(recon::keys::kVioFeatureCount, 512);

  const std::string json = p.ToJson();
  Check(json.find("\"preset\":\"high_precision\"") != std::string::npos,
        "JSON 应含预设标识");
  Check(json.find("\"vio_feature_count\":512") != std::string::npos,
        "JSON 应含用户覆盖值");
  Check(json.find("\"overridden\":[\"vio_feature_count\"]") != std::string::npos,
        "JSON 应标记被覆盖的键");

  // UI 可见性清单（普通可调 / 专家专属）。
  //
  // 为什么必须锁：Swift 侧 `ParamsView` 的分组计数与展开行完全依赖这两个数组。
  // 若它们静默缺失，Swift 会退回「读不到 → 显示占位」，UI 看起来仍是好的，
  // 但用户永远看不到任何参数 —— 这正是本项目反复出现的「静默失效」形态。
  // 因此这里不仅验「数组存在」，还验「两清单互斥」与「具体归属正确」。
  const size_t kAdjPos = json.find("\"adjustable\":[");
  const size_t kExpPos = json.find("\"expert_only\":[");
  Check(kAdjPos != std::string::npos, "JSON 应导出普通可调键清单");
  Check(kExpPos != std::string::npos, "JSON 应导出专家专属键清单");
  if (kAdjPos != std::string::npos && kExpPos != std::string::npos &&
      kAdjPos < kExpPos) {
    const std::string adj = json.substr(kAdjPos, kExpPos - kAdjPos);
    const std::string exp = json.substr(kExpPos);
    // 正向：普通可调键在 adjustable 里。
    Check(adj.find("\"cap_exposure_time_us\"") != std::string::npos,
          "普通可调清单应含 cap_exposure_time_us");
    Check(exp.find("\"cap_exposure_time_us\"") == std::string::npos,
          "普通可调键不应出现在专家专属清单");
    // 正向：专家专属键在 expert_only 里。
    Check(exp.find("\"cap_resolution_width\"") != std::string::npos,
          "专家专属清单应含 cap_resolution_width");
    Check(adj.find("\"cap_resolution_width\"") == std::string::npos,
          "专家专属键不应出现在普通可调清单");
    // 不可调键（user_adjustable=false 且 expert_only=false）两处都不出现。
    // 实测全注册表仅 3 个：GPU 线程组尺寸，由运行时按设备派生，不面向用户。
    Check(adj.find("\"rgbd_gpu_workgroup_x\"") == std::string::npos,
          "不可调键不应出现在普通可调清单");
    Check(exp.find("\"rgbd_gpu_workgroup_x\"") == std::string::npos,
          "不可调键不应出现在专家专属清单");
  }
}

}  // namespace

int main() {
  std::printf("=== recon 参数系统测试 ===\n");
  TestRegistryIntegrity();
  TestClampNotFallback();
  TestPresetOrdering();
  TestUserOverridePreserved();
  TestBoolAndIntCoercion();
  TestUnknownKey();
  TestG6Contract();
  TestJsonExport();

  if (g_failures == 0) {
    std::printf("\n全部通过\n");
    return 0;
  }
  std::printf("\n失败 %d 项\n", g_failures);
  return 1;
}
