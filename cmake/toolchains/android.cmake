# SPDX-License-Identifier: BSD-3-Clause
#
# cmake/toolchains/android.cmake — Android NDK 交叉编译工具链。
#
# 契约：docs/12_构建与打包.md §3.1（版本要求）、§3.2（ABI 矩阵）、§3.3（本文件规格）。
#
# 用法：
#   # 一：由 Gradle externalNativeBuild 通过 -DCMAKE_TOOLCHAIN_FILE 传入（推荐）
#   # 二：独立使用
#   cmake -S . -B build/android/arm64-v8a \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/android.cmake \
#         -DANDROID_ABI=arm64-v8a \
#         -DANDROID_NDK=/path/to/ndk/28.1.13356709
#
# 注意：本文件在 CMake 的"工具链预处理"阶段被 include，此时 project() 尚未调用，
# 因此只能使用 set()/if() 等最小语法，不得使用需要 project 上下文的命令。

# ---------------------------------------------------------------------------
# 目标平台
# ---------------------------------------------------------------------------
set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_SYSTEM_VERSION 24)                 # minSdk 24（docs/12 §3.1）

if(NOT DEFINED ANDROID_ABI OR ANDROID_ABI STREQUAL "")
  set(ANDROID_ABI "arm64-v8a")
endif()

set(CMAKE_ANDROID_ARCH_ABI ${ANDROID_ABI})
set(CMAKE_ANDROID_NDK ${ANDROID_NDK})
set(CMAKE_ANDROID_STL_TYPE c++_shared)

# ABI 矩阵白名单（docs/12 §3.2）：三 ABI，其它值直接拒绝，避免静默产出怪 ABI 产物。
if(NOT ANDROID_ABI MATCHES "^(arm64-v8a|armeabi-v7a|x86_64)$")
  message(FATAL_ERROR
    "不支持的 ANDROID_ABI='${ANDROID_ABI}'。允许值见 docs/12_构建与打包.md §3.2："
    " arm64-v8a / armeabi-v7a / x86_64。")
endif()

# ---------------------------------------------------------------------------
# NDK 版本硬断言：低于 r28 (28.0.13004108) 拒绝构建
# ---------------------------------------------------------------------------
# 原因：r28 起默认 16 KB 页对齐（docs/12 §3.5 为硬门禁）。旧 NDK 产出的 .so
# 无法通过 zipalign -P 16 校验，且 Google Play 自 2025-11 起对 targetSdk 35+
# 强制 16 KB 页对齐。
#
# ANDROID_NDK_VERSION 由 NDK 自带 toolchain 计算；若用户直接用本文件（未先
# include NDK 官方 toolchain），该变量可能为空 —— 此时从 source.properties 兜底读取。
if(NOT DEFINED ANDROID_NDK_VERSION OR ANDROID_NDK_VERSION STREQUAL "")
  if(DEFINED ANDROID_NDK AND EXISTS "${ANDROID_NDK}/source.properties")
    file(STRINGS "${ANDROID_NDK}/source.properties" _ndk_rev_line
         REGEX "^Pkg.Revision[ \t]*=")
    if(_ndk_rev_line MATCHES "=[ \t]*([0-9]+\\.[0-9]+\\.[0-9]+)")
      set(ANDROID_NDK_VERSION "${CMAKE_MATCH_1}")
    endif()
  endif()
endif()

if(ANDROID_NDK_VERSION AND ANDROID_NDK_VERSION VERSION_LESS 28.0.13004108)
  message(FATAL_ERROR
    "NDK ${ANDROID_NDK_VERSION} 不支持 16KB 页对齐产物。"
    "要求 NDK >= r28 (28.0.13004108)，见 docs/12_构建与打包.md §3.1。")
endif()

# ---------------------------------------------------------------------------
# 16 KB 页对齐（docs/12 §3.5）
# ---------------------------------------------------------------------------
# NDK r28 已默认启用 -Wl,-z,max-page-size=16384；此处显式写出以防工具链默认值
# 被改动，并让意图在代码中可见（顶层 CMakeLists.txt 亦会 add_link_options）。
set(RECON_LINK_ALIGN_FLAGS "-Wl,-z,max-page-size=16384" CACHE STRING "" FORCE)

# ---------------------------------------------------------------------------
# 编译/链接选项（docs/12 §1.1 编译器与选项表）
# ---------------------------------------------------------------------------
set(CMAKE_POSITION_INDEPENDENT_CODE ON)      # Android 共享库必需

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)                # -std=c++17，非 gnu++17

# 隐藏默认可见性，只导出 RECON_API 标注的符号（docs/12 §1.4 跨 .so 约束）
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# 确定性构建（docs/12 §1.1）
# 去掉产物中的构建绝对路径；SOURCE_DATE_EPOCH 由 CI 显式注入。
if(NOT DEFINED RECON_SOURCE_PREFIX_MAP)
  set(RECON_SOURCE_PREFIX_MAP "-ffile-prefix-map=${CMAKE_CURRENT_LIST_DIR}/../..=.")
endif()
