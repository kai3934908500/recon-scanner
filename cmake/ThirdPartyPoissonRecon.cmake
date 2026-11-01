# PoissonRecon (MIT) —— 泊松曲面重建
#
# 来源：https://github.com/mkazhdan/PoissonRecon （V7.0 标签）
# 许可：MIT（见同目录 LICENSE）
#
# 集成方式：编译为静态库，只暴露重建所需的核心模块
# （Octree / BSplineData / MarchingCubes / MultiGridOctreeData / SparseMatrix）。
# 不编译上游的 CLI 工具（PoissonRecon / ChunkPLY / ScaleNormals 等），
# 它们依赖 Ply / 命令行解析，且本项目不需要。

set(POISSON_RECON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/poisson_recon")

# 注意：**不**把上游 .cpp 编译成独立静态库。
#
# 上游把 Polynomial / aligned_malloc / hash_map 等符号放在**全局命名空间**，
# 单独成库会让这些符号泄漏到最终可执行文件，与本项目符号冲突
#（实测链接期 `multiple definition of Polynomial<2>::operator()`）。
#
# 解决：把上游源码由 core/src/mesh/poisson_mc.cpp **在命名空间内直接 include**，
# 符号全部落在 poisson_upstream 里，不外泄。
# 因此本文件只提供 include 路径与编译选项。

add_library(poisson_recon INTERFACE)

target_include_directories(poisson_recon INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/poisson_recon")

# 上游代码使用大量可变参数宏与 GNU 扩展，按上游要求放宽警告。
if(MSVC)
  target_compile_options(poisson_recon INTERFACE /W0)
  target_compile_definitions(poisson_recon INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX
      _USE_MATH_DEFINES)
else()
  target_compile_options(poisson_recon INTERFACE -w)
endif()

# **强制预包含兼容垫片**。
# 垫片提供：
#   · 标准库符号（malloc / printf / memset 等上游隐式依赖的）
#   · DIMENSION 宏（上游定义在 Octree.h，但 PointStream.inl 需要）
#   · 基础头文件的预包含（上游多处依赖包含顺序）
# Hash.h 的 hash_map 已按 PATCHES.md 的 P3 直接改为标准实现。
if(MSVC)
  target_compile_options(poisson_recon INTERFACE /FI"${CMAKE_CURRENT_SOURCE_DIR}/third_party/poisson_recon/recon_hash_compat.h")
else()
  target_compile_options(poisson_recon INTERFACE -include "${CMAKE_CURRENT_SOURCE_DIR}/third_party/poisson_recon/recon_hash_compat.h")
endif()

# 上游使用 OpenMP 做多线程（若可用则启用）。
find_package(OpenMP QUIET)
if(OpenMP_CXX_FOUND)
  target_link_libraries(poisson_recon INTERFACE OpenMP::OpenMP_CXX)
  target_compile_definitions(poisson_recon INTERFACE _OPENMP)
endif()
