// SPDX-License-Identifier: BSD-3-Clause
//
// 位姿图优化与闭环检测。契约：docs/05_单目管线.md §4.5（闭环检测）、
// §4.6（全局 BA / 位姿图）。
//
// 定位（docs/14 §5.1.1 登记的真缺口之一）：
//   MONO 长序列会累积漂移；闭环检测 + 位姿图优化把漂移拉回。
//
// 两条硬约束（与 COLMAP 的关键差异）：
//   · **闭环不做 Sim(3) 尺度自由**（§4.5.2）：尺度由 docs/07 参照物标定唯一确定；
//     闭环只优化 SE(3)。尺度偏差显式检测并作为**拒绝条件**，不改尺度。
//   · 闭环导致全局 BA 残差上升 > 20% → **回滚该闭环边**（§4.5.3）。
//
// 位姿口径：节点为 CameraPose（world→camera，与全仓库一致）。边为相对位姿
// 约束 Z_ij，语义「T_j = T_i · Z_ij」（T 为 camera→world，即 CameraPose 的逆）。
//
// 数值内核：复用 robust_fit.h 的 SolveSymmetricDamped（与 VIO / BA 同一求解器，
// 避免「同一逻辑写两遍必然漂移」）。雅可比用**中心差分**（项目的既定做法：
// 差分校验优先于手推闭式；见 MEMORY.md §4）。

#ifndef RECON_CORE_POSE_GRAPH_H_
#define RECON_CORE_POSE_GRAPH_H_

#include <cstdint>
#include <vector>

#include "recon/camera.h"
#include "recon/recon_c_api.h"
#include "recon/sfm.h"

namespace recon {

// ---------------------------------------------------------------------------
// 闭环检测配置。默认值取自 docs/05 §4.5.1 表（标准档）。
// ---------------------------------------------------------------------------
struct LoopConfig {
  int lsh_bits = 64;               // sfm_lsh_bits（指纹位数，固定 64）
  int lsh_hamming = 12;            // sfm_lsh_hamming（快 16 / 标 12 / 高 10）
  int descriptors_per_frame = 512; // §4.5.1 参与指纹的描述子数
  int max_candidates_per_frame = 20;  // sfm_loop_max_candidates

  // §4.5.2 几何验证。
  int min_matches = 30;            // sfm_loop_min_matches（快 20 / 标 30 / 高 40）
  int min_inliers = 30;            // sfm_loop_min_inliers
  double min_inlier_ratio = 0.30;  // sfm_loop_min_inlier_ratio
  double min_angle_deg = 15.0;     // sfm_loop_min_angle_deg（闭环须有大视角变化）
  double reproj_median_px = 1.5;   // sfm_loop_reproj_median_px
  double similarity_scale_tolerance = 0.10;  // sfm_loop_similarity_scale_tolerance

  // 排除时间上相邻的帧（避免把序列近邻当闭环）。
  int min_frame_separation = 20;
};

// 帧指纹（64 bit LSH 签名）。
//
// **规格口径澄清**：docs/05 §4.5.1 字面写「512 个描述子 × 每个 8 bit = 64 bit」，
// 但 512×8 = 4096 ≠ 64。本实现取**可用且确定**的口径：每个描述子经 8 次固定
// 随机超平面投影得 8 bit，再把 512 个 8-bit 码**按位折叠（XOR）**成 64 bit。
// 该口径用满全部描述子信息、恰好产出 64 bit，且随机投影矩阵由**固定种子**生成
// （跨会话一致）。此澄清记录于 docs/14 §5.1.1。
struct FrameFingerprint {
  int image_id = -1;
  uint64_t bits = 0;
  bool valid = false;
};

// 由一个特征集构造指纹。无描述子时 invalid。
FrameFingerprint ComputeFrameFingerprint(const SfmFeatureSet& set,
                                         const LoopConfig& config);

// 汉明距离。
int HammingDistance(uint64_t a, uint64_t b);

// 一个闭环候选对。
struct LoopCandidate {
  int index0 = -1;
  int index1 = -1;
  int hamming = 0;
};

// 由指纹序列找候选对（汉明距离 < 阈值，且帧间隔 ≥ min_frame_separation，且
// 每帧候选数受 max_candidates_per_frame 限制，按汉明距离升序保留）。
std::vector<LoopCandidate> DetectLoopCandidates(
    const std::vector<FrameFingerprint>& fingerprints, const LoopConfig& config);

// 一条闭环边（已通过几何验证）。
struct LoopClosure {
  int index0 = -1;
  int index1 = -1;
  Mat3d R_rel = Mat3d::Identity();  // 相对旋转
  Vec3d t_rel = Vec3d::Zero();      // 相对平移（ru，尺度待定）
  int matches = 0;
  int inliers = 0;
  double inlier_ratio = 0.0;
  double reproj_median_px = 0.0;
  double angle_deg = 0.0;
  double scale_ratio = 0.0;      // 闭环平移长度 / 地图对应长度
  bool accepted = false;
  recon_status status = RECON_OK;  // 拒绝原因（若有）
};

// 几何验证一条候选闭环（§4.5.2）。
//
// @param sets       全部特征集
// @param poses      当前地图位姿（用于尺度一致性与视角夹角判定）
// @param map_scale  当前地图尺度（ru → m 的乘子；由 docs/07 提供。>0）
// @param cand       候选对
// 判据全过 → accepted=true，否则 status 指示拒绝原因。
LoopClosure VerifyLoopClosure(const std::vector<SfmFeatureSet>& sets,
                              const std::vector<CameraPose>& poses,
                              double map_scale, const LoopCandidate& cand,
                              const LoopConfig& config);

// ---------------------------------------------------------------------------
// 位姿图
// ---------------------------------------------------------------------------

// 位姿图配置。
struct PoseGraphConfig {
  int max_iterations = 50;
  double convergence_tol = 1e-9;       // 代价相对下降阈值
  double initial_damping = 1e-4;       // LM 初始阻尼
  double huber_delta = 1.0;            // 白化后残差范数的 Huber 阈值
  double rot_sigma_rad = 0.01;         // 默认相对旋转 σ（边未指定时）
  double trans_sigma_m = 0.01;         // 默认相对平移 σ
  int fixed_node = 0;                  // gauge 固定节点（§4.6.1 GA-1）
  // 闭环导致代价上升超过该比例 → 拒绝并回滚（§4.5.3）。
  double loop_reject_increase = 0.20;
};

// 一条位姿图边。
struct PoseGraphEdge {
  int i = -1;
  int j = -1;
  Mat3d R_ij = Mat3d::Identity();  // 相对旋转
  Vec3d t_ij = Vec3d::Zero();      // 相对平移（米 / ru，与位姿单位一致）
  double rot_sigma_rad = 0.0;      // 0 → 用 config 默认
  double trans_sigma_m = 0.0;
  bool is_loop = false;
};

// 优化结果。
struct PoseGraphResult {
  bool ok = false;
  int iterations = 0;
  double initial_cost = 0.0;
  double final_cost = 0.0;
  int num_nodes = 0;
  int num_edges = 0;
  int num_loop_edges = 0;
};

// SE(3) 位姿图。
class PoseGraph {
 public:
  // 追加节点（world→camera）。返回节点下标。
  int AddNode(const CameraPose& world_to_camera);

  // 追加边。
  void AddEdge(const PoseGraphEdge& edge);

  // 由两条已定位姿构造一条普通（里程计）边：Z_ij = T_i^{-1} T_j。
  // 返回边下标。
  int AddRelativeEdge(int i, int j, const CameraPose& pose_i,
                      const CameraPose& pose_j, double rot_sigma_rad,
                      double trans_sigma_m);

  size_t num_nodes() const { return nodes_.size(); }
  size_t num_edges() const { return edges_.size(); }

  const std::vector<CameraPose>& poses() const { return nodes_; }
  std::vector<CameraPose>* mutable_poses() { return &nodes_; }

  // LM 优化。就地更新节点位姿。
  PoseGraphResult Optimize(const PoseGraphConfig& config);

  // 计算当前总代价（白化 + Huber 后的目标函数值）。
  double ComputeCost(const PoseGraphConfig& config) const;

  // 计算残差平方和（未 Huber，用于 §4.5.3 的「残差上升 > 20%」判据）。
  double ComputeResidualSumSq() const;

 private:
  std::vector<CameraPose> nodes_;
  std::vector<PoseGraphEdge> edges_;
};

// 便利函数：尝试加入一条闭环边并优化；若代价上升 > loop_reject_increase 则
// 回滚位姿并移除该边，返回 false（§4.5.3 W_SFM_LOOP_REJECTED）。
//
// 就地修改 graph。
bool TryAddLoopEdge(PoseGraph* graph, const PoseGraphEdge& loop_edge,
                    const PoseGraphConfig& config);

}  // namespace recon

#endif  // RECON_CORE_POSE_GRAPH_H_
