# SPDX-License-Identifier: BSD-3-Clause
#
# cmake/toolchains/ios.cmake — iOS 交叉编译工具链。
#
# 契约：docs/12_构建与打包.md §4.1（版本要求）、§4.3（CMake → Xcode 与 xcframework
#       产物）、§4.6（最低部署版本）、§4.7（设备架构）。
#
# 用法（须在 macOS 上执行；Xcode 是唯一 iOS 编译入口）：
#
#   # 一：真机 arm64 slice
#   cmake -S . -B build/ios/device -G Xcode \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ios.cmake \
#         -DCMAKE_OSX_ARCHITECTURES=arm64
#
#   # 二：模拟器 slice（Apple Silicon 与 Intel 各一份，lipo 前分别构建）
#   cmake -S . -B build/ios/simulator -G Xcode \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ios.cmake \
#         -DRECON_IOS_SIMULATOR=ON \
#         -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
#
# 注意：本文件在 CMake 的"工具链预处理"阶段被 include，此时 project() 尚未调用，
# 因此只能使用 set()/if() 等最小语法，不得使用需要 project 上下文的命令。

# ---------------------------------------------------------------------------
# 目标平台
# ---------------------------------------------------------------------------
set(CMAKE_SYSTEM_NAME iOS)

# CMAKE_SYSTEM_VERSION 即 IPHONEOS_DEPLOYMENT_TARGET（docs/12 §4.6 结论：iOS 15.0）。
# 选 15.0 的三条理由（12 §4.6 原文）：sceneDepth 支持面完整、ARFrame.sceneDepth
# 语义稳定（沿光轴 z，与 02 §5.5 F-1 一致）、SwiftUI 生命周期 API 完整。
if(NOT DEFINED RECON_IOS_DEPLOYMENT_TARGET)
  set(RECON_IOS_DEPLOYMENT_TARGET "15.0")
endif()
set(CMAKE_SYSTEM_VERSION ${RECON_IOS_DEPLOYMENT_TARGET})

# 部署目标下限硬断言：低于 15.0 直接拒绝。
# 原因：15.0 之前 sceneDepth 支持面不完整、ARSession 回调频率不稳定，会直接
# 破坏 02 文档的时间同步精度预算（τ_pipeline）。若放宽会静默降低内核精度。
if(RECON_IOS_DEPLOYMENT_TARGET VERSION_LESS 15.0)
  message(FATAL_ERROR
    "RECON_IOS_DEPLOYMENT_TARGET='${RECON_IOS_DEPLOYMENT_TARGET}' 低于下限 15.0。"
    "见 docs/12_构建与打包.md §4.6：iOS 15.0 是本项目的最低部署版本，"
    "低于该值会破坏 sceneDepth 语义与时间同步精度。")
endif()

# ---------------------------------------------------------------------------
# 设备 / 模拟器 slice 选择（docs/12 §4.3）
# ---------------------------------------------------------------------------
# 真机 slice 走 iphoneos SDK；模拟器 slice 走 iphonesimulator SDK。两者必须分别
# 构建后再由 xcodebuild -create-xcframework 合并 —— xcframework 要求的两个 slice
# 在本工具链里就是两次独立的 CMake 配置。
option(RECON_IOS_SIMULATOR "构建 iphonesimulator slice（默认 iphoneos 真机 slice）" OFF)

if(RECON_IOS_SIMULATOR)
  set(CMAKE_OSX_SYSROOT iphonesimulator CACHE STRING "" FORCE)
  set(RECON_IOS_SLICE_NAME "ios-arm64_x86_64-simulator" CACHE STRING "" FORCE)
else()
  set(CMAKE_OSX_SYSROOT iphoneos CACHE STRING "" FORCE)
  set(RECON_IOS_SLICE_NAME "ios-arm64" CACHE STRING "" FORCE)
endif()

# 架构白名单（docs/12 §4.7）：真机 arm64 only；模拟器允许 arm64 + x86_64。
# 明确不做 armv7 / arm64_32（iOS 15 起已无 32 位设备）；不做 arm64e 单独切片
# （arm64e 与 arm64 二进制兼容，Xcode 自动处理）。
if(NOT DEFINED CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL "")
  if(RECON_IOS_SIMULATOR)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
  else()
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
  endif()
endif()

if(RECON_IOS_SIMULATOR)
  if(NOT CMAKE_OSX_ARCHITECTURES MATCHES "^(arm64;x86_64|x86_64;arm64|arm64|x86_64)$")
    message(FATAL_ERROR
      "模拟器 slice 的 CMAKE_OSX_ARCHITECTURES='${CMAKE_OSX_ARCHITECTURES}' 不合法。"
      "允许值：arm64;x86_64（见 docs/12_构建与打包.md §4.7）。")
  endif()
else()
  if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    message(FATAL_ERROR
      "真机 slice 只允许 arm64，当前为 '${CMAKE_OSX_ARCHITECTURES}'。"
      "iOS 15 起无 32 位设备，见 docs/12_构建与打包.md §4.7。")
  endif()
endif()

# ---------------------------------------------------------------------------
# 编译/链接选项（docs/12 §1.1 编译器与选项表）
# ---------------------------------------------------------------------------
# iOS 静态库同样要求位置无关代码（被 App 可执行文件链接）。
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# C++17（docs/12 §1.1）：与 NDK r28 clang 17 保持同一标准，避免双端行为漂移。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)                # -std=c++17，非 gnu++17

# 隐藏默认可见性，只导出 RECON_API 标注的符号。
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# 提前启用 ObjC++：iOS 侧的 C ABI 桥（platform/ios/Recon/CaptureStateBridge.mm）
# 依赖它，在 toolchain 阶段声明可避免 project() 后再改语言列表带来的配置漂移。
set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# 零网络硬约束（00 R-LOCAL / 11 §1.6）
# ---------------------------------------------------------------------------
# 本产品为纯本地内核：不得链接任何网络框架。此变量由顶层 CMakeLists.txt 的
# iOS 块读取，用于对最终 xcframework 做 otool -L 校验（docs/12 §4.3 XC 校验项）。
set(RECON_IOS_FORBIDDEN_LIBS
    "CFNetwork;Network;WebKit;Security"
    CACHE STRING "" FORCE)

# ---------------------------------------------------------------------------
# 确定性构建（docs/12 §1.1）
# ---------------------------------------------------------------------------
# 去掉产物中的构建绝对路径；SOURCE_DATE_EPOCH 由 CI 显式注入。
if(NOT DEFINED RECON_SOURCE_PREFIX_MAP)
  set(RECON_SOURCE_PREFIX_MAP "-ffile-prefix-map=${CMAKE_CURRENT_LIST_DIR}/../..=.")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
