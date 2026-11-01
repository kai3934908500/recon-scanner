// SPDX-License-Identifier: BSD-3-Clause
//
// 端到端编排测试。
//
// 目的：把 L4 的「单测孤岛」接成一条**真正跑通**的流水线并验收产物。
// 之前每个模块都有自己的单测（相机、VIO、MVS、泊松、UV、烘焙、导出），
// 但 L5 编排层（orchestrator.cpp，1500 行）**一行集成测试都没有** ——
// 这正违反本项目「单测孤岛不等于系统可跑」的教训。
//
// 本测试覆盖：
//   A. RGBD 分支全链路：合成帧 → recon_run_reconstruction → recon_export，
//      断言 8 个阶段全部执行、网格信息自洽、导出的 GLB 可被解析。
//   B. C ABI 端到端：recon_create → probe → start_session → push_frame
//      → finish_capture → run_reconstruction → get_mesh_info → export。
//   C. 取消语义：cancel 后 run_reconstruction 必须返回 CANCELLED。
//   D. 空会话：无帧时 run_reconstruction 必须返回 MVS_NO_DEPTH。
//
// 场景设计（**为什么是长方体而不是球 / 平面**）：
//   · 平面是开放面 → 泊松输出开放壳 → 导出校验 V10（边界环）失败，
//     无法验证「导出成功」这一环。
//   · 球面要闭合需要**全方向覆盖**（十几到几十个视图），测试变慢且脆弱。
//   · 长方体的优越性：**6 个正交视图即可完整覆盖 6 个面**（每面被正视
//     一次），TSDF 提取出的点云天然接近闭合 → 泊松输出可闭合 → 导出能过。
//     这也是 TSDF 融合最擅长的一类几何（平面片 + 正交观测）。
//
// 深度口径：与 `TsdfVolume::IntegrateDepthFrame` 一致，用**光轴深度**
// （camera-frame z，单位米 → 毫米）。渲染时相机射线方向取 (px, py, 1)，
// 故射线参数 t 恰好等于光轴深度（见 RenderBox 的推导）。

#include "recon/context.h"
#include "recon/recon_c_api.h"
#include "recon/sfm.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
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

using recon::CameraIntrinsics;
using recon::CameraPose;
using recon::Mat3d;
using recon::Vec3d;
using recon::kPi;

// ---------------------------------------------------------------------------
// 合成长方体场景
// ---------------------------------------------------------------------------

constexpr int kWidth = 160;
constexpr int kHeight = 160;
constexpr double kHalf = 0.05;      // 长方体半边长（米）
constexpr double kViewDist = 0.25;  // 相机到物体中心距离（米）

struct Frame {
  std::vector<uint8_t> rgb;    // BGRA8
  std::vector<uint16_t> depth; // 毫米，光轴深度
  std::vector<uint8_t> conf;
  CameraIntrinsics intrinsics;
  CameraPose pose;
};

CameraIntrinsics MakeIntrinsics() {
  CameraIntrinsics k;
  k.fx = k.fy = 0.9 * kWidth;
  k.cx = kWidth * 0.5;
  k.cy = kHeight * 0.5;
  k.width = kWidth;
  k.height = kHeight;
  k.k1 = k.k2 = k.k3 = 0.0;
  k.p1 = k.p2 = 0.0;
  return k;
}

// 指定分辨率的近似内参（fx = fy = 0.9×宽，约 58° 水平 FOV 的手机主摄）。
CameraIntrinsics MakeIntrinsicsWH(int w, int h) {
  CameraIntrinsics k;
  k.fx = k.fy = 0.9 * static_cast<double>(w);
  k.cx = 0.5 * static_cast<double>(w);
  k.cy = 0.5 * static_cast<double>(h);
  k.width = w;
  k.height = h;
  k.k1 = k.k2 = k.k3 = 0.0;
  k.p1 = k.p2 = 0.0;
  return k;
}

// 沿方向 n（单位向量）在距离 dist 处放置相机，看向原点。
// 返回 world → camera 位姿。沿用已被验证的构造：R 的**第 2 行**是光轴。
CameraPose LookAtOrigin(const Vec3d& n, double dist) {
  const Vec3d center = n * dist;
  Vec3d forward = (-n).normalized();
  Vec3d world_down(0.0, -1.0, 0.0);
  Vec3d right = forward.cross(world_down);
  if (right.norm() < 1e-6) right = Vec3d(1.0, 0.0, 0.0);
  right.normalize();
  Vec3d down = forward.cross(right);
  if (down.norm() < 1e-6) down = Vec3d(0.0, 1.0, 0.0);
  down.normalize();

  Mat3d R_wc;
  R_wc(0, 0) = right(0);   R_wc(0, 1) = right(1);   R_wc(0, 2) = right(2);
  R_wc(1, 0) = down(0);    R_wc(1, 1) = down(1);    R_wc(1, 2) = down(2);
  R_wc(2, 0) = forward(0); R_wc(2, 1) = forward(1); R_wc(2, 2) = forward(2);

  CameraPose p;
  p.R = R_wc;
  p.t = -(R_wc * center);
  return p;
}

// 长方体某一点的纹理：按世界坐标做块状棋盘（强边缘，便于烘焙/跟踪）。
uint8_t BoxTexture(double wx, double wy, double wz) {
  const double cell = 0.005;  // 5 mm
  const int ax = static_cast<int>(std::floor(wx / cell));
  const int ay = static_cast<int>(std::floor(wy / cell));
  const int az = static_cast<int>(std::floor(wz / cell));
  const uint32_t h = static_cast<uint32_t>(ax * 73856093u ^ ay * 19349663u ^
                                           az * 83492791u);
  const bool odd = ((ax + ay + az) & 1) != 0;
  double v = odd ? 200.0 : 55.0;
  v += static_cast<double>((h >> 8) & 0x3F) * 0.9;  // 叠加细纹
  return static_cast<uint8_t>(std::clamp(v, 0.0, 255.0));
}

// 射线与长方体求交（slab 法）。返回最近正参数 t；无交返回 -1。
double RayBox(const Vec3d& o, const Vec3d& d) {
  double tmin = -1e30;
  double tmax = 1e30;
  for (int i = 0; i < 3; ++i) {
    if (std::fabs(d(i)) < 1e-12) {
      if (o(i) < -kHalf || o(i) > kHalf) return -1.0;
      continue;
    }
    double t1 = (-kHalf - o(i)) / d(i);
    double t2 = (kHalf - o(i)) / d(i);
    if (t1 > t2) std::swap(t1, t2);
    tmin = std::max(tmin, t1);
    tmax = std::min(tmax, t2);
    if (tmin > tmax) return -1.0;
  }
  if (tmax < 1e-6) return -1.0;
  const double t = (tmin > 1e-6) ? tmin : tmax;
  return t;
}

// 渲染一帧：深度（光轴，毫米）+ BGRA 颜色 + 置信度。
Frame RenderBox(const CameraPose& pose, const CameraIntrinsics& k) {
  Frame f;
  f.intrinsics = k;
  f.pose = pose;
  f.depth.assign(static_cast<size_t>(kWidth) * kHeight, 0);
  f.conf.assign(static_cast<size_t>(kWidth) * kHeight, 0);
  f.rgb.assign(static_cast<size_t>(kWidth) * kHeight * 4, 0);

  const Vec3d origin = pose.CenterWorld();

  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      // 相机系射线方向（z=1）→ 世界系。
      const Vec3d d_cam((x + 0.5 - k.cx) / k.fx, (y + 0.5 - k.cy) / k.fy, 1.0);
      const Vec3d d_world = pose.R.transpose() * d_cam;

      const double t = RayBox(origin, d_world);
      if (t <= 0.0) continue;

      // 相机系交点 = t · d_cam（因为 R·(o + t·d_world) + t_cw = t·d_cam）。
      // 光轴深度 = 该点的 z 分量 = t · 1 = t。
      const double depth_m = t;
      if (depth_m < 0.02 || depth_m > 2.0) continue;

      const Vec3d p_world = origin + d_world * t;

      const size_t pix = static_cast<size_t>(y) * kWidth + x;
      f.depth[pix] = static_cast<uint16_t>(std::lround(depth_m * 1000.0));
      f.conf[pix] = 255;

      const uint8_t v = BoxTexture(p_world(0), p_world(1), p_world(2));
      f.rgb[pix * 4 + 0] = v;  // B
      f.rgb[pix * 4 + 1] = v;  // G
      f.rgb[pix * 4 + 2] = v;  // R
      f.rgb[pix * 4 + 3] = 255;
    }
  }
  return f;
}

// 观测方向：3×3×3 网格去掉中心 → 26 个方向。
//
// **为什么不能只用 6 个正交视图（实测踩过的坑）**：
//   长方体有 12 条**凸棱**。若只沿 6 个面法向正视，每张深度图只覆盖单个面，
//   棱两侧的两个面**没有任何一帧同时看到**。TSDF 在棱处的截断带只含单侧
//   法向的信息 → 零交叉点云在棱上出现细缝 → 泊松输出沿棱断开 →
//   实测边界环 **131 个**（≈ 12 条棱 × 每条碎成十余段）→ 网格不闭合 →
//   导出校验 V10（边界环）不通过。
//
//   加入「棱方向」（n=(1,1,0)/√2 等 12 个）与「角方向」（n=(1,1,1)/√3 等
//   8 个）后，每条棱/每个角都至少被一帧**斜视**覆盖，深度在棱处连续 →
//   零交叉点云闭合 → 泊松可输出闭合网格。
//
//   这不是「把测试调绿」，而是修正一个**错误的任务假设**：单靠面法向
//   正视**在原理上**不足以重建筑凸棱（TSDF 的固有边界问题），后处理
//   补洞只能补「尺寸有限、边界简单」的洞，补不了贯穿整条棱的长缝。
std::vector<Frame> MakeBoxFrames() {
  const CameraIntrinsics k = MakeIntrinsics();
  std::vector<Frame> frames;
  for (int dz = -1; dz <= 1; ++dz) {
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        if (dx == 0 && dy == 0 && dz == 0) continue;
        const Vec3d n = Vec3d(dx, dy, dz).normalized();
        frames.push_back(RenderBox(LookAtOrigin(n, kViewDist), k));
      }
    }
  }
  return frames;
}

// SfM 用例视图：**连续弧线上的多视图**，相邻帧视角仅转 ~7°。
//
// **为什么不能用 MakeBoxFrames**（实测教训，与本项目 test_sfm 端到端同源）：
//   SfM 在**无位姿初值**时只走 MM-1 序列邻接，即「假定相邻帧空间上也相邻」。
//   而环绕长方体的相邻视图（如从 +Z 面到 +X 面）相差 90°，看到的是**不同
//   物理面**，无重叠 → 粗匹配内点率近 0 → 选不出初始对 → 0 注册。
//
//   正确做法：相机沿一条**连续弧线**缓慢移动（每帧仅转 7°、平移 0.03），
//   相邻帧看到几乎相同的场景区域 → 强重叠 + 小基线 → 序列邻接可靠。
//
//   场景是**真正的 3D 深度场**（z(x,y) 起伏），而非平面 —— 纯平面是 E 矩阵
//   估计的经典退化配置（docs/05 §4.3.3），E/F 内点率会崩。
//
// 分辨率用 320×240（与 test_sfm 同口径，经验证特征量充足）。
constexpr int kSfmW = 320;
constexpr int kSfmH = 240;

// 低频深度场（世界 x,y）→ 世界 z。制造明确的三维结构，使基线方向可观。
double SfmDepthField(double x, double y) {
  return 2.0 + 0.30 * std::sin(1.2 * x + 0.5) * std::cos(1.0 * y - 0.3) +
         0.20 * std::sin(0.7 * y + 1.1);
}

// 相机绕 Y 轴转 ry、平移 C（world）→ world→camera 位姿。
CameraPose MakePoseY(double ry, const Vec3d& C) {
  const double c = std::cos(ry), s = std::sin(ry);
  Mat3d R = Mat3d::Identity();
  R(0, 0) = c;  R(0, 2) = s;
  R(2, 0) = -s; R(2, 2) = c;
  CameraPose p;
  p.R = R;
  p.t = -(R * C);
  return p;
}

// 以深度场 + 波纹理渲染一帧灰度（BGRA，R=G=B=灰度）。
// 波纹理在 (Xw.x, Xw.y) 上高频采样，保证局部梯度方向唯一（可匹配）。
Frame MakeDepthFieldFrame(const CameraPose& pose, const CameraIntrinsics& k,
                          double ry) {
  Frame f;
  f.intrinsics = k;
  f.pose = pose;
  const int W = k.width;
  const int H = k.height;
  f.rgb.assign(static_cast<size_t>(W) * H * 4, 0);
  const Vec3d Cw = pose.CenterWorld();
  // 纹理波：确定性 + 覆盖足够频率（周期 24–170 px，与 test_sfm 同量级）。
  const double kx[8] = {0.31, -0.22, 0.13, 0.27, -0.35, 0.18, -0.09, 0.24};
  const double ky[8] = {0.14, 0.29, -0.33, -0.17, 0.21, -0.26, 0.36, 0.11};
  const double ph[8] = {0.3, 1.7, 2.9, 4.1, 5.3, 0.9, 3.7, 2.1};
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      const double ux = (x - k.cx) / k.fx;
      const double uy = (y - k.cy) / k.fy;
      const Vec3d dir = pose.R.transpose() * Vec3d(ux, uy, 1.0);
      if (std::fabs(dir(2)) < 1e-6) continue;
      // 固定点迭代求射线与深度场的交点。
      double s = (2.0 - Cw(2)) / dir(2);
      for (int it = 0; it < 6; ++it) {
        const Vec3d Xs = Cw + dir * s;
        s = (SfmDepthField(Xs(0), Xs(1)) - Cw(2)) / dir(2);
      }
      const Vec3d Xw = Cw + dir * s;
      double t = 0.0;
      for (int w = 0; w < 8; ++w) {
        t += 90.0 * std::sin(kx[w] * Xw(0) * 300.0 + ky[w] * Xw(1) * 300.0 + ph[w]);
      }
      const uint8_t v = static_cast<uint8_t>(
          std::llround(std::clamp(128.0 + t * 0.30, 0.0, 255.0)));
      const size_t pix = static_cast<size_t>(y) * W + x;
      f.rgb[pix * 4 + 0] = v;
      f.rgb[pix * 4 + 1] = v;
      f.rgb[pix * 4 + 2] = v;
      f.rgb[pix * 4 + 3] = 255;
    }
  }
  (void)ry;
  return f;
}

std::vector<Frame> MakeOrbitFrames(int n_views) {
  const CameraIntrinsics k = MakeIntrinsicsWH(kSfmW, kSfmH);
  std::vector<Frame> frames;
  frames.reserve(static_cast<size_t>(n_views));
  // 弧线参数：每帧转 0.03 rad（~1.7°）、沿 X 平移 0.035。
  //
  // 关键权衡（实测）：平移/转角过大 → 首尾帧基线 3.8 单位，远端帧与
  // 中间帧无重叠 → 只有少数帧能被注册。SfM 无位姿初值只走序列邻接，
  // 必须保证**相邻帧强重叠**（这里是每步 ~1.7°、0.035 单位），
  // 同时整段累积出足够视差（首尾 ~40°、0.8 单位）供三角化。
  const double d_ang = 0.05;
  const double d_x = 0.10;
  for (int v = 0; v < n_views; ++v) {
    const double ang = (v - (n_views - 1) * 0.5) * d_ang;
    const Vec3d C(static_cast<double>(v) * d_x, 0.0, 0.0);
    const CameraPose pose = MakePoseY(ang, C);
    frames.push_back(MakeDepthFieldFrame(pose, k, ang));
  }
  return frames;
}

// ---------------------------------------------------------------------------
// 组装 C ABI 帧包
// ---------------------------------------------------------------------------
struct PacketStorage {
  recon_camera_intrinsics intr{};
  recon_pose pose{};
  recon_frame_packet packet{};
};

void FillPacket(const Frame& f, uint64_t id, uint64_t ts_ns,
                PacketStorage* st) {
  st->intr.fx = f.intrinsics.fx;
  st->intr.fy = f.intrinsics.fy;
  st->intr.cx = f.intrinsics.cx;
  st->intr.cy = f.intrinsics.cy;
  st->intr.width = f.intrinsics.width;
  st->intr.height = f.intrinsics.height;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      st->pose.r[i * 3 + j] = f.pose.R(i, j);
    }
    st->pose.t[i] = f.pose.t(i);
  }
  st->pose.timestamp_ns = static_cast<double>(ts_ns);

  st->packet = recon_frame_packet{};
  st->packet.frame_id = id;
  st->packet.timestamp_ns = ts_ns;
  st->packet.rgb_data = f.rgb.data();
  st->packet.rgb_width = kWidth;
  st->packet.rgb_height = kHeight;
  st->packet.rgb_stride = kWidth * 4;
  st->packet.depth_data = reinterpret_cast<const uint8_t*>(f.depth.data());
  st->packet.depth_width = kWidth;
  st->packet.depth_height = kHeight;
  st->packet.depth_stride = kWidth * 2;
  st->packet.depth_confidence = f.conf.data();
  st->packet.depth_source = RECON_DEPTH_SRC_LIDAR;
  st->packet.has_depth = 1;
  st->packet.intrinsics = &st->intr;
  st->packet.pose = &st->pose;
  st->packet.exposure_us = 8000.0;
  st->packet.iso = 100.0;
}

// ---------------------------------------------------------------------------
// 进度回调探针：记录被上报过的阶段集合。
// ---------------------------------------------------------------------------
struct ProgressLog {
  bool seen[16] = {false};
  int callbacks = 0;
};

void OnProgress(void* ud, int32_t stage, double /*progress*/,
                const char* /*msg*/) {
  auto* log = static_cast<ProgressLog*>(ud);
  if (stage >= 0 && stage < 16) log->seen[stage] = true;
  ++log->callbacks;
}

void OnLog(void* /*ud*/, int32_t level, const char* msg) {
  std::printf("      [log %d] %s\n", level, msg);
}

// ---------------------------------------------------------------------------
// A. RGBD 分支全链路
// ---------------------------------------------------------------------------
void TestRgbdEndToEnd() {
  std::printf("[TEST] RGBD 分支端到端（帧 → 重建 → 导出 GLB）\n");

  recon::SetPlatformProbeProvider(nullptr);  // 用强制分支，避免依赖平台桩
  std::vector<Frame> frames = MakeBoxFrames();

  recon_config cfg{};
  cfg.abi_version = RECON_ABI_VERSION;
  cfg.forced_branch = RECON_BRANCH_RGBD;   // 强制 RGBD：本测试要跑 TSDF 主线
  cfg.preset = 0;                          // 快速档：本测试验链路，不验极限精度
  cfg.enable_mvs = 0;                      // MVS 有独立单测；此处关闭以聚焦编排链路
  cfg.enable_texture = 1;
  cfg.enable_rgbd = 1;
  cfg.voxel_size_m = 0.0025;               // TSDF 体素

  void* ctx = nullptr;
  Check(recon_create(&cfg, &ctx) == RECON_OK, "create 应成功");
  if (ctx == nullptr) return;

  // 约束泊松深度：本测试的几何很小（0.1 m 边长、点间距约 5 mm），
  // 由采样间距反推得到的 depth 约 5（r ≈ 3.4 mm，与点间距同量级）。
  // **必须放开下限到 4**：默认下限 6 会把 depth 抬到 6（r = 1.76 mm），
  // 而 r 远小于点间距时点云支撑断开 → 等值面碎成孤立片（docs/06 §1.3 的
  // 硬约束正是「八叉树单元边长 r ≥ 点云采样间距」）。上限压到 8 保证
  // 不因偶然的密点云把测试拖到分钟级。
  recon_set_param(ctx, recon::keys::kMeshPoissonDepthMin, 4, 0.0);
  recon_set_param(ctx, recon::keys::kMeshPoissonDepthMax, 8, 0.0);
  recon_set_param(ctx, recon::keys::kPostTextureAtlasSize, 512, 0.0);

  Check(recon_start_session(ctx, "e2e_rgbd") == RECON_OK, "开始会话应成功");

  for (size_t i = 0; i < frames.size(); ++i) {
    PacketStorage st;
    FillPacket(frames[i], i + 1, 1000000000ull * (i + 1), &st);
    Check(recon_push_frame(ctx, &st.packet) == RECON_OK, "推帧应成功");
  }

  Check(recon_finish_capture(ctx) == RECON_OK, "结束采集应成功");

  ProgressLog plog;
  const recon_status st =
      recon_run_reconstruction(ctx, OnProgress, nullptr, OnLog, &plog);
  Check(st == RECON_OK, "重建应成功");

  // 阶段执行覆盖：8 个业务阶段都应被上报过进度。
  const int32_t kStages[] = {
      RECON_STAGE_PREPROCESS, RECON_STAGE_VIO,     RECON_STAGE_MVS,
      RECON_STAGE_RGBD_FUSION, RECON_STAGE_MESH,   RECON_STAGE_POSTPROC,
      RECON_STAGE_UV_TEXTURE,  RECON_STAGE_SCALE,
  };
  for (int32_t s : kStages) {
    Check(plog.seen[s], "每个业务阶段都应上报进度");
  }
  Check(plog.seen[RECON_STAGE_DONE], "应上报 DONE");
  std::printf("    进度回调 %d 次\n", plog.callbacks);

  // 网格信息自洽。
  recon_mesh_info info{};
  Check(recon_get_mesh_info(ctx, &info) == RECON_OK, "读取网格信息应成功");
  std::printf("    顶点 %llu，三角形 %llu，非流形边 %u，边界环 %u，"
              "泊松深度 %u，mean_edge %.3f mm\n",
              static_cast<unsigned long long>(info.vertex_count),
              static_cast<unsigned long long>(info.triangle_count),
              info.non_manifold_edges, info.boundary_loops, info.poisson_depth,
              info.mean_edge_length_m * 1000.0);

  Check(info.vertex_count > 0, "应产生顶点");
  Check(info.triangle_count > 0, "应产生三角形");
  Check(info.non_manifold_edges == 0, "后处理应消除非流形边");
  Check(info.mean_edge_length_m > 0.0, "平均边长应为正");

  // 关键帧时间戳契约（docs/05 §3.10：每关键帧带时间戳）。
  //
  // 本用例所有帧都由平台位姿路径接受（pose 齐备），故编排层的关键帧数
  // 应等于推入帧数，且第 k 个关键帧的时间戳应**精确等于**第 k 帧推入时
  // 用的纳秒值（FillPacket 用 1000000000ull*(i+1)）。
  //
  // 该断言的价值在于：时间戳曾在 VIO→编排边界被吞掉（CameraPose 是纯几何
  // 量、无时间字段），下游只能靠「回查 frames_ 下标」重建时间，而
  // frame_id 与 frames_ 下标在有空灰度帧时会错位。此处直接锁定数值与顺序。
  {
    const std::vector<uint64_t>& ts =
        static_cast<recon::Context*>(ctx)->orchestrator().keyframe_timestamps_ns();
    const size_t n_frames = 26;  // MakeBoxFrames：3³−1
    Check(ts.size() == n_frames, "关键帧时间戳数应等于推入帧数");
    bool ts_ok = (ts.size() == n_frames);
    for (size_t k = 0; k < ts.size() && ts_ok; ++k) {
      if (ts[k] != 1000000000ull * (k + 1)) ts_ok = false;
    }
    std::printf("    关键帧时间戳 %zu 个，逐帧精确匹配 %s\n", ts.size(),
                ts_ok ? "是" : "否");
    Check(ts_ok, "关键帧时间戳应逐帧精确等于推入时刻（单调、无错位）");
  }
  // 包围盒应贴合长方体（半边长 0.05，容许外扩）。
  const double ext_x = info.bbox_max[0] - info.bbox_min[0];
  const double ext_y = info.bbox_max[1] - info.bbox_min[1];
  const double ext_z = info.bbox_max[2] - info.bbox_min[2];
  Check(ext_x > 0.05 && ext_x < 0.30, "包围盒 X 尺寸应接近长方体尺度");
  Check(ext_y > 0.05 && ext_y < 0.30, "包围盒 Y 尺寸应接近长方体尺度");
  Check(ext_z > 0.05 && ext_z < 0.30, "包围盒 Z 尺寸应接近长方体尺度");

  // 导出 GLB（自包含，单文件）。
  const std::string out = "test_pipeline_model.glb";
  recon_export_options eo{};
  eo.format = RECON_EXPORT_GLB;
  eo.out_path = out.c_str();
  eo.apply_scale = 0;
  eo.write_binary = 1;
  eo.embed_texture = 1;
  eo.generate_normals = 1;
  eo.target_face_count = 0.0;
  const recon_status est = recon_export(ctx, &eo);
  std::printf("    导出 GLB：status=%d（%s）\n", static_cast<int>(est),
              est == RECON_OK ? "OK" : "失败");
  Check(est == RECON_OK, "导出 GLB 应成功（非流形边为 0 即通过校验）");

  recon_destroy(ctx);
}

// ---------------------------------------------------------------------------
// B. C ABI 生命周期（含 probe 与能力分支）
// ---------------------------------------------------------------------------
recon::PlatformProbe g_probe;
recon::PlatformProbe StubProbe() { return g_probe; }

void TestCApiLifecycleEndToEnd() {
  std::printf("[TEST] C ABI 生命周期端到端（含能力探测）\n");

  // 桩：报告 LiDAR → 期望自动判定 RGBD 分支（不强制分支）。
  g_probe = recon::PlatformProbe{};
  g_probe.has_camera = true;
  g_probe.has_lidar = true;
  recon::SetPlatformProbeProvider(&StubProbe);

  recon_config cfg{};
  cfg.abi_version = RECON_ABI_VERSION;
  cfg.forced_branch = static_cast<recon_pipeline_branch>(0xFFFFFFFF);  // 自动
  cfg.preset = 0;
  cfg.enable_mvs = 0;
  cfg.enable_texture = 0;   // 关闭纹理：本测试只验分派与产物，不验烘焙
  cfg.enable_rgbd = 1;
  cfg.voxel_size_m = 0.005;

  void* ctx = nullptr;
  Check(recon_create(&cfg, &ctx) == RECON_OK, "create 应成功");
  if (ctx == nullptr) return;

  recon_capability cap{};
  Check(recon_probe_capability(ctx, &cap) == RECON_OK, "探测应成功");
  Check(cap.branch == RECON_BRANCH_RGBD, "应自动判定 RGBD 分支");
  Check(cap.has_lidar == 1, "应识别到 LiDAR");

  recon_set_param(ctx, recon::keys::kMeshPoissonDepthMin, 4, 0.0);
  recon_set_param(ctx, recon::keys::kMeshPoissonDepthMax, 8, 0.0);

  Check(recon_start_session(ctx, "e2e_capi") == RECON_OK, "开始会话应成功");

  std::vector<Frame> frames = MakeBoxFrames();
  for (size_t i = 0; i < frames.size(); ++i) {
    PacketStorage st;
    FillPacket(frames[i], i + 1, 1000000000ull * (i + 1), &st);
    Check(recon_push_frame(ctx, &st.packet) == RECON_OK, "推帧应成功");
  }

  // 采集统计：帧数应与推入数一致。
  recon_scan_stats stats{};
  Check(recon_get_scan_stats(ctx, &stats) == RECON_OK, "读取统计应成功");

  Check(recon_finish_capture(ctx) == RECON_OK, "结束采集应成功");
  Check(recon_run_reconstruction(ctx, nullptr, nullptr, nullptr, nullptr) ==
            RECON_OK,
        "重建应成功");

  recon_mesh_info info{};
  Check(recon_get_mesh_info(ctx, &info) == RECON_OK, "读取网格信息应成功");
  Check(info.triangle_count > 0, "应产生三角形");

  // 导出 PLY（无纹理依赖，避免与 A 用例重复）。
  const std::string out = "test_pipeline_model.ply";
  recon_export_options eo{};
  eo.format = RECON_EXPORT_PLY;
  eo.out_path = out.c_str();
  eo.write_binary = 1;
  eo.generate_normals = 1;
  eo.apply_scale = 0;
  const recon_status est = recon_export(ctx, &eo);
  std::printf("    导出 PLY：status=%d\n", static_cast<int>(est));
  Check(est == RECON_OK, "导出 PLY 应成功");

  recon_destroy(ctx);
}

// ---------------------------------------------------------------------------
// C. 取消语义
// ---------------------------------------------------------------------------
void TestCancelBeforeRun() {
  std::printf("[TEST] 取消后重建必须返回 CANCELLED\n");

  recon::SetPlatformProbeProvider(nullptr);
  recon_config cfg{};
  cfg.abi_version = RECON_ABI_VERSION;
  cfg.forced_branch = RECON_BRANCH_RGBD;
  cfg.preset = 0;

  void* ctx = nullptr;
  recon_create(&cfg, &ctx);
  if (ctx == nullptr) return;

  recon_start_session(ctx, "e2e_cancel");
  std::vector<Frame> frames = MakeBoxFrames();
  PacketStorage st;
  FillPacket(frames[0], 1, 1000000000ull, &st);
  recon_push_frame(ctx, &st.packet);
  recon_finish_capture(ctx);

  Check(recon_is_cancelled(ctx) == 0, "初始不应为已取消");
  Check(recon_cancel(ctx) == RECON_OK, "取消应成功");
  Check(recon_is_cancelled(ctx) == 1, "取消标志应置位");

  const recon_status st2 =
      recon_run_reconstruction(ctx, nullptr, nullptr, nullptr, nullptr);
  Check(st2 == RECON_ERR_CANCELLED, "取消后重建应返回 CANCELLED");

  recon_destroy(ctx);
}

// ---------------------------------------------------------------------------
// D. 空会话
// ---------------------------------------------------------------------------
void TestEmptySession() {
  std::printf("[TEST] 空会话（无帧）应拒绝重建\n");

  recon::SetPlatformProbeProvider(nullptr);
  recon_config cfg{};
  cfg.abi_version = RECON_ABI_VERSION;
  cfg.forced_branch = RECON_BRANCH_RGBD;

  void* ctx = nullptr;
  recon_create(&cfg, &ctx);
  if (ctx == nullptr) return;

  recon_start_session(ctx, "e2e_empty");
  // 不推任何帧。
  const recon_status st =
      recon_run_reconstruction(ctx, nullptr, nullptr, nullptr, nullptr);
  Check(st == RECON_ERR_MVS_NO_DEPTH, "无帧时重建应返回 MVS_NO_DEPTH");

  recon_destroy(ctx);
}

// ---------------------------------------------------------------------------
// E. MONO 无 IMU 分支：SfM 承担位姿（docs/05 §2.4 C1、§4）
//
// 场景：只推 **RGB**（无深度）、**无平台位姿**、**无 IMU**，强制 MONO 分支。
// 编排层的 VIO 阶段应识别出「无 IMU」→ 转 StageSfmMonol → SfM 建图。
//
// 为什么用长方体 + 26 视图（复用 A 用例的场景）：
//   SfM 需要**大视差**才能三角化；26 个方向对长方体中心形成环绕，
//   相邻视图基线充足。纹理复用 BoxTexture（强边缘），便于特征匹配。
//
// 本用例的验收点不是「重建出高精度 mesh」（MONO 分支的精度验收在
// docs/05 §6 有独立口径），而是**证明 SfM 真的被编排层调用并产出了位姿**：
//   · VIO 阶段成功（而非像以前那样直接 VIO_NOT_INITIALIZED）；
//   · 关键帧数 > 0；
//   · SfM 注册帧数与稀疏点数被记录且 > 0。
// 这补上了「单测孤岛不等于系统可跑」的最后一环 —— SfM 单测全绿，
// 但编排层从未调用过它。
// ---------------------------------------------------------------------------
void TestSfmMonoEndToEnd() {
  std::printf("[TEST] MONO 无 IMU 分支端到端（RGB-only → SfM 位姿）\n");

  recon::SetPlatformProbeProvider(nullptr);  // 无平台能力 → 纯 MONO
  std::vector<Frame> frames = MakeOrbitFrames(12);

  recon_config cfg{};
  cfg.abi_version = RECON_ABI_VERSION;
  cfg.forced_branch = RECON_BRANCH_MONO;  // 强制 MONO：验 SfM 位姿路径
  cfg.preset = 0;
  cfg.enable_mvs = 0;
  cfg.enable_texture = 0;
  cfg.enable_rgbd = 0;

  void* ctx = nullptr;
  Check(recon_create(&cfg, &ctx) == RECON_OK, "create 应成功");
  if (ctx == nullptr) return;

  Check(recon_start_session(ctx, "e2e_sfm_mono") == RECON_OK, "开始会话应成功");

  // 只推 RGB：不填深度、不填位姿、不填 IMU。
  // 堆上分配 packet 存储，保证指针在 push 期间有效。
  std::vector<PacketStorage> stores(frames.size());
  for (size_t i = 0; i < frames.size(); ++i) {
    PacketStorage& st = stores[i];
    st.packet = recon_frame_packet{};
    st.packet.frame_id = i + 1;
    st.packet.timestamp_ns = 1000000000ull * (i + 1);
    st.packet.rgb_data = frames[i].rgb.data();
    st.packet.rgb_width = kSfmW;
    st.packet.rgb_height = kSfmH;
    st.packet.rgb_stride = kSfmW * 4;
    st.packet.has_depth = 0;   // 关键：无深度
    st.packet.depth_data = nullptr;
    // 内参仍要提供（SfM 需要 K）。
    st.intr.fx = frames[i].intrinsics.fx;
    st.intr.fy = frames[i].intrinsics.fy;
    st.intr.cx = frames[i].intrinsics.cx;
    st.intr.cy = frames[i].intrinsics.cy;
    st.intr.width = kSfmW;
    st.intr.height = kSfmH;
    st.packet.intrinsics = &st.intr;
    st.packet.pose = nullptr;  // 无平台位姿
    Check(recon_push_frame(ctx, &st.packet) == RECON_OK, "推 RGB 帧应成功");
  }

  Check(recon_finish_capture(ctx) == RECON_OK, "结束采集应成功");

  // 先跑 PREPROCESS 生成灰度图（SfM 的输入）；再单跑 VIO 阶段。
  // 本用例聚焦位姿来源，不要求整链跑通（MONO 分支的 MVS 需要更多视图，
  // 属独立验收）。
  recon::Orchestrator& orch = static_cast<recon::Context*>(ctx)->orchestrator();
  recon::StageResult pres;
  const recon_status pst = orch.runStage(RECON_STAGE_PREPROCESS, &pres);
  std::printf("    PREPROCESS：status=%d，接受帧 %zu\n", static_cast<int>(pst),
              orch.accepted_frame_count());
  Check(pst == RECON_OK, "PREPROCESS 应成功");

  recon::StageResult vres;
  const recon_status vst = orch.runStage(RECON_STAGE_VIO, &vres);
  std::printf("    VIO/SfM 阶段：status=%d，message=%s，相似帧数=%zu\n",
              static_cast<int>(vst), vres.message.c_str(),
              orch.keyframe_timestamps_ns().size());
  Check(vst == RECON_OK, "无 IMU 时应由 SfM 给出位姿，不得返回 VIO_NOT_INITIALIZED");
  Check(vres.message == "SfM 位姿（无 IMU）", "位姿应标注来自 SfM");

  const size_t nkf = orch.keyframe_timestamps_ns().size();
  std::printf("    关键帧 %zu，SfM 注册 %d 帧，稀疏点 %d，BA RMS %.4f px\n", nkf,
              orch.sfm_registered_frames(), orch.sfm_sparse_points(),
              orch.sfm_ba_rms_px());
  Check(nkf >= 2, "SfM 应注册至少 2 帧");
  Check(orch.keyframe_timestamps_ns().size() ==
            static_cast<size_t>(orch.sfm_registered_frames()),
        "关键帧数应等于 SfM 注册帧数");
  Check(orch.sfm_sparse_points() > 0, "SfM 应产出稀疏点");
  // BA 收敛判据：亚像素级重投影（与 test_sfm 端到端同口径，< 2.0 px）。
  Check(orch.sfm_ba_rms_px() < 2.0, "SfM 最终 BA 重投影 RMS 应达亚像素/像素级");

  recon_destroy(ctx);
}

// ---------------------------------------------------------------------------
// F. 深度内参按深度分辨率换算（docs/02 §3.9.1 C-1 / docs/04 §3.5.3；缺口 L）
//
// **为什么必须有一个「深度分辨率 ≠ RGB 分辨率」的端到端用例**：
//   平台（docs/11 §3.7）上报的是**彩色相机内参**，而 TSDF 积分在**深度
//   原生分辨率**上进行（docs/04 IR-2）。ARKit 的深度图是 256×192，而彩色
//   是 1920×1440 —— 分辨率差 7.5 倍。此前 test_pipeline 的 A 用例把深度
//   与 RGB 设为**同一分辨率（160×160）**，于是内参换算的比例恒为 1.0，
//   任何换算错误都被掩盖（2026-10-31 实测发现的缺口 L）。
//
// 本用例同时锁住旧实现在**纯深度帧**（`rgb_data == nullptr`，C ABI 明确
// 允许）下的静默退化：旧代码的缩放守卫是 `rgb_width > 0`，而纯深度帧的
// `rgb_width` 恒为 0 → 比例停在 1.0 → 深度内参完全不缩放。
//
// 两个判据（诊断出口 + 几何交叉验证）都落在**目标对象**上：
//   ① `StageResult.metrics` 里 TSDF 实际使用的深度内参值；
//   ② 反投影出的点云 X 跨度（内参错则跨度按比例缩放）。
// ---------------------------------------------------------------------------
void TestDepthIntrinsicsRescaledToDepthResolution() {
  std::printf("[TEST] 深度内参按深度分辨率换算（彩色 640×480 → 深度 160×160，纯深度帧）\n");

  recon::SetPlatformProbeProvider(nullptr);

  const int kW = 160;
  const int kH = 160;
  // 深度：一堵平面墙，光轴深度统一 0.5 m。
  std::vector<uint16_t> depth(static_cast<size_t>(kW) * kH, 500);
  std::vector<uint8_t> conf(static_cast<size_t>(kW) * kH, 255);

  recon_config cfg{};
  cfg.abi_version = RECON_ABI_VERSION;
  cfg.forced_branch = RECON_BRANCH_RGBD;
  cfg.enable_mvs = 0;       // 聚焦 TSDF 内参口径
  cfg.enable_texture = 0;
  cfg.enable_rgbd = 1;
  cfg.voxel_size_m = 0.005;

  void* ctx = nullptr;
  Check(recon_create(&cfg, &ctx) == RECON_OK, "create 应成功");
  if (ctx == nullptr) return;
  Check(recon_start_session(ctx, "e2e_depthintr") == RECON_OK, "开始会话应成功");

  // 内参报的是**彩色**分辨率 640×480（与深度 160×160 不同）。
  recon_camera_intrinsics intr{};
  intr.fx = 0.9 * 640.0;   // 576
  intr.fy = 0.9 * 480.0;   // 432
  intr.cx = 320.0;
  intr.cy = 240.0;
  intr.width = 640.0;
  intr.height = 480.0;

  recon_pose pose{};       // identity：相机在原点朝 +Z
  pose.r[0] = 1.0;
  pose.r[4] = 1.0;
  pose.r[8] = 1.0;

  for (int i = 0; i < 3; ++i) {
    recon_frame_packet pkt{};
    pkt.frame_id = static_cast<uint64_t>(i + 1);
    pkt.timestamp_ns = 1000000000ull * (static_cast<uint64_t>(i) + 1);
    pkt.rgb_data = nullptr;        // 关键：纯深度帧（C ABI 允许 rgb_data 为 NULL）
    pkt.rgb_width = 0;
    pkt.rgb_height = 0;
    pkt.rgb_stride = 0;
    pkt.depth_data = reinterpret_cast<const uint8_t*>(depth.data());
    pkt.depth_width = static_cast<uint32_t>(kW);
    pkt.depth_height = static_cast<uint32_t>(kH);
    pkt.depth_stride = static_cast<uint32_t>(kW * 2);
    pkt.depth_confidence = conf.data();
    pkt.depth_source = RECON_DEPTH_SRC_LIDAR;
    pkt.has_depth = 1;
    pkt.intrinsics = &intr;
    pkt.pose = &pose;
    pkt.exposure_us = 8000.0;
    pkt.iso = 100.0;
    Check(recon_push_frame(ctx, &pkt) == RECON_OK, "推纯深度帧应成功");
  }
  Check(recon_finish_capture(ctx) == RECON_OK, "结束采集应成功");

  recon::Orchestrator& orch = static_cast<recon::Context*>(ctx)->orchestrator();
  recon::StageResult res;
  const recon_status st = orch.runStage(RECON_STAGE_RGBD_FUSION, &res);
  Check(st == RECON_OK, "TSDF 融合阶段应成功");

  // --- 判据 ①：TSDF 实际使用的深度内参 --------------------------------
  double fx_used = -1.0;
  double cx_used = -1.0;
  for (const auto& kv : res.metrics) {
    if (kv.first == "tsdf_depth_intrinsics_fx") fx_used = kv.second;
    if (kv.first == "tsdf_depth_intrinsics_cx") cx_used = kv.second;
  }
  // 期望：fx = 576 × (160/640) = 144；
  //       cx = (320 + 0.5) × (160/640) − 0.5 = 79.625。
  std::printf("    TSDF 深度内参：fx=%.6f（期望 144），cx=%.6f（期望 79.625）\n",
              fx_used, cx_used);
  CheckNear(fx_used, 144.0, 1e-6,
            "深度 fx 必须按深度分辨率换算（旧实现/纯深度帧会停留 576）");
  CheckNear(cx_used, 79.625, 1e-6,
            "深度 cx 必须含 +0.5/−0.5 主点补偿");

  // --- 判据 ②：几何交叉验证（点云 X 跨度） ----------------------------
  const recon::UnifiedPointCloud& cloud = orch.cloud();
  Check(cloud.size() > 0, "应有 TSDF 点云产出");
  double xmin = 1e30, xmax = -1e30, zsum = 0.0;
  for (const recon::Vec3d& p : cloud.positions) {
    xmin = std::min(xmin, p(0));
    xmax = std::max(xmax, p(0));
    zsum += p(2);
  }
  const double x_span = xmax - xmin;
  const double z_mean = cloud.size() > 0
                            ? zsum / static_cast<double>(cloud.size())
                            : 0.0;
  std::printf("    点云 %zu 点，X 跨度 %.4f m（期望 ≈0.55），z 均值 %.4f m\n",
              cloud.size(), x_span, z_mean);
  // 正确 fx=144 → dx=(u−79.625)/144 ∈ [−0.553, 0.558] → x 跨度 ≈ 0.55。
  // 错误 fx=576 → 跨度 ≈ 0.14。以 0.30 为界可清晰区分。
  Check(x_span > 0.30,
        "X 跨度应反映正确的深度内参（内参未换算则被压缩数倍）");
  Check(x_span < 1.0, "X 跨度不应异常放大");
  CheckNear(z_mean, 0.5, 0.03, "等值面 z 应接近墙的深度 0.5 m");

  recon_destroy(ctx);
}

}  // namespace

int main() {
  std::printf("=== recon 端到端编排测试 ===\n");
  TestRgbdEndToEnd();
  TestCApiLifecycleEndToEnd();
  TestCancelBeforeRun();
  TestEmptySession();
  TestSfmMonoEndToEnd();
  TestDepthIntrinsicsRescaledToDepthResolution();

  if (g_failures == 0) {
    std::printf("\n全部通过\n");
    return 0;
  }
  std::printf("\n失败 %d 项\n", g_failures);
  return 1;
}
