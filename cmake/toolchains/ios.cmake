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
#
# ⚠️ 判定源必须是 CMAKE_OSX_SYSROOT，不能只靠自定义的 RECON_IOS_SIMULATOR。
#
#   本文件会被 include **两次**：主工程一次；CMake 做「编译器 ABI 探测」时创建的
#   try_compile 子工程再一次（CMakeDetermineCompilerABI.cmake）。子工程有全新的
#   CMakeCache.txt，CMake 只把 CMAKE_TRY_COMPILE_PLATFORM_VARIABLES 表里的变量
#   复制过去，Apple 平台上该表含 CMAKE_OSX_ARCHITECTURES / CMAKE_OSX_SYSROOT /
#   CMAKE_OSX_DEPLOYMENT_TARGET，**而自定义的 -DRECON_IOS_SIMULATOR=ON 不在表里**。
#   只认自定义变量 → 子工程里回落到 OFF → 把模拟器 slice 误判成真机 slice →
#   撞上下面「真机只允许 arm64」的断言 → 配置直接失败。CI 上的实际表现就是
#   「配置模拟器 slice」这一步报 ios.cmake 真机分支的错（第一次 configure 的前
#   两行 -- The C compiler identification / -- Detecting C compiler ABI info 正常，
#   失败发生在 ABI 探测的 try_compile 里）。
#
#   CMAKE_OSX_SYSROOT 是被转发的那一类，且 docs/12 §4.7 给模拟器的标准命令本来就是
#   直接传 -DCMAKE_OSX_SYSROOT=iphonesimulator（根本不传 RECON_IOS_SIMULATOR）——
#   以 sysroot 为判定源即与该文档的正式接口对齐。
#
#   优先级：显式 -DRECON_IOS_SIMULATOR=... 优先；未显式给定时，由 CMAKE_OSX_SYSROOT
#   推断。子工程里自定义变量不可见 → 走推断；主工程里用户传了 -D → 走显式值
#   （主工程 configure 开头 CMAKE_OSX_SYSROOT 还没设置，不能反过来以它为准，
#   否则会把用户显式传的 ON 冲掉）。
if(NOT DEFINED RECON_IOS_SIMULATOR)
  if(DEFINED CMAKE_OSX_SYSROOT AND CMAKE_OSX_SYSROOT STREQUAL "iphonesimulator")
    set(_recon_ios_simulator_default ON)
  else()
    set(_recon_ios_simulator_default OFF)
  endif()
  set(RECON_IOS_SIMULATOR "${_recon_ios_simulator_default}" CACHE BOOL
      "构建 iphonesimulator slice（未显式指定时由 CMAKE_OSX_SYSROOT 推断）")
  unset(_recon_ios_simulator_default)
endif()

if(RECON_IOS_SIMULATOR)
  set(CMAKE_OSX_SYSROOT iphonesimulator CACHE STRING "" FORCE)
  set(RECON_IOS_SLICE_NAME "ios-arm64_x86_64-simulator" CACHE STRING "" FORCE)
else()
  set(CMAKE_OSX_SYSROOT iphoneos CACHE STRING "" FORCE)
  set(RECON_IOS_SLICE_NAME "ios-arm64" CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# ⚠️ CMAKE_OSX_DEPLOYMENT_TARGET：**必须**与 RECON_IOS_DEPLOYMENT_TARGET 同源
# ---------------------------------------------------------------------------
# 背景（第四十一轮 CI 的实测事故，勿删）：
#   build-app 第一次推进到链接阶段时刷出上百条
#     ld: warning: object file (...png_encoder.o) was built for newer
#                 'iOS-simulator' version (18.5) than being linked (15.0)
#   —— App 侧（XcodeGen 容器）的 IPHONEOS_DEPLOYMENT_TARGET = 15.0（docs/12 §4.6），
#   而静态库切片里的每个 `.o` 都按 **SDK 默认部署目标 18.5** 编出来。
#
# 机理：`CMAKE_SYSTEM_VERSION` 只描述"目标系统版本"，用于 CMake 的平台判定；
#   真正进编译命令行的是 **`CMAKE_OSX_DEPLOYMENT_TARGET`**（clang 的
#   `-target arm64-apple-ios15.0` / `-mios-version-min`）。只设前者、不设后者时，
#   CMake 的 Xcode generator 会把后者留空 → Xcode 交出一个 SDK 默认值。
#   CI 日志里能直接看到这个漏配：编译命令是 `-target arm64-apple-ios18.5`
#   （18.5 正是 iPhoneOS18.5.sdk 的默认），而 App 链接命令是
#   `-target arm64-apple-ios15.0-simulator`。
#
# 后果不只是噪声：某个 `.o` 的 min-version 高于链接目标时，Apple 链接器**允许**
#   它通过，但可能带出只在更高系统版本可用的弱符号绑定 —— 即"本机编得过、
#   装到 iOS 15/16 真机上才炸"的那类问题。故必须与部署目标同源。
#
# ⚠️ 取值方式：**只在调用方未给时兜底**，与上面 RECON_IOS_SIMULATOR /
#   CMAKE_OSX_SYSROOT 的写法逐字对称。刻意**不用** `CACHE ... FORCE`：
#   docs/12 §4.3 的正式命令行本身就传
#   `-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0`，FORCE 会把它**静默覆盖** ——
#   那等于"判据可被工具覆盖"，与 shader_parity.py 拒绝收参是同一类问题
#   （见 §6.2 任务 3 的说明）。
#
#   为什么兜底放在 `if(NOT DEFINED ...)` 而不是"干脆删掉兜底、只信调用方"：
#   `-DCMAKE_OSX_DEPLOYMENT_TARGET` 会被**转发进 try_compile 子工程**（见上方
#   判定源说明），而 CI 的 configure 命令没有传它 —— 子工程便会退回空值并按
#   SDK 默认编 ABI 探测程序，于是同一个 CMake 配置里出现两种 ABI 假设。
#   兜底一次、全链路同源才是正确的收敛点。
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR
   CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "${RECON_IOS_DEPLOYMENT_TARGET}" CACHE STRING
      "iOS 最低部署版本（docs/12 §4.6；未显式指定时跟随 RECON_IOS_DEPLOYMENT_TARGET）")
  message(STATUS "recon: CMAKE_OSX_DEPLOYMENT_TARGET 未显式指定，"
                 "按 RECON_IOS_DEPLOYMENT_TARGET 兜底为 ${RECON_IOS_DEPLOYMENT_TARGET}")
elseif(NOT CMAKE_OSX_DEPLOYMENT_TARGET VERSION_EQUAL RECON_IOS_DEPLOYMENT_TARGET)
  # 显式值与部署目标打架 → 硬失败，不静默取其一。
  # 这正是上面那条 `ld: warning` 的成因（两处事实来源各说各话），故在此挡住。
  message(FATAL_ERROR
    "CMAKE_OSX_DEPLOYMENT_TARGET='${CMAKE_OSX_DEPLOYMENT_TARGET}' 与 "
    "RECON_IOS_DEPLOYMENT_TARGET='${RECON_IOS_DEPLOYMENT_TARGET}' 不一致。\n"
    "静态库切片的 min-version 与 App 的 IPHONEOS_DEPLOYMENT_TARGET 必须是同一个值，"
    "否则链接期会出现\n"
    "  ld: warning: object file ... was built for newer 'iOS-simulator' version "
    "(X) than being linked (Y)\n"
    "并把「只在更高系统版本可用的弱符号」带进按更低版本链接的产物里。\n"
    "见 docs/12_构建与打包.md §4.6「与静态库切片的关系」。")
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
