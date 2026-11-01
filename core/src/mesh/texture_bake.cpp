// SPDX-License-Identifier: BSD-3-Clause
//
// S9 纹理烘焙。契约：docs/06_网格化与后处理.md §2.9。

#include "recon/texture_bake.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "recon/mesh_repair.h"

namespace recon {
namespace {

// sRGB ↔ 线性。**融合必须在线性空间做**：sRGB 编码是非线性的，
// 直接对 sRGB 值取平均会让暗部偏亮、亮部偏暗（典型表现是接缝处发灰）。
inline double SrgbToLinear(double c) {
  if (c <= 0.04045) return c / 12.92;
  return std::pow((c + 0.055) / 1.055, 2.4);
}

inline double LinearToSrgb(double c) {
  if (c <= 0.0031308) return c * 12.92;
  return 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

inline double Clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

// 双线性采样 RGB8。返回线性空间的三个分量。
bool SampleRgbLinear(const BakeView& view, double u, double v, double* rgb) {
  if (view.rgb == nullptr) return false;
  if (u < 0.0 || v < 0.0) return false;
  const int x0 = static_cast<int>(std::floor(u));
  const int y0 = static_cast<int>(std::floor(v));
  if (x0 < 0 || y0 < 0 || x0 + 1 >= view.width || y0 + 1 >= view.height) {
    return false;
  }
  const double fx = u - static_cast<double>(x0);
  const double fy = v - static_cast<double>(y0);
  double acc[3] = {0.0, 0.0, 0.0};
  for (int dy = 0; dy <= 1; ++dy) {
    for (int dx = 0; dx <= 1; ++dx) {
      const double w = (dx ? fx : 1.0 - fx) * (dy ? fy : 1.0 - fy);
      if (w <= 0.0) continue;
      const size_t off = static_cast<size_t>(y0 + dy) *
                             static_cast<size_t>(view.stride_bytes) +
                         static_cast<size_t>(x0 + dx) * 3u;
      for (int c = 0; c < 3; ++c) {
        acc[c] += w * SrgbToLinear(static_cast<double>(view.rgb[off + c]) / 255.0);
      }
    }
  }
  rgb[0] = acc[0];
  rgb[1] = acc[1];
  rgb[2] = acc[2];
  return true;
}

// 双线性采样深度（米）。
bool SampleDepth(const BakeView& view, double u, double v, double* z) {
  if (view.depth == nullptr) return false;
  const int x0 = static_cast<int>(std::floor(u));
  const int y0 = static_cast<int>(std::floor(v));
  if (x0 < 0 || y0 < 0 || x0 + 1 >= view.width || y0 + 1 >= view.height) {
    return false;
  }
  const double fx = u - static_cast<double>(x0);
  const double fy = v - static_cast<double>(y0);
  const size_t w = static_cast<size_t>(view.width);
  const double z00 = view.depth[static_cast<size_t>(y0) * w + x0];
  const double z10 = view.depth[static_cast<size_t>(y0) * w + x0 + 1];
  const double z01 = view.depth[static_cast<size_t>(y0 + 1) * w + x0];
  const double z11 = view.depth[static_cast<size_t>(y0 + 1) * w + x0 + 1];
  if (!(z00 > 0.0) || !(z10 > 0.0) || !(z01 > 0.0) || !(z11 > 0.0)) return false;
  *z = (1.0 - fx) * (1.0 - fy) * z00 + fx * (1.0 - fy) * z10 +
       (1.0 - fx) * fy * z01 + fx * fy * z11;
  return true;
}

// 三角形内的重心坐标。返回 false 表示点在三角形外或三角形退化。
bool Barycentric(double x0, double y0, double x1, double y1, double x2,
                 double y2, double px, double py, double* w0, double* w1,
                 double* w2) {
  const double d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
  if (std::fabs(d) < 1e-12) return false;
  const double a = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / d;
  const double b = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / d;
  const double c = 1.0 - a - b;
  constexpr double kEps = -1e-6;
  if (a < kEps || b < kEps || c < kEps) return false;
  *w0 = a;
  *w1 = b;
  *w2 = c;
  return true;
}

}  // namespace

BakeResult BakeTexture(const TriangleMesh& mesh, const UvAtlas& atlas,
                       const std::vector<BakeView>& views,
                       const BakeConfig& config) {
  BakeResult out;
  if (!atlas.ok || atlas.uvs.empty() || atlas.indices.empty()) return out;
  if (views.empty()) return out;

  const int size = std::max(1, config.texture_size);
  out.width = size;
  out.height = size;
  const size_t texels = static_cast<size_t>(size) * static_cast<size_t>(size);

  // 累加缓冲。**注意内存**：4096² 时 rgb 累加器是 4096·4096·3·4B = 192 MB，
  // 权重 64 MB —— 移动端必须先分块（见 docs/06 §2.9 的工程约束）。
  std::vector<float> accum_rgb(texels * 3, 0.0f);
  std::vector<float> accum_w(texels, 0.0f);
  std::vector<uint16_t> accum_views(texels, 0);

  // 目标像素足迹（米/像素）。未指定时用「所有视图在典型距离上的足迹中位数」。
  double target_footprint = config.target_footprint;
  if (target_footprint <= 0.0) {
    std::vector<double> fps;
    fps.reserve(views.size());
    for (const auto& v : views) {
      if (v.fx > 0.0) fps.push_back(1.0 / v.fx);  // 单位距离处的足迹
    }
    if (!fps.empty()) {
      std::sort(fps.begin(), fps.end());
      target_footprint = fps[fps.size() / 2];
    } else {
      target_footprint = 1e-3;
    }
  }

  // 相机中心：p_cam = R·x + t ⇒ 中心 C 满足 R·C + t = 0 ⇒ C = -Rᵀ·t。
  std::vector<Vec3d> centers(views.size());
  for (size_t i = 0; i < views.size(); ++i) {
    centers[i] = -(views[i].rotation.transpose() * views[i].translation);
  }

  // ---- 逐三角形光栅化 UV 并逐 texel 融合 ----
  const double px_scale = static_cast<double>(size);
  for (size_t t = 0; t < atlas.indices.size() / 3; ++t) {
    const uint32_t i0 = atlas.indices[t * 3 + 0];
    const uint32_t i1 = atlas.indices[t * 3 + 1];
    const uint32_t i2 = atlas.indices[t * 3 + 2];
    if (i0 >= atlas.uvs.size() || i1 >= atlas.uvs.size() ||
        i2 >= atlas.uvs.size()) {
      continue;
    }
    if (i0 >= atlas.source_vertex.size() || i1 >= atlas.source_vertex.size() ||
        i2 >= atlas.source_vertex.size()) {
      continue;
    }
    const Vec3d& p0 = mesh.positions[atlas.source_vertex[i0]];
    const Vec3d& p1 = mesh.positions[atlas.source_vertex[i1]];
    const Vec3d& p2 = mesh.positions[atlas.source_vertex[i2]];
    Vec3d n = (p1 - p0).cross(p2 - p0);
    if (n.norm() < 1e-20) continue;
    n.normalize();

    // UV → 像素。约定：**v = 0 对应纹理第 0 行**（与 glTF 一致，v 向下）。
    const double ux0 = atlas.uvs[i0](0) * px_scale;
    const double uy0 = atlas.uvs[i0](1) * px_scale;
    const double ux1 = atlas.uvs[i1](0) * px_scale;
    const double uy1 = atlas.uvs[i1](1) * px_scale;
    const double ux2 = atlas.uvs[i2](0) * px_scale;
    const double uy2 = atlas.uvs[i2](1) * px_scale;

    int min_x = static_cast<int>(std::floor(std::min(ux0, std::min(ux1, ux2))));
    int max_x = static_cast<int>(std::ceil(std::max(ux0, std::max(ux1, ux2))));
    int min_y = static_cast<int>(std::floor(std::min(uy0, std::min(uy1, uy2))));
    int max_y = static_cast<int>(std::ceil(std::max(uy0, std::max(uy1, uy2))));
    min_x = std::max(0, min_x);
    min_y = std::max(0, min_y);
    max_x = std::min(size - 1, max_x);
    max_y = std::min(size - 1, max_y);

    for (int py = min_y; py <= max_y; ++py) {
      for (int px = min_x; px <= max_x; ++px) {
        double w0 = 0.0, w1 = 0.0, w2 = 0.0;
        if (!Barycentric(ux0, uy0, ux1, uy1, ux2, uy2,
                         static_cast<double>(px) + 0.5,
                         static_cast<double>(py) + 0.5, &w0, &w1, &w2)) {
          continue;
        }
        const Vec3d x = p0 * w0 + p1 * w1 + p2 * w2;

        double sum_w = 0.0;
        double sum_rgb[3] = {0.0, 0.0, 0.0};
        int used_views = 0;

        for (size_t vi = 0; vi < views.size(); ++vi) {
          const BakeView& view = views[vi];
          if (view.rgb == nullptr || view.fx <= 0.0 || view.fy <= 0.0) continue;

          // 入射角权重：法向与「指向相机」方向的夹角。
          const Vec3d to_cam = centers[vi] - x;
          const double dist = to_cam.norm();
          if (dist < 1e-12) continue;
          const double cos_ang = n.dot(to_cam / dist);
          if (cos_ang <= 0.0) continue;  // 背向
          const double w_ang = std::pow(cos_ang, config.alpha);

          // 投影。
          const Vec3d pc = view.rotation * x + view.translation;
          if (pc(2) <= 1e-9) continue;  // 在相机后方
          const double u = view.fx * pc(0) / pc(2) + view.cx;
          const double v = view.fy * pc(1) / pc(2) + view.cy;
          if (u < 0.0 || v < 0.0 || u >= view.width - 1 ||
              v >= view.height - 1) {
            continue;
          }
          // 该处的像素足迹（米/像素）。
          const double footprint = pc(2) / view.fx;
          if (!(footprint > 0.0)) continue;

          // 深度一致性：**软权重，不硬剔除**。
          //
          // 早期实现用「diff > tau 就 continue」—— 一票否决。这在真实数据上
          // 太脆：网格与深度图只要有几个像素的系统性偏差（多视图融合的
          // 深度噪声、网格三角化、亚像素配准），整块区域就会被全部剔除，
          // 覆盖率归零。
          //
          // 改成高斯型软权重：diff = 0 → 1，diff = σ → 0.61，diff = 3σ → 0.011。
          // 单调、连续、有界，遮挡点自然被压到接近 0 而不会被误当成有效样本。
          double w_res = 1.0;
          if (view.depth != nullptr) {
            double z_ref = 0.0;
            if (SampleDepth(view, u, v, &z_ref)) {
              const double sigma = config.tau_vis_mult * footprint;
              if (sigma > 0.0) {
                const double r = (pc(2) - z_ref) / sigma;
                w_res = std::exp(-0.5 * r * r);
              }
            } else {
              // 深度图该处无有效值（物体外/空洞）→ 无法验证遮挡，保守降权。
              w_res = 0.25;
            }
          }

          // 分辨率匹配：足迹比目标更粗的视图降权；比目标更细不额外加分。
          const double ratio = target_footprint / footprint;
          const double w_area = Clamp01(ratio * ratio);

          const double w_blur = 1.0 - Clamp01(view.motion_blur);
          const double w = w_ang * w_res * w_area * w_blur;
          if (w <= 0.0) continue;

          double linear[3] = {0.0, 0.0, 0.0};
          if (!SampleRgbLinear(view, u, v, linear)) continue;

          sum_w += w;
          for (int c = 0; c < 3; ++c) sum_rgb[c] += w * linear[c];
          ++used_views;
        }

        if (sum_w <= 0.0) continue;
        const size_t idx = static_cast<size_t>(py) * static_cast<size_t>(size) +
                           static_cast<size_t>(px);
        for (int c = 0; c < 3; ++c) {
          accum_rgb[idx * 3 + c] += static_cast<float>(sum_rgb[c]);
        }
        accum_w[idx] += static_cast<float>(sum_w);
        if (accum_views[idx] < 0xFFFF) {
          accum_views[idx] = static_cast<uint16_t>(
              accum_views[idx] + static_cast<uint16_t>(used_views));
        }
      }
    }
  }

  // ---- 归一化 + 转回 sRGB ----
  out.rgb.assign(texels * 3, 0);
  out.weight_sum.assign(texels, 0.0f);
  size_t covered = 0;
  double view_sum = 0.0;
  for (size_t i = 0; i < texels; ++i) {
    const double w = accum_w[i];
    out.weight_sum[i] = static_cast<float>(w);
    if (w <= config.min_weight_sum) continue;
    ++covered;
    view_sum += accum_views[i];
    for (int c = 0; c < 3; ++c) {
      double linear = accum_rgb[i * 3 + c] / w;
      linear = Clamp01(linear);
      const double srgb = config.linear_space ? LinearToSrgb(linear) : linear;
      out.rgb[i * 3 + c] = static_cast<uint8_t>(
          std::min(255.0, std::max(0.0, srgb * 255.0 + 0.5)));
    }
  }
  out.covered_ratio =
      texels > 0 ? static_cast<double>(covered) / static_cast<double>(texels)
                 : 0.0;
  out.mean_views_per_texel =
      covered > 0 ? view_sum / static_cast<double>(covered) : 0.0;

  // ---- 空洞填充：把已覆盖 texel 的颜色向邻域扩散 ----
  //
  // chart 内部的微小空洞（三角形之间没被 texel 中心命中的缝隙）必须填掉，
  // 否则采样时会出现黑点。用「逐轮膨胀」而不是最近邻搜索：简单、无额外依赖。
  if (config.dilate_pixels > 0) {
    std::vector<float> val(texels * 3, 0.0f);
    std::vector<uint8_t> filled(texels, 0);
    for (size_t i = 0; i < texels; ++i) {
      if (accum_w[i] > config.min_weight_sum) {
        filled[i] = 1;
        for (int c = 0; c < 3; ++c) {
          val[i * 3 + c] = static_cast<float>(out.rgb[i * 3 + c]);
        }
      }
    }
    for (int round = 0; round < config.dilate_pixels; ++round) {
      std::vector<uint8_t> next = filled;
      std::vector<float> next_val = val;
      bool changed = false;
      for (int py = 0; py < size; ++py) {
        for (int px = 0; px < size; ++px) {
          const size_t i = static_cast<size_t>(py) * static_cast<size_t>(size) +
                           static_cast<size_t>(px);
          if (filled[i]) continue;
          float acc[3] = {0.0f, 0.0f, 0.0f};
          int cnt = 0;
          for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
              if (dx == 0 && dy == 0) continue;
              const int nx = px + dx;
              const int ny = py + dy;
              if (nx < 0 || ny < 0 || nx >= size || ny >= size) continue;
              const size_t ni = static_cast<size_t>(ny) *
                                    static_cast<size_t>(size) +
                                static_cast<size_t>(nx);
              if (!filled[ni]) continue;
              for (int c = 0; c < 3; ++c) acc[c] += val[ni * 3 + c];
              ++cnt;
            }
          }
          if (cnt == 0) continue;
          for (int c = 0; c < 3; ++c) {
            next_val[i * 3 + c] =
                static_cast<float>(acc[c] / static_cast<float>(cnt));
          }
          next[i] = 1;
          changed = true;
        }
      }
      val.swap(next_val);
      filled.swap(next);
      if (!changed) break;
    }
    size_t unfilled = 0;
    for (size_t i = 0; i < texels; ++i) {
      if (filled[i]) {
        for (int c = 0; c < 3; ++c) {
          out.rgb[i * 3 + c] = static_cast<uint8_t>(
              std::min(255.0f, std::max(0.0f, val[i * 3 + c])));
        }
      } else {
        ++unfilled;
      }
    }
    out.unfilled_texels = unfilled;
  } else {
    for (size_t i = 0; i < texels; ++i) {
      if (accum_w[i] <= config.min_weight_sum) ++out.unfilled_texels;
    }
  }

  out.ok = true;
  return out;
}

}  // namespace recon
