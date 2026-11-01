// SPDX-License-Identifier: BSD-3-Clause
//
// 参数系统实现。
//
// R-PREC-6 合规要点：
//   用户写入越界值时，夹取到 [min, max] 合法区间，**不回退到默认值**。
//   因为回退到默认值会静默改变用户的精度意图，而夹取保留了「尽可能接近用户意图」。

#include "recon/params.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace recon {
namespace {

// 构造参数规格的辅助函数，减少注册表的书写噪音。
ParamSpec MakeInt(const char* key, double def, double fast, double std,
                   double high, double lo, double hi, bool adjustable,
                   bool expert, const char* affects, const char* note) {
  ParamSpec s;
  s.key = key;
  s.type = ParamType::kInt;
  s.default_value = def;
  s.fast_value = fast;
  s.standard_value = std;
  s.high_precision_value = high;
  s.min_value = lo;
  s.max_value = hi;
  s.user_adjustable = adjustable;
  s.expert_only = expert;
  s.affects = affects;
  s.note = note;
  return s;
}

ParamSpec MakeDouble(const char* key, double def, double fast, double std,
                      double high, double lo, double hi, bool adjustable,
                      bool expert, const char* affects, const char* note) {
  ParamSpec s;
  s.key = key;
  s.type = ParamType::kDouble;
  s.default_value = def;
  s.fast_value = fast;
  s.standard_value = std;
  s.high_precision_value = high;
  s.min_value = lo;
  s.max_value = hi;
  s.user_adjustable = adjustable;
  s.expert_only = expert;
  s.affects = affects;
  s.note = note;
  return s;
}

ParamSpec MakeBool(const char* key, bool def, bool fast, bool std, bool high,
                    bool adjustable, bool expert, const char* affects,
                    const char* note) {
  ParamSpec s;
  s.key = key;
  s.type = ParamType::kBool;
  s.default_value = def ? 1.0 : 0.0;
  s.fast_value = fast ? 1.0 : 0.0;
  s.standard_value = std ? 1.0 : 0.0;
  s.high_precision_value = high ? 1.0 : 0.0;
  s.min_value = 0.0;
  s.max_value = 1.0;
  s.user_adjustable = adjustable;
  s.expert_only = expert;
  s.affects = affects;
  s.note = note;
  return s;
}

}  // namespace

ParamStore::ParamStore() {
  specs_.reserve(256);
  values_.reserve(256);
  user_overridden_.reserve(256);
}

bool ParamStore::Register(const ParamSpec& spec) {
  if (Find(spec.key) != nullptr) {
    return false;
  }
  specs_.push_back(spec);
  values_.push_back(PresetValue(spec));
  user_overridden_.push_back(false);
  return true;
}

const ParamSpec* ParamStore::Find(const std::string& key) const {
  for (const auto& s : specs_) {
    if (s.key == key) return &s;
  }
  return nullptr;
}

double ParamStore::PresetValue(const ParamSpec& spec) const {
  switch (preset_) {
    case Preset::kFast:
      return spec.fast_value;
    case Preset::kHighPrecision:
      return spec.high_precision_value;
    case Preset::kStandard:
    default:
      return spec.standard_value;
  }
}

void ParamStore::ApplyPreset(Preset preset) {
  preset_ = preset;
  for (size_t i = 0; i < specs_.size(); ++i) {
    // 用户显式覆盖过的参数不被预设重置。
    if (user_overridden_[i]) continue;
    values_[i] = PresetValue(specs_[i]);
  }
}

bool ParamStore::Get(const std::string& key, double* out_value) const {
  for (size_t i = 0; i < specs_.size(); ++i) {
    if (specs_[i].key == key) {
      if (out_value != nullptr) *out_value = values_[i];
      return true;
    }
  }
  return false;
}

bool ParamStore::Set(const std::string& key, double value) {
  for (size_t i = 0; i < specs_.size(); ++i) {
    if (specs_[i].key != key) continue;

    // R-PREC-6：夹取，不回退。
    double v = value;
    if (std::isnan(v) || std::isinf(v)) {
      // 非法输入同样夹取到合法区间下界，保持确定性行为。
      v = specs_[i].min_value;
    }
    v = std::clamp(v, specs_[i].min_value, specs_[i].max_value);

    // 整型参数取整。
    if (specs_[i].type == ParamType::kInt) {
      v = std::nearbyint(v);
    }
    // 布尔参数吸附到 0/1。
    if (specs_[i].type == ParamType::kBool) {
      v = (v >= 0.5) ? 1.0 : 0.0;
    }

    values_[i] = v;
    user_overridden_[i] = true;
    return true;
  }
  return false;
}

bool ParamStore::ResetToPreset(const std::string& key) {
  for (size_t i = 0; i < specs_.size(); ++i) {
    if (specs_[i].key == key) {
      values_[i] = PresetValue(specs_[i]);
      user_overridden_[i] = false;
      return true;
    }
  }
  return false;
}

void ParamStore::ResetAll(Preset preset) {
  preset_ = preset;
  for (size_t i = 0; i < specs_.size(); ++i) {
    values_[i] = PresetValue(specs_[i]);
    user_overridden_[i] = false;
  }
}

std::string ParamStore::ToJson() const {
  std::ostringstream os;
  os << "{\"preset\":";
  switch (preset_) {
    case Preset::kFast:
      os << "\"fast\"";
      break;
    case Preset::kHighPrecision:
      os << "\"high_precision\"";
      break;
    default:
      os << "\"standard\"";
      break;
  }
  os << ",\"params\":{";
  bool first = true;
  for (size_t i = 0; i < specs_.size(); ++i) {
    if (!first) os << ",";
    first = false;
    os << "\"" << specs_[i].key << "\":";
    if (specs_[i].type == ParamType::kInt) {
      os << static_cast<long long>(std::llround(values_[i]));
    } else if (specs_[i].type == ParamType::kBool) {
      os << (values_[i] >= 0.5 ? "true" : "false");
    } else {
      os << values_[i];
    }
  }
  os << "},\"overridden\":[";
  first = true;
  for (size_t i = 0; i < specs_.size(); ++i) {
    if (!user_overridden_[i]) continue;
    if (!first) os << ",";
    first = false;
    os << "\"" << specs_[i].key << "\"";
  }
  // UI 分组与专家区的可见性来源。
  //
  // 为什么放在这里而不是新增一个 C ABI 函数：`recon_export_params` 已经由
  // Swift 侧（`ReconSessionController.exportParamsJSON`）持有，追加两个键名数组
  // 即可让 UI **真实读取注册表**，避免「在 Swift 里再抄一份可调性表」——
  // 那正是本项目的漂移铁律所禁止的第二事实来源。
  //
  // 语义：`adjustable` = 普通模式展开行可见；`expert_only` = 仅专家模式可见。
  // 由 `user_adjustable` / `expert_only` 原样导出，**不做任何加工**，
  // 使 UI 的可见性子集恒等于内核的 `user_adjustable || expert_only`。
  os << "],\"adjustable\":[";
  first = true;
  for (size_t i = 0; i < specs_.size(); ++i) {
    if (!specs_[i].user_adjustable) continue;
    if (!first) os << ",";
    first = false;
    os << "\"" << specs_[i].key << "\"";
  }
  os << "],\"expert_only\":[";
  first = true;
  for (size_t i = 0; i < specs_.size(); ++i) {
    if (!specs_[i].expert_only) continue;
    if (!first) os << ",";
    first = false;
    os << "\"" << specs_[i].key << "\"";
  }
  os << "]}";
  return os.str();
}

ParamStore BuildDefaultParams() {
  ParamStore p;

  // ---- 采集类 ----
  p.Register(MakeInt(keys::kCapResolutionWidth, 1920, 1920, 1920, 1920, 640,
                     4096, false, true, "几何分辨率",
                     "采集分辨率宽度。全分辨率是 MVS 精度上限，禁止因速度降低"));
  p.Register(MakeInt(keys::kCapFrameRate, 30, 30, 30, 30, 5, 60, false, true,
                     "几何分辨率", "采集帧率。降低帧率不降低单帧精度"));
  p.Register(MakeInt(keys::kCapExposureTimeUs, 8000, 8000, 8000, 8000, 1000,
                     60000, true, false, "抗噪性",
                     "曝光时间微秒。锁定短曝光抑制运动模糊"));
  p.Register(MakeInt(keys::kCapIso, 400, 800, 400, 200, 50, 6400, true, false,
                     "抗噪性", "感光度。高精度档压低 ISO 换取信噪比"));
  p.Register(MakeBool(keys::kCapImuEnabled, true, true, true, true, true, false,
                      "定位精度", "IMU 采集开关。关闭将失去紧耦合能力"));
  p.Register(MakeInt(keys::kCapQualityGateBlurVol, 60, 60, 60, 60, 10, 500,
                     true, false, "抗噪性", "拉普拉斯方差模糊门限，低于则拒帧"));
  p.Register(MakeDouble(keys::kCapQualityGateCoverage, 0.55, 0.55, 0.55, 0.55,
                         0.15, 0.95, true, false, "几何覆盖率",
                         "帧重叠度门限。低于则拒帧"));
  p.Register(MakeDouble(keys::kCapQualityGateSatRatio, 0.02, 0.02, 0.02, 0.02,
                         0.001, 0.30, true, false, "抗噪性",
                         "饱和像素比例门限。超过则判高光反光"));
  p.Register(MakeDouble(keys::kCapQualityGateTextureDensity, 0.15, 0.15, 0.15,
                         0.15, 0.01, 0.80, true, false, "几何完整性",
                         "低纹理网格占比门限"));
  p.Register(MakeDouble(keys::kCapMaxAngleSpeed, 2.5, 2.5, 2.5, 2.5, 0.5, 20.0,
                        true, false, "定位精度", "角速度上限 rad/s"));
  p.Register(MakeDouble(keys::kCapMaxLinearSpeed, 0.6, 0.6, 0.6, 0.6, 0.05, 5.0,
                        true, false, "定位精度", "线速度上限 m/s"));

  // ---- 同步类 ----
  p.Register(MakeInt(keys::kSyncMaxDriftPpm, 50, 50, 50, 50, 1, 500, true,
                     false, "定位精度",
                     "相机时钟速率偏差 ppm。超过 50 触发重新标定，100 拒用"));
  p.Register(MakeInt(keys::kSyncMaxImuGapUs, 20000, 20000, 20000, 20000, 1000,
                     200000, true, false, "定位精度", "IMU 采样间隙上限微秒"));
  p.Register(MakeInt(keys::kSyncImuPreintegrationIntervalUs, 200, 200, 200, 200,
                     50, 2000, false, true, "定位精度",
                     "IMU 预积分离散间隔微秒"));

  // ---- VIO 类 ----
  p.Register(MakeInt(keys::kVioFeatureCount, 250, 200, 250, 400, 50, 2000, true,
                     false, "定位精度",
                     "每帧特征点数。黑名单参数，禁止因速度降级"));
  p.Register(MakeInt(keys::kVioMinFeatureDistance, 8, 8, 8, 8, 3, 40, true, false,
                     "定位精度", "特征最小间距像素"));
  p.Register(MakeDouble(keys::kVioCornerResponseThreshold, 0.01, 0.01, 0.01,
                         0.01, 0.001, 0.10, true, false, "定位精度",
                         "Shi-Tomasi 角点响应阈值"));
  p.Register(MakeInt(keys::kVioKltWindowSize, 21, 21, 21, 31, 7, 51, true, false,
                     "定位精度", "KLT 跟踪窗口半边长"));
  p.Register(MakeInt(keys::kVioKltPyramidLevels, 3, 3, 3, 4, 1, 6, true, false,
                     "定位精度", "KLT 金字塔层数。增加可跟更大位移"));
  p.Register(MakeInt(keys::kVioKltMaxIterations, 30, 20, 30, 50, 5, 100, true,
                     false, "定位精度", "KLT 迭代次数上限"));
  p.Register(MakeDouble(keys::kVioKltMinError, 1.0, 1.0, 1.0, 0.5, 0.1, 10.0,
                        true, false, "定位精度", "KLT 最小误差，超过判跟踪失败"));
  p.Register(MakeInt(keys::kVioMsckfWindowSize, 10, 10, 10, 12, 4, 30, true,
                     false, "定位精度", "MSCKF 滑窗长度"));
  p.Register(MakeDouble(keys::kVioHuberDelta, 1.0, 1.0, 1.0, 2.0, 0.1, 50.0,
                        true, false, "抗噪性", "Huber 核参数"));
  p.Register(MakeDouble(keys::kVioImuNoiseGyro, 1.5e-4, 1.5e-4, 1.5e-4, 1.0e-4,
                        1e-6, 1e-2, true, false, "定位精度", "陀螺仪噪声 rad/s"));
  p.Register(MakeDouble(keys::kVioImuNoiseAcc, 1.0e-2, 1.0e-2, 1.0e-2, 5.0e-3,
                        1e-4, 1.0, true, false, "定位精度", "加速度计噪声 m/s^2"));
  p.Register(MakeDouble(keys::kVioImuBiasNoiseGyro, 1e-5, 1e-5, 1e-5, 5e-6,
                        1e-7, 1e-3, true, false, "定位精度",
                        "陀螺零偏随机游走 rad/s^2/sqrt(s)"));
  p.Register(MakeDouble(keys::kVioImuBiasNoiseAcc, 1e-4, 1e-4, 1e-4, 5e-5,
                        1e-6, 1e-2, true, false, "定位精度",
                        "加计零偏随机游走 m/s^3/sqrt(s)"));
  p.Register(MakeDouble(keys::kVioStaticDetectionAccelTol, 0.1, 0.1, 0.1, 0.1,
                        0.01, 1.0, true, false, "定位精度",
                        "静止判定加速度模长容差 m/s^2"));
  p.Register(MakeDouble(keys::kVioStaticDetectionGyroTol, 0.05, 0.05, 0.05, 0.05,
                        0.005, 0.5, true, false, "定位精度",
                        "静止判定角速度容差 rad/s"));
  p.Register(MakeDouble(keys::kVioStaticDetectionDurationS, 2.0, 2.0, 2.0, 2.0,
                        0.5, 10.0, true, false, "定位精度",
                        "静止判定持续时长秒"));
  p.Register(MakeDouble(keys::kVioPriorRotationSigmaDeg, 1.0, 1.0, 1.0, 0.5,
                        0.05, 10.0, true, false, "定位精度",
                        "VIO 位姿先验旋转标准差"));
  p.Register(MakeDouble(keys::kVioPriorTranslationSigmaM, 0.003, 0.003, 0.003,
                        0.001, 0.0001, 0.1, true, false, "定位精度",
                        "VIO 位姿先验平移标准差"));

  // ---- SfM 类（docs/08 §G4，共 20 项；键名以此为准） ----
  p.Register(MakeInt(keys::kSfmFeatureCount, 12000, 8000, 12000, 20000, 1000,
                     60000, true, false, "定位精度、几何绝对精度",
                     "每帧提取的特征点数。黑名单参数，不得因速度降级"));
  p.Register(MakeDouble(keys::kSfmFeatureEdgeThreshold, 10.0, 12.0, 10.0, 8.0,
                        1.0, 50.0, false, true, "定位精度",
                        "特征首层边缘阈值。专家项"));
  p.Register(MakeInt(keys::kSfmMaxNumNeighbors, 80, 50, 80, 120, 10, 500, true,
                     false, "定位精度", "匹配时每图最大候选邻居数"));
  p.Register(MakeDouble(keys::kSfmMatchRatioThreshold, 0.85, 0.90, 0.85, 0.82,
                        0.5, 0.99, true, false, "抗噪性、定位精度",
                        "Lowe 比值检验。降低会引入更多误匹配"));
  p.Register(MakeInt(keys::kSfmMinTrackLength, 3, 3, 3, 2, 2, 20, true, false,
                     "定位精度、几何绝对精度", "稀疏点的最小轨迹长度"));
  p.Register(MakeInt(keys::kSfmRansacMaxIterations, 10000, 5000, 10000, 30000,
                     100, 1000000, true, false, "抗噪性、求解收敛性",
                     "几何验证 RANSAC 迭代数。黑名单参数，不得因速度降级"));
  p.Register(MakeInt(keys::kSfmRansacMinInliers, 30, 20, 30, 50, 8, 500, true,
                     false, "抗噪性、求解收敛性", "最小内点数。黑名单参数"));
  p.Register(MakeDouble(keys::kSfmRansacConfidence, 0.999, 0.99, 0.999, 0.9999,
                        0.5, 1.0, false, true, "求解收敛性、抗噪性",
                        "提前终止置信度。黑名单参数，专家项"));
  p.Register(MakeDouble(keys::kSfmRansacMaxErrorPx, 4.0, 6.0, 4.0, 3.0, 0.5,
                        20.0, true, false, "抗噪性",
                        "重投影内点阈值像素。黑名单参数"));
  p.Register(MakeInt(keys::kSfmBaMaxIterations, 100, 50, 100, 200, 5, 1000, true,
                     false, "求解收敛性、定位精度",
                     "BA 迭代上限。高精度档 200。黑名单参数"));
  p.Register(MakeDouble(keys::kSfmBaConvergenceTolerance, 1e-8, 1e-7, 1e-8, 1e-9,
                        1e-12, 1e-3, true, false, "求解收敛性",
                        "BA 函数收敛容差"));
  p.Register(MakeInt(keys::kSfmBaLossFunction, 1, 0, 1, 1, 0, 3, true, false,
                     "抗噪性", "0=无 1=Huber 2=Cauchy 3=Trivial"));
  p.Register(MakeDouble(keys::kSfmBaHuberDelta, 1.5, 2.0, 1.5, 1.0, 0.1, 10.0,
                        true, false, "抗噪性", "BA 鲁棒核阈值像素"));
  p.Register(MakeInt(keys::kSfmBaMinNumImages, 5, 6, 5, 4, 3, 50, true, false,
                     "求解收敛性", "触发全局 BA 的最少帧数"));
  p.Register(MakeBool(keys::kSfmLoopDetectionEnabled, true, true, true, true,
                      true, false, "定位精度", "闭环检测开关"));
  p.Register(MakeInt(keys::kSfmLoopMinInliers, 30, 20, 30, 50, 10, 500, true,
                     false, "定位精度", "闭环几何验证最小内点数"));
  p.Register(MakeDouble(keys::kSfmLoopSimilarityScaleTolerance, 0.10, 0.15, 0.10,
                        0.05, 0.01, 1.0, false, true, "定位精度",
                        "Sim(3) 尺度容差。需与 07 文档标定尺度匹配，专家项"));
  p.Register(MakeBool(keys::kSfmIncrementalBaEnabled, true, true, true, true, true,
                      false, "定位精度、速度", "增量式 BA 开关"));
  p.Register(MakeInt(keys::kSfmMaxSparsePoints, 0, 0, 0, 0, 0, 5000000, true,
                     false, "内存", "稀疏点上限，0=不限"));
  p.Register(MakeBool(keys::kSfmScalePriorEnabled, true, true, true, true, true,
                      false, "尺度精度", "注入 07 文档的尺度先验"));

  // ---- MVS 类 ----
  p.Register(MakeInt(keys::kMvsPatchSize, 7, 7, 7, 11, 3, 21, true, false,
                     "几何精度", "PatchMatch 块尺寸"));
  p.Register(MakeInt(keys::kMvsPatchIterations, 3, 3, 3, 6, 1, 20, true, false,
                     "几何精度", "PatchMatch 外迭代轮数"));
  p.Register(MakeInt(keys::kMvsPatchRandomSearchRadius, 2, 2, 2, 4, 0, 16, true,
                     false, "几何精度", "随机搜索半径"));
  p.Register(MakeInt(keys::kMvsNumViews, 5, 5, 5, 8, 2, 32, true, false,
                     "几何精度",
                     "每个像素参与融合的视图数。黑名单参数"));
  p.Register(MakeInt(keys::kMvsSubpixelIterations, 3, 2, 3, 5, 0, 10, true, false,
                     "几何精度", "亚像素视差细化迭代次数"));
  p.Register(MakeDouble(keys::kMvsGeoConsistencyThreshold, 0.02, 0.02, 0.02, 0.01,
                        0.001, 0.20, true, false, "几何精度",
                        "几何一致性过滤阈值，比值"));
  p.Register(MakeDouble(keys::kMvsPhotoThreshold, 0.2, 0.2, 0.2, 0.1, 0.01, 1.0,
                        true, false, "几何精度", "光度一致性阈值，1-NCC"));
  p.Register(MakeDouble(keys::kMvsFusionVoxelSizeM, 0.001, 0.002, 0.001, 0.0005,
                        0.0002, 0.005, true, false, "几何分辨率",
                        "MVS 点云融合体素边长米。黑名单参数"));
  p.Register(MakeDouble(keys::kMvsImageScale, 1.0, 1.0, 1.0, 1.0, 0.25, 1.0,
                        true, false, "几何分辨率",
                        "MVS 图像缩放。高精度档锁定 1.0 不降采样"));
  p.Register(MakeDouble(keys::kMvsMinDepthM, 0.10, 0.10, 0.10, 0.10, 0.02, 1.0,
                        true, false, "几何精度", "深度搜索下界米"));
  p.Register(MakeDouble(keys::kMvsMaxDepthM, 1.50, 1.50, 1.50, 2.00, 0.2, 5.0,
                        true, false, "几何精度", "深度搜索上界米"));
  p.Register(MakeInt(keys::kMvsGpuTileSize, 256, 256, 256, 128, 32, 1024, false,
                     true, "速度", "GPU 分块边长像素。降级白名单项"));
  p.Register(MakeDouble(keys::kMvsPmSearchRadiusRatio, 0.30, 0.40, 0.30, 0.22,
                        0.01, 1.0, true, false, "几何分辨率",
                        "随机搜索初始半径（逆深度相对比例）。见 05 §5.3.1.1"));
  p.Register(MakeDouble(keys::kMvsSubpixelWindowRatio, 0.50, 0.70, 0.50, 0.35,
                        0.05, 4.0, true, false, "几何绝对精度",
                        "细化窗口半宽（1 px 视差对应逆深度变化为 1.0）。见 05 §5.3.1.1"));
  p.Register(MakeInt(keys::kMvsNumThreads, 0, 0, 0, 0, 0, 64, true, false, "速度",
                     "MVS 并行线程数。0=自动。降级白名单项，不降精度"));

  // ---- 深度融合类（G6）----
  // 由 tools/gen_g6_params.py 从 docs/04 §7.1 生成，勿手改。
  p.Register(MakeDouble(keys::kRgbdTsdfVoxelSize, 0.002, 0.004, 0.002, 0.0005, 0.00025, 0.02, true, false, "几何分辨率、几何绝对精度",
                   "主网格体素边长。**黑名单参数**（00 §1.2），不得因内存降级（08 G6）"));
  p.Register(MakeDouble(keys::kRgbdTsdfTruncation, 0.008, 0.016, 0.008, 0.002, 0.0005, 0.1, true, false, "几何绝对精度",
                   "主网格截断距离，默认 = 4 × 体素。**语义需回填 08 G6**：本值是**下限**，实际 `μ_eff = max(本值, 2σ_z)`；见 §4.2.3"));
  p.Register(MakeInt(keys::kRgbdVoxelBlockSize, 64, 32, 64, 128, 8, 512, true, false, "内存、速度",
                   "分块大小（白名单可降级项，08 G6）"));
  p.Register(MakeInt(keys::kRgbdMaxBlocksInMemory, 256, 64, 256, 1024, 8, 8192, true, false, "内存",
                   "常驻内存块数上限（白名单，08 G6）"));
  p.Register(MakeInt(keys::kRgbdFusionEnabled, 1, 1, 1, 1, 0, 1, true, false, "几何绝对精度",
                   "TSDF 融合总开关（08 G6）"));
  p.Register(MakeInt(keys::kRgbdDepthPreprocessEnabled, 1, 1, 1, 1, 0, 1, true, false, "抗噪性",
                   "深度预处理（飞行点剔除、双边区间估计，08 G6）"));
  p.Register(MakeDouble(keys::kRgbdFlyingPixelThreshold, 0.002, 0.004, 0.002, 0.001, 0.0002, 0.02, true, false, "抗噪性",
                   "跳变幅度下限（08 G6）。**语义需回填 08 G6**：实际判据 `max(本值, k·σ_z)`，见 §3.3.2"));
  p.Register(MakeDouble(keys::kRgbdBilateralSigmaColor, 0.08, 0.12, 0.08, 0.05, 0.0, 1.0, true, false, "抗噪性",
                   "双边滤波色彩 σ（归一化，08 G6）"));
  p.Register(MakeInt(keys::kRgbdBilateralSigmaSpace, 4, 6, 4, 3, 1, 16, true, false, "抗噪性",
                   "双边滤波空间 σ（08 G6）"));
  p.Register(MakeInt(keys::kRgbdAlignToRgbEnabled, 1, 1, 1, 1, 0, 1, true, false, "几何绝对精度",
                   "深度对齐到 RGB（08 G6）。**仅用于颜色关联与交叉校验**，TSDF 几何走原生分辨率路径（RA-5）"));
  p.Register(MakeInt(keys::kRgbdConfidenceThreshold, 0, 0, 0, 0, 0, 255, true, false, "抗噪性",
                   "置信度硬门槛，0 = 不过滤（全部走降权路径，08 G6）"));
  p.Register(MakeInt(keys::kRgbdPoseGraphOptimization, 1, 1, 1, 1, 0, 1, true, false, "定位精度",
                   "位姿图优化（08 G6）"));
  p.Register(MakeInt(keys::kRgbdPoseGraphIterations, 30, 15, 30, 60, 1, 500, true, false, "定位精度、求解收敛性",
                   "位姿图迭代次数（08 G6）"));
  p.Register(MakeDouble(keys::kRgbdPoseGraphEdgeThreshold, 0.02, 0.05, 0.02, 0.01, 0.001, 1.0, true, false, "定位精度",
                   "位姿图边的深度一致性阈值（08 G6）"));
  p.Register(MakeInt(keys::kRgbdLidarRole, 1, 1, 1, 1, 0, 3, false, true, "几何绝对精度",
                   "0=不用, 1=尺度锚定+先验, 2=TSDF 主输入, 3=全量融合（08 G6）。**不可运行时切换**；Tier 1/3 被强制为 1（§2.1.2）"));
  p.Register(MakeDouble(keys::kRgbdMvsFusionWeight, 0.75, 0.6, 0.75, 0.9, 0.0, 1.0, true, false, "几何绝对精度",
                   "最终网格中 MVS 深度相对深度先验的权重（08 G6）。**只影响补盲区边界**，不影响 MVS 主导区几何（§4.7.2）"));
  p.Register(MakeDouble(keys::kRgbdTsdfTruncationRatio, 4.0, 4.0, 4.0, 4.0, 2.0, 12.0, false, true, "几何绝对精度",
                   "`μ = ratio × h_voxel`。**需回填 08 G6**。取值理由见 §4.2.2"));
  p.Register(MakeDouble(keys::kRgbdTsdfPriorVoxelSize, 0.008, 0.016, 0.008, 0.008, 0.002, 0.05, false, true, "几何绝对精度、抗噪性",
                   "先验网格体素边长 `h_prior`（role=1 源专用）。**需回填 08 G6**。取 `max(4h_voxel, 8 mm)`，保证 `μ_prior ≥ 2σ_z`（§4.2.3）"));
  p.Register(MakeDouble(keys::kRgbdTsdfMinWeight, 1.0, 1.0, 1.0, 1.0, 0.1, 10.0, false, true, "抗噪性",
                   "体素有效性的权重下限 `W ≥ W_min`。**需回填 08 G6**"));
  p.Register(MakeInt(keys::kRgbdTsdfMinViews, 2, 2, 2, 2, 1, 8, false, true, "抗噪性",
                   "体素有效性的支撑视图数下限。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdTsdfDispersionMax, 0.10, 0.15, 0.10, 0.08, 0.01, 1.0, false, true, "抗噪性",
                   "体素内加权深度离散度上限，超限降权 ×0.5。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdTsdfConfidenceRef, 8.0, 8.0, 8.0, 8.0, 1.0, 64.0, false, true, "几何绝对精度",
                   "补盲置信度的参考支撑权重。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdTsdfColorViewWeightExp, 2.0, 1.5, 2.0, 3.0, 0.0, 8.0, true, false, "纹理质量",
                   "颜色融合的视角权重指数 `n`（`cos^n θ`）。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdSigmaRef, 0.002, 0.002, 0.002, 0.002, 0.0005, 0.02, false, true, "抗噪性",
                   "权重归一化的参考噪声 `σ_ref`（ToF 标档基准）。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdSigmaTof, 0.002, 0.002, 0.002, 0.002, 0.0005, 0.01, false, true, "几何绝对精度、抗噪性",
                   "ToF 深度单像素噪声 `σ_z`。**需实测标定**。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdSigmaLidar, 0.012, 0.012, 0.012, 0.012, 0.005, 0.05, false, true, "几何绝对精度、抗噪性",
                   "LiDAR 深度单像素噪声 `σ_z`（ARKit sceneDepth）。**需实测标定**。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdSigmaAlgoRel, 0.03, 0.03, 0.03, 0.03, 0.01, 0.10, false, true, "几何绝对精度、抗噪性",
                   "ALGO_DEPTH 相对噪声。**需实测标定**。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdSigmaAlgoAbs, 0.0005, 0.0005, 0.0005, 0.0005, 0.0001, 0.005, false, true, "几何绝对精度、抗噪性",
                   "ALGO_DEPTH 绝对噪声项 `σ_z = rel·z + abs`。**需实测标定**。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdIncidenceCosMin, 0.20, 0.25, 0.20, 0.15, 0.05, 1.0, false, true, "抗噪性",
                   "入射角余弦下限 `cos θ_min`，防止 `1/cos` 爆炸。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdWeightCap, 4.0, 2.0, 4.0, 8.0, 1.0, 64.0, true, false, "抗噪性",
                   "相对权重上限 `w_cap`。**需回填 08 G6**"));
  p.Register(MakeInt(keys::kRgbdConfidenceSource, 1, 1, 1, 1, 0, 2, false, true, "抗噪性",
                   "0=仅平台, 1=平台+推导取 min, 2=仅推导。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdConfidenceLow, 0.25, 0.25, 0.25, 0.25, 0.0, 0.9, true, false, "抗噪性",
                   "置信度下拐点 `c_low`。**需实测标定**。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdConfidenceHigh, 0.75, 0.75, 0.75, 0.75, 0.5, 1.0, true, false, "抗噪性",
                   "置信度上拐点 `c_high`。**需实测标定**。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdConfidenceFloor, 0.05, 0.05, 0.05, 0.03, 0.0, 0.5, false, true, "抗噪性",
                   "可靠性地板 `ρ_floor`，**保证\"降权不丢弃\"**（RW-1）。**需回填 08 G6**"));
  p.Register(MakeInt(keys::kRgbdConfidencePlaneWindow, 5, 5, 5, 5, 3, 9, false, true, "抗噪性",
                   "推导置信度的平面拟合窗口半径。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdConfidenceEdgeScale, 0.02, 0.02, 0.02, 0.02, 0.005, 0.1, false, true, "抗噪性",
                   "推导置信度的边缘因子尺度 `g_0`。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdFlyingPixelNoiseK, 3.0, 3.0, 3.0, 3.0, 1.0, 6.0, true, false, "抗噪性",
                   "`τ_eff = max(阈值, k·σ_z)` 的 k。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdBilateralSupportRatio, 0.50, 0.50, 0.50, 0.50, 0.2, 0.9, false, true, "抗噪性",
                   "BMRE 的邻域支持比 `w_sup`。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdBilateralEdgeConfidence, 0.50, 0.50, 0.50, 0.50, 0.05, 1.0, false, true, "抗噪性",
                   "BMRE 边缘像素的置信度折扣 `w_edge`。**需回填 08 G6**"));
  p.Register(MakeInt(keys::kRgbdBilateralMinBranchMedian, 1, 1, 1, 1, 0, 1, false, true, "抗噪性",
                   "前景分支用中位数(1)/最小值(0)。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdBilateralFgColorMin, 0.35, 0.35, 0.35, 0.35, 0.05, 0.95, false, true, "抗噪性",
                   "BMRE 前景组的最小颜色权重 `w_fg_min`。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdAlignDerivedConfScale, 0.50, 0.50, 0.50, 0.50, 0.05, 1.0, false, true, "抗噪性",
                   "对齐图派生像点的置信度折扣（RA-6）。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdAlignMaxBaseline, 0.05, 0.05, 0.05, 0.05, 0.001, 0.2, false, true, "几何绝对精度",
                   "深度–RGB 基线合理性上限（§3.5.5）。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdDepthMin, 0.05, 0.08, 0.05, 0.03, 0.01, 1.0, true, false, "几何绝对精度",
                   "有效深度下界。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdDepthMax, 2.0, 1.5, 2.0, 3.0, 0.1, 10.0, true, false, "几何绝对精度",
                   "有效深度上界。**需回填 08 G6**"));
  p.Register(MakeInt(keys::kRgbdIcpEnabled, 1, 1, 1, 1, 0, 1, true, false, "定位精度",
                   "帧对深度 ICP 开关。**需回填 08 G6**"));
  p.Register(MakeInt(keys::kRgbdIcpMaxIterations, 20, 12, 20, 30, 3, 100, true, false, "求解收敛性、定位精度",
                   "滑窗内 ICP 迭代上限。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdIcpConvergenceTrans, 1e-5, 1e-4, 1e-5, 1e-6, 1e-8, 1e-2, true, false, "求解收敛性、定位精度",
                   "平移收敛判据。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdIcpConvergenceRot, 1e-5, 1e-4, 1e-5, 1e-6, 1e-8, 1e-2, true, false, "求解收敛性、定位精度",
                   "旋转收敛判据。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdIcpPointToPointWeight, 0.3, 0.3, 0.3, 0.4, 0.0, 1.0, true, false, "抗噪性、定位精度",
                   "点—点残差相对点—面的权重。**需回填 08 G6**"));
  p.Register(MakeInt(keys::kRgbdIcpPlaneWindow, 5, 5, 5, 7, 3, 15, false, true, "定位精度",
                   "局部平面拟合窗口半径（法向估计）。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdIcpHuberDeltaScale, 2.5, 2.5, 2.5, 3.0, 1.0, 6.0, false, true, "抗噪性",
                   "Huber `δ = k·σ_z(tier)`。**需回填 08 G6**"));
  p.Register(MakeInt(keys::kRgbdIcpMinCorrespondences, 200, 200, 200, 300, 20, 10000, false, true, "定位精度",
                   "对应点数量下限（退化判据）。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdIcpMinDepth, 0.05, 0.08, 0.05, 0.03, 0.01, 1.0, true, false, "几何绝对精度",
                   "ICP 参与计算的深度下界。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdIcpMaxDepth, 1.5, 1.0, 1.5, 2.0, 0.1, 10.0, true, false, "几何绝对精度",
                   "ICP 参与计算的深度上界。**需回填 08 G6**"));
  p.Register(MakeInt(keys::kRgbdIcpSampleStride, 1, 2, 1, 1, 1, 8, true, false, "速度、定位精度",
                   "像素采样步长（白名单可降级项）。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdImuPriorWeight, 1.0, 0.5, 1.0, 2.0, 0.0, 100.0, true, false, "定位精度、尺度精度",
                   "IMU 预积分残差权重。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdImuRotationSigmaDeg, 1.0, 1.5, 1.0, 0.5, 0.05, 20.0, false, true, "定位精度",
                   "IMU 旋转约束等效 σ。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdImuTranslationSigmaM, 0.003, 0.005, 0.003, 0.0015, 1e-4, 0.1, false, true, "定位精度",
                   "IMU 平移约束等效 σ。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdPlatformPriorWeight, 0.1, 0.1, 0.1, 0.2, 0.0, 10.0, false, true, "定位精度",
                   "平台位姿弱先验权重。**需回填 08 G6**"));
  p.Register(MakeInt(keys::kRgbdLoopEnabled, 1, 1, 1, 1, 0, 1, true, false, "定位精度",
                   "闭环检测开关（08 `sfm_loop_detection_enabled` 不覆盖 RGBD 专用闭环）。**需回填 08 G6**"));
  p.Register(MakeInt(keys::kRgbdLoopMinInliers, 50, 30, 50, 80, 10, 1000, true, false, "定位精度",
                   "闭环最小内点数。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdLoopMaxReprojPx, 1.5, 2.5, 1.5, 1.0, 0.1, 20.0, true, false, "定位精度",
                   "闭环重投影残差中位数上限。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdLoopScaleConsistencyMax, 0.05, 0.08, 0.05, 0.03, 0.005, 0.5, false, true, "定位精度、尺度精度",
                   "回环的尺度一致性容差（RGBD 特有门槛）。**需回填 08 G6**"));
  p.Register(MakeInt(keys::kRgbdScaleAnchorEnabled, 1, 1, 1, 1, 0, 1, true, false, "尺度精度",
                   "深度尺度锚定与强制复核开关。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdScaleAnchorSigma, 0.02, 0.02, 0.02, 0.02, 0.002, 0.2, false, true, "尺度精度",
                   "尺度锚定残差 σ（ToF 取值；ALGO 实际用 0.05、LiDAR 0.10）。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdDepthMvsMedianMax, 0.03, 0.03, 0.03, 0.02, 0.005, 0.3, true, false, "尺度精度",
                   "深度 vs MVS 中位偏差上限（07 §4.3）。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdDepthMvsConsistencyMin, 0.40, 0.40, 0.40, 0.40, 0.0, 1.0, false, true, "尺度精度",
                   "交叉一致性统计的最小有效覆盖率（RA-7）。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdDepthConsistencyWeight, 0.5, 0.2, 0.5, 1.0, 0.0, 10.0, true, false, "定位精度",
                   "全局优化中深度一致性因子的权重（**不得**与视觉项同量级，§5.5.3）。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdDepthConsistencySigma, 0.03, 0.05, 0.03, 0.02, 0.002, 0.5, false, true, "定位精度、尺度精度",
                   "深度一致性因子的相对 σ。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdHoleFillMvsConfMax, 0.30, 0.35, 0.30, 0.25, 0.0, 1.0, true, false, "几何绝对精度、拓扑完整性",
                   "MVS 失效判据（C_mvs 低于此值进入补盲通道）。**需回填 08 G6**"));
  p.Register(MakeInt(keys::kRgbdHoleFillTransitionPx, 12, 12, 12, 16, 0, 64, false, true, "几何绝对精度",
                   "补盲区与 MVS 区的过渡带宽度。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdHoleFillDepthWeightScale, 1.0, 1.0, 1.0, 1.0, 0.0, 4.0, true, false, "几何绝对精度",
                   "补盲点权重整体缩放。**需回填 08 G6**"));
  p.Register(MakeInt(keys::kRgbdReflectiveRejectEnabled, 1, 1, 1, 1, 0, 1, true, false, "几何绝对精度",
                   "镜面/透明区深度点拒绝注入。**需回填 08 G6**"));
  p.Register(MakeDouble(keys::kRgbdReflectiveRejectSigma, 3.0, 3.0, 3.0, 3.5, 1.0, 8.0, false, true, "几何绝对精度",
                   "拒绝判据 `"));
  p.Register(MakeInt(keys::kRgbdGpuWorkgroupX, 8, 8, 8, 8, 4, 32, false, false, "速度",
                   "TSDF 融合 workgroup 宽（§4.6.1）。**需回填 08 G6**"));
  p.Register(MakeInt(keys::kRgbdGpuWorkgroupY, 8, 8, 8, 8, 4, 32, false, false, "速度",
                   "TSDF 融合 workgroup 高（§4.6.1）。**需回填 08 G6**"));
  p.Register(MakeInt(keys::kRgbdGpuVoxelPerThread, 1, 1, 1, 1, 1, 64, false, false, "速度",
                   "每线程处理的体素数（体素中心式，§4.6.2）。**需回填 08 G6**"));
  // 遗留键（不在 §7.1 规格内，但被 TSDF 融合真实消费，保留）。
  p.Register(MakeDouble(keys::kRgbdTsdfMaxDepth, 3.0, 3.0, 3.0, 4.0, 0.2, 10.0,
                        true, false, "几何精度", "融合最大深度米"));
  p.Register(MakeInt(keys::kRgbdMinDepthConfidence, 30, 30, 30, 30, 0, 255, true,
                     false, "几何精度", "深度最小置信度"));
  p.Register(MakeDouble(keys::kRgbdBilateralFilterSigma, 0.05, 0.05, 0.05, 0.03,
                        0.0, 0.5, true, false, "抗噪性",
                        "双边滤波空间 sigma，原始深度副本始终保留"));
  p.Register(MakeInt(keys::kRgbdOutlierRemovalK, 16, 16, 16, 24, 4, 64, true,
                     false, "几何精度", "离群点剔除 KNN 邻居数"));

  // ---- 网格化类 ----
  p.Register(MakeInt(keys::kMeshPoissonDepth, 11, 11, 12, 14, 6, 14, true, false,
                     "几何分辨率",
                     "泊松八叉树深度。黑名单参数，禁止降级"));
  p.Register(MakeInt(keys::kMeshPoissonDepthMin, 6, 6, 6, 6, 4, 14, false, true,
                     "几何分辨率", "泊松深度下限。专家项"));
  p.Register(MakeInt(keys::kMeshPoissonDepthMax, 14, 11, 12, 14, 8, 16, false,
                     true, "几何分辨率", "泊松深度上限。专家项"));
  p.Register(MakeDouble(keys::kMeshPoissonScale, 1.1, 1.1, 1.1, 1.05, 0.5, 2.0,
                        true, false, "几何分辨率", "泊松立方体与包围盒比例"));
  p.Register(MakeInt(keys::kMeshPoissonSamplesPerNode, 1, 1, 1, 1, 1, 16, true,
                     false, "几何精度", "每节点最少采样点数"));
  p.Register(MakeBool(keys::kMeshPoissonPointWeight, true, true, true, true, true,
                      false, "几何精度", "是否使用点法向加权"));
  p.Register(MakeInt(keys::kMeshPoissonDegree, 2, 2, 2, 3, 1, 3, false, true,
                     "几何精度", "B 样条有限元阶数。专家项"));
  p.Register(MakeInt(keys::kMeshPoissonBType, 3, 3, 3, 3, 0, 3, false, true,
                     "几何精度", "边界条件 0=自由 1=Dirichlet 2=Neumann。专家项"));
  p.Register(MakeBool(keys::kMeshPoissonPrune, true, true, true, true, true, false,
                      "几何分辨率", "裁剪低置信度外推区域"));
  p.Register(MakeBool(keys::kMeshPoissonScaleIsotropic, false, false, false, true,
                      true, false, "几何分辨率", "各向同性缩放"));
  p.Register(MakeBool(keys::kMeshPoissonConfidence, true, true, true, true, false,
                      true, "几何精度", "是否输出置信度。专家项"));
  p.Register(MakeBool(keys::kMeshPoissonInterpolate, true, true, true, true, false,
                      true, "几何精度", "是否插值顶点属性。专家项"));
  p.Register(MakeBool(keys::kMeshChunkedEnabled, true, true, true, true, false,
                      true, "速度", "分块泊松开关。专家项"));
  p.Register(MakeInt(keys::kMeshChunkTargetCells, 4000000, 4000000, 4000000,
                     2000000, 100000, 64000000, false, true, "速度",
                     "单块目标节点数。专家项"));
  // **契约（docs/08 §G7）**：保留「距离最近（≈ 置信度最高）」的前 p 分位顶点。
  //
  // 曾经误登记为 `0.02/0.05/0.02/0.01`、值域 `[0, 0.30]` —— 与
  // `PoissonConfig::trim_confidence_quantile` 的头文件默认 `0.999` 及
  // docs/08 §G7 的 `0.995/0.999/0.9995`、`[0.9, 1.0]` **都不一致**。
  // 后果是**致命的**：编排层永远从注册表取值，快速档拿到 0.05 →
  // 裁剪只保留最近的 5% 顶点 → 网格被整片删空（实测顶点 2199 → 110、
  // 三角形 3428 → 0）→ `PoissonReconstruct` 返回 false →
  // `RECON_ERR_MESH_POISSON_FAILED`。而直接调用 `PoissonReconstruct`
  // 的单测（test_poisson）**自己写死 0.99**，从不经过注册表 →
  // 这个缺陷在「单测孤岛」里完全不可见，只有端到端编排测试能暴露。
  p.Register(MakeDouble(keys::kMeshTrimConfQuantile, 0.999, 0.995, 0.999, 0.9995,
                        0.9, 1.0, true, false, "几何绝对精度",
                        "置信度分位数裁剪（保留最近 p 分位）"));

  // ---- 后处理类 ----
  p.Register(MakeInt(keys::kPostOutlierK, 16, 16, 16, 24, 4, 64, true, false,
                     "几何精度", "统计离群点 KNN 邻居数"));
  p.Register(MakeDouble(keys::kPostOutlierStdRatio, 2.0, 2.0, 2.0, 2.5, 1.0, 6.0,
                        true, false, "几何精度", "离群点标准差倍数"));
  p.Register(MakeDouble(keys::kPostHoleAreaMicroRatio, 0.02, 0.02, 0.02, 0.005,
                        0.0, 1.0, true, false, "拓扑完整性",
                        "微孔面积占比阈值（占总面积比值）"));
  p.Register(MakeDouble(keys::kPostHoleAreaSmallRatio, 0.20, 0.20, 0.20, 0.05,
                        0.0, 5.0, true, false, "拓扑完整性", "小孔面积占比阈值"));
  p.Register(MakeDouble(keys::kPostHoleAreaLargeRatio, 3.0, 3.0, 3.0, 0.8, 0.0,
                        20.0, true, false, "拓扑完整性", "大孔面积占比阈值"));
  p.Register(MakeDouble(keys::kPostNormalConsistencyMin, 0.9, 0.9, 0.9, 0.95, 0.0,
                        1.0, true, false, "几何绝对精度",
                        "法线一致性下限"));
  p.Register(MakeDouble(keys::kPostEdgeLengthVarianceMax, 0.5, 0.5, 0.5, 0.3,
                        0.0, 2.0, true, false, "几何分辨率",
                        "边长方差上限"));
  p.Register(MakeDouble(keys::kPostMinAspectRatio, 0.1, 0.1, 0.1, 0.2, 0.01, 1.0,
                        true, false, "拓扑完整性", "三角形最小长宽比"));
  p.Register(MakeInt(keys::kPostUvChartCount, 64, 64, 64, 128, 4, 1024, true,
                     false, "纹理质量", "UV chart 数量"));
  p.Register(MakeDouble(keys::kPostUvPadding, 0.005, 0.005, 0.005, 0.002, 0.0,
                        0.05, true, false, "纹理质量", "UV 间距比例"));
  p.Register(MakeInt(keys::kPostUvIterations, 10, 10, 10, 20, 1, 100, true, false,
                     "纹理质量", "UV 展开迭代次数"));
  p.Register(MakeInt(keys::kPostTextureAtlasSize, 4096, 2048, 4096, 8192, 512,
                     16384, true, false, "纹理质量", "纹理图集边长像素"));
  p.Register(MakeDouble(keys::kPostTextureViewWeight, 1.0, 1.0, 1.0, 1.0, 0.0,
                        10.0, true, false, "纹理质量", "多视图融合权重"));
  p.Register(MakeDouble(keys::kPostTextureOcclusionThreshold, 0.2, 0.2, 0.2, 0.15,
                        0.01, 1.0, true, false, "纹理质量",
                        "遮挡判定深度差阈值米"));
  p.Register(MakeInt(keys::kPostSimplifyTargetFaces, 0, 0, 0, 0, 0, 100000000,
                     true, false, "几何分辨率",
                     "简化目标面数。0 表示不简化，默认保留最高精度"));
  p.Register(MakeDouble(keys::kPostSimplifyErrorRatio, 0.001, 0.001, 0.001,
                        0.0005, 0.0, 0.1, true, false, "几何分辨率",
                        "简化允许误差比例"));

  // ---- 尺度类 ----
  p.Register(MakeInt(keys::kScaleReferenceType, 1, 1, 1, 1, 0, 3, true, false,
                     "尺度精度", "0=身份证 1=银行卡 2=自定义 3=手动"));
  p.Register(MakeDouble(keys::kScaleRefWidthMm, 85.60, 85.60, 85.60, 85.60,
                        10.0, 500.0, true, false, "尺度精度",
                        "参照物宽度毫米。ID-1 与中国二代身份证均为 85.60"));
  p.Register(MakeDouble(keys::kScaleRefHeightMm, 54.00, 54.00, 54.00, 54.00,
                        10.0, 500.0, true, false, "尺度精度",
                        "参照物高度毫米。中国二代身份证实际 54.00"));
  p.Register(MakeDouble(keys::kScaleRefSizeToleranceMm, 0.20, 0.50, 0.29, 0.20,
                        0.01, 5.0, true, false, "尺度精度",
                        "参照物尺寸公差毫米"));
  p.Register(MakeDouble(keys::kScaleRefFlatnessTolMm, 1.0, 3.0, 2.0, 1.0, 0.1,
                        20.0, true, false, "尺度精度", "参照物平整度容差毫米"));
  p.Register(MakeDouble(keys::kScaleImuPriorRelativeSigma, 0.10, 0.10, 0.10, 0.10,
                        0.01, 1.0, true, false, "尺度精度",
                        "IMU 尺度先验相对不确定度。仅作初值与校验"));
  p.Register(MakeDouble(keys::kScaleConsistencyMax, 0.03, 0.03, 0.03, 0.02, 0.001,
                        0.50, true, false, "尺度精度",
                        "长短边独立解的相对差上限"));
  p.Register(MakeDouble(keys::kScaleMinIncidenceDeg, 25.0, 25.0, 25.0, 20.0, 5.0,
                        60.0, true, false, "尺度精度", "最小入射角"));
  p.Register(MakeDouble(keys::kScaleRefineHuberPx, 2.0, 2.0, 2.0, 1.0, 0.1, 20.0,
                        true, false, "尺度精度", "标定优化鲁棒核像素"));
  p.Register(MakeInt(keys::kScaleRefineIterations, 50, 20, 50, 100, 5, 500, true,
                     false, "尺度精度", "标定非线性优化迭代数"));

  // ---- 扫描与关键帧类 ----
  p.Register(MakeInt(keys::kScanMode, 1, 1, 1, 1, 0, 1, true, false, "几何完整性",
                     "0=自由环绕 1=定点引导。默认定点引导"));
  p.Register(MakeDouble(keys::kScanViewpointDistanceM, 0.45, 0.45, 0.45, 0.45,
                        0.15, 1.50, true, false, "几何精度",
                        "目标扫描距离米。随物体尺寸缩放，公式见 docs/03"));
  p.Register(MakeDouble(keys::kScanIdealDistanceMinM, 0.35, 0.35, 0.35, 0.35,
                        0.10, 1.50, true, false, "几何精度", "理想距离下界米"));
  p.Register(MakeDouble(keys::kScanIdealDistanceMaxM, 0.60, 0.60, 0.60, 0.60,
                        0.20, 2.00, true, false, "几何精度", "理想距离上界米"));
  p.Register(MakeInt(keys::kScanAzimuthBins, 36, 36, 36, 36, 8, 180, true, false,
                     "几何完整性", "方位覆盖直方图分箱数"));
  p.Register(MakeInt(keys::kScanElevationBins, 18, 18, 18, 18, 4, 90, true, false,
                     "几何完整性", "俯仰覆盖直方图分箱数"));
  p.Register(MakeDouble(keys::kScanKfMinRotationDeg, 0.35, 0.35, 0.35, 0.35, 0.05,
                        10.0, true, false, "几何精度", "关键帧最小旋转增量角"));
  p.Register(MakeDouble(keys::kScanKfMinTranslationRatio, 0.008, 0.008, 0.008,
                        0.008, 0.0005, 0.2, true, false, "几何精度",
                        "关键帧最小平移增量占物体尺寸比"));
  p.Register(MakeDouble(keys::kScanKfMinQualityScore, 0.5, 0.5, 0.5, 0.5, 0.0, 1.0,
                        true, false, "几何精度", "关键帧最低质量分"));
  p.Register(MakeDouble(keys::kScanKfMinViewIncrementDeg, 0.8, 0.8, 0.8, 0.8, 0.1,
                        30.0, true, false, "几何精度",
                        "关键帧最小视角增量角。需实测标定"));
  p.Register(MakeDouble(keys::kScanCoverageTargetAzimuth, 0.85, 0.85, 0.85, 0.85,
                        0.3, 1.0, true, false, "几何完整性", "方位覆盖达标线"));
  p.Register(MakeDouble(keys::kScanCoverageTargetElevation, 0.60, 0.60, 0.60, 0.60,
                        0.2, 1.0, true, false, "几何完整性", "俯仰覆盖达标线"));
  p.Register(MakeInt(keys::kScanBottomStrategy, 1, 1, 1, 1, 0, 2, true, false,
                     "几何完整性",
                     "0=贴近支撑面朝上 1=翻转物体 2=泊松外推并标注"));
  p.Register(MakeBool(keys::kScanStabilizationEnabled, true, true, true, true, true,
                      false, "定位精度", "IMU 稳像开关"));
  p.Register(MakeDouble(keys::kScanReviewMinCoverage, 0.85, 0.85, 0.85, 0.85, 0.3,
                        1.0, true, false, "几何完整性", "确认前覆盖度门槛"));
  p.Register(MakeInt(keys::kScanReviewMaxAnomalies, 3, 3, 3, 3, 0, 100, true, false,
                     "几何完整性", "确认前允许的异常项数"));

  // ---- 性能类 ----
  //
  // 白名单 W-1..W-10 的承载键（docs/13 §4.2）。所有 W-* 只影响并行度 / 批大小 /
  // 分块粒度 / 内存池，**不触碰任何精度阈值**（00 R-PREC-4）。
  //
  // 注意：键名以 docs/08 §G11 为唯一真源。此前实现用的是 `perf_thread_count`
  // 而 docs/00/04/05/08/11/13 全线引用 `perf_thread_pool_size` —— 属键名漂移，
  // 已在 2026-10-31 按文档口径修正（tools/param_drift_check.py 加闸门防复发）。
  p.Register(MakeInt(keys::kPerfThreadPoolSize, 0, 0, 0, 0, 0, 32, true, false,
                     "速度、内存", "线程池并发数。0=自动（CPU 核数-1，上限 8）。白名单 W-1"));
  p.Register(MakeInt(keys::kPerfGpuBatchSize, 32, 16, 32, 64, 1, 256,
                     true, false, "速度、内存", "GPU 批次大小。白名单 W-2"));
  p.Register(MakeInt(keys::kPerfBlockBatchSize, 16, 8, 16, 32, 1, 256,
                     true, false, "速度", "后处理算子分批粒度。白名单 W-5"));
  p.Register(MakeInt(keys::kPerfMemoryPoolPreallocMb, 256, 128, 256, 512, 32, 4096,
                     true, false, "速度、内存", "内存池预分配 MB。白名单 W-6"));
  p.Register(MakeInt(keys::kPerfReadAheadFrames, 32, 32, 32, 32, 1, 256,
                     false, true, "速度、内存", "重建期读盘预读窗口。白名单 W-10"));
  p.Register(MakeInt(keys::kPerfMemoryLimitMb, 2048, 1024, 2048, 4096, 256, 16384,
                     true, false, "速度",
                     "内存软上限 MB。触达时分块处理，不降精度"));
  p.Register(MakeDouble(keys::kPerfMemoryHighWaterRatio, 0.85, 0.80, 0.85, 0.90, 0.5,
                        0.98, true, false, "内存", "内存水位触发落盘/分块的阈值"));
  p.Register(MakeBool(keys::kPerfSpillEnabled, true, true, true, true, true, false,
                      "速度、内存", "允许把可重建中间结果落盘（docs/13 §3.3）"));
  p.Register(MakeBool(keys::kPerfTsdfSpillEnabled, true, true, true, true, true,
                      false, "速度、内存", "允许 TSDF 块级落盘"));
  p.Register(MakeBool(keys::kPerfPoissonSpillEnabled, true, true, true, true, true,
                      false, "速度、内存", "允许泊松块结果落盘（06 §1.7 C）"));
  p.Register(MakeInt(keys::kPerfMaxConcurrentTasks, 4, 2, 4, 6, 1, 32, true, false,
                     "速度", "L5 编排层并发任务数。白名单 W-9"));
  p.Register(MakeInt(keys::kPerfMemoryTier, -1, -1, -1, -1, -1, 3, false, true,
                     "速度、内存", "内存档位。-1=自动探测；0=T0..3=T3"));
  p.Register(MakeInt(keys::kPerfThermalThrottleLevel, 0, 0, 0, 0, 0, 4, false,
                     true, "速度", "热降级档位。专家项"));

  return p;
}

}  // namespace recon
