// SPDX-License-Identifier: BSD-3-Clause
//
// Marching Cubes 边表接口（包装 PoissonRecon 的标准表）。
//
// 契约见 core/src/mesh/poisson_mc.cpp 的说明。

#ifndef RECON_CORE_POISSON_MC_H_
#define RECON_CORE_POISSON_MC_H_

namespace recon {
namespace poisson_mc {

// 立方体角点的局部偏移。corner ∈ [0,8)。
void CornerOffset(int corner, int* dx, int* dy, int* dz);

// 立方体棱的两个端点角点。edge ∈ [0,12)。
void EdgeCorners(int edge, int* c1, int* c2);

// 计算 MC case 索引（0–255）。
int GetIndex(const double values[8], double iso);

// 该 case 下被等值面穿过的棱掩码。
int EdgeMask(int mc_index);

// 该 case 的三角表（3 个一组，−1 结束）。
const int* TriangleTable(int mc_index);

}  // namespace poisson_mc
}  // namespace recon

#endif  // RECON_CORE_POISSON_MC_H_
