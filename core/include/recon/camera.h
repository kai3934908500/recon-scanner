// SPDX-License-Identifier: BSD-3-Clause
//
// 针孔相机模型与投影几何。
//
// 契约：docs/05_单目管线.md §5.2（视图选择）、§5.3.3（代价函数中的投影）。
//
// 精度要点：
//   1. 投影/反投影必须与畸变模型**严格互逆**。若投影带畸变而反投影不带，
//      三角化会引入系统性偏差（边缘区可达 0.15 mm，见 05 §5.1.3）。
//      因此这里实现「迭代反投影」：带畸变 → 无畸变 → 迭代收敛。
//   2. 所有距离单位为米，像素单位为像素。
//   3. 深度图沿**光轴**投影（ARCore/ARKit 口径），与 04 文档一致。

#ifndef RECON_CORE_CAMERA_H_
#define RECON_CORE_CAMERA_H_

#include <cmath>

#include "recon/linalg.h"

namespace recon {

// 相机内参 + 畸变系数。
struct CameraIntrinsics {
  double fx = 1.0;
  double fy = 1.0;
  double cx = 0.0;
  double cy = 0.0;

  // Brown-Conrady 畸变（OpenCV 顺序）。
  double k1 = 0.0;
  double k2 = 0.0;
  double k3 = 0.0;
  double p1 = 0.0;
  double p2 = 0.0;

  int width = 0;
  int height = 0;

  // 归一化坐标 → 畸变归一化坐标。
  void ApplyDistortion(double x, double y, double* out_x, double* out_y) const;

  // 畸变归一化坐标 → 归一化坐标（迭代求解，5 次迭代足够）。
  void RemoveDistortion(double x_d, double y_d, double* out_x,
                         double* out_y) const;

  // 像素坐标 → 归一化坐标（已去畸变）。
  void PixelToNormalized(double u, double v, double* x, double* y) const;

  // 归一化坐标 → 像素坐标（**不含**畸变，用于生成无畸变图）。
  void NormalizedToPixel(double x, double y, double* u, double* v) const;

  // 像素坐标 → 相机系方向向量（已去畸变），z = 1 归一化。
  void PixelToRay(double u, double v, Vec3d* ray) const;

  // 相机系点 → 像素坐标。返回 false 表示投影在相机后方。
  bool ProjectPoint(const Vec3d& p_cam, double* u, double* v) const;

  // 相机系点 → 光轴深度（ARCore/ARKit 口径的深度图值）。
  double AxialDepth(const Vec3d& p_cam) const { return p_cam(2); }
};

// 相机位姿。世界→相机。
struct CameraPose {
  Mat3d R = Mat3d::Identity();  // world → camera 旋转
  Vec3d t = Vec3d::Zero();      // world → camera 平移（米）

  // 相机系 → 世界系。
  Vec3d CameraToWorld(const Vec3d& p_cam) const {
    return R.transpose() * (p_cam - t);
  }
  // 世界系 → 相机系。
  Vec3d WorldToCamera(const Vec3d& p_world) const {
    return R * p_world + t;
  }
  // 相机光心在世界系的位置：t_cw = −Rᵀ t
  Vec3d CenterWorld() const { return -(R.transpose() * t); }
};

// 基线向量（从参考帧到源帧，单位米）。
inline Vec3d Baseline(const CameraPose& ref, const CameraPose& src) {
  return src.CenterWorld() - ref.CenterWorld();
}

// 视图间的相对旋转（源帧坐标系下的参考帧旋转）。
inline Mat3d RelativeRotation(const CameraPose& ref, const CameraPose& src) {
  return src.R * ref.R.transpose();
}

// 内参按「源分辨率 → 目标分辨率」缩放。
//
// 契约：docs/02_采集与同步.md §3.9.1 C-1、docs/04_RGBD管线.md §3.5.3。
//
//   fx' = fx · (W_dst / W_src),  fy' = fy · (H_dst / H_src)
//   cx' = (cx + 0.5) · (W_dst / W_src) − 0.5
//   cy' = (cy + 0.5) · (H_dst / H_src) − 0.5
//
// **三条不可省的正确性要点**（都曾在别处被漏掉，故集中在此一处实现）：
//   ① **主点必须做 +0.5/−0.5 补偿**。像素中心的坐标约定是「索引 + 0.5」，
//      纯比例缩放主点会引入 0.5 px 系统偏差。C-1 第 921 行明文要求此项，
//      docs/04 §3.5.3 进一步注明它「恰好是 00 §4.6 验收线的量级」。
//   ② **x 与 y 各自的比例**（W_dst/W_src 与 H_dst/H_src），不得复用一个标量
//      —— 宽高比不同时（如 4:3 深度 → 16:9 RGB）单标量必然错。
//   ③ **源尺寸必须来自内参自身**（`intrinsics.width/height`），不得从
//      RGB 缓冲尺寸间接推断 —— 后者在 depth-only 帧（`rgb_data == nullptr`，
//      C ABI 明确允许）下不可用，会静默退化为「不缩放」。
//
// 若 `intrinsics` 尺寸非正或目标尺寸非正，返回**原内参不变**（调用方负责
// 判断该情形是否合法；本函数不做静默猜测）。
inline CameraIntrinsics ScaleIntrinsicsToResolution(
    const CameraIntrinsics& k, int dst_width, int dst_height) {
  CameraIntrinsics out = k;
  if (k.width <= 0 || k.height <= 0 || dst_width <= 0 || dst_height <= 0) {
    return out;
  }
  if (dst_width == k.width && dst_height == k.height) {
    out.width = dst_width;
    out.height = dst_height;
    return out;
  }
  const double sx = static_cast<double>(dst_width) / static_cast<double>(k.width);
  const double sy = static_cast<double>(dst_height) / static_cast<double>(k.height);

  out.fx = k.fx * sx;
  out.fy = k.fy * sy;
  // 主点补偿：+0.5 → 缩放 → −0.5（见上方要点 ①）。
  out.cx = (k.cx + 0.5) * sx - 0.5;
  out.cy = (k.cy + 0.5) * sy - 0.5;
  out.width = dst_width;
  out.height = dst_height;
  return out;
}

}  // namespace recon

#endif  // RECON_CORE_CAMERA_H_
