cmake_minimum_required(VERSION 3.22)
project(recon VERSION 0.1.0 LANGUAGES C CXX)

# SPDX-License-Identifier: BSD-3-Clause
#
# 顶层构建。
#
# 许可合规硬门禁：本项目仅接受 MIT / Apache-2.0 / BSD 系依赖。
# Eigen 为 MPL-2.0（文件级 copyleft，非传染），列为白名单边界项 B-1，
# 处置要求见 docs/10_许可与合规.md §4。
# 任何 GPL / AGPL / LGPL / FIPL 依赖必须在 configure 阶段即失败。

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(RECON_BUILD_TESTS "构建单元测试" ON)
option(RECON_BUILD_CLI "构建桌面验证工具 recon_cli" ON)
option(RECON_USE_POISSON_RECON "使用上游 PoissonRecon 而非自实现" OFF)
option(RECON_USE_VULKAN "启用 Vulkan 计算后端" OFF)
option(RECON_USE_METAL "启用 Metal 计算后端" OFF)

# ---------------------------------------------------------------------------
# 平台检测
# ---------------------------------------------------------------------------
if(ANDROID)
  set(RECON_PLATFORM_ANDROID ON)
elseif(IOS)
  set(RECON_PLATFORM_IOS ON)
else()
  set(RECON_PLATFORM_DESKTOP ON)
endif()

# ---------------------------------------------------------------------------
# Android 专属编译/链接选项（docs/12 §3.3「顶层 CMakeLists.txt 中的链接参数挂载」）
# ---------------------------------------------------------------------------
# 16 KB 页对齐 + 隐藏默认可见性 + NDK 版本双保险断言。
# toolchain（cmake/toolchains/android.cmake）已检一次；此处再检一次，防止
# toolchain 被替换/覆盖后静默产出不合规产物。
if(RECON_PLATFORM_ANDROID)
  add_compile_options(-fvisibility=hidden -fvisibility-inlines-hidden)
  if(NOT DEFINED RECON_LINK_ALIGN_FLAGS)
    set(RECON_LINK_ALIGN_FLAGS "-Wl,-z,max-page-size=16384")
  endif()
  add_link_options(${RECON_LINK_ALIGN_FLAGS})

  if(ANDROID_NDK_VERSION AND ANDROID_NDK_VERSION VERSION_LESS 28.0.13004108)
    message(FATAL_ERROR
      "NDK ${ANDROID_NDK_VERSION} 必须 >= r28 (28.0.13004108)，"
      "见 docs/12_构建与打包.md §3.1（16 KB 页对齐为硬门禁）。")
  endif()
endif()

# ---------------------------------------------------------------------------
# iOS 专属编译/链接选项（docs/12 §4.1「版本要求」、§4.6「最低部署版本」）
# ---------------------------------------------------------------------------
# 部署目标 15.0 双保险断言 + 架构白名单 + 零网络硬约束声明。
# toolchain（cmake/toolchains/ios.cmake）已检一次；此处再检一次，防止 toolchain
# 被替换/覆盖后静默产出不合规产物（与 Android 块的处理方式保持一致）。
if(RECON_PLATFORM_IOS)
  if(NOT CMAKE_SYSTEM_VERSION OR CMAKE_SYSTEM_VERSION STREQUAL "")
    message(FATAL_ERROR
      "iOS 构建必须显式指定 CMAKE_SYSTEM_VERSION（= IPHONEOS_DEPLOYMENT_TARGET）。"
      "见 docs/12_构建与打包.md §4.6，最低部署版本为 15.0。")
  endif()
  if(IOS_DEPLOYMENT_TARGET AND IOS_DEPLOYMENT_TARGET VERSION_LESS 15.0)
    message(FATAL_ERROR
      "IOS_DEPLOYMENT_TARGET='${IOS_DEPLOYMENT_TARGET}' 低于下限 15.0，"
      "见 docs/12_构建与打包.md §4.6。")
  endif()

  # 架构白名单（docs/12 §4.7）：真机 arm64 only；模拟器允许 arm64 + x86_64。
  set(_recon_ios_archs "${CMAKE_OSX_ARCHITECTURES}")
  if(_recon_ios_archs STREQUAL "")
    set(_recon_ios_archs "arm64")
  endif()
  foreach(_arch IN LISTS _recon_ios_archs)
    if(NOT _arch MATCHES "^(arm64|x86_64)$")
      message(FATAL_ERROR
        "不支持的 iOS 架构 '${_arch}'。允许值见 docs/12_构建与打包.md §4.7："
        " 真机 arm64；模拟器 arm64 / x86_64。")
    endif()
  endforeach()

  # 零网络硬约束：本产品为纯本地内核，禁止链接网络框架（00 R-LOCAL / 11 §1.6）。
  # 该变量由 CI 的 otool -L 校验步骤读取（docs/12 §4.3 XC 校验项）。
  if(NOT DEFINED RECON_IOS_FORBIDDEN_LIBS)
    set(RECON_IOS_FORBIDDEN_LIBS "CFNetwork;Network;WebKit;Security")
  endif()

  # iOS 静态库不需要生成 .dSYM 之外的额外产物；Bitcode 自 Xcode 14 起废弃
  # （docs/12 §4.5），此处显式关闭以防旧工程设置残留。
  set(ENABLE_BITCODE OFF)

  add_compile_options(-fvisibility=hidden -fvisibility-inlines-hidden)
endif()

# ---------------------------------------------------------------------------
# 许可门禁：扫描 third_party 下是否存在传染性协议
# ---------------------------------------------------------------------------
set(RECON_FORBIDDEN_LICENSES
    "GPL-3" "GPL-2" "AGPL-3" "AGPL" "LGPL-3" "LGPL-2.1" "FIPL" "Commons Clause")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
  file(GLOB_RECURSE _tp_license_files
       "${CMAKE_CURRENT_SOURCE_DIR}/third_party/*/LICENSE*"
       "${CMAKE_CURRENT_SOURCE_DIR}/third_party/*/COPYING*"
       "${CMAKE_CURRENT_SOURCE_DIR}/third_party/*/COPYRIGHT*")

  set(_tp_violations "")
  foreach(_f IN LISTS _tp_license_files)
    file(READ "${_f}" _content)
    string(TOLOWER "${_content}" _lower)
    foreach(_bad IN LISTS RECON_FORBIDDEN_LICENSES)
      string(FIND "${_lower}" "${_bad}" _hit)
      if(NOT _hit EQUAL -1)
        list(APPEND _tp_violations "${_f} (命中: ${_bad})")
      endif()
    endforeach()
  endforeach()

  if(_tp_violations)
    string(REPLACE ";" "\n  - " _tp_violations_str "${_tp_violations}")
    message(FATAL_ERROR
      "检测到传染性协议依赖，违反项目商用合规要求：\n  - ${_tp_violations_str}\n"
      "详见 docs/10_许可与合规.md")
  endif()
endif()

# ---------------------------------------------------------------------------
# 第三方依赖登记真源 + 许可清单生成（docs/12 §2.1 D-1 / §2.7）
# ---------------------------------------------------------------------------
# ThirdParty.cmake 是「四表一致」链条的唯一真源；Licenses.cmake 由它派生
# THIRD_PARTY_LICENSES.txt，并登记平台化嵌入路径（iOS / Android / 桌面）。
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThirdParty.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Licenses.cmake")

# ---------------------------------------------------------------------------
# 核心库
# ---------------------------------------------------------------------------
set(RECON_CORE_SOURCES
    core/src/util/params.cpp
    core/src/util/linalg.cpp
    core/src/util/robust_fit.cpp
    core/src/util/memory_guard.cpp
    core/src/util/context.cpp
    core/src/util/c_api.cpp
    core/src/capture/quality_gate.cpp
    core/src/capture/depth_preprocess.cpp
    core/src/sync/imu_preintegration.cpp
    core/src/sync/time_sync.cpp
    core/src/rgbd/tsdf_fusion.cpp
    core/src/calib/scale_calibration.cpp
    core/src/postproc/mesh_repair.cpp
    core/src/export/exporter.cpp
    core/src/export/png_encoder.cpp
    core/src/util/camera.cpp
    core/src/mvs/patchmatch_stereo.cpp
    core/src/vio/msckf.cpp
    core/src/vio/vio_estimator.cpp
    core/src/mesh/poisson_mesh.cpp
    core/src/mesh/uv_unwrap.cpp
    core/src/mesh/texture_bake.cpp
    core/src/mesh/poisson_mc.cpp
    core/src/sfm/geometry.cpp
    core/src/sfm/matching.cpp
    core/src/sfm/registration.cpp
    core/src/sfm/pose_graph.cpp
    core/src/orchestrator/orchestrator.cpp)

# ── 库形态：Android 必须是 SHARED，其余平台 STATIC ──────────────────────────
# 为什么 Android 不能是 STATIC（第四十三轮实测的**致命缺口 A**）：
#   `platform/android/.../ReconApplication.kt:23` 在 `onCreate` 里
#   `System.loadLibrary("recon_core")`。`loadLibrary` 只认 **`.so` 文件名**，
#   而本仓库此前 **全部 target 都是 STATIC** —— 根本没有 `librecon_core.so`
#   可加载，真机启动**第一步**即崩：
#     java.lang.UnsatisfiedLinkError: dlopen failed:
#       library "librecon_core.so" not found
#   而这一缺口在**编译期、ctest 28/28、以及既有全部静态闸门**下**全绿**：
#   静态库照样编译、照样被 App 链接，`add_library(STATIC)` 也不报错。
#   属 MEMORY.md「静态闸门结构上查不出」类，只能靠真机/真编译暴露。
# 为什么桌面与 iOS **保持 STATIC**：
#   · 桌面：`recon_cli` 与 28 个单测直接链接静态库，改 SHARED 会引入运行时
#     库搜索路径问题（Windows 上尤甚），且无任何收益；
#   · iOS：`ios.yml` 的 `build-xcframework` 用 `libtool -static` 合并三个
#     `librecon_*.a` 再用 `xcodebuild -create-xcframework -library` 打包。
#     `-create-xcframework -library` **只接受静态库**，改 SHARED 会直接破掉
#     已跑通四 job 的 iOS 链路。
# 判据落点：Android 产物中 `librecon_core.so` 与 `librecon_platform_android.so`
# 真实存在（见 `tools/check_android_contract.py` G-AND-5）。
if(RECON_PLATFORM_ANDROID)
  set(RECON_CORE_LIB_TYPE SHARED)
else()
  set(RECON_CORE_LIB_TYPE STATIC)
endif()

add_library(recon_core ${RECON_CORE_LIB_TYPE} ${RECON_CORE_SOURCES})
add_library(recon::core ALIAS recon_core)

target_include_directories(recon_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/core/include")

if(MSVC)
  target_compile_options(recon_core PRIVATE /W4 /permissive-)
  target_compile_definitions(recon_core PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  target_compile_options(recon_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_package(Threads REQUIRED)
target_link_libraries(recon_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# 第三方依赖（全部宽松协议）
# ---------------------------------------------------------------------------
# 说明：以下依赖通过 third_party/ 内联源码或系统包引入。
# 具体版本与许可见 docs/12_构建与打包.md §2。

# zlib（Zlib 许可，宽松）—— PNG 纹理导出的压缩后端。
#
# 优先用系统 zlib；没有就用 PoissonRecon vendor 的那份纯 C 源码
# （third_party/PoissonRecon-master/ZLIB，1.3.1，许可已在 docs/10 登记）。
# 两者都拿不到时，PNG 编码器自动退到「存储型 deflate」——文件仍然合法，
# 只是体积大（见 core/include/recon/png_encoder.h 的说明）。
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
  target_link_libraries(recon_core PUBLIC ZLIB::ZLIB)
  target_compile_definitions(recon_core PRIVATE RECON_HAVE_ZLIB=1)
  message(STATUS "recon: 使用系统 zlib ${ZLIB_VERSION_STRING}（PNG 压缩）")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/PoissonRecon-master/ZLIB/zlib.h")
  set(_recon_zlib_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/PoissonRecon-master/ZLIB")
  add_library(recon_zlib STATIC
      "${_recon_zlib_dir}/adler32.c"
      "${_recon_zlib_dir}/compress.c"
      "${_recon_zlib_dir}/crc32.c"
      "${_recon_zlib_dir}/deflate.c"
      "${_recon_zlib_dir}/trees.c"
      "${_recon_zlib_dir}/zutil.c")
  target_include_directories(recon_zlib PUBLIC "${_recon_zlib_dir}")
  target_compile_definitions(recon_zlib PRIVATE _CRT_SECURE_NO_WARNINGS)
  if(MSVC)
    target_compile_options(recon_zlib PRIVATE /w)
  else()
    target_compile_options(recon_zlib PRIVATE -w)
  endif()
  # 第三方源码不参与本项目的告警口径（它不是我们的代码），故降为 /w。
  target_link_libraries(recon_core PUBLIC recon_zlib)
  target_compile_definitions(recon_core PRIVATE RECON_HAVE_ZLIB=1)
  message(STATUS "recon: 使用 vendor zlib 1.3.1（PNG 压缩）")
else()
  message(STATUS "recon: 无 zlib，PNG 走存储型 deflate（合法但体积大）")
endif()

# Ceres Solver (BSD-3-Clause) —— BA 与 VIO 优化求解器
find_package(Ceres QUIET)
if(Ceres_FOUND)
  target_link_libraries(recon_core PUBLIC Ceres::ceres)
  message(STATUS "recon: 使用系统 Ceres ${Ceres_VERSION}")
else()
  message(STATUS "recon: 未找到 Ceres，相关优化模块将被跳过构建")
endif()

# Eigen (MPL-2.0，边界项 B-1) —— Ceres 的硬依赖
find_package(Eigen3 QUIET)
if(Eigen3_FOUND)
  target_link_libraries(recon_core PUBLIC Eigen3::Eigen)
  message(STATUS "recon: 使用系统 Eigen3 ${EIGEN3_VERSION}（边界项 B-1）")
else()
  message(STATUS "recon: 未找到 Eigen3")
endif()

# OpenCV (Apache-2.0) —— 图像与特征算子
find_package(OpenCV QUIET COMPONENTS core imgproc calib3d features2d photo)
if(OpenCV_FOUND)
  target_link_libraries(recon_core PUBLIC ${OpenCV_LIBS})
  message(STATUS "recon: 使用系统 OpenCV ${OpenCV_VERSION}")
endif()

# xatlas (MIT) —— UV 展开
find_package(xatlas QUIET)
if(xatlas_FOUND)
  target_link_libraries(recon_core PUBLIC xatlas::xatlas)
endif()

# PoissonRecon (MIT) —— 泊松网格化的第三方依赖。
#
# 当前**未启用**：上游 V7.0 标签的打包损坏（多个文件来自不同版本，
# 交叉引用对不上），master 分支则需额外的 AdaptiveSolvers 依赖。
# 现阶段由 core/src/mesh/poisson_mesh.cpp 的自实现承担（已实测通过
# 拓扑与精度验收）。详见 third_party/poisson_recon/PATCHES.md
# 与 docs/06_网格化与后处理.md 的「实现说明」。
#
# 恢复方式：把下面的 if 去掉条件即可（集成层代码已就位）。
# Marching Cubes 标准边表已内联到 core/src/mesh/poisson_mc.cpp
# （数据取自 PoissonRecon，MIT，见 third_party/poisson_recon/PATCHES.md 的 P4）。
# 无需链接上游库 —— 上游符号在全局命名空间，会与本项目冲突。

# ---------------------------------------------------------------------------
# 着色器（docs/12 §4.4：算法同源、两端分别编译）
# ---------------------------------------------------------------------------
# 三个 target 的分工（与 docs/12 §1.3 的产物表逐条对应）：
#
#   recon_shaders_common   INTERFACE  纯头，把 `shaders/common/` 的算法描述
#                                     暴露给 C++ 侧（用于拿到工作组的默认常量
#                                     与参数块布局）。**不产生任何产物**。
#   recon_shaders_vulkan   INTERFACE  Vulkan 侧 GLSL 源码 + 内联为 C 数组的
#                                     产物（`shaders_vulkan.spv`，见 §1.3）。
#                                     交叉编译需 glslangValidator/glslc；
#                                     本机（Windows，无着色器工具链）**不可用**，
#                                     故默认只做"源码存在性 + 一致性"，
#                                     真正的 SPIR-V 编译走 `RECON_COMPILE_SHADERS=ON`。
#   recon_shaders_metal    INTERFACE  iOS 的 default.metallib（§4.4）。
#                                     必须 macOS + Xcode；非 macOS 上**不尝试**，
#                                     也**不伪造**产物（见 build_metal_shaders.sh）。
#
# ── 为什么都用 INTERFACE 而不是 STATIC ────────────────────────────────────
# 着色器不是 C++ 代码，无法"编译成静态库"。它们是被 host 侧（Metal/Vulkan
# 后端）在运行期加载的资源。用 INTERFACE target 是为了：
#   ① 让 host target 能用 `target_link_libraries(... recon_shaders_metal)`
#      表达"我依赖这套着色器"——依赖关系进 CMake 图，可被 `--graphviz` 审计；
#   ② 把「产物路径」与「生成命令」挂在一个有名字的对象上，
#      而不是散落在各处的 `add_custom_command`。
add_library(recon_shaders_common INTERFACE)
add_library(recon::shaders_common ALIAS recon_shaders_common)
target_include_directories(recon_shaders_common INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common")

# --- Metal 侧 --------------------------------------------------------------
# 空 INTERFACE target；`RECON_COMPILE_SHADERS=ON && RECON_PLATFORM_IOS` 时才
# 往它上面挂产物（见下方）。即使不挂产物它也**必须存在** ——
# host 侧的 `target_link_libraries(... recon::shaders_metal)` 不应因构建
# 开关不同而变成"未定义 target"（那会让 CMake 配置期报错，掩盖真实问题）。
add_library(recon_shaders_metal INTERFACE)
add_library(recon::shaders_metal ALIAS recon_shaders_metal)

# --- Vulkan 侧 -------------------------------------------------------------
add_library(recon_shaders_vulkan INTERFACE)
add_library(recon::shaders_vulkan ALIAS recon_shaders_vulkan)
set(RECON_VULKAN_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan")
file(GLOB RECON_VULKAN_SHADER_SOURCES CONFIGURE_DEPENDS
     "${RECON_VULKAN_SHADER_DIR}/*.comp" "${RECON_VULKAN_SHADER_DIR}/*.glsl")
if(RECON_VULKAN_SHADER_SOURCES)
  target_sources(recon_shaders_vulkan INTERFACE ${RECON_VULKAN_SHADER_SOURCES})
endif()

option(RECON_COMPILE_SHADERS
  "编译着色器产物（Vulkan SPIR-V / Metal metallib）。需要相应工具链，默认 OFF" OFF)
# ⚠️ 默认 OFF 的理由（如实声明，不是偷懒）：
#   ① 本开发机（Windows）无 glslangValidator / glslc / xcrun —— 打开也编不出；
#   ② 着色器编译需要**目标平台 SDK**（Metal 必须 iPhoneOS SDK；SPIR-V 可交叉
#      但需要 glslang 的 Android NDK 版本匹配），不是"随便编编就行"；
#   ③ 「编不出来」与「编出来是错的」必须可区分：若在无工具链时静默产出一个
#      空的 .spv / metallib，运行期会以"库存在但函数找不到"的形式失败 ——
#      比直接报"没编译"更难查（MEMORY.md「静默丢弃最难查」）。
#    故此处**默认不编**，并由 `tools/shader_parity.py` 承担一致性检查
#    （CLAUDE 契约：docs/14 CT-6 的 P1 级闸门），真机阶段再开本开关。

if(RECON_COMPILE_SHADERS AND RECON_PLATFORM_IOS)
  # Metal 着色器：必须 macOS + Xcode（docs/12 §4.4）。
  set(RECON_METALLIB_OUT_DIR
      "${CMAKE_BINARY_DIR}/ios/Recon.app/Contents/Resources")
  set(RECON_METALLIB_AIR_DIR
      "${CMAKE_BINARY_DIR}/ios/metal-air")
  set(RECON_METALLIB_OUT "${RECON_METALLIB_OUT_DIR}/default.metallib")
  # ⚠️ 必须把产物目录**显式传给脚本**（RECON_METALLIB_OUT_DIR / _AIR_DIR）。
  #    脚本的默认 OUT_DIR 是 `build/ios/Recon.app/Contents/Resources`（相对**仓库根**），
  #    而这里的 CMAKE_BINARY_DIR 在 CI 里是 `build/ios/device` —— 两者不一致时，
  #    本命令声明的 OUTPUT 永远不会被生成，`ninja`/`make` 会以
  #    "missing output" 或反复重跑收场（只有真在 macOS 上配置才会暴露）。
  #
  # ⚠️ 同样必须把 **SDK 与部署目标**显式传给脚本（2026-11-01 CI 实证，两个真 bug）：
  #    ① `RECON_IOS_SDK`：脚本默认 `iphoneos`。模拟器 slice（RECON_IOS_SIMULATOR=ON）
  #       若吃默认值，会**用真机 SDK 编出模拟器目录里的产物** —— CI 首次暴露即此：
  #       `build/ios/simulator/**/default.metallib` 的前一行日志写着 `sdk : iphoneos`。
  #       后果是模拟器 slice 的着色器**从未被真正用 iphonesimulator 编过**，
  #       `build-xcframework` 的 16/16 全绿里有一半是假的。
  #    ② `RECON_IOS_DEPLOYMENT_TARGET`：`-target` 三元组由它推出，进而决定
  #       `__METAL_VERSION__`。不给则脚本回落 15.0（与工具链文件同值），
  #       但还是显式传更稳 —— 一旦有人改了 RECON_IOS_DEPLOYMENT_TARGET，
  #       这里不传就会静默用旧值编着色器（配置与产物不一致）。
  if(RECON_IOS_SIMULATOR)
    set(RECON_METALLIB_SDK "iphonesimulator")
  else()
    set(RECON_METALLIB_SDK "iphoneos")
  endif()
  add_custom_command(
    OUTPUT "${RECON_METALLIB_OUT}"
    COMMAND "${CMAKE_COMMAND}" -E env
            RECON_REQUIRE_METAL=1
            "RECON_IOS_SDK=${RECON_METALLIB_SDK}"
            "RECON_IOS_DEPLOYMENT_TARGET=${RECON_IOS_DEPLOYMENT_TARGET}"
            "RECON_METALLIB_OUT_DIR=${RECON_METALLIB_OUT_DIR}"
            "RECON_METAL_AIR_DIR=${RECON_METALLIB_AIR_DIR}"
            bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_metal_shaders.sh"
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_metal_shaders.sh
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common/gpu_algo.h"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "编译 Metal 着色器 → default.metallib（docs/12 §4.4）"
    VERBATIM)
  add_custom_target(recon_metallib DEPENDS "${RECON_METALLIB_OUT}")
  # 把产物挂到 INTERFACE target 上：host 侧 `target_link_libraries(... 
  # recon_shaders_metal)` 即表达"我依赖 default.metallib 已生成"。
  #
  # ⚠️ 本列表必须与 shaders/metal/*.metal **逐文件一致**。
  #    曾漏列 yuv.metal（第三十七轮新增的第 8 个算法 recon_yuv_to_bgra）——
  #    漏列不会让构建失败，而是让该 .metal 静默不进 build phase，属
  #    MEMORY.md「静默丢弃最难查」。由 tools/shader_parity.py 的 P-12 判据守护。
  target_sources(recon_shaders_metal INTERFACE
      "${CMAKE_CURRENT_SOURCE_DIR}/shaders/metal/mvs.metal"
      "${CMAKE_CURRENT_SOURCE_DIR}/shaders/metal/tsdf.metal"
      "${CMAKE_CURRENT_SOURCE_DIR}/shaders/metal/yuv.metal"
      "${CMAKE_CURRENT_SOURCE_DIR}/shaders/metal/gpu_common.h"
      "${RECON_METALLIB_OUT}")
  add_dependencies(recon_shaders_metal recon_metallib)
endif()

if(RECON_COMPILE_SHADERS AND (RECON_PLATFORM_ANDROID OR RECON_PLATFORM_DESKTOP))
  # Vulkan 着色器：GLSL → SPIR-V（glslangValidator），再内联为 C 数组（docs/12 §1.3）。
  # 两者的具体命令由 tools/build_vulkan_shaders.sh 承担（与 Metal 侧对称）。
  #
  # ── 为什么必须显式传产物目录（与 Metal 侧同一类真 bug，对称修法）──────────
  # 脚本的默认输出是 `build/vulkan/...`（相对**仓库根**），而 CMAKE_BINARY_DIR
  # 在 Gradle/CI 下是 `build/android/<abi>`。两者不一致时，产物会落在
  # 「没人会去看、也没人声明依赖」的仓库根 `build/` 下 —— 构建不报错，
  # 但消费方 include 不到内联头（属 MEMORY.md「静默丢弃最难查」）。
  set(RECON_SPIRV_OUT_DIR "${CMAKE_BINARY_DIR}/vulkan/spirv")
  set(RECON_SPIRV_HEADER_OUT "${CMAKE_BINARY_DIR}/vulkan/recon_shaders_vulkan_spv.h")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_vulkan_shaders.sh")
    add_custom_target(recon_spirv
      COMMAND "${CMAKE_COMMAND}" -E env
              "RECON_SPIRV_DIR=${RECON_SPIRV_OUT_DIR}"
              "RECON_SPIRV_HEADER=${RECON_SPIRV_HEADER_OUT}"
              bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_vulkan_shaders.sh"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_vulkan_shaders.sh"
              "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common/gpu_algo.h"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      COMMENT "编译 Vulkan 着色器 → SPIR-V（docs/12 §1.3）")
    # ── 缺口 B 的修复：把生成目标接进依赖图 ──────────────────────────────
    # 此前**没有**这一行，`recon_spirv` 是孤儿 custom target —— 任何构建都不会
    # 触发它，SPIR-V 与内联头永不生成，而构建全绿。这与 iOS 侧
    # `recon_shaders_metal` 早已修过的「设计意图未落实」完全同构。
    # 由此处 `add_dependencies` + 下方 consumers 的 `target_link_libraries`，
    # `RECON_COMPILE_SHADERS=ON` 时构建 Android 门面即自动产出着色器产物。
    add_dependencies(recon_shaders_vulkan recon_spirv)
    # 把产物头挂到 INTERFACE 上：C++ 侧（Vulkan 后端）include 它取内联数组。
    target_sources(recon_shaders_vulkan INTERFACE "${RECON_SPIRV_HEADER_OUT}")
  else()
    message(STATUS "recon_shaders_vulkan: 未找到 tools/build_vulkan_shaders.sh，"
                   "跳过 SPIR-V 编译（仅源码参与一致性检查）")
  endif()
endif()

# ---------------------------------------------------------------------------
# 平台适配层（docs/12 §1.2：recon_platform_* target）
# ---------------------------------------------------------------------------
# 设计：把**平台无关**的胶合逻辑（采集状态机 docs/09 §4.1）下沉为
# `recon_platform_common`（纯 C++17，全平台可编译、桌面可单测）；Android/iOS
# 专属的采集实现（Camera2/SensorManager、AVFoundation/CoreMotion）各自薄封装。
#
# 这样做的理由（铁律：同一逻辑写两遍必然漂移）：采集状态机的迁移规则（T-01..T-30）
# 是双端共享契约，若在 Kotlin 与 Swift 各写一份必然漂移。故以 C++ 单实现承载。
add_library(recon_platform_common STATIC
    platform/common/src/capture_state.cpp
    platform/common/src/imu_window.cpp
    platform/common/src/depth_convert.cpp
    # YUV 4:2:0 → BGRA8 的**色彩数学本体**（平台无关）。
    # iOS(ARKit 420YpCbCr8BiPlanarFullRange) 与 Android(YUV_420_888) 都要做
    # 这一步，故按「同一逻辑不写两遍」下沉；两端只负责像素格式判定与平面取址。
    # 见 platform/common/include/recon_platform/yuv_convert.h 的文件头。
    platform/common/src/yuv_convert.cpp)
add_library(recon::platform_common ALIAS recon_platform_common)
target_include_directories(recon_platform_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/common/include")
# 平台无关胶合层需要 core 的 **C ABI POD 类型**（如 `recon_imu_sample`）。
# 只取类型，**绝不调用 RECON_API 函数** —— 那是各端桥接层（recon_platform_ios /
# recon_platform_android）的职责。`imu_window.h` 直接复用 `recon_imu_sample`
# 而非另定义同形结构体，正是为了消灭「第二事实来源」（两处同形定义必然漂移）。
# 该边界由 tools/check_platform_parity.py 的闸门 8 静态守住。
target_include_directories(recon_platform_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/core/include")

if(MSVC)
  target_compile_options(recon_platform_common PRIVATE /W4 /permissive-)
else()
  target_compile_options(recon_platform_common PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Android 专属门面：JNI 桥接 + 明确定义 target 名（docs/12 §1.2）。
# 仅在 Android 构建时存在；桌面构建不含 JNI 头，故不参与。
if(RECON_PLATFORM_ANDROID)
  # 形态同 recon_core：**必须 SHARED**，因为 `ReconApplication.kt:24` 有
  # `System.loadLibrary("recon_platform_android")`。STATIC 下无 `.so` 可加载 →
  # 同一个 `UnsatisfiedLinkError`（缺口 A 的第二处）。详见 recon_core 处注释。
  add_library(recon_platform_android SHARED
      platform/android/app/src/main/cpp/native-lib.cpp
      platform/android/app/src/main/cpp/platform-bridge.cpp
      # IMU 环形缓冲的 JNI 薄封装：桥接 `platform/common/src/imu_window.cpp`
      # 的规则本体到 Kotlin `object ImuRingBuffer`。本文件只做类型搬运与
      # 交错布局重排，**不含任何切窗规则**（见 imu-bridge.cpp 文件头）。
      platform/android/app/src/main/cpp/imu-bridge.cpp)
  target_link_libraries(recon_platform_android PUBLIC recon_core recon_platform_common)
  # ── 着色器依赖进 CMake 图（第四十三轮实测的**致命缺口 B**）──────────────
  # 此前 `recon_shaders_vulkan` 从未被任何 target 链接，于是
  # `add_custom_target(recon_spirv)` **永远不会被任何构建触发** —— 与 iOS 侧
  # `recon_shaders_metal` 早已修过的那个「设计意图未落实」（见 recon_platform_ios
  # 处的注释）**完全同构**，只是 Android 侧漏了同一个修法。
  # 后果：`RECON_COMPILE_SHADERS=ON` 时 SPIR-V 产物与内联头**永不生成**，
  # 而构建**不报错**（INTERFACE target 无人链接 = 静默空转）。
  # 注：即使 `RECON_COMPILE_SHADERS=OFF` 也必须链接 —— 那时它们是空 INTERFACE，
  # 链接无副作用；这正是把 target 无条件建出来、并无条件链接的理由。
  target_link_libraries(recon_platform_android PUBLIC recon_shaders_vulkan)
  target_link_libraries(recon_platform_android PUBLIC recon_shaders_common)
  target_include_directories(recon_platform_android PRIVATE
      # GPU 参数块的唯一事实来源（gpu_algo.h）。Vulkan 后端将来 include 它取
      # 工作组常量/参数块布局，不得在 Android 侧手抄一份（必然漂移）。
      "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common")
  if(NOT MSVC)
    target_compile_options(recon_platform_android PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

# iOS 专属门面：Objective-C++ 桥接 + 明确定义 target 名（docs/12 §1.2）。
# 仅在 iOS 构建（Xcode + CMake）时存在。
if(RECON_PLATFORM_IOS)
  enable_language(OBJCXX)
  add_library(recon_platform_ios STATIC
      platform/ios/Recon/CaptureStateBridge.mm
      platform/ios/Recon/PlatformCaptureBridge.mm
      # YUV→BGRA8 转换门面（docs/11 §693 IOS-AR-5 指定的文件名）。
      # GPU 主路径（Metal compute，零拷贝 CVMetalTextureCache）+ CPU 回退
      # （platform/common/src/yuv_convert.cpp）。host 侧**不含色彩数学** ——
      # 系数由 MakeYuvToBgraParams 算出，参数块走 shaders/common/gpu_algo.h。
      platform/ios/Recon/MetalComputeBackend.mm)
  target_link_libraries(recon_platform_ios PUBLIC recon_platform_common)
  # 着色器依赖进 CMake 图：host 侧是**唯一**会加载 default.metallib 的目标
  # （MetalComputeBackend.mm）。此前只声明了这两个 INTERFACE target 却无人链接，
  # 于是 `add_custom_target(recon_metallib)` 永远不会被任何构建触发 —— 属
  # MEMORY.md「设计意图未落实」类。链接后 `RECON_COMPILE_SHADERS=ON` 时
  # 构建 recon_platform_ios 即自动产出 metallib；OFF 时它们是空 INTERFACE，
  # 链接无副作用（这正是把 target 无条件建出来的理由）。
  target_link_libraries(recon_platform_ios PUBLIC recon_shaders_metal)
  target_link_libraries(recon_platform_ios PUBLIC recon_shaders_common)
  # Metal 框架：host 侧要 CVMetalTextureCache 与 MTLComputePipelineState。
  # 注意 **不链接任何网络框架**（00 R-LOCAL / 11 §1.6）。
  target_link_libraries(recon_platform_ios PUBLIC "-framework Metal"
                                                  "-framework CoreVideo")
  target_include_directories(recon_platform_ios PRIVATE
      # 复用 GPU 参数块的唯一事实来源（gpu_algo.h）。该头是纯 C++，
      # 由 .mm 直接 include —— 不在 host 侧手抄一份结构体（必然漂移）。
      # 注：链接 recon_shaders_common 已传递该 include 目录，此处保留是为了
      # 让「为什么需要它」在目标定义处可见；两者指向同一路径，不产生第二来源。
      "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common")
  target_compile_options(recon_platform_ios PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# 桌面验证工具
# ---------------------------------------------------------------------------
if(RECON_BUILD_CLI AND RECON_PLATFORM_DESKTOP)
  add_executable(recon_cli tools/recon_cli.cpp)
  target_link_libraries(recon_cli PRIVATE recon_core)
endif()

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
if(RECON_BUILD_TESTS AND RECON_PLATFORM_DESKTOP)
  enable_testing()

  add_executable(test_params tests/unit/test_params.cpp)
  target_link_libraries(test_params PRIVATE recon_core)
  add_test(NAME params COMMAND test_params)

  add_executable(test_c_api tests/unit/test_c_api.cpp)
  target_link_libraries(test_c_api PRIVATE recon_core)
  add_test(NAME c_api COMMAND test_c_api)

  add_executable(test_export tests/unit/test_export.cpp)
  target_link_libraries(test_export PRIVATE recon_core)
  add_test(NAME export COMMAND test_export)

  add_executable(test_sync_depth tests/unit/test_sync_depth.cpp)
  target_link_libraries(test_sync_depth PRIVATE recon_core)
  add_test(NAME sync_depth COMMAND test_sync_depth)

  # 时间轴归一化（docs/02 §3.2 / §3.7 / §3.8）：仿射拟合、5σ 剔除、
  # 置信度分级、漂移拒用、倒退/跳变/丢帧、重锚定延续性。
  add_executable(test_time_sync tests/unit/test_time_sync.cpp)
  target_link_libraries(test_time_sync PRIVATE recon_core)
  add_test(NAME time_sync COMMAND test_time_sync)

  # 深度预处理（docs/04 §3）：置信度掩码、飞行点剔除、BMRE、深度→RGB 对齐、
  # 原始缓冲不可变铁律。
  add_executable(test_depth_preprocess tests/unit/test_depth_preprocess.cpp)
  target_link_libraries(test_depth_preprocess PRIVATE recon_core)
  add_test(NAME depth_preprocess COMMAND test_depth_preprocess)

  # 位姿图优化与闭环检测（docs/05 §4.5–§4.6）：指纹、候选检测、几何验证、
  # 位姿图 LM、gauge 固定、闭环回滚。
  add_executable(test_pose_graph tests/unit/test_pose_graph.cpp)
  target_link_libraries(test_pose_graph PRIVATE recon_core)
  add_test(NAME pose_graph COMMAND test_pose_graph)

  # 内存保护与降级器（docs/13 §3.4 / §3.5 / §4）：水位分级、白名单从基线重算、
  # 黑名单不触碰断言、迟滞恢复、blacklist_touched 恒空。
  add_executable(test_memory_guard tests/unit/test_memory_guard.cpp)
  target_link_libraries(test_memory_guard PRIVATE recon_core)
  add_test(NAME memory_guard COMMAND test_memory_guard)

  # 分块 TSDF 一致性（docs/13 §3.2.1；docs/14 §5.1 TSDF 行 ③④⑤）：
  # 分块与单块逐点一致、体素边长不被改（黑名单 B-1）、分块省内存。
  add_executable(test_tsdf_tiled tests/unit/test_tsdf_tiled.cpp)
  target_link_libraries(test_tsdf_tiled PRIVATE recon_core)
  add_test(NAME tsdf_tiled COMMAND test_tsdf_tiled)

  # 预积分零偏雅可比（docs/02 §3.5.5 P-1、docs/05 §3.3.5 J-1）：
  # 中心差分校验 ∂Δ/∂b 三式，相对误差 < 1e-5。
  add_executable(test_preint_jacobian tests/unit/test_preint_jacobian.cpp)
  target_link_libraries(test_preint_jacobian PRIVATE recon_core)
  add_test(NAME preint_jacobian COMMAND test_preint_jacobian)

  add_executable(test_mvs tests/unit/test_mvs.cpp)
  target_link_libraries(test_mvs PRIVATE recon_core)
  add_test(NAME mvs COMMAND test_mvs)

  # SfM 几何原语（docs/05 §4.3–§4.4）：E/F 估计、位姿恢复、多视图三角化。
  add_executable(test_sfm tests/unit/test_sfm.cpp)
  target_link_libraries(test_sfm PRIVATE recon_core)
  add_test(NAME sfm COMMAND test_sfm)

  add_executable(test_vio tests/unit/test_vio.cpp)
  target_link_libraries(test_vio PRIVATE recon_core)
  add_test(NAME vio COMMAND test_vio)

  add_executable(test_poisson tests/unit/test_poisson.cpp)
  target_link_libraries(test_poisson PRIVATE recon_core)
  add_test(NAME poisson COMMAND test_poisson)

  add_executable(test_uv tests/unit/test_uv.cpp)
  target_link_libraries(test_uv PRIVATE recon_core)
  add_test(NAME uv COMMAND test_uv)

  # S9 纹理烘焙。**已达标**。
  # 单视图 RMSE 0.0115 / 多视图 0.0124（目标 < 0.05）。
  # 根因是「整体朝外定向缺失」：`OrientFacesConsistently` 只保证相邻面绕序
  # 局部一致，不保证整体朝外；球面网格整体朝内 → `cos^α` 入射角权重塌到 0
  # → 整片纹理变黑（RMSE 0.51）。已在 `RunMeshPipeline` 末端接入
  # `OrientMeshOutward`（逐分量径向符号，**不做 BFS 传播**）。
  # 详见 docs/06 §2.5.1 (24)(26)。
  add_executable(test_bake tests/unit/test_bake.cpp)
  target_link_libraries(test_bake PRIVATE recon_core)
  add_test(NAME bake COMMAND test_bake)

  # 端到端编排测试（L5）。合成长方体 6 视图 → 全链路 → 导出。
  # 覆盖 runReconstruction 的 8 个阶段、C ABI 生命周期、取消语义、空会话。
  add_executable(test_pipeline tests/unit/test_pipeline.cpp)
  target_link_libraries(test_pipeline PRIVATE recon_core)
  add_test(NAME pipeline COMMAND test_pipeline)

  # 平台适配层：采集状态机（docs/09 §4.1/§4.2，T-01..T-30）。
  # 平台无关逻辑，故可在桌面单测 —— 保证双端共享的迁移规则本身正确。
  add_executable(test_capture_state tests/unit/test_capture_state.cpp)
  target_link_libraries(test_capture_state PRIVATE recon_platform_common)
  add_test(NAME capture_state COMMAND test_capture_state)

  # 平台无关采集支撑：IMU 环形缓冲 + 时间窗切片、深度单位换算。
  # 这两块在 iOS 上曾以 Swift 实现（IMUWindow.swift / FrameConverter.swift）；
  # 下沉到 platform/common 后由本测试在桌面验收，双端只做薄封装。
  # 详见 platform/common/include/recon_platform/imu_window.h 的文件头。
  add_executable(test_platform_capture tests/unit/test_platform_capture.cpp)
  target_link_libraries(test_platform_capture PRIVATE recon_platform_common)
  add_test(NAME platform_capture COMMAND test_platform_capture)

  # YUV 4:2:0 → BGRA8 的**色彩数学**（platform/common/yuv_convert）。
  # 为什么必须有这份桌面测试：该数学此前只存在于平台层（Swift），本机不能编译；
  # 而它的错误表现是「颜色整体偏」而非崩溃（不报错）。下沉后可在桌面锁定，
  # 且 GPU 侧着色器刻意不含任何数值字面量（系数全经参数块传入、由本模块算出）
  # → 本测试绿即「数学对」，真机只剩「GPU 是否忠实执行这几行 FMA」待验。
  # 见 platform/common/include/recon_platform/yuv_convert.h 的文件头。
  add_executable(test_platform_yuv tests/unit/test_platform_yuv.cpp)
  target_link_libraries(test_platform_yuv PRIVATE recon_platform_common)
  add_test(NAME platform_yuv COMMAND test_platform_yuv)

  # 契约闸门也纳入 ctest：一条 `ctest` 覆盖「算法 + 契约」。
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_FOUND)
    add_test(NAME abi_drift
      COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/abi_drift_check.py"
              --root "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME platform_parity
      COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_platform_parity.py")
    add_test(NAME params_drift
      COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_params.py")
  # iOS 侧静态契约闸门（locale / Liquid Glass 白名单 / U-X 禁令 / plist / C ABI）。
  # 该闸门不需要 macOS —— 它检查的是规格一致性，与编译器无关。
  add_test(NAME ios_contract
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_ios_contract.py")
  # 着色器两端一致性闸门（docs/14 CT-6 / docs/12 MT-3）。
  # 同样不需要 macOS / glslang —— 它比对的是**源码结构**（算法名三方一致、
  # 参数块字段序列、workgroup 尺寸、跨端共享常量、缺口/差异标注同步）。
  # 在无着色器工具链的机器上，这是"两端算法同源"这一契约**唯一**可执行的检查。
  add_test(NAME shader_parity
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/shader_parity.py")
  # 上述闸门自身的**负向注入**自检：对每类判据注入一个缺陷，要求闸门拒收。
  # 没有这条，"闸门通过"与"闸门根本没检查"无法区分（见该脚本文件头）。
  add_test(NAME shader_parity_negative
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_shader_parity_negative.py")
  # iOS 契约闸门（G11 错误码逐值对等 / G12 阶段映射自洽）的负向注入自检。
  # 与 shader_parity_negative 同理：证明这两类**新增**判据真的会拒收，
  # 而不是「恒不触发」的假闸门。
  add_test(NAME ios_contract_negative
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_ios_contract_negative.py")
  # 平台对等闸门（recon_status 数值集合 / JNI 数组布局条数）的负向注入自检。
  add_test(NAME platform_parity_negative
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_platform_parity_negative.py")
endif()
endif()

# ---------------------------------------------------------------------------
# 许可合规硬门禁
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  # 五阶段全量扫描 + 四表一致性（docs/12 §2.6.1 模式 C）
  add_custom_target(license_scan
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/license_scan.py"
            --root "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
            --registry "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThirdParty.cmake"
            --licenses-cmake "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Licenses.cmake"
            --output "${CMAKE_BINARY_DIR}/license_scan_report.json"
            --fail-on-violation
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "扫描第三方依赖许可（GPL/AGPL 检测 + 四表一致性）")

  # 只做四表登记一致性（docs/12 §2.6.1 模式 A；对应 CI job license-registry）
  add_custom_target(license_registry
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/license_scan.py"
            --registry "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThirdParty.cmake"
            --check-registry
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "校验四表登记一致性（ThirdParty.cmake / docs 表 / third_party 目录）")

  # 校验已生成的许可清单与 third_party 逐字节一致（docs/12 §2.7）
  add_custom_target(license_manifest_verify
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/license_scan.py"
            --licenses-cmake "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Licenses.cmake"
            --licenses-text "${RECON_THIRD_PARTY_LICENSES_FILE}"
            --verify-licenses-cmake
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS licenses_manifest
    COMMENT "校验许可清单与 third_party 逐字节一致")

  # C ABI 契约漂移检测（docs/01 §2 裁定锚点的守护闸门，tools/abi_drift_check.py）
  add_custom_target(abi_drift_check
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/abi_drift_check.py"
            --root "${CMAKE_CURRENT_SOURCE_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "检测 C ABI 契约漂移（真实头文件 vs docs/01 草案 vs 下游引用）")

  # 双端功能对等闸门（iOS / Android 同等对待铁律的守护，tools/check_platform_parity.py）
  add_custom_target(platform_parity_check
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_platform_parity.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "校验 iOS / Android 双端功能对等（权限 / 零网络 / 状态机序号 / C ABI 覆盖）")

  # 参数契约漂移闸门（docs/12 §6.2 任务 12，tools/check_params.py）
  add_custom_target(params_drift_check
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_params.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "检测参数契约漂移（白名单键 / 键名 / perf_ 组登记）")

  # iOS 侧静态契约闸门（docs/09 §1.2.1 LG-2 / §6.1 locale / docs/11 §1.1 权限）。
  # 与 macos 无关，故在桌面 CI 即可运行 —— 把 iOS 规格违规挡在提交之前。
  add_custom_target(ios_contract_check
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_ios_contract.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "校验 iOS 侧静态契约（locale 对等 / 玻璃白名单 / U-X 禁令 / 权限 / C ABI 覆盖）")
endif()

# ---------------------------------------------------------------------------
# 安装
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS recon_core ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY core/include/recon DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(RECON_PLATFORM_ANDROID)
  set(RECON_PLATFORM_NAME "android")
elseif(RECON_PLATFORM_IOS)
  set(RECON_PLATFORM_NAME "ios")
else()
  set(RECON_PLATFORM_NAME "desktop")
endif()
message(STATUS "recon: 配置完成 (平台=${RECON_PLATFORM_NAME})")
