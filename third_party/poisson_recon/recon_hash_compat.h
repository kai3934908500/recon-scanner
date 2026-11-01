// SPDX-License-Identifier: BSD-3-Clause
//
// 哈希容器兼容性垫片（**集成层新增，不修改上游源码**）。
//
// 背景：PoissonRecon 用 `#ifdef WIN32` 切换两种 hash_map：
//   WIN32  → <hash_map> + stdext::hash_map      （VC++ 专有，MinGW 无此头）
//   否则  → <ext/hash_map> + __gnu_cxx::hash_map （GCC 废弃扩展，新版已移除）
//
// 两条路在现代工具链上都会失败。本垫片把三种写法统一映射到
// 标准 C++11 的 std::unordered_map，行为等价且可移植。
//
// 使用方式：在包含任何上游头文件**之前**包含本文件
// （由 CMake 的 -include / /FI 强制注入）。
//
// 许可：MIT，与上游一致。

#ifndef RECON_POISSON_HASH_COMPAT_H_
#define RECON_POISSON_HASH_COMPAT_H_

// 上游多个头文件使用 malloc / free / printf / memset 等 C 标准库函数，
// 依赖被间接引入的 <cstdlib> / <cstdio> / <cstring>。
// 这里显式包含，保证任何包含顺序下都可用。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>

// hash_map 的兼容处理已移入 Hash.h 本身（见 PATCHES.md 的 P3），
// 本垫片只负责标准库符号与隐式包含顺序。

// ---- 上游的隐式包含顺序依赖 ----
//
// 上游多个头文件使用了其它头文件定义的类型，但**没有** #include 它们，
// 依赖的是 PoissonRecon.cpp 里的包含顺序：
//   · BSplineData.h → BinaryNode.h
//   · Ply.h         → Array.h
//   · PointStream.inl → Vector.h
//   · Geometry.h    → FunctionData.h / MAT.h
//
// 我们的集成层不按上游那个顺序包含，因此在这里按**拓扑顺序**
// 预包含这些基础头文件，恢复上游依赖的隐式顺序。
// 这些头文件自身依赖少，先包含它们不影响其它头文件。
#include "Array.h"
#include "BinaryNode.h"
#include "FunctionData.h"
#include "MAT.h"
#include "Polynomial.h"
#include "Vector.h"

// `DIMENSION` 宏在上游定义于 Octree.h（值为 3），
// 但 PointStream.inl 需要它却不 include Octree.h。
// 这里按上游同值定义（带 #ifndef 保护，不覆盖已定义值）。
#ifndef DIMENSION
#define DIMENSION 3
#endif

#endif  // RECON_POISSON_HASH_COMPAT_H_
