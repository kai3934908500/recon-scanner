// SPDX-License-Identifier: BSD-3-Clause
//
// TSDF 融合。
//
// 契约：docs/04_RGBD管线.md §4。
//
// 加权 TSDF 更新（Kurz & Rusinkiewicz 扩展形式）：
//
//   sdf(p) = depth(p) − D(p)
//   w(p)   = 1 / σ_ε(p)²
//
//   TSDF(p) = (w_old·tsdf_old + w_new·sdf) / (w_old + w_new)
//   W(p)    = w_old + w_new
//
// tsdf 为归一化到 [−1,1] 的截断 SDF：tsdf = sdf / truncation。
//
// 精度铁律（docs/00 §1.2 黑名单）：
//   深度图不降采样；低置信区域降权但不丢弃；体素边长不因速度降低。

#include "recon/tsdf_fusion.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "tsdf_internal.h"

namespace recon {

TsdfVolume::TsdfVolume(const TsdfConfig& config) : config_(config) {
  const float origin_x = static_cast<float>(config.origin_x);
  const float origin_y = static_cast<float>(config.origin_y);
  const float origin_z = static_cast<float>(config.origin_z);
  const float voxel = static_cast<float>(config.voxel_size_m);

  const int nx = std::max(
      1, tsdf_internal::FloorToIntScaled(
             static_cast<float>(config.bbox_max_x - origin_x), voxel) + 1);
  const int ny = std::max(
      1, tsdf_internal::FloorToIntScaled(
             static_cast<float>(config.bbox_max_y - origin_y), voxel) + 1);
  const int nz = std::max(
      1, tsdf_internal::FloorToIntScaled(
             static_cast<float>(config.bbox_max_z - origin_z), voxel) + 1);

  dims_[0] = nx;
  dims_[1] = ny;
  dims_[2] = nz;
  const size_t total = static_cast<size_t>(nx) * ny * nz;

  tsdf_.assign(total, 1.0f);
  weight_.assign(total, 0.0f);
  color_.assign(total * 3, 0.0f);

  origin_ = {origin_x, origin_y, origin_z};
  voxel_size_ = voxel;
}

size_t TsdfVolume::Index(int ix, int iy, int iz) const {
  return (static_cast<size_t>(iz) * dims_[1] + iy) * dims_[0] + ix;
}

bool TsdfVolume::InBounds(int ix, int iy, int iz) const {
  return ix >= 0 && iy >= 0 && iz >= 0 && ix < dims_[0] && iy < dims_[1] &&
         iz < dims_[2];
}

void TsdfVolume::WorldToVoxel(float wx, float wy, float wz, int* ix, int* iy,
                              int* iz) const {
  *ix = tsdf_internal::FloorToIntScaled(wx - origin_(0), voxel_size_);
  *iy = tsdf_internal::FloorToIntScaled(wy - origin_(1), voxel_size_);
  *iz = tsdf_internal::FloorToIntScaled(wz - origin_(2), voxel_size_);
}

void TsdfVolume::VoxelToWorld(int ix, int iy, int iz, float* wx, float* wy,
                              float* wz) const {
  *wx = origin_(0) + (static_cast<float>(ix) + 0.5f) * voxel_size_;
  *wy = origin_(1) + (static_cast<float>(iy) + 0.5f) * voxel_size_;
  *wz = origin_(2) + (static_cast<float>(iz) + 0.5f) * voxel_size_;
}

uint32_t TsdfVolume::IntegrateDepthFrame(
    const uint16_t* depth_mm, const uint8_t* confidence, int width, int height,
    const Mat4d& world_to_camera, float depth_uncertainty_m) {
  return tsdf_internal::IntegrateFrame(
      config_, origin_, voxel_size_, depth_mm, confidence, width, height,
      world_to_camera, depth_uncertainty_m,
      [this](int ix, int iy, int iz) { return InBounds(ix, iy, iz); },
      [this](int ix, int iy, int iz, float sdf, float w_new) {
        const size_t idx = Index(ix, iy, iz);
        const float w_old = weight_[idx];
        const float w_sum = w_old + w_new;
        if (w_sum > 0.0f) {
          tsdf_[idx] = (w_old * tsdf_[idx] + w_new * sdf) / w_sum;
          weight_[idx] = w_sum;
        }
      });
}

void TsdfVolume::ExtractPoints(std::vector<Vec3d>* out_positions,
                               std::vector<Vec3d>* out_normals,
                               std::vector<float>* out_confidence) const {
  if (out_positions == nullptr) return;

  for (int iz = 0; iz < dims_[2]; ++iz) {
    for (int iy = 0; iy < dims_[1]; ++iy) {
      for (int ix = 0; ix < dims_[0]; ++ix) {
        float nx = 0.0f, ny = 0.0f, nz = 0.0f, len = 0.0f;
        const bool is_surface = tsdf_internal::SurfaceGradient(
            [this](int a, int b, int c, float* t, float* w) {
              if (!InBounds(a, b, c)) return false;
              const size_t j = Index(a, b, c);
              *t = tsdf_[j];
              *w = weight_[j];
              return true;
            },
            ix, iy, iz, &nx, &ny, &nz, &len);
        if (!is_surface) continue;

        float wx, wy, wz;
        VoxelToWorld(ix, iy, iz, &wx, &wy, &wz);
        out_positions->emplace_back(wx, wy, wz);
        if (out_normals != nullptr) {
          out_normals->emplace_back(nx, ny, nz);
        }
        if (out_confidence != nullptr) {
          out_confidence->push_back(std::min(1.0f, weight_[Index(ix, iy, iz)] / 64.0f));
        }
      }
    }
  }
}

void TsdfVolume::MemoryUsageBytes(uint64_t* out_bytes) const {
  if (out_bytes == nullptr) return;
  const size_t n = tsdf_.size();
  *out_bytes = static_cast<uint64_t>(n) * (sizeof(float) * 2 + sizeof(float) * 3);
}

// ===========================================================================
// 分块 TSDF 体素网格（docs/13 §3.2.1）。
// ===========================================================================

TsdfTiledVolume::TsdfTiledVolume(const TsdfConfig& config, int block_voxels)
    : config_(config) {
  const float origin_x = static_cast<float>(config.origin_x);
  const float origin_y = static_cast<float>(config.origin_y);
  const float origin_z = static_cast<float>(config.origin_z);
  const float voxel = static_cast<float>(config.voxel_size_m);

  dims_[0] = std::max(
      1, tsdf_internal::FloorToIntScaled(
             static_cast<float>(config.bbox_max_x - origin_x), voxel) + 1);
  dims_[1] = std::max(
      1, tsdf_internal::FloorToIntScaled(
             static_cast<float>(config.bbox_max_y - origin_y), voxel) + 1);
  dims_[2] = std::max(
      1, tsdf_internal::FloorToIntScaled(
             static_cast<float>(config.bbox_max_z - origin_z), voxel) + 1);

  block_voxels_ = std::max(1, block_voxels);
  origin_ = {origin_x, origin_y, origin_z};
  voxel_size_ = voxel;
}

TsdfTiledVolume::Block* TsdfTiledVolume::TouchBlock(int gx, int gy, int gz) {
  const int bx = tsdf_internal::FloorDiv(gx, block_voxels_);
  const int by = tsdf_internal::FloorDiv(gy, block_voxels_);
  const int bz = tsdf_internal::FloorDiv(gz, block_voxels_);

  // 线性查找（块数受内存预算约束，规模有限；如需可换哈希）。
  for (auto& b : blocks_) {
    if (b.bx == bx && b.by == by && b.bz == bz) return &b;
  }

  Block blk;
  blk.bx = bx;
  blk.by = by;
  blk.bz = bz;
  blk.x0 = bx * block_voxels_;
  blk.y0 = by * block_voxels_;
  blk.z0 = bz * block_voxels_;
  // 本块在全局网格内的实际跨度（末块可能不足 block_voxels_）。
  blk.nx = std::max(0, std::min(block_voxels_, dims_[0] - blk.x0));
  blk.ny = std::max(0, std::min(block_voxels_, dims_[1] - blk.y0));
  blk.nz = std::max(0, std::min(block_voxels_, dims_[2] - blk.z0));
  if (blk.nx <= 0 || blk.ny <= 0 || blk.nz <= 0) return nullptr;

  const size_t total = static_cast<size_t>(blk.nx) * blk.ny * blk.nz;
  blk.tsdf.assign(total, 1.0f);
  blk.weight.assign(total, 0.0f);
  blocks_.push_back(std::move(blk));
  return &blocks_.back();
}

const TsdfTiledVolume::Block* TsdfTiledVolume::FindBlock(int gx, int gy,
                                                         int gz) const {
  if (gx < 0 || gy < 0 || gz < 0 || gx >= dims_[0] || gy >= dims_[1] ||
      gz >= dims_[2]) {
    return nullptr;
  }
  const int bx = tsdf_internal::FloorDiv(gx, block_voxels_);
  const int by = tsdf_internal::FloorDiv(gy, block_voxels_);
  const int bz = tsdf_internal::FloorDiv(gz, block_voxels_);
  for (const auto& b : blocks_) {
    if (b.bx == bx && b.by == by && b.bz == bz) return &b;
  }
  return nullptr;
}

uint32_t TsdfTiledVolume::IntegrateDepthFrame(
    const uint16_t* depth_mm, const uint8_t* confidence, int width, int height,
    const Mat4d& world_to_camera, float depth_uncertainty_m) {
  auto in_bounds = [this](int ix, int iy, int iz) {
    return ix >= 0 && iy >= 0 && iz >= 0 && ix < dims_[0] && iy < dims_[1] &&
           iz < dims_[2];
  };
  return tsdf_internal::IntegrateFrame(
      config_, origin_, voxel_size_, depth_mm, confidence, width, height,
      world_to_camera, depth_uncertainty_m, in_bounds,
      [this](int ix, int iy, int iz, float sdf, float w_new) {
        Block* b = TouchBlock(ix, iy, iz);
        if (b == nullptr) return;
        const int lx = ix - b->x0;
        const int ly = iy - b->y0;
        const int lz = iz - b->z0;
        const size_t idx =
            (static_cast<size_t>(lz) * b->ny + ly) * b->nx + lx;
        const float w_old = b->weight[idx];
        const float w_sum = w_old + w_new;
        if (w_sum > 0.0f) {
          b->tsdf[idx] = (w_old * b->tsdf[idx] + w_new * sdf) / w_sum;
          b->weight[idx] = w_sum;
        }
      });
}

void TsdfTiledVolume::ExtractPoints(
    std::vector<Vec3d>* out_positions, std::vector<Vec3d>* out_normals,
    std::vector<float>* out_confidence) const {
  if (out_positions == nullptr) return;

  // 按全局索引升序遍历（与 TsdfVolume::ExtractPoints 的输出顺序一致）。
  auto get = [this](int a, int b, int c, float* t, float* w) {
    const Block* blk = FindBlock(a, b, c);
    if (blk == nullptr) return false;
    const int lx = a - blk->x0;
    const int ly = b - blk->y0;
    const int lz = c - blk->z0;
    const size_t idx = (static_cast<size_t>(lz) * blk->ny + ly) * blk->nx + lx;
    *t = blk->tsdf[idx];
    *w = blk->weight[idx];
    return true;
  };

  for (int iz = 0; iz < dims_[2]; ++iz) {
    for (int iy = 0; iy < dims_[1]; ++iy) {
      for (int ix = 0; ix < dims_[0]; ++ix) {
        float nx = 0.0f, ny = 0.0f, nz = 0.0f, len = 0.0f;
        if (!tsdf_internal::SurfaceGradient(get, ix, iy, iz, &nx, &ny, &nz,
                                            &len)) {
          continue;
        }
        const float wx =
            static_cast<float>(origin_(0)) + (static_cast<float>(ix) + 0.5f) * voxel_size_;
        const float wy =
            static_cast<float>(origin_(1)) + (static_cast<float>(iy) + 0.5f) * voxel_size_;
        const float wz =
            static_cast<float>(origin_(2)) + (static_cast<float>(iz) + 0.5f) * voxel_size_;
        out_positions->emplace_back(wx, wy, wz);
        if (out_normals != nullptr) out_normals->emplace_back(nx, ny, nz);
        if (out_confidence != nullptr) {
          float t = 0.0f, w = 0.0f;
          get(ix, iy, iz, &t, &w);
          out_confidence->push_back(std::min(1.0f, w / 64.0f));
        }
      }
    }
  }
}

void TsdfTiledVolume::MemoryUsageBytes(uint64_t* out_bytes) const {
  if (out_bytes == nullptr) return;
  uint64_t total = 0;
  for (const auto& b : blocks_) {
    total += static_cast<uint64_t>(b.tsdf.size() + b.weight.size()) *
             sizeof(float);
  }
  *out_bytes = total;
}

}  // namespace recon
