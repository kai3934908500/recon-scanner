// SPDX-License-Identifier: BSD-3-Clause
//
// 稀疏 SfM 与全局 BA 接口。契约：docs/05_单目管线.md §4。
//
// 为什么自研（而不是复用 COLMAP + Ceres）：
//   docs/05 §4.1.2 的主线是「复用 COLMAP 稀疏模块 + 自研 Ceres BA」，
//   并在 §4.1.2「风险与回退」中给出回退条件 —— 若 COLMAP 子集在
//   NDK/Xcode 上编译不可行，则自研几何原语。本实现走该回退路径，理由：
//     1. 本仓库现有 11 个模块**全部**自研零依赖（Ceres/OpenCV/Eigen 均
//        NOTFOUND）。SfM 引入 COLMAP+Ceres 会成为唯一的第三方依赖链，
//        破坏「core 可独立编译」的不变量（robust_fit.h 文件头已明确此约束）。
//     2. 移动端（iOS/Android）是最终交付形态，依赖链越短越稳。
//     3. BA 的数值内核（阻尼正定求解 + 稳健核）已在 robust_fit.h 抽出，
//        与 VIO 共用，避免「同一逻辑写两遍必然漂移」（MEMORY.md §4）。
//   与 COLMAP 的能力差距（词袋闭环 / 成熟增量注册调度）如实记录于
//   .workbuddy/memory 的遗留项，不假装等价。
//
// 与 VIO 的分工（docs/00 §5）：
//   · RGBD 分支：不走 SfM，位姿来自平台 AR 或 VIO。
//   · MONO 分支：有 IMU → VIO 提供米制位姿；**无 IMU** → SfM 承担位姿
//     （这正是本模块存在的核心理由，见 docs/05 §2.4 C1 的降级路径）。
//   · SfM 输出的是**重建单位 ru**，尺度由 docs/07 注入（§4.4.2）。

#ifndef RECON_CORE_SFM_H_
#define RECON_CORE_SFM_H_

#include <cstdint>
#include <vector>

#include "recon/camera.h"
#include "recon/linalg.h"

namespace recon {

// ---------------------------------------------------------------------------
// 配置。默认值取自 docs/05 §4.3–§4.4 与 docs/08 G4 组。
// ---------------------------------------------------------------------------
struct SfmConfig {
  // 特征提取（§4.2.2）
  int feature_count = 2000;         // 每帧特征上限
  int feature_grid_size = 16;       // 均匀分布网格边长（像素）
  int feature_min_distance = 6;     // 特征最小间距（像素）
  double feature_quality = 0.01;    // Shi-Tomasi 响应阈值（相对最大）
  int feature_block = 5;            // 角点窗口边长

  // 匹配（§4.3.2）
  double match_ratio_threshold = 0.85;  // Lowe 比值检验
  bool cross_check_enabled = true;      // 双向一致性（互为最近邻）
  int max_matches_per_image = 8000;     // 每帧匹配上限
  int max_num_neighbors = 80;           // 匹配图最大候选邻居数（MM-2）
  double spatial_neighbor_angle_deg = 45.0;  // 空间近邻的方位差上限（MM-2）
  int neighbor_sequence_span = 3;       // 序列邻接跨距（MM-1，i 与 i+1..i+span）

  // 几何验证 RANSAC（§4.3.3）
  double f_threshold_px = 1.5;      // F 矩阵 Sampson 阈值（像素）
  double e_threshold_px = 1.0;      // E 矩阵 Sampson 阈值（像素）
  double h_threshold_px = 2.0;      // H 矩阵对称转移阈值（像素）
  int ransac_max_iterations = 10000;// 迭代上限（黑名单参数，不得因速度降级）
  double ransac_confidence = 0.999; // 提前终止置信度
  int ransac_min_inliers = 30;      // 最小内点数（黑名单参数）
  double min_inlier_ratio = 0.25;   // 最小内点率，低于则退化到 H/F

  // 三角化（§4.4.2）
  int min_track_length = 3;          // 最小轨迹长度
  double triangulation_min_angle_deg = 1.5;  // 最小视差角
  double max_depth_ratio = 0.10;     // σ_z/z 上限
  double triangulation_cheirality_eps = 1e-6;

  // 全局 BA（§4.6）
  int ba_max_iterations = 100;       // 迭代上限（黑名单参数）
  double ba_convergence_tol = 1e-8;  // 函数收敛容差
  double ba_huber_delta_px = 1.5;    // Huber 核阈值（像素）
  double ba_sigma_v_px = 0.8;        // 视觉残差标准差（像素，§4.6.2.1 标档）
  // 固定首视图以消除 gauge（§4.6.1 GA-1）。无 VIO 强先验时**必须**为真，
  // 否则位姿整体可平移/旋转而代价不变，法方程奇异。
  bool ba_fix_first_pose = true;
  double prior_rotation_sigma_deg = 1.0;   // VIO 位姿先验旋转标准差
  double prior_translation_sigma_m = 0.003;// VIO 位姿先验平移标准差（米）
  double prior_vio_weight = 1.0;           // VIO 先验权重（§4.6.2.5，需实测标定）

  // 增量注册（§4.4.1）
  double pnp_ransac_error_px = 4.0;      // PnP RANSAC 内点阈值（像素）
  double pnp_min_inlier_ratio = 0.30;    // PnP 内点率下限，低于则注册失败
  int pnp_ransac_iterations = 2000;      // PnP RANSAC 迭代上限
  double triangulation_max_reproj_px = 2.0;  // 三角化重投影上限（像素）
  int local_ba_window = 8;               // 局部 BA 窗口（帧）
  int ba_min_num_images = 5;             // 触发全局 BA 的最少图像数
  int ba_trigger_every = 10;             // 每新增多少帧触发一次全局 BA

  // 深度过滤（§4.4.2）
  double min_depth = 1e-4;           // 深度下界（ru）
  double max_depth_scene_scale = 10.0;  // 深度上界 = 该值 × 场景尺度

  // 数值保护
  double cond_threshold = 1e9;       // DLT 条件数上限
};

// ---------------------------------------------------------------------------
// 特征与对应
// ---------------------------------------------------------------------------

// 一个 2D 观测（像素）。
struct SfmObservation {
  int image_id = -1;
  Vec2d uv = Vec2d(0.0, 0.0);
};

// 一条轨迹（同一 3D 点在不同视图上的观测）。
struct SfmTrack {
  std::vector<SfmObservation> observations;
  Vec3d point = Vec3d::Zero();  // 三角化结果（ru）
  bool valid = false;           // 三角化成功且通过全部过滤
};

// ---------------------------------------------------------------------------
// 几何原语（§4.3.3、§4.3.4、§4.4.2）
// ---------------------------------------------------------------------------

// RANSAC 结果。
struct RansacResult {
  int inliers = 0;
  int iterations_used = 0;
  bool ok = false;
  std::vector<uint8_t> inlier_mask;  // 与输入点数同长，1 = 内点
};

// 由归一化坐标（已去畸变、已除内参）对应点估计基础矩阵 F（8 点法 + RANSAC）。
//
// @param pts0/pts1  两视图的**归一化坐标**对应点（长度相同）
// @param config     阈值取 config.f_threshold_px 经内参折算后的归一化阈值
// @param intr       用于把像素阈值折算到归一化平面
// @param out_f      输出 F（满足 x1ᵀ·F·x0 = 0）
// 距离度量：Sampson 距离（像素单位）。
RansacResult EstimateFundamentalMatrix(const std::vector<Vec2d>& pts0,
                                        const std::vector<Vec2d>& pts1,
                                        const CameraIntrinsics& intr,
                                        const SfmConfig& config, Mat3d* out_f);

// 由归一化坐标对应点估计本质矩阵 E（K 已知 ⇒ 用归一化坐标等价于 5 点法的
// 8 点松弛形式；本实现用 8 点法在归一化坐标上解，再由 E 分解恢复位姿）。
//
// 说明：8 点法解 E 需要「归一化坐标」而非「像素坐标」—— 像素坐标解出的
// 是 F。这是 §4.3.3「优先 E（已标定内参）」的实现口径。
RansacResult EstimateEssentialMatrix(const std::vector<Vec2d>& pts0_norm,
                                      const std::vector<Vec2d>& pts1_norm,
                                      const CameraIntrinsics& intr,
                                      const SfmConfig& config, Mat3d* out_e);

// 单应矩阵 H（4 点 DLT + RANSAC）。平面场景 / E 内点率过低时的退化模型。
RansacResult EstimateHomography(const std::vector<Vec2d>& pts0,
                                const std::vector<Vec2d>& pts1,
                                const SfmConfig& config, Mat3d* out_h);

// 从 E 恢复相对位姿（§4.3.4）。
//
// E 分解给出 4 组 (R, t) 候选，按**正深度点数最多**者选取（cheirality）。
// 输出位姿口径与 CameraPose 一致：pose 把**视图 0 的世界系**当作参考，
// 即 out_pose1 满足 X_cam1 = out_pose1.R · X_cam0 + out_pose1.t。
// 返回 false 表示无候选解通过 cheirality。
bool RecoverPose(const Mat3d& e, const std::vector<Vec2d>& pts0_norm,
                 const std::vector<Vec2d>& pts1_norm, CameraPose* out_pose1,
                 std::vector<uint8_t>* out_inlier_mask);

// 线性 DLT 三角化（多视图）。§4.4.2。
//
// 用完整投影矩阵构造 2N×4 方程组，取 AᵀA 最小特征向量。
// @param poses   各视图位姿（世界→相机，与 obs 一一对应）
// @param intr    各视图内参（与 poses 一一对应）
// @param obs     各视图的像素观测
// @param out     输出 3D 点（世界系，ru）
// 返回 false：点数 < 2 / 病态 / 视差角不足 / 深度越界。
bool TriangulateMultiView(const std::vector<CameraPose>& poses,
                          const std::vector<CameraIntrinsics>& intr,
                          const std::vector<Vec2d>& obs, const SfmConfig& config,
                          Vec3d* out);

// ---------------------------------------------------------------------------
// 特征匹配与匹配图（§4.2、§4.3）
// ---------------------------------------------------------------------------

// 一个 SfM 特征：位置 + 自研描述子。
//
// 描述子选型（§4.2.2）：文档主线是 ORB / rootSIFT，但两者都要 OpenCV。
// 自研零依赖路径下，本实现用**旋转归一化的梯度方向直方图**：
//   · 与 SIFT 同源（梯度直方图 + 主方向对齐 + 128 维 uint8 量化）；
//   · 量化到 uint8 后内存与 rootSIFT 同量级（128 B/特征），汉明/欧氏均可；
//   · 明确代价：无金字塔尺度空间 → **尺度不变性弱于 SIFT**，只适用于
//     关键帧间尺度变化较小的场景（本产品手持扫描正好满足：
//     03 §4.5 的角度步长 4°–35°，视图间深度变化有限）。
//     这个差距如实记录，不假装等价于 SIFT。
struct SfmFeature {
  Vec2d uv = Vec2d(0.0, 0.0);
  double response = 0.0;
  double angle = 0.0;                    // 主方向（弧度）
  std::vector<uint8_t> descriptor;       // 128 维 uint8（未归一化）
};

// 单帧特征集合。
struct SfmFeatureSet {
  int image_id = -1;
  std::vector<SfmFeature> features;
  // 本帧内参（供几何验证折算像素/归一化阈值）。§4.3.3 假定同设备同分辨率。
  CameraIntrinsics intrinsics;
  bool intrinsics_valid = false;
};

// 提取特征（复用 VIO 的 Shi-Tomasi 角点 + 自研描述子）。
SfmFeatureSet ExtractSfmFeatures(const uint8_t* gray, int width, int height,
                                 int stride, int image_id,
                                 const SfmConfig& config);

// 一对匹配。
struct SfmMatch {
  int index0 = -1;  // 帧 0 的特征下标
  int index1 = -1;  // 帧 1 的特征下标
  double distance = 0.0;
};

// 匹配两帧特征（ratio test + 可选双向一致性，§4.3.2）。
std::vector<SfmMatch> MatchFeatureSets(const SfmFeatureSet& a,
                                       const SfmFeatureSet& b,
                                       const SfmConfig& config);

// 匹配图的一条候选边。
struct SfmCandidatePair {
  int index0 = -1;
  int index1 = -1;
  int matches = 0;         // 粗匹配数
  int inliers = 0;         // 几何验证内点（未验证时为 0）
  bool verified = false;
  bool is_essential = false;  // 用 E（已标定）还是 F
};

// 构建候选匹配图（§4.3.1 的混合策略）。
//
// 序列邻接（MM-1）+ 空间近邻（MM-2，按相机中心方向差）+ 分数截断（MM-4）。
std::vector<SfmCandidatePair> BuildMatchGraph(
    const std::vector<SfmFeatureSet>& sets,
    const std::vector<CameraPose>* poses,  // 可为空（无初值时只走序列邻接）
    const SfmConfig& config);

// 按几何验证打分并排序候选对（§4.4.1 的「新增三角化点数 / 代价」近似）。
// 就地填充 verified/inliers/matches 并返回按分数降序的索引列表。
std::vector<int> ScoreCandidatePairs(const std::vector<SfmFeatureSet>& sets,
                                     std::vector<SfmCandidatePair>* pairs,
                                     const SfmConfig& config);

// 在候选对上做几何验证（E/F + RANSAC），返回匹配的内点对应。
// 输出 kept 为 (index0, index1) 特征下标的对应列表。
bool VerifyPairGeometry(const SfmFeatureSet& a, const SfmFeatureSet& b,
                        const std::vector<SfmMatch>& matches,
                        const SfmConfig& config, std::vector<SfmMatch>* inlier_matches,
                        Mat3d* out_e_or_f, bool* out_is_essential);



// BA 输入：一个待优化的地图。
struct Bamap {
  std::vector<CameraPose> poses;             // 各视图位姿（世界→相机）
  std::vector<CameraIntrinsics> intrinsics;  // 各视图内参（固定，不优化）
  std::vector<SfmTrack> tracks;              // 轨迹（观测 + 当前 3D 点）
  // 可选位姿先验（来自 VIO，§4.6「以 VIO 为强先验」）。为空则不使用。
  std::vector<CameraPose> prior_poses;
  std::vector<uint8_t> has_prior;
};

// BA 结果。
struct BaResult {
  bool ok = false;
  int iterations = 0;
  double initial_cost = 0.0;
  double final_cost = 0.0;
  double rms_reproj_px = 0.0;   // 最终重投影均方根（像素）
  int num_observations = 0;
  int num_points = 0;
};

// 全局 BA：重投影残差 + Huber 核 + 可选 VIO 位姿先验。
//
// 参数化：位姿用右扰动 [δθ, δp]（与 VIO 的 SO(3) 约定一致），
// 3D 点用加性扰动。阻尼求解复用 robust_fit.h 的 SolveSymmetricDamped —— 
// 与 VIO 共用同一数值内核，避免两套求解语义漂移。
//
// 就地修改 map->poses 与 map->tracks[].point。
BaResult BundleAdjust(Bamap* map, const SfmConfig& config);

// ---------------------------------------------------------------------------
// 增量注册（§4.4）
// ---------------------------------------------------------------------------

// 绝对位姿估计（PnP + RANSAC）：把一组 3D 点与 2D 观测的对应解出相机位姿。
//
// 实现口径（零依赖）：DLT 线性解 + RANSAC + 正交投影到 SO(3)。
//   · 优于 P3P：不需要处理多解与「点共面/近共线」的退化分支，
//     且天然支持 >4 点（噪声下更稳）。代价是需 ≥ 6 对点（3×4 矩阵 12 未知）。
//   · 内点判据用像素重投影误差 ≤ config.pnp_ransac_error_px（§4.4.1，4.0 px）。
//
// @param points_world  3D 点（世界系，ru）
// @param obs_px        对应的像素观测（与 points_world 等长）
// @param intr          目标视图内参
// @param out_pose      输出位姿（世界→相机）
// @param out_inlier_mask  与输入等长，1 = 内点（可为 nullptr）
// 返回 false：点数不足 / 无有效解 / 内点数低于 config.ransac_min_inliers。
bool EstimateAbsolutePose(const std::vector<Vec3d>& points_world,
                          const std::vector<Vec2d>& obs_px,
                          const CameraIntrinsics& intr, const SfmConfig& config,
                          CameraPose* out_pose,
                          std::vector<uint8_t>* out_inlier_mask);

// ---------------------------------------------------------------------------
// 增量式 SfM 主入口（§4.4）
// ---------------------------------------------------------------------------

// 输入视图（灰度图 + 初始内参）。
struct SfmView {
  const uint8_t* gray = nullptr;
  int width = 0;
  int height = 0;
  int stride = 0;
  CameraIntrinsics intrinsics;
};

// 重建结果。
struct SfmResult {
  bool ok = false;
  std::vector<CameraPose> poses;       // 各视图位姿（世界→相机，ru）
  std::vector<uint8_t> registered;     // 各视图是否注册成功
  std::vector<SfmTrack> tracks;        // 稀疏点云
  BaResult ba;                         // 最后一次全局 BA 的统计
  int num_registered = 0;
  int num_points = 0;
};

// 增量式稀疏重建。
//
// 流程（§4.4.1）：
//   特征提取 → 匹配图 → 选初始对（几何验证 + 三角化点数打分）
//   → 增量注册（PnP + RANSAC）→ 三角化 → 局部/全局 BA
// 零依赖：特征复用 VIO 的 Shi-Tomasi（DetectCorners）+ 自研梯度描述子。
SfmResult RunIncrementalSfm(const std::vector<SfmView>& views,
                            const SfmConfig& config);

// ---------------------------------------------------------------------------
// 自研几何原语的低声部工具（供测试直接调用，便于单元验证）
// ---------------------------------------------------------------------------

// Sampson 距离（像素单位）。x0/x1 为归一化坐标，K 用于折算回像素。
double SampsonDistanceNormalized(const Mat3d& f, const Vec2d& x0,
                                 const Vec2d& x1, const CameraIntrinsics& k);

// 对称转移距离（像素单位），用于 H 矩阵的内点判据。
double SymmetricTransferDistance(const Mat3d& h, const Vec2d& x0,
                                 const Vec2d& x1, const CameraIntrinsics& k);

// 计算重投影误差（像素）。投影矩阵由 pose + intrinsics 给出。
bool ReprojectionErrorPx(const CameraPose& pose, const CameraIntrinsics& k,
                         const Vec3d& point_world, const Vec2d& obs,
                         double* out_err_px);

}  // namespace recon

#endif  // RECON_CORE_SFM_H_
