// SPDX-License-Identifier: BSD-3-Clause
//
// ViOEstimator：MSCKF（多状态约束卡尔曼滤波）的滑窗优化实现。
//
// 契约：docs/05_单目管线.md §3（§3.2 状态、§3.6 零空间与边缘化、§3.7 残差、
// §3.8 初始化）。与 §3.1「总体结构」的阶段对应：
//
//   灰度 ─► ExtractAndTrack（KLT + 双向检查）─► SpawnFeatures（网格空位补充）
//   IMU  ─► ImuPreintegrator ─► AppendKeyframe（状态递推，挂新块）
//                                      │
//                                      ▼
//                             UpdateState（MSCKF 滑窗优化）
//                                      │
//                                      ▼
//                              MarginalizeOldest（滑窗满）
//
// ---------------------------------------------------------------------------
// 四处必须写清楚的实现选择
// ---------------------------------------------------------------------------
//
// **1. 优化形式：阻尼 Gauss-Newton，而非 EKF 递推。**
//   §3.6.3 的边缘化是「Schur 补 + 协方差传播」。标准 MSCKF 用 EKF 递推
//   （时间更新传播 P、量测更新用 Kalman 增益）。这里用等价的最小二乘形式：
//   每次更新在滑窗状态上做阻尼 Gauss-Newton，H 即信息矩阵。
//   两者在线性化点与先验相同时解相同，但最小二乘形式的数值行为更易验证
//   （可直接检查代价单调下降、残差趋零），且复用 core 已有的 linalg，
//   不引入 Ceres（CI 无 Ceres，见 CMakeLists）。
//
// **2. 参考系约定（最容易搞错的一处，写死在此）。**
//   · `kf_orientation_[i]` = R_i = **IMU → 世界**（§3.2.1 明确规定）。
//     因此 world→camera 旋转 = `R_ic · R_iᵀ`，**不是** `R_ic · R_i`。
//     （写成后者会让相机绕反方向转 —— 表现为「更新后残差变大、代价不降」，
//      又因为只是符号问题而极难定位。）
//   · `kf_position_[i]` = p_i，IMU 在世界系的位置（米）。
//   · 预积分只积分 **body 系比力**，重力在世界系残差里显式出现
//     （与 imu_preintegration.cpp 的约定严格一致）。
//   · 世界系 Z 轴朝上（重力 ≈ (0,0,−9.80665)），导出到重建世界系（Y-up）
//     的转换在 SfM 初始化时统一执行一次（§3.10），不在本模块做。
//
// **3. 零偏在更新中是「可优化量」（本轮接通 ∂Δ/∂b）。**
//   预积分对零偏的雅可比 ∂Δ/∂b（§3.3.5）已在 ImuPreintegrator 中实现
//   （dr/dvg/dvba/dp 五块），残差装配时用 `pm.Correct(bg,ba)` 把当前零偏
//   一阶修正进预积分量，于是 ∂r/∂b ≠ 0，零偏进入 H 的列、可被优化。
//   · 零偏初值仍由初始化给出（静止段均值）；
//   · 弱先验 σ=2e-4（§3.7.3）保留，防止视觉不可观时零偏漂移；
//   · 重力作为 soft 状态参与优化（雅可比解析、形式简单）。
//   收敛校验见 tests/unit/test_preint_jacobian.cpp（中心差分，相对误差 < 1e-5）。
//
// **4. 视觉雅可比解析、IMU 雅可比数值差分。**
//   · 视觉（§3.6.2）：投影 + 零空间投影的导数形式明确、且是精度关键路径，
//     写解析式便于逐项对照文档审计。
//   · IMU（§3.7.2）：每区间只有 9 个残差 × 12 个相关列，数值差分成本可忽略，
//     而解析式（含 Exp/Log 的右雅可比）写错极难发现。差分以 h=1e-6 给出
//     ~1e-9 级一致性，作为第一版更稳。

#include <algorithm>
#include <cmath>
#include <cstring>

#include "recon/robust_fit.h"
#include "recon/vio.h"

namespace recon {
namespace {

constexpr double kGravityMagnitude = 9.80665;

// IMU 残差的白化标准差。
//
// 口径来源：§3.7.2 要求用预积分协方差 P(15×15) 的白化。本实现尚未做
// 完整的 P 传播（见文件头「零偏静态」说明），改用等效的**经验白化尺度**：
// 残差被零偏不确定性主导，其量级随区间长度线性增长。
//
//   σ_rot = 2.0e-3 rad   @ Δt = 33 ms
//   σ_vel = 2.0e-2 m/s   @ Δt = 33 ms
//   σ_pos = 1.0e-2 m     @ Δt = 33 ms
//
// 三者按 Δt/0.033 线性缩放，并各设一个下限防止 Δt→0 时权重爆炸。
// 这一组值使「静止段残差 ≈ 0.1σ、正常手持运动残差 ≈ 1σ」，
// 即不让 IMU 项压过视觉项、也不让视觉项无视 IMU 项。
struct ImuNoise { double rot, vel, pos; };

ImuNoise ImuResidualSigma(double dt) {
  const double k = (dt > 1e-6) ? dt / 0.033 : 1.0;
  ImuNoise s;
  s.rot = std::max(2.0e-3 * k, 1.0e-4);
  s.vel = std::max(2.0e-2 * k, 1.0e-3);
  s.pos = std::max(1.0e-2 * k, 1.0e-3);
  return s;
}

// 3×3 反对称矩阵。
Mat3d Skew(const Vec3d& v) {
  Mat3d m;
  m(0, 1) = -v(2);
  m(0, 2) = v(1);
  m(1, 0) = v(2);
  m(1, 2) = -v(0);
  m(2, 0) = -v(1);
  m(2, 1) = v(0);
  return m;
}

// 把单位向量 from 旋到 to 的最小旋转。
Mat3d RotationBetween(const Vec3d& from, const Vec3d& to) {
  const Vec3d a = from.normalized();
  const Vec3d b = to.normalized();
  if (a.norm() < 1e-9 || b.norm() < 1e-9) return Mat3d::Identity();
  const double c = std::clamp(a.dot(b), -1.0, 1.0);
  const Vec3d axis = a.cross(b);
  const double s = axis.norm();
  if (s > 1e-9) return ExpSO3(axis * (std::atan2(s, c) / s));
  if (c > 0.0) return Mat3d::Identity();
  Vec3d perp = a.cross(Vec3d(1.0, 0.0, 0.0));
  if (perp.norm() < 1e-6) perp = a.cross(Vec3d(0.0, 1.0, 0.0));
  return ExpSO3(perp.normalized() * kPi);
}

// 相邻关键帧的 IMU 残差（9 维，顺序 [旋转; 速度; 位置]）。
//
//   r_R = Log( R_iᵀ·R_j·ΔR̂ᵀ )
//   r_v = R_iᵀ(v_j − v_i − g·Δt)   − Δv̂
//   r_p = R_iᵀ(p_j − p_i − v_i·Δt − ½g·Δt²) − Δp̂
//
// **零偏处理（本轮新增，关键）**：`ΔR̂/Δv̂/Δp̂` 不是直接用积分时的值，而是先按
// 当前零偏 `bg/ba` 做一阶修正（`pm.Correct`，docs/05 §3.3.5）。只有这样，
// 残差才对零偏有非零导数，零偏才进入 H 的列、才可被优化 —— 这正是本函数
// 新增 `bg`/`ba` 两个入参的原因。若不修正，∂r/∂b ≡ 0，零偏只能被弱先验
// 拉向零，永远动不了（旧版即如此）。
//
// 自洽性（这是判据，不是巧合）：静止水平放置时 a_m=(0,0,+g)、g_W=(0,0,−g)，
// 预积分给出 Δv=(0,0,+g)Δt、Δp=½(0,0,+g)Δt²，代入即三项同时为 0；
// 此时 bg/ba 提供的修正为零（δb=0），残差仍为 0。
void ImuResidual(const Mat3d& R0, const Vec3d& p0, const Vec3d& v0,
                 const Mat3d& R1, const Vec3d& p1, const Vec3d& v1,
                 const Vec3d& gravity, const PreintegratedMeasurement& pm,
                 const Vec3d& bg, const Vec3d& ba, double out[9]) {
  const double dt = pm.duration_s;

  // 用当前零偏修正预积分量（一阶，δb=0 时与原值逐位相同）。
  Vec3d d_p_c, d_v_c;
  Mat3d d_q_c;
  pm.Correct(&d_p_c, &d_q_c, &d_v_c, bg, ba);

  const Vec3d dp = p1 - p0 - v0 * dt - 0.5 * (gravity * (dt * dt));
  const Vec3d dv = v1 - v0 - gravity * dt;
  const Vec3d rp = R0.transpose() * dp - d_p_c;
  const Vec3d rv = R0.transpose() * dv - d_v_c;
  const Vec3d rR = LogSO3(R0.transpose() * R1 * d_q_c.transpose());
  for (int i = 0; i < 3; ++i) {
    out[i] = rR(i);
    out[3 + i] = rv(i);
    out[6 + i] = rp(i);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// 构造与配置
// ---------------------------------------------------------------------------

ViOEstimator::ViOEstimator(const VioConfig& config) : config_(config) {
  state_.gravity = Vec3d(0.0, 0.0, -kGravityMagnitude);
  state_.initialized = false;
}

void ViOEstimator::SetIntrinsics(const CameraIntrinsics& intr) {
  intrinsics_ = intr;
  intrinsics_set_ = true;
}

int ViOEstimator::StateDim() const {
  // 每关键帧 9（δθ, δv, δp）+ 全局 9（δb_g 3、δb_a 3、δg 3）。
  // §3.2.2 写的是 +6（零偏），本实现多 3 维重力 —— §3.2.3 明确采用
  // 「方案 B：g_W 为 3 维状态 + 软约束」，故状态维数是 K×9+9。
  return static_cast<int>(state_.keyframes.size()) * 9 + 9;
}

// ---------------------------------------------------------------------------
// 初始化（docs/05 §3.8）
// ---------------------------------------------------------------------------
bool ViOEstimator::Initialize(const std::vector<ImuSample>& imu_samples) {
  if (imu_samples.size() < 2) return false;
  // 静止检测（§3.8.1）。失败不阻断 —— 走 §3.8.4 的运动初始化降级路径
  // （本实现取其退化的形式：零偏取均值、重力取标称方向）。
  const StaticDetectionResult sd = DetectStatic(imu_samples, 0.10, 0.05, 2.0);
  static_detected_ = sd.is_static;

  Vec3d mean_accel = Vec3d::Zero();
  Vec3d mean_gyro = Vec3d::Zero();
  for (const ImuSample& s : imu_samples) {
    mean_accel += s.accel;
    mean_gyro += s.gyro;
  }
  const double inv_n = 1.0 / static_cast<double>(imu_samples.size());
  mean_accel *= inv_n;
  mean_gyro *= inv_n;

  if (mean_accel.norm() < 1e-3) return false;  // 自由落体或传感器异常

  // 重力方向（§3.8.1）：
  //   ā_m ≈ −R_WIᵀ·g_W ，g_W = (0,0,−g)  ⇒  ā_m/‖ā_m‖ = R_WIᵀ·ẑ
  //   故令 R_WI·(ā_m/‖ā_m‖) = ẑ，即 R_WI = RotationBetween(ā_m, ẑ)。
  const Mat3d R_imu_world = RotationBetween(mean_accel, Vec3d(0.0, 0.0, 1.0));

  if (static_detected_) {
    // 静止时零偏可分离（§3.8.1 的零偏初值公式）。
    state_.bias_gyro = mean_gyro;
    state_.bias_acc =
        mean_accel - (R_imu_world.transpose() * Vec3d(0.0, 0.0, kGravityMagnitude));
  } else {
    // 运动初始化：零偏不可分离，陀螺零偏仍可取均值，
    // 加计零偏置零（§3.8.4 步骤 2 的等价形式）。
    state_.bias_gyro = mean_gyro;
    state_.bias_acc = Vec3d::Zero();
  }
  state_.gravity = Vec3d(0.0, 0.0, -kGravityMagnitude);
  init_R_imu_world_ = R_imu_world;
  init_pose_valid_ = true;

  // 清空滑窗与全部派生状态。
  state_.keyframes.clear();
  kf_orientation_.clear();
  kf_position_.clear();
  kf_frame_id_.clear();
  covariance_.clear();
  features_.clear();
  prev_points_.clear();
  landmark_positions_.clear();
  active_count_ = 0;

  state_.initialized = true;
  return true;
}

// ---------------------------------------------------------------------------
// 关键帧挂载（状态递推）
// ---------------------------------------------------------------------------
int ViOEstimator::AppendKeyframe(const Vec3d& velocity,
                                 const PreintegratedMeasurement& preint,
                                 bool has_preint) {
  const int K = static_cast<int>(state_.keyframes.size());
  const int old_n = K * 9 + 9;
  const int new_n = old_n + 9;

  Mat3d R_new = init_R_imu_world_;
  Vec3d p_new = Vec3d::Zero();
  Vec3d v_new = velocity;

  if (K > 0 && has_preint && preint.duration_s > 1e-6) {
    const Mat3d& R_prev = kf_orientation_.back();
    const Vec3d& p_prev = kf_position_.back();
    const Vec3d& v_prev = state_.keyframes.back().velocity;
    const double dt = preint.duration_s;
    const Vec3d& g = state_.gravity;

    // §3.7.2 残差定义的严格逆解（不含任何额外近似）：
    //   R_j = R_i·ΔR̂
    //   v_j = v_i + g·Δt + R_i·Δv̂
    //   p_j = p_i + v_i·Δt + ½g·Δt² + R_i·Δp̂
    R_new = R_prev * preint.delta_q;
    v_new = v_prev + g * dt + R_prev * preint.delta_v;
    p_new = p_prev + v_prev * dt + 0.5 * (g * (dt * dt)) + R_prev * preint.delta_p;
    if (!R_new.isFinite() || !p_new.isFinite() || !v_new.isFinite()) return -1;
  } else if (K > 0) {
    // 无预积分（IMU 空洞 / 首个区间）：位姿保持，速度继承。
    R_new = kf_orientation_.back();
    p_new = kf_position_.back();
    v_new = state_.keyframes.back().velocity;
  }

  KeyframeState kf;
  kf.frame_id = next_frame_id_ - 1;  // AddFrame 已自增
  // 时间戳取本帧（AppendKeyframe 在 AddFrame 内被调用，last_frame_time_s_
  // 已是本帧的值）。VIO 输出契约要求每关键帧带时间戳（docs/05 §3.10）。
  kf.timestamp_ns = static_cast<int64_t>(std::llround(last_frame_time_s_ * 1e9));
  kf.velocity = v_new;
  kf.has_velocity = true;
  kf.has_preint = has_preint;
  if (has_preint) kf.preint = preint;

  state_.keyframes.push_back(kf);
  kf_orientation_.push_back(R_new);
  kf_position_.push_back(p_new);
  kf_frame_id_.push_back(kf.frame_id);

  // 协方差扩张：新块给一个与预积分噪声相称的先验方差。
  const double dt = has_preint ? std::max(preint.duration_s, 1e-3) : 1e-3;
  std::vector<double> wider(static_cast<size_t>(new_n) * new_n, 0.0);
  for (int i = 0; i < old_n; ++i) {
    for (int j = 0; j < old_n; ++j) {
      wider[static_cast<size_t>(i) * new_n + j] =
          covariance_.empty() ? 0.0
                              : covariance_[static_cast<size_t>(i) * old_n + j];
    }
  }
  // 全局块从旧的 9 行/列平移到底部（零偏 6 + 重力 3）。
  if (old_n > 9) {
    const int old_global = old_n - 9;
    const int new_global = new_n - 9;
    for (int i = 0; i < 9; ++i) {
      for (int j = 0; j < 9; ++j) {
        wider[static_cast<size_t>(new_global + i) * new_n + (new_global + j)] =
            covariance_[static_cast<size_t>(old_global + i) * old_n +
                        (old_global + j)];
      }
    }
  }
  const ImuNoise ns = ImuResidualSigma(dt);
  const int base = K * 9;
  for (int i = 0; i < 3; ++i) {
    const double v_rot = ns.rot * ns.rot;
    const double v_vel = ns.vel * ns.vel;
    const double v_pos = ns.pos * ns.pos;
    wider[static_cast<size_t>(base + i) * new_n + (base + i)] = v_rot;
    wider[static_cast<size_t>(base + 3 + i) * new_n + (base + 3 + i)] = v_vel;
    wider[static_cast<size_t>(base + 6 + i) * new_n + (base + 6 + i)] = v_pos;
  }
  covariance_.swap(wider);
  return base;
}

Vec3d ViOEstimator::ImuToCameraLeverArm() const {
  // X_cam = R_ic·X_imu + t_ic ⇒ 相机光心在 IMU 系的位置 = −R_icᵀ·t_ic。
  // 保留为独立函数以便与 07 文档的标定口径对照（目前外参为单位）。
  return -(extrin_R_ic_.transpose() * extrin_t_ic_);
}

// ---------------------------------------------------------------------------
// 相机位姿同步
// ---------------------------------------------------------------------------
CameraPose ViOEstimator::CameraPoseAt(int k) const {
  const Mat3d R_i = kf_orientation_[static_cast<size_t>(k)];
  const Vec3d p_i = kf_position_[static_cast<size_t>(k)];
  // X_imu = R_iᵀ(X_w − p_i) ； X_cam = R_ic·X_imu + t_ic
  // ⇒ R_cw = R_ic·R_iᵀ ， t_cw = t_ic − R_cw·p_i
  CameraPose pose;
  pose.R = extrin_R_ic_ * R_i.transpose();
  pose.t = extrin_t_ic_ - pose.R * p_i;
  return pose;
}

void ViOEstimator::SyncCameraPoses() {
  for (size_t k = 0; k < state_.keyframes.size(); ++k) {
    state_.keyframes[k].pose = CameraPoseAt(static_cast<int>(k));
  }
}

// ---------------------------------------------------------------------------
// 特征提取与跟踪
// ---------------------------------------------------------------------------
void ViOEstimator::ExtractAndTrack(const uint8_t* gray, int width, int height,
                                   int stride) {
  if (!has_prev_frame_ || prev_w_ != width || prev_h_ != height) {
    // 首帧或分辨率变化：无法跟踪，清空重来。
    features_.clear();
    prev_points_.clear();
    landmark_positions_.clear();
    active_count_ = 0;
    last_motion_px_ = 1e9;  // 首帧必定建立关键帧
    return;
  }

  std::vector<FeatureTrack> kept;
  std::vector<Vec2d> kept_prev;
  std::vector<Vec3d> kept_pos;
  kept.reserve(features_.size());

  std::vector<double> displacements;
  displacements.reserve(features_.size());

  const uint64_t cur_frame = next_frame_id_ - 1;
  const size_t n = std::min(features_.size(), prev_points_.size());

  for (size_t i = 0; i < n; ++i) {
    const Vec2d p0 = prev_points_[i];
    if (p0(0) < 1.0 || p0(1) < 1.0 || p0(0) > width - 2.0 ||
        p0(1) > height - 2.0) {
      continue;
    }

    const TrackResult tr = TrackFeature(prev_gray_.data(), prev_w_, prev_h_,
                                        prev_w_, gray, width, height, stride, p0,
                                        config_);
    if (!tr.success) continue;
    // 双向一致性（§3.5.4）：把失败率从 ~8% 压到 ~1.5%，代价是每帧一次额外 KLT。
    if (!CheckForwardBackward(prev_gray_.data(), prev_w_, prev_h_, prev_w_, gray,
                              width, height, stride, p0, tr.position, config_)) {
      continue;
    }
    if (tr.position(0) < 1.0 || tr.position(1) < 1.0 ||
        tr.position(0) > width - 2.0 || tr.position(1) > height - 2.0) {
      continue;
    }

    FeatureTrack f = features_[i];
    ++f.age;
    ++f.maturity;
    if (f.age > config_.max_age) continue;  // 超寿主动移除（§3.4.3）

    FeatureTrack::Observation obs;
    obs.frame_id = cur_frame;
    obs.u = tr.position(0);
    obs.v = tr.position(1);
    f.observations.push_back(obs);

    displacements.push_back(tr.displacement);
    kept.push_back(f);
    kept_prev.push_back(tr.position);
    kept_pos.push_back(i < landmark_positions_.size() ? landmark_positions_[i]
                                                      : Vec3d::Zero());
  }

  features_.swap(kept);
  prev_points_.swap(kept_prev);
  landmark_positions_.swap(kept_pos);
  active_count_ = static_cast<int>(features_.size());

  if (displacements.empty()) {
    last_motion_px_ = 0.0;
  } else {
    std::sort(displacements.begin(), displacements.end());
    last_motion_px_ = displacements[displacements.size() / 2];
  }
}

void ViOEstimator::SpawnFeatures(const uint8_t* gray, int width, int height,
                                 int stride) {
  const int target = config_.feature_count;
  // 存活数已足够 → 不补充（§3.4.3：低于 0.6×上限才补充）。
  if (active_count_ >= static_cast<int>(target * config_.feature_spawn_ratio)) {
    return;
  }

  // 已有特征位置作为最小间距约束输入，避免同区域重复播种（FL-1）。
  std::vector<Vec2d> existing = prev_points_;
  const CornerDetection corners =
      DetectCorners(gray, width, height, stride, config_, &existing);

  const uint64_t cur_frame = next_frame_id_ - 1;
  for (size_t i = 0; i < corners.points.size(); ++i) {
    if (active_count_ >= target) break;
    FeatureTrack f;
    FeatureTrack::Observation obs;
    obs.frame_id = cur_frame;
    obs.u = corners.points[i](0);
    obs.v = corners.points[i](1);
    f.observations.push_back(obs);
    f.age = 0;
    f.maturity = 0;
    f.spawn_keyframe = static_cast<int>(state_.keyframes.size()) - 1;
    features_.push_back(f);
    prev_points_.push_back(corners.points[i]);
    landmark_positions_.push_back(Vec3d::Zero());
    ++active_count_;
  }
}

// ---------------------------------------------------------------------------
// 逐帧入口
// ---------------------------------------------------------------------------
bool ViOEstimator::AddFrame(const uint8_t* gray, int width, int height, int stride,
                            int64_t timestamp_ns,
                            const std::vector<ImuSample>& imu_samples) {
  if (gray == nullptr || width <= 0 || height <= 0) return false;
  if (stride <= 0) stride = width;

  if (!intrinsics_set_) {
    // 未标定时的保守缺省：fx=fy=max(w,h)（约 60° FOV），主点居中，无畸变。
    // 真实设备必须由平台传入标定值（docs/05 §2.3）—— 缺省值只保证
    // 「不会崩、几何自洽」，不保证精度。
    intrinsics_.fx = intrinsics_.fy =
        static_cast<double>(std::max(width, height));
    intrinsics_.cx = (width - 1) * 0.5;
    intrinsics_.cy = (height - 1) * 0.5;
    intrinsics_.width = width;
    intrinsics_.height = height;
    intrinsics_set_ = true;
  }

  const uint64_t frame_id = next_frame_id_++;
  last_frame_time_s_ = static_cast<double>(timestamp_ns) * 1e-9;

  // 归一化为紧凑灰度（去掉行间 stride），后续算子按 width 步长访问。
  std::vector<uint8_t> cur(static_cast<size_t>(width) * height);
  for (int y = 0; y < height; ++y) {
    std::memcpy(cur.data() + static_cast<size_t>(y) * width,
                gray + static_cast<size_t>(y) * stride,
                static_cast<size_t>(width));
  }

  // 未初始化：把本帧 IMU 攒进缓冲，攒够静止检测窗口再初始化。
  //
  // 不能直接用单帧样本初始化 —— 单帧只有 ~5 ms 的 IMU（C ABI 口径），
  // 而 §3.8.1 的静止窗口要 ≥ 2.0 s。用 5 ms 去跑 DetectStatic 必然返回
  // 「观测窗口不足」，估计器会永远停在未初始化状态（本项目实测踩过：
  // 24 帧全部返回 false、0 个关键帧）。
  if (!state_.initialized) {
    for (const ImuSample& s : imu_samples) {
      if (imu_buffer_.empty() ||
          s.timestamp_ns > imu_buffer_.back().timestamp_ns) {
        imu_buffer_.push_back(s);
      }
    }
    // 缓冲上限：保留最近 4 s（静止窗口 2 s 的 2 倍，够 DetectStatic 判定）。
    const double span_s =
        imu_buffer_.empty()
            ? 0.0
            : static_cast<double>(imu_buffer_.back().timestamp_ns -
                                  imu_buffer_.front().timestamp_ns) *
                  1e-9;
    if (span_s > 4.0) {
      const int64_t cut = imu_buffer_.back().timestamp_ns -
                          static_cast<int64_t>(2.0e9);
      size_t keep = 0;
      while (keep < imu_buffer_.size() &&
             imu_buffer_[keep].timestamp_ns < cut) {
        ++keep;
      }
      imu_buffer_.erase(imu_buffer_.begin(),
                        imu_buffer_.begin() + static_cast<long>(keep));
    }

    prev_gray_.swap(cur);
    prev_w_ = width;
    prev_h_ = height;
    has_prev_frame_ = true;

    // 窗口不足 → 继续攒（返回 false，不建立关键帧）。
    if (imu_buffer_.size() < 4 || span_s < 2.0) return false;

    if (!Initialize(imu_buffer_)) {
      // 缓冲内容异常（例如自由落体）：清空重攒。
      imu_buffer_.clear();
      return false;
    }
    // 初始化完成。预积分器从**本帧开始**累积（不回溯历史帧的 IMU）：
    // 回溯会让「上一关键帧 → 本帧」区间与初始化窗口重叠，区间长度失真。
    imu_buffer_.clear();
    has_prev_frame_ = false;  // 让下一帧重新播种特征（初始化期无跟踪价值）
    preint_.Reset(state_.bias_gyro, state_.bias_acc,
                  Vec3d(config_.imu_noise_gyro * config_.imu_noise_gyro,
                        config_.imu_noise_gyro * config_.imu_noise_gyro,
                        config_.imu_noise_gyro * config_.imu_noise_gyro),
                  Vec3d(config_.imu_noise_acc * config_.imu_noise_acc,
                        config_.imu_noise_acc * config_.imu_noise_acc,
                        config_.imu_noise_acc * config_.imu_noise_acc),
                  Vec3d(config_.imu_noise_bias_gyro * config_.imu_noise_bias_gyro,
                        config_.imu_noise_bias_gyro * config_.imu_noise_bias_gyro,
                        config_.imu_noise_bias_gyro * config_.imu_noise_bias_gyro),
                  Vec3d(config_.imu_noise_bias_acc * config_.imu_noise_bias_acc,
                        config_.imu_noise_bias_acc * config_.imu_noise_bias_acc,
                        config_.imu_noise_bias_acc * config_.imu_noise_bias_acc));
    preint_.SetGravity(state_.gravity);
    preint_active_ = true;
    return false;  // 本帧不建关键帧，下一帧开始正常流程
  }

  // IMU 预积分累加（本帧曝光前的采样，构成「上一关键帧 → 本帧」区间）。
  if (preint_active_) {
    for (const ImuSample& s : imu_samples) {
      preint_.AddSample(static_cast<double>(s.timestamp_ns), s.accel, s.gyro);
    }
  }

  ExtractAndTrack(cur.data(), width, height, width);

  // 关键帧判据。
  //
  // 用「帧间中位位移」而非时间间隔：场景静止时不该产生关键帧 ——
  // 那样的观测几何全零视差，会被 §3.6.2 的判据整体丢弃，白跑一遍更新。
  // 阈值取最小特征间距的一半，保证新关键帧确实带来了新的几何。
  const bool is_first = state_.keyframes.empty();
  const double kf_motion_px = std::max(3.0, config_.min_pixel_distance * 0.5);
  const bool want_keyframe = is_first || last_motion_px_ >= kf_motion_px;

  bool accepted = false;
  if (want_keyframe) {
    PreintegratedMeasurement pm;
    bool has_pm = false;
    if (preint_active_ && !is_first && preint_.duration_s() > 1e-3) {
      has_pm = preint_.Finalize(&pm).ok;
    }

    const Vec3d v_guess =
        is_first ? Vec3d::Zero() : state_.keyframes.back().velocity;
    const int base = AppendKeyframe(v_guess, pm, has_pm);
    if (base < 0) return false;

    // 本帧新观测归到刚挂上的关键帧（§3.4.3 FL-3：持续累积观测）。
    for (FeatureTrack& f : features_) {
      if (!f.observations.empty() &&
          f.observations.back().frame_id == frame_id) {
        f.spawn_keyframe = static_cast<int>(state_.keyframes.size()) - 1;
      }
    }

    // 区间结束 → 重置预积分（§3.3.6）。
    if (preint_active_) {
      preint_.Reset(state_.bias_gyro, state_.bias_acc,
                    Vec3d(config_.imu_noise_gyro * config_.imu_noise_gyro,
                          config_.imu_noise_gyro * config_.imu_noise_gyro,
                          config_.imu_noise_gyro * config_.imu_noise_gyro),
                    Vec3d(config_.imu_noise_acc * config_.imu_noise_acc,
                          config_.imu_noise_acc * config_.imu_noise_acc,
                          config_.imu_noise_acc * config_.imu_noise_acc),
                    Vec3d(config_.imu_noise_bias_gyro *
                              config_.imu_noise_bias_gyro,
                          config_.imu_noise_bias_gyro *
                              config_.imu_noise_bias_gyro,
                          config_.imu_noise_bias_gyro *
                              config_.imu_noise_bias_gyro),
                    Vec3d(config_.imu_noise_bias_acc *
                              config_.imu_noise_bias_acc,
                          config_.imu_noise_bias_acc *
                              config_.imu_noise_bias_acc,
                          config_.imu_noise_bias_acc *
                              config_.imu_noise_bias_acc));
      preint_.SetGravity(state_.gravity);
    }
    accepted = true;
  }

  // 特征补充：新关键帧必补（此时 maturity 全部归零，需要新观测），
  // 或存活数已低于阈值。
  if (accepted || active_count_ < static_cast<int>(config_.feature_count *
                                                   config_.feature_spawn_ratio)) {
    SpawnFeatures(cur.data(), width, height, width);
  }

  // 三角化 + MSCKF 更新。
  if (accepted && state_.keyframes.size() >= 2) {
    RetriangulateLandmarks();
    // 滑窗满 → 先边缘化最旧帧（§3.6.3），再重新线性化。
    while (static_cast<int>(state_.keyframes.size()) > config_.window_size) {
      MarginalizeOldest();
      RetriangulateLandmarks();
    }
    UpdateState();
  }
  SyncCameraPoses();

  prev_gray_.swap(cur);
  prev_w_ = width;
  prev_h_ = height;
  has_prev_frame_ = true;
  return accepted;
}

// ---------------------------------------------------------------------------
// 地标三角化
// ---------------------------------------------------------------------------
void ViOEstimator::RetriangulateLandmarks() {
  const int K = static_cast<int>(state_.keyframes.size());
  if (K < 2) return;

  auto kf_index_of = [&](uint64_t fid) -> int {
    for (int i = 0; i < K; ++i) {
      if (kf_frame_id_[static_cast<size_t>(i)] == fid) return i;
    }
    return -1;
  };

  for (size_t i = 0; i < features_.size(); ++i) {
    FeatureTrack& f = features_[i];
    f.has_position = false;
    if (f.observations.size() < 2) continue;

    // 取视差最大的一对观测。相邻帧的视差可能极小 → 深度病态；
    // 用首末两帧（时间跨度最大）比用相邻帧稳定得多。
    const size_t a = 0;
    const size_t b = f.observations.size() - 1;
    const int ka = kf_index_of(f.observations[a].frame_id);
    const int kb = kf_index_of(f.observations[b].frame_id);
    if (ka < 0 || kb < 0 || ka == kb) continue;

    std::vector<Vec2d> obs;
    std::vector<CameraPose> poses;
    obs.push_back(Vec2d(f.observations[a].u, f.observations[a].v));
    obs.push_back(Vec2d(f.observations[b].u, f.observations[b].v));
    poses.push_back(CameraPoseAt(ka));
    poses.push_back(CameraPoseAt(kb));

    // 复用零空间构造：它内部就是 DLT 三角化 + 视差/退化判据，
    // 与 §3.6.2 的判据完全同源（不另写一份，避免漂移）。
    Vec3d x_cam0;
    const NullspaceConstraint c =
        BuildNullspaceConstraint(obs, poses, intrinsics_, config_, &x_cam0);
    if (c.reject != NullspaceConstraint::Reject::kNone) continue;

    if (i < landmark_positions_.size()) {
      landmark_positions_[i] = c.position_world;
    }
    f.position = c.position_world;
    f.has_position = true;
  }
}

// ---------------------------------------------------------------------------
// 滑窗边缘化（docs/05 §3.6.3）
// ---------------------------------------------------------------------------
void ViOEstimator::MarginalizeOldest() {
  const int K = static_cast<int>(state_.keyframes.size());
  if (K <= 1) return;

  const uint64_t oldest_frame = kf_frame_id_.front();
  const int old_n = K * 9 + 9;
  const int new_n = old_n - 9;

  // ---- ① Schur 补（MG-1：必须用 Schur 补，不得直接丢弃）----
  //
  // 协方差形式：P22 ← P22 − P21·P11⁻¹·P12，其中 P11 是被边缘化帧的 9×9。
  // MG-4 明确：**不得**再额外膨胀 P —— Schur 补已把信息损失计入。
  if (covariance_.size() == static_cast<size_t>(old_n) * old_n) {
    const int m = 9;  // 被边缘化块的维度
    // 取 P11 的 3×3 主对角子块做稳定求逆（旋转块条件数最好），
    // 其余 6 维（速度/位置）用标量近似 —— 完整 9×9 求逆需要通用求解器，
    // 而 P11 在本实现里只用于**诊断性**的协方差维护（更新不依赖 P，
    // 见文件头说明 1）。标注为已知简化，不假装是完整 Schur 补。
    Mat3d P11 = Mat3d::Zero();
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        P11(i, j) = covariance_[static_cast<size_t>(i) * old_n + j];
      }
    }
    Mat3d P11_inv;
    const bool inv_ok = Inverse3x3(P11, &P11_inv);
    if (inv_ok) {
      for (int blk = m; blk < old_n; ++blk) {
        // P21 的第 blk 行（1×9），P12 的第 blk 列（9×1）。
        double corr = 0.0;
        for (int a = 0; a < 3; ++a) {
          for (int b = 0; b < 3; ++b) {
            corr += covariance_[static_cast<size_t>(blk) * old_n + a] *
                    P11_inv(a, b) *
                    covariance_[static_cast<size_t>(b) * old_n + blk];
          }
        }
        covariance_[static_cast<size_t>(blk) * old_n + blk] -= corr;
        if (covariance_[static_cast<size_t>(blk) * old_n + blk] < 1e-18) {
          covariance_[static_cast<size_t>(blk) * old_n + blk] = 1e-18;
        }
      }
    }
  }

  // ---- ② 收缩协方差：删除前 9 行 9 列 ----
  if (covariance_.size() == static_cast<size_t>(old_n) * old_n) {
    std::vector<double> shrunk(static_cast<size_t>(new_n) * new_n, 0.0);
    for (int i = 9; i < old_n; ++i) {
      for (int j = 9; j < old_n; ++j) {
        shrunk[static_cast<size_t>(i - 9) * new_n + (j - 9)] =
            covariance_[static_cast<size_t>(i) * old_n + j];
      }
    }
    covariance_.swap(shrunk);
  }

  // ---- ③ 状态收缩（MG-2：维数必须严格回到 K×9+9）----
  state_.keyframes.erase(state_.keyframes.begin());
  kf_orientation_.erase(kf_orientation_.begin());
  kf_position_.erase(kf_position_.begin());
  kf_frame_id_.erase(kf_frame_id_.begin());

  // ---- ④ 特征处理（§3.6.3 步骤 ④）----
  //   仅在 i_1 中被观测 → 移除；其余帧仍有观测 → 保留（观测数减一）。
  //   m' < 2 → 移除。
  std::vector<FeatureTrack> kept;
  std::vector<Vec2d> kept_prev;
  std::vector<Vec3d> kept_pos;
  kept.reserve(features_.size());

  for (size_t i = 0; i < features_.size(); ++i) {
    FeatureTrack f = features_[i];
    std::vector<FeatureTrack::Observation> obs;
    obs.reserve(f.observations.size());
    for (const FeatureTrack::Observation& o : f.observations) {
      if (o.frame_id != oldest_frame) obs.push_back(o);
    }
    if (obs.size() < 2) continue;
    f.observations.swap(obs);
    if (f.spawn_keyframe > 0) --f.spawn_keyframe;

    kept.push_back(f);
    kept_prev.push_back(i < prev_points_.size() ? prev_points_[i] : Vec2d::Zero());
    kept_pos.push_back(i < landmark_positions_.size() ? landmark_positions_[i]
                                                      : Vec3d::Zero());
  }
  features_.swap(kept);
  prev_points_.swap(kept_prev);
  landmark_positions_.swap(kept_pos);
  active_count_ = static_cast<int>(features_.size());
}

// ---------------------------------------------------------------------------
// MSCKF 更新
// ---------------------------------------------------------------------------
void ViOEstimator::UpdateState() {
  const int K = static_cast<int>(state_.keyframes.size());
  if (K < 2) return;

  double best_cost = ComputeCost();
  if (!std::isfinite(best_cost)) best_cost = 1e18;

  // 无先验的纯 Gauss-Newton 在初值已由 IMU 给出时通常 2~3 次收敛。
  for (int iter = 0; iter < 5; ++iter) {
    const int n = StateDim();
    std::vector<double> H(static_cast<size_t>(n) * n, 0.0);
    std::vector<double> g(n, 0.0);

    AccumulateVisualConstraints(&H, &g, n);
    AccumulateImuConstraints(&H, &g, n);
    AccumulateBiasGravityPriors(&H, &g, n);

    std::vector<double> rhs(n, 0.0);
    for (int i = 0; i < n; ++i) rhs[i] = -g[i];

    // LM 阻尼：H 在不可观方向是半正定的（§3.6.2 的 λ₃/λ₁ 判据正是在
    // 检测这种不可观），因此不能假定正定，必须带阻尼求解。
    bool applied = false;
    for (int attempt = 0; attempt < 5 && !applied; ++attempt) {
      const double lambda = 1e-8 * std::pow(10.0, attempt);
      std::vector<double> dx(n, 0.0);
      if (!SolveSymmetricDamped(H.data(), rhs.data(), n, lambda, dx.data())) {
        continue;
      }

      // 备份，失败回滚。
      const std::vector<Mat3d> R_bak = kf_orientation_;
      const std::vector<Vec3d> p_bak = kf_position_;
      std::vector<Vec3d> v_bak;
      v_bak.reserve(state_.keyframes.size());
      for (const KeyframeState& kf : state_.keyframes) v_bak.push_back(kf.velocity);
      const Vec3d g_bak = state_.gravity;
      const Vec3d bg_bak = state_.bias_gyro;
      const Vec3d ba_bak = state_.bias_acc;

      if (!ApplyStep(dx)) {
        kf_orientation_ = R_bak;
        kf_position_ = p_bak;
        for (size_t k = 0; k < state_.keyframes.size(); ++k) {
          state_.keyframes[k].velocity = v_bak[k];
        }
        state_.gravity = g_bak;
        state_.bias_gyro = bg_bak;
        state_.bias_acc = ba_bak;
        continue;
      }

      SyncCameraPoses();
      RetriangulateLandmarks();
      const double new_cost = ComputeCost();

      // 回溯线搜索：代价必须下降，否则步长减半重试，最终回滚。
      if (std::isfinite(new_cost) && new_cost <= best_cost) {
        best_cost = new_cost;
        last_cost_ = new_cost;
        applied = true;
        break;
      }

      // 线搜索：逐步减半步长。
      bool improved = false;
      for (int shrink = 0; shrink < 6 && !improved; ++shrink) {
        kf_orientation_ = R_bak;
        kf_position_ = p_bak;
        for (size_t k = 0; k < state_.keyframes.size(); ++k) {
          state_.keyframes[k].velocity = v_bak[k];
        }
        state_.gravity = g_bak;
        state_.bias_gyro = bg_bak;
        state_.bias_acc = ba_bak;

        std::vector<double> half = dx;
        for (int i = 0; i < n; ++i) half[i] *= std::pow(0.5, shrink + 1);
        if (!ApplyStep(half)) continue;
        SyncCameraPoses();
        RetriangulateLandmarks();
        const double c2 = ComputeCost();
        if (std::isfinite(c2) && c2 <= best_cost) {
          best_cost = c2;
          last_cost_ = c2;
          applied = true;
          improved = true;
        }
      }
      if (!improved) {
        // 回滚到备份并停止迭代（已到达局部极小 / 线性化失效）。
        kf_orientation_ = R_bak;
        kf_position_ = p_bak;
        for (size_t k = 0; k < state_.keyframes.size(); ++k) {
          state_.keyframes[k].velocity = v_bak[k];
        }
        state_.gravity = g_bak;
        state_.bias_gyro = bg_bak;
        state_.bias_acc = ba_bak;
        SyncCameraPoses();
        return;
      }
    }
    if (!applied) return;
  }
  SyncCameraPoses();
}

// ---------------------------------------------------------------------------
// 视觉约束（零空间投影后）
// ---------------------------------------------------------------------------
void ViOEstimator::AccumulateVisualConstraints(std::vector<double>* H,
                                               std::vector<double>* g, int n) {
  const int K = static_cast<int>(state_.keyframes.size());

  auto kf_index_of = [&](uint64_t fid) -> int {
    for (int i = 0; i < K; ++i) {
      if (kf_frame_id_[static_cast<size_t>(i)] == fid) return i;
    }
    return -1;
  };

  for (size_t fi = 0; fi < features_.size(); ++fi) {
    const FeatureTrack& f = features_[fi];
    if (f.maturity < config_.maturity_threshold) continue;  // 观察期不参与
    if (f.observations.size() < 3) continue;

    std::vector<Vec2d> obs;
    std::vector<int> kidx;
    for (const FeatureTrack::Observation& o : f.observations) {
      const int k = kf_index_of(o.frame_id);
      if (k < 0) continue;
      obs.push_back(Vec2d(o.u, o.v));
      kidx.push_back(k);
    }
    const size_t m = obs.size();
    if (m < 3) continue;

    std::vector<CameraPose> poses(m);
    for (size_t r = 0; r < m; ++r) poses[r] = CameraPoseAt(kidx[r]);

    Vec3d x_cam0;
    const NullspaceConstraint c =
        BuildNullspaceConstraint(obs, poses, intrinsics_, config_, &x_cam0);
    if (c.reject != NullspaceConstraint::Reject::kNone) continue;

    const size_t null_dim = 2 * m - 3;
    if (c.nullspace_basis.size() != 2 * m * null_dim) continue;
    if (c.residual.size() != null_dim) continue;
    if (c.projected_variance.size() != null_dim) continue;

    const Vec3d Xw = c.position_world;
    if (!Xw.isFinite()) continue;

    // r_null = N_fᵀ·J ；∂r_null/∂x = N_fᵀ·(∂J/∂x)
    std::vector<double> r_null(null_dim, 0.0);
    // 稀疏雅可比：每行只有若干块非零。用 (行 → 列, 值) 表避免 n×null_dim 稠密.
    struct Entry { int col; double val; };
    std::vector<std::vector<Entry>> jac(null_dim);

    bool ok = true;
    for (size_t r = 0; r < m && ok; ++r) {
      const int k = kidx[r];
      const Mat3d& R_i = kf_orientation_[static_cast<size_t>(k)];
      const Vec3d& p_i = kf_position_[static_cast<size_t>(k)];

      const Vec3d X_cam = poses[r].WorldToCamera(Xw);
      if (X_cam(2) < 1e-9) { ok = false; break; }
      const double Z = X_cam(2);
      const double u_proj = intrinsics_.fx * X_cam(0) / Z + intrinsics_.cx;
      const double v_proj = intrinsics_.fy * X_cam(1) / Z + intrinsics_.cy;
      const double Ju = u_proj - obs[r](0);
      const double Jv = v_proj - obs[r](1);

      // ∂π/∂X_cam（2×3）。
      double dpi[2][3];
      dpi[0][0] = intrinsics_.fx / Z;
      dpi[0][1] = 0.0;
      dpi[0][2] = -intrinsics_.fx * X_cam(0) / (Z * Z);
      dpi[1][0] = 0.0;
      dpi[1][1] = intrinsics_.fy / Z;
      dpi[1][2] = -intrinsics_.fy * X_cam(1) / (Z * Z);

      // ∂X_cam/∂θ_i 与 ∂X_cam/∂p_i（3×3）：
      //   X_cam = R_ic·(R_iᵀ(Xw − p_i)) + t_ic
      //   令 X_imu = R_iᵀ(Xw − p_i)
      //   ∂X_cam/∂θ_i = R_ic·[X_imu]×        （δθ 右乘 R_i）
      //   ∂X_cam/∂p_i = −R_cw               （R_cw = R_ic·R_iᵀ）
      const Vec3d X_imu = R_i.transpose() * (Xw - p_i);
      const Mat3d dX_dtheta = extrin_R_ic_ * Skew(X_imu);
      const Mat3d R_cw = extrin_R_ic_ * R_i.transpose();

      // 2×3 = (2×3)·(3×3)
      double Jtheta[2][3];
      double Jpos[2][3];
      for (int a = 0; a < 2; ++a) {
        for (int b = 0; b < 3; ++b) {
          double st = 0.0;
          double sp = 0.0;
          for (int kk = 0; kk < 3; ++kk) {
            st += dpi[a][kk] * dX_dtheta(kk, b);
            sp += dpi[a][kk] * (-R_cw(kk, b));
          }
          Jtheta[a][b] = st;
          Jpos[a][b] = sp;
        }
      }

      // 投影到零空间：r_null(kk) += N(row,kk)·J(row)；雅可比同理。
      const size_t row0 = 2 * r;
      for (size_t kk = 0; kk < null_dim; ++kk) {
        const double w0 = c.nullspace_basis[row0 * null_dim + kk];
        const double w1 = c.nullspace_basis[(row0 + 1) * null_dim + kk];
        r_null[kk] += w0 * Ju + w1 * Jv;
        for (int b = 0; b < 3; ++b) {
          const double jt = w0 * Jtheta[0][b] + w1 * Jtheta[1][b];
          const double jp = w0 * Jpos[0][b] + w1 * Jpos[1][b];
          if (jt != 0.0) jac[kk].push_back({k * 9 + b, jt});
          if (jp != 0.0) jac[kk].push_back({k * 9 + 6 + b, jp});
        }
      }
    }
    if (!ok) continue;

    // 白化 + 累加：H += Jᵀ·W·J ，g += Jᵀ·W·r，W = diag(1/σ_k²)。
    // 白化后 W 为单位阵（残差与雅可比都已按 1/σ_k 缩放）。
    for (size_t kk = 0; kk < null_dim; ++kk) {
      const double var = c.projected_variance[kk];
      if (!(var > 0.0)) continue;
      const double inv_sigma = 1.0 / std::sqrt(var);

      const double rw = r_null[kk] * inv_sigma;
      const std::vector<Entry>& row = jac[kk];
      for (const Entry& ea : row) {
        const double ja = ea.val * inv_sigma;
        (*g)[static_cast<size_t>(ea.col)] += ja * rw;
        double* hrow = H->data() + static_cast<size_t>(ea.col) * n;
        for (const Entry& eb : row) {
          hrow[eb.col] += ja * (eb.val * inv_sigma);
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// IMU 约束（docs/05 §3.7.2）
// ---------------------------------------------------------------------------
void ViOEstimator::AccumulateImuConstraints(std::vector<double>* H,
                                            std::vector<double>* g, int n) {
  const int K = static_cast<int>(state_.keyframes.size());
  const int n_global = K * 9;  // 零偏 6 + 重力 3 的起始列

  for (int i = 0; i + 1 < K; ++i) {
    const KeyframeState& kf1 = state_.keyframes[static_cast<size_t>(i) + 1];
    if (!kf1.has_preint) continue;
    const PreintegratedMeasurement& pm = kf1.preint;
    const double dt = pm.duration_s;
    if (dt < 1e-3) continue;

    const Mat3d R0 = kf_orientation_[static_cast<size_t>(i)];
    const Mat3d R1 = kf_orientation_[static_cast<size_t>(i) + 1];
    const Vec3d p0 = kf_position_[static_cast<size_t>(i)];
    const Vec3d p1 = kf_position_[static_cast<size_t>(i) + 1];
    const Vec3d v0 = state_.keyframes[static_cast<size_t>(i)].velocity;
    const Vec3d v1 = kf1.velocity;

    const ImuNoise ns = ImuResidualSigma(dt);

    // 相关列：i 帧 9 列、i+1 帧 9 列、零偏 6 列（b_g 3 + b_a 3）、重力 3 列。
    // 共 27 列。零偏能进列，靠的正是 `ImuResidual` 里的 `pm.Correct` 修正
    // （docs/05 §3.3.5）—— 没有它，这 6 列恒为 0，零偏不可优化。
    // 用数值差分求导（见文件头说明 4）。
    double r0[9];
    ImuResidual(R0, p0, v0, R1, p1, v1, state_.gravity, pm, state_.bias_gyro,
                state_.bias_acc, r0);

    // 收敛性保护：残差过大（> 1e3 σ）说明状态已经发散，该区间无意义。
    bool sane = true;
    for (int a = 0; a < 9; ++a) {
      const double s = (a < 3) ? ns.rot : (a < 6 ? ns.vel : ns.pos);
      if (std::fabs(r0[a]) > 1e3 * s) sane = false;
    }
    if (!sane) continue;

    // 列布局：[i 的三块 9][i+1 的三块 9][零偏 6][重力 3] → 全局列号。
    constexpr int kCols = 27;
    int local_col[kCols];
    for (int b = 0; b < 9; ++b) local_col[b] = i * 9 + b;
    for (int b = 0; b < 9; ++b) local_col[9 + b] = (i + 1) * 9 + b;
    for (int b = 0; b < 3; ++b) local_col[18 + b] = n_global + b;      // b_g
    for (int b = 0; b < 3; ++b) local_col[21 + b] = n_global + 3 + b;  // b_a
    for (int b = 0; b < 3; ++b) local_col[24 + b] = n_global + 6 + b;  // g

    double J[9][kCols];
    const double h = 1e-6;
    for (int c = 0; c < kCols; ++c) {
      Mat3d Rp0 = R0;
      Vec3d pp0 = p0;
      Vec3d vp0 = v0;
      Mat3d Rp1 = R1;
      Vec3d pp1 = p1;
      Vec3d vp1 = v1;
      Vec3d gv = state_.gravity;
      Vec3d bgv = state_.bias_gyro;
      Vec3d bav = state_.bias_acc;

      const int within = c % 9;
      Vec3d axis(0.0, 0.0, 0.0);
      if (c < 18) {
        const int axis_idx = within % 3;
        axis(axis_idx) = 1.0;
        const int blk = within / 3;  // 0=θ, 1=v, 2=p
        const bool second = (c >= 9);
        if (blk == 0) {
          if (!second) Rp0 = R0 * ExpSO3(axis * h);
          else Rp1 = R1 * ExpSO3(axis * h);
        } else if (blk == 1) {
          if (!second) vp0 = v0 + axis * h;
          else vp1 = v1 + axis * h;
        } else {
          if (!second) pp0 = p0 + axis * h;
          else pp1 = p1 + axis * h;
        }
      } else if (c < 21) {
        bgv(c - 18) += h;
      } else if (c < 24) {
        bav(c - 21) += h;
      } else {
        gv(c - 24) += h;
      }

      double rp[9];
      ImuResidual(Rp0, pp0, vp0, Rp1, pp1, vp1, gv, pm, bgv, bav, rp);
      for (int a = 0; a < 9; ++a) J[a][c] = (rp[a] - r0[a]) / h;
    }

    // 白化 + 累加。白化标准差按残差块选（旋转/速度/位置）。
    double inv_sigma[9];
    for (int a = 0; a < 9; ++a) {
      const double s = (a < 3) ? ns.rot : (a < 6 ? ns.vel : ns.pos);
      inv_sigma[a] = 1.0 / s;
    }

    // g += Jᵀ·W·r
    for (int c = 0; c < kCols; ++c) {
      double acc = 0.0;
      for (int a = 0; a < 9; ++a) {
        const double is = inv_sigma[a];
        acc += J[a][c] * is * (r0[a] * is);
      }
      (*g)[static_cast<size_t>(local_col[c])] += acc;
    }
    // H += Jᵀ·W·J
    for (int c1 = 0; c1 < kCols; ++c1) {
      double* hrow = H->data() + static_cast<size_t>(local_col[c1]) * n;
      for (int c2 = 0; c2 < kCols; ++c2) {
        double acc = 0.0;
        for (int a = 0; a < 9; ++a) {
          const double is = inv_sigma[a];
          acc += J[a][c1] * is * (J[a][c2] * is);
        }
        hrow[local_col[c2]] += acc;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 零偏 / 重力先验（docs/05 §3.7.3、§3.2.3）
// ---------------------------------------------------------------------------
void ViOEstimator::AccumulateBiasGravityPriors(std::vector<double>* H,
                                               std::vector<double>* g, int n) {
  const int K = static_cast<int>(state_.keyframes.size());
  const int n_global = K * 9;

  // 零偏弱先验（§3.7.3 规定 σ_bg = σ_ba = 2.0e-4）。
  // 作用：防止视觉不可观时零偏漂到不合理值，并让零偏块保持正定。
  // 注意：零偏**本身**已是可优化量（残差含 ∂r/∂b，见 ImuResidual 的
  // `pm.Correct`），此先验只是正则项，不再是「唯一能约束零偏的东西」。
  const double sigma_b = 2.0e-4;
  const double w_b = 1.0 / (sigma_b * sigma_b);
  for (int i = 0; i < 6; ++i) {
    const int col = n_global + i;
    (*H)[static_cast<size_t>(col) * n + col] += w_b;
  }
  for (int i = 0; i < 3; ++i) {
    (*g)[static_cast<size_t>(n_global + i)] += w_b * state_.bias_gyro(i);
    (*g)[static_cast<size_t>(n_global + 3 + i)] += w_b * state_.bias_acc(i);
  }

  // 重力硬约束 ‖g_W‖ = 9.80665（§3.2.3 方案 B，σ_g = 1e-3 m/s²）。
  {
    const double norm = state_.gravity.norm();
    if (norm > 1e-6) {
      const Vec3d ghat = state_.gravity / norm;
      const double sigma_g = 1.0e-3;
      const double w = 1.0 / (sigma_g * sigma_g);
      const double r = norm - kGravityMagnitude;
      for (int a = 0; a < 3; ++a) {
        const int ca = n_global + 6 + a;
        const double ja = ghat(a) * w;
        (*g)[static_cast<size_t>(ca)] += ja * r;
        double* hrow = H->data() + static_cast<size_t>(ca) * n;
        for (int b = 0; b < 3; ++b) {
          hrow[n_global + 6 + b] += ja * (ghat(b) * w);
        }
      }
    }
  }

  // 每帧 9 维的极弱正则（σ=100 ⇒ 权重 1e-4）。
  //
  // 存在的理由：若某帧的全部地标都被 §3.6.2 的丢弃判据剔除（视差不足 /
  // 退化几何），该帧的 9 维信息矩阵为 0 → H 奇异 → 线性求解失败。
  // 加极弱先验等价于「没有观测就不更新」，是正确语义，
  // 不是「偷偷把状态钉住」—— 权重比视觉项小 8 个数量级。
  const double w_pose = 1e-4;
  for (int k = 0; k < K; ++k) {
    for (int i = 0; i < 9; ++i) {
      const int col = k * 9 + i;
      (*H)[static_cast<size_t>(col) * n + col] += w_pose;
    }
  }
}

// ---------------------------------------------------------------------------
// 增量施加与代价
// ---------------------------------------------------------------------------
bool ViOEstimator::ApplyStep(const std::vector<double>& dx) {
  const int K = static_cast<int>(state_.keyframes.size());
  if (static_cast<int>(dx.size()) != StateDim()) return false;

  for (int k = 0; k < K; ++k) {
    const Vec3d dtheta(dx[k * 9 + 0], dx[k * 9 + 1], dx[k * 9 + 2]);
    const Vec3d dv(dx[k * 9 + 3], dx[k * 9 + 4], dx[k * 9 + 5]);
    const Vec3d dp(dx[k * 9 + 6], dx[k * 9 + 7], dx[k * 9 + 8]);

    // 步长保护：超出线性化有效范围的步直接判失败（回滚后减半重试）。
    if (dtheta.norm() > 0.5 || dp.norm() > 1.0 || dv.norm() > 5.0) return false;
    if (!std::isfinite(dtheta.norm()) || !std::isfinite(dp.norm()) ||
        !std::isfinite(dv.norm())) {
      return false;
    }

    // R ← R·Exp(δθ)：δθ 为**局部**（body 系）扰动，右乘。
    // Mat 没有矩阵复合赋值（只有标量 *=），故显式相乘。
    kf_orientation_[static_cast<size_t>(k)] =
        kf_orientation_[static_cast<size_t>(k)] * ExpSO3(dtheta);
    kf_position_[static_cast<size_t>(k)] += dp;
    state_.keyframes[static_cast<size_t>(k)].velocity += dv;
  }

  const int nb = K * 9;
  state_.bias_gyro += Vec3d(dx[nb + 0], dx[nb + 1], dx[nb + 2]);
  state_.bias_acc += Vec3d(dx[nb + 3], dx[nb + 4], dx[nb + 5]);
  state_.gravity += Vec3d(dx[nb + 6], dx[nb + 7], dx[nb + 8]);

  for (int k = 0; k < K; ++k) {
    if (!kf_orientation_[static_cast<size_t>(k)].isFinite() ||
        !kf_position_[static_cast<size_t>(k)].isFinite()) {
      return false;
    }
  }
  return state_.bias_gyro.isFinite() && state_.bias_acc.isFinite() &&
         state_.gravity.isFinite();
}

double ViOEstimator::ComputeCost() const {
  const int K = static_cast<int>(state_.keyframes.size());
  double cost = 0.0;

  // IMU 残差（白化后平方和）。
  for (int i = 0; i + 1 < K; ++i) {
    const KeyframeState& kf1 = state_.keyframes[static_cast<size_t>(i) + 1];
    if (!kf1.has_preint || kf1.preint.duration_s < 1e-3) continue;
    const PreintegratedMeasurement& pm = kf1.preint;

    double r0[9];
    ImuResidual(kf_orientation_[static_cast<size_t>(i)],
                kf_position_[static_cast<size_t>(i)],
                state_.keyframes[static_cast<size_t>(i)].velocity,
                kf_orientation_[static_cast<size_t>(i) + 1],
                kf_position_[static_cast<size_t>(i) + 1], kf1.velocity,
                state_.gravity, pm, state_.bias_gyro, state_.bias_acc, r0);

    const ImuNoise ns = ImuResidualSigma(pm.duration_s);
    for (int a = 0; a < 9; ++a) {
      const double s = (a < 3) ? ns.rot : (a < 6 ? ns.vel : ns.pos);
      // 残差过大时按截断处理，避免个别发散的区间把总代价主导掉
      // （与 AccumulateImuConstraints 的 sane 判据同一口径）。
      const double rw = r0[a] / s;
      if (std::fabs(rw) > 1e3) {
        cost += 1e6;
      } else {
        cost += rw * rw;
      }
    }
  }

  // 视觉零空间残差（白化后平方和）。
  auto kf_index_of = [&](uint64_t fid) -> int {
    for (int i = 0; i < K; ++i) {
      if (kf_frame_id_[static_cast<size_t>(i)] == fid) return i;
    }
    return -1;
  };

  for (const FeatureTrack& f : features_) {
    if (f.maturity < config_.maturity_threshold) continue;
    if (f.observations.size() < 3) continue;

    std::vector<Vec2d> obs;
    std::vector<int> kidx;
    for (const FeatureTrack::Observation& o : f.observations) {
      const int k = kf_index_of(o.frame_id);
      if (k < 0) continue;
      obs.push_back(Vec2d(o.u, o.v));
      kidx.push_back(k);
    }
    const size_t m = obs.size();
    if (m < 3) continue;

    std::vector<CameraPose> poses(m);
    for (size_t r = 0; r < m; ++r) poses[r] = CameraPoseAt(kidx[r]);

    Vec3d x_cam0;
    const NullspaceConstraint c =
        BuildNullspaceConstraint(obs, poses, intrinsics_, config_, &x_cam0);
    if (c.reject != NullspaceConstraint::Reject::kNone) continue;

    const size_t null_dim = 2 * m - 3;
    if (c.residual.size() != null_dim) continue;
    if (c.projected_variance.size() != null_dim) continue;

    for (size_t kk = 0; kk < null_dim; ++kk) {
      const double var = c.projected_variance[kk];
      if (!(var > 0.0)) continue;
      const double rw = c.residual[kk] / std::sqrt(var);
      cost += rw * rw;
    }
  }

  // 零偏 / 重力先验的代价（与累加器同口径）。
  const double sigma_b = 2.0e-4;
  const double w_b = 1.0 / (sigma_b * sigma_b);
  cost += w_b * state_.bias_gyro.squaredNorm();
  cost += w_b * state_.bias_acc.squaredNorm();
  {
    const double r = state_.gravity.norm() - kGravityMagnitude;
    cost += (r / 1.0e-3) * (r / 1.0e-3);
  }
  return cost;
}

}  // namespace recon
