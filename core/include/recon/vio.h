// SPDX-License-Identifier: BSD-3-Clause
//
// VIO（视觉惯性里程计）接口。契约：docs/05_单目管线.md §3。
//
// MSCKF 的核心思想：
//   不用「每个特征一个 3D 点」的经典 BA，而是用**零空间消去**：
//   把一个特征在滑窗内所有观测的 2m 维残差，
//   通过投影算子压成 (2m−3) 维、**只含位姿**的约束。
//   这样三维点本身被完全消掉，滑窗状态只剩位姿 + 零偏 + 重力。
//
// 为什么这对精度重要：
//   1. 三维点的观测噪声被正确传播（不像「先三角化再优化」那样
//      把三角化误差当成零）。
//   2. 状态维度低 → 优化条件数好 → 数值稳定。
//   3. 视觉与 IMU 在**同一个**优化问题里联合求解，不是两阶段拼接。

#ifndef RECON_CORE_VIO_H_
#define RECON_CORE_VIO_H_

#include <cstdint>
#include <vector>

#include "recon/camera.h"
#include "recon/imu_preintegration.h"
#include "recon/linalg.h"

namespace recon {

// ---------------------------------------------------------------------------
// 特征
// ---------------------------------------------------------------------------

// 一个被跟踪的特征。记录它在滑窗内各关键帧中的观测。
struct FeatureTrack {
  // 各关键帧中的像素观测（亚像素精度）。
  // 与 vio_state_.keyframes 一一对应（用 keyframe_index 索引）。
  struct Observation {
    uint64_t frame_id = 0;
    double u = 0.0;
    double v = 0.0;
  };

  std::vector<Observation> observations;
  int age = 0;          // 存活帧数
  int maturity = 0;     // 成熟帧数（≥ maturity_threshold 才参与更新）

  // 本特征被播种时的滑窗关键帧下标（-1 = 尚未落到任何关键帧上）。
  // 用途：① 判定「同一关键帧内只参与一次零空间构造」（FL-2）；
  //       ② 边缘化最旧帧后，据此判断特征是否已无可用观测。
  int spawn_keyframe = -1;

  // 当前的三维估计（相机系，全局参考帧）。
  Vec3d position = Vec3d::Zero();
  bool has_position = false;
};

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------

// 单个关键帧的位姿（世界→相机）。
struct KeyframeState {
  uint64_t frame_id = 0;
  int64_t timestamp_ns = 0;
  CameraPose pose;        // world → camera
  Vec3d velocity = Vec3d::Zero();  // 世界系速度
  bool has_velocity = false;

  // IMU 预积分（从上一关键帧到本关键帧）。
  PreintegratedMeasurement preint;
  bool has_preint = false;
};

// VIO 状态。
struct VioState {
  // 滑窗内的关键帧。
  std::vector<KeyframeState> keyframes;

  // 全局量（不边缘化）。
  Vec3d bias_gyro = Vec3d::Zero();
  Vec3d bias_acc = Vec3d::Zero();
  Vec3d gravity = Vec3d(0.0, 0.0, -9.80665);  // 世界系重力

  bool initialized = false;
};

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

// VIO 配置。默认值取自 docs/05 §3.4–§3.8。
struct VioConfig {
  // 特征提取
  int feature_count = 250;              // 每帧特征上限
  int min_pixel_distance = 8;           // 特征最小间距（px）
  int grid_cell_size = 16;              // 网格分区边长（px）
  double shi_tomasi_quality = 0.01;     // 响应阈值（相对最大）
  int shi_tomasi_block = 5;             // 角点窗口

  // 亚像素定位
  int subpixel_window = 5;              // 5×5（高 7×7）
  int subpixel_max_iters = 20;
  double subpixel_eps = 0.01;           // 收敛判据（px）

  // KLT 跟踪
  int klt_window_size = 21;
  int klt_pyramid_levels = 3;
  int klt_max_iterations = 20;
  double klt_convergence_eps = 0.01;
  double klt_max_displacement = 60.0;
  double klt_min_error = 1.0;           // 残差 RMS 上限
  double klt_fb_threshold = 1.0;         // 双向一致性阈值
  double klt_min_eig = 1e-4;             // 病态检测

  // 特征寿命
  int maturity_threshold = 5;           // 成熟所需帧数
  int max_age = 60;                     // 超寿移除
  double feature_spawn_ratio = 0.6;     // 低于此比例触发补充

  // 滑窗
  int window_size = 10;                 // 关键帧滑窗长度

  // 零空间构造
  double nullspace_ratio_reject = 1e-6; // λ₃/λ₁ 低于此值丢弃特征
  double nullspace_ratio_ill = 1e-4;     // 低于此值权重放大 10 倍
  double max_depth_ratio = 0.10;        // σ_z/z 上限
  double min_triangulation_angle_deg = 1.5;

  // 鲁棒核
  double visual_huber_delta = 1.0;      // Cauchy δ（px）
  double imu_noise_gyro = 1.5e-4;       // 陀螺白噪声标准差
  double imu_noise_acc = 1.0e-2;        // 加计白噪声标准差
  double imu_noise_bias_gyro = 1.0e-5;
  double imu_noise_bias_acc = 1.0e-4;
};

// ---------------------------------------------------------------------------
// 特征提取
// ---------------------------------------------------------------------------

// Shi-Tomasi 角点检测结果。
struct CornerDetection {
  std::vector<Vec2d> points;   // 亚像素角点位置
  std::vector<double> response;
};

// 在灰度图上检测 Shi-Tomasi 角点（亚像素精化）。
//
// 实现要点：
//   1. 计算每个像素的 Shi-Tomasi 响应（结构张量最小特征值）。
//   2. 非极大值抑制 + 最小间距约束 + 网格均匀分布。
//   3. 亚像素精化用**逆相关加权最小二乘**（cornerSubPix 等价），
//      定位精度可达 0.05 px —— 这是 VIO 精度的根基。
CornerDetection DetectCorners(const uint8_t* gray, int width, int height,
                               int stride, const VioConfig& config,
                               const std::vector<Vec2d>* existing);

// ---------------------------------------------------------------------------
// KLT 跟踪
// ---------------------------------------------------------------------------

// 单个特征的跟踪结果。
struct TrackResult {
  bool success = false;
  Vec2d position = Vec2d::Zero();
  double residual_rms = 0.0;
  double min_eigenvalue = 0.0;
  double displacement = 0.0;
};

// 稀疏 LK 跟踪（带金字塔）。
//
// 逆组合形式：增量的 Hessian 在**模板坐标系**中求解并累加，
// 模板保持不变 → 避免模板更新漂移。
//
// @param prev_gray  上一帧灰度
// @param next_gray  当前帧灰度
// @param prev_point 上一帧特征位置（亚像素）
// @param out_point  跟踪结果
TrackResult TrackFeature(const uint8_t* prev_gray, int prev_w, int prev_h,
                          int prev_stride, const uint8_t* next_gray, int next_w,
                          int next_h, int next_stride, const Vec2d& prev_point,
                          const VioConfig& config);

// 双向一致性检查。
bool CheckForwardBackward(const uint8_t* prev_gray, int prev_w, int prev_h,
                          int prev_stride, const uint8_t* next_gray, int next_w,
                          int next_h, int next_stride, const Vec2d& prev_point,
                          const Vec2d& next_point, const VioConfig& config);

// ---------------------------------------------------------------------------
// 零空间构造
// ---------------------------------------------------------------------------

// 一个特征的零空间约束。
struct NullspaceConstraint {
  // 零空间残差（长度 = 2m − 3）。
  std::vector<double> residual;
  // 白化后的残差（已乘 (Σ^null)^{-1/2}）。
  std::vector<double> whitened;
  // 病态标记（权重需放大）。
  bool ill_conditioned = false;
  // 观测数。
  int observation_count = 0;

  // 拒绝原因（构造失败时非空）。用于诊断与上层提示。
  enum class Reject : uint8_t {
    kNone = 0,
    kTooFewObservations,     // 观测数 < 3
    kTriangulationFailed,    // DLT 求解失败
    kInsufficientParallax,   // 视线夹角不足
    kDegenerateGeometry,     // λ₃/λ₁ 过小（三维点近乎不可观）
    kPointBehindCamera,      // 三维点在相机后方
  };
  Reject reject = Reject::kNone;

  // 诊断量。
  double parallax_angle_deg = 0.0;   // 视线夹角
  double eigenvalue_ratio = 0.0;     // λ₃/λ₁

  // ---- 供 MSCKF 更新使用的量（docs/05 §3.6.2 步骤 4/5）----
  //
  // 零空间基 N_f ∈ R^{2m×(2m−3)}，**行主序**（2m 行 × (2m−3) 列）。
  // 投影后的残差 = N_f(t)ᵀ·J，而状态雅可比 = N_fᵀ·(∂J/∂x)。
  // 因此更新时必须把 N_f 一起带出来 —— 只带残差是不够的。
  //
  // 构造失败（residual 为空）时本向量为空。
  std::vector<double> nullspace_basis;

  // 投影后残差的协方差对角线（长度 = 2m−3）。
  // 由 Σ_f^null = N_fᵀ Σ_J N_f、Σ_J = σ_v²·I 得
  //   Σ_f^null(r,r) = σ_v²·Σ_c N_f(r,c)²
  // 白化即除以 sqrt(该值)。
  std::vector<double> projected_variance;

  // 三角化得到的三维点，**世界系**（不是相机 0 系）。
  // out_position 给出的是相机 0 系；更新雅可比需要在世界系导数，
  // 而世界系 ⇄ 相机系的旋转与被优化的量无关（∂/∂δθ 作用在相机系），
  // 故这里额外保留世界系坐标，由调用方用关键帧位姿换算。
  Vec3d position_world = Vec3d::Zero();
};

// 为一个特征构造 MSCKF 零空间约束。
//
// @param observations 各关键帧中的像素观测
// @param keyframe_poses 对应关键帧的位姿（world→camera）
// @param intrinsics 相机内参
// @param config 配置
// @param out_position 输出的三维点估计（相机系）
// @return 约束；构造失败时 residual 为空
NullspaceConstraint BuildNullspaceConstraint(
    const std::vector<Vec2d>& observations,
    const std::vector<CameraPose>& keyframe_poses,
    const CameraIntrinsics& intrinsics, const VioConfig& config,
    Vec3d* out_position);

// ---------------------------------------------------------------------------
// VIO 估计器
// ---------------------------------------------------------------------------

// 估计器接口。
class ViOEstimator {
 public:
  explicit ViOEstimator(const VioConfig& config);

  // 输入一帧。
  //
  // @param gray       灰度图
  // @param timestamp_ns 时间戳
  // @param imu_samples 本帧时间窗内的 IMU 采样
  // @return true 表示该帧被接受为关键帧
  bool AddFrame(const uint8_t* gray, int width, int height, int stride,
                int64_t timestamp_ns, const std::vector<ImuSample>& imu_samples);

  // 状态查询。
  const VioState& state() const { return state_; }
  bool initialized() const { return state_.initialized; }

  // 内参。不设置时按首帧尺寸自动推导（fx=fy=max(w,h)，主点在中心，
  // 无畸变）。真实标定请显式传入（docs/05 §2.3）。
  void SetIntrinsics(const CameraIntrinsics& intr);

  // 诊断信息。
  int tracked_feature_count() const { return active_count_; }
  int keyframe_count() const { return static_cast<int>(state_.keyframes.size()); }
  double last_update_cost() const { return last_cost_; }

 private:
  // --- 契约里声明的流程步骤（docs/05 §3.1 的阶段表）---
  bool Initialize(const std::vector<ImuSample>& imu_samples);
  void ExtractAndTrack(const uint8_t* gray, int width, int height, int stride);
  void SpawnFeatures(const uint8_t* gray, int width, int height, int stride);
  void UpdateState();
  void MarginalizeOldest();

  // --- 内部步骤 ---

  // 当前滑窗状态维数：K×9 + 9（每帧 9 + 零偏 6 + 重力 3）。
  int StateDim() const;

  // 把新关键帧挂到滑窗末尾（含 IMU 预积分的状态递推），并扩张协方差。
  // @return 新块的起始列号；失败返回 -1。
  int AppendKeyframe(const Vec3d& velocity, const PreintegratedMeasurement& preint,
                     bool has_preint);

  // 第 k 个关键帧的 world→camera 位姿（由 IMU 位姿 + 外参换算）。
  CameraPose CameraPoseAt(int kf_index) const;

  // 把 kf_orientation_ / kf_position_ 同步进 state_.keyframes[i].pose。
  void SyncCameraPoses();

  // 用关键帧位姿把地标重新三角化（MG-3：必须用更新后的位姿重新线性化）。
  void RetriangulateLandmarks();

  // COM 相对 IMU 的杠杆臂（由外参推出）。Infrastructure 层的对应量在
  // c_api 里叫 r_ic；保留单一来源，避免两处独立推导漂移。
  Vec3d ImuToCameraLeverArm() const;

  // --- 残差累加（各自把 H 与 g 按白化后口径叠加）---

  // 视觉零空间约束（docs/05 §3.6.2）。已成熟（maturity ≥ 阈值）且
  // 观测数 ≥ 3 的特征参与。
  void AccumulateVisualConstraints(std::vector<double>* H, std::vector<double>* g,
                                   int n);
  // 相邻关键帧的 IMU 预积分约束（docs/05 §3.7.2）。
  void AccumulateImuConstraints(std::vector<double>* H, std::vector<double>* g,
                                int n);
  // 零偏弱先验 + 重力硬约束 ‖g_W‖=9.80665（docs/05 §3.7.3、§3.2.3）。
  void AccumulateBiasGravityPriors(std::vector<double>* H, std::vector<double>* g,
                                   int n);

  // 施加 Gauss-Newton 增量（含步长保护）。返回 false 表示该步被拒绝。
  bool ApplyStep(const std::vector<double>& dx);

  // 白化后的总代价（仅用残差，不组装雅可比）。用于收敛判据与诊断。
  double ComputeCost() const;

  VioConfig config_;
  VioState state_;

  // --- 特征与图像 ---

  std::vector<FeatureTrack> features_;
  int active_count_ = 0;

  // 上一帧灰度（跟踪的模板帧）与上一帧各特征位置。
  std::vector<uint8_t> prev_gray_;
  int prev_w_ = 0;
  int prev_h_ = 0;
  std::vector<Vec2d> prev_points_;
  bool has_prev_frame_ = false;

  // 本帧相对上一帧的**中位像素位移**。关键帧判据用它而不是时间间隔：
  // 时间固定但场景静止时不该产生关键帧（会产生全零视差的退化约束）。
  double last_motion_px_ = 0.0;

  // --- IMU 状态 ---

  // 每关键帧的 IMU 状态（与 state_.keyframes 一一对应）。
  // state_.keyframes[i].pose 是导出的相机位姿，这里存真正的优化变量。
  std::vector<Mat3d> kf_orientation_;  // R_i：IMU → 世界（docs/05 §3.2.1）
  std::vector<Vec3d> kf_position_;     // p_i：IMU 在世界系的位置（米）
  std::vector<uint64_t> kf_frame_id_;  // 对应的原始帧号

  // 帧间预积分累积器与「区间是否已开始」标志。
  ImuPreintegrator preint_;
  bool preint_active_ = false;

  // 初始化用的 IMU 缓冲。
  //
  // 为什么必须有：docs/05 §3.8.1 要求静止检测窗口 ≥ 2.0 s，
  // 而 C ABI 每帧只投递「本帧时间窗内」的 IMU 采样（约 5 ms @200 Hz）。
  // 用单帧样本做静止检测在数学上不可能通过（窗口差 400 倍），
  // 因此估计器必须自己把 IMU 攒起来，攒够窗口再初始化。
  // 初始化完成后即可释放（置空）。
  std::vector<ImuSample> imu_buffer_;

  // 初始化得到的 IMU→世界 初始姿态（R_WI(0)，由静止重力方向确定）。
  Mat3d init_R_imu_world_ = Mat3d::Identity();
  bool init_pose_valid_ = false;
  bool static_detected_ = false;

  // 最近一帧的时间戳（秒），用于关键帧时间戳与诊断。
  double last_frame_time_s_ = 0.0;

  // --- 相机 ---

  CameraIntrinsics intrinsics_;
  bool intrinsics_set_ = false;

  // 相机–IMU 外参：X_cam = R_ic·X_imu + t_ic。缺省为单位（无杠杆臂）。
  Mat3d extrin_R_ic_ = Mat3d::Identity();
  Vec3d extrin_t_ic_ = Vec3d::Zero();

  // --- 地标与协方差 ---

  // 地标位置（与 features_ 一一对应，**世界系**，米）。
  std::vector<Vec3d> landmark_positions_;

  // 滑窗状态协方差（行主序 n×n）。用于诊断与边缘化的 Schur 补；
  // 更新本身用最小二乘的 H 而不是 P（见 vio_estimator.cpp 文件头说明）。
  std::vector<double> covariance_;

  double last_cost_ = 0.0;
  uint64_t next_frame_id_ = 1;
};

}  // namespace recon

#endif  // RECON_CORE_VIO_H_
