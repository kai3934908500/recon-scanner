#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""gen_ios_project.py — 生成 iOS 的 Xcode 工程与 SPM 包描述。

用途
----
iOS 的构建入口是 Xcode（docs/12 §4.1：Xcode 15.4 / iOS 17 SDK）。但本仓库
的**唯一构建入口是 CMake**（docs/12 §1.1 B-1：「任何平台不得绕过 CMake 直接
编译 `core/` 源码」）。因此本脚本不生成「编译 core 的 Xcode 工程」，而是生成：

  1. `platform/ios/Package.swift` —— Swift Package 描述。**只声明
     `ReconCore.xcframework` 二进制目标**，让 UI 层以「消费 C ABI」的方式接入。
  2. `platform/ios/Recon.xcconfig` —— 把 docs/12 §4.2 的 Xcode Build Settings
     逐条写成 xcconfig，避免在 .pbxproj 里手改（.pbxproj 是不可审查的巨大 XML，
     任何手改都无法在 diff 中复核）。
  3. `platform/ios/Container/project.yml` + `README.md` —— App 容器定义。
     容器（把 `ui/ios/` 与 `platform/ios/Recon/` 两个源码树拢进一个 App
     target）交给 macOS 侧生成，理由见 §「为什么容器不在这里生成」。

为什么用 xcconfig 而不是直接生成 .pbxproj
----------------------------------------
生成 .pbxproj 需要解析 Apple 的私有格式，一旦生成就产生一个数百 KB、diff
不可读的文件，且 Xcode 版本升级常导致格式漂移。用 xcconfig 把**规格值**固化，
.pbxproj 由 Xcode 自己在 macOS 上生成（或由 CI 用 `xcodegen` 生成），
两者的分工是：本仓库管「值」，macOS 管「容器」。

为什么容器不在这里生成（一次返工记录，值得留档）
----------------------------------------
初版 `Package.swift` 试图用一个 `.target(path: ".", sources: ["../Recon/Sources"])`
同时编译 UI 与桥接层。**这是错的**：SwiftPM 明令禁止一个 target 的源码位于
**包根目录之外**（否则 `swift build` 会报 "target ... has sources located
outside the package directory"）。而 UI 层在仓库里位于 `ui/ios/`，不在
`platform/ios/` 之内 —— 想用 SPM 硬凑，只能往 `platform/ios/Recon/Sources/`
里塞指向 `ui/ios/...` 的符号链接，那是「用花招掩盖结构问题」。
再者初版把库名写成 `ReconUI`，而 C ABI 头（`recon_c_api.h`）里模块名与
`docs/12 §4.3` 的产物名都是 `ReconCore` —— 名字对不上，接入方会 import 错。

因此改为**由 XcodeGen 在 macOS 上生成容器**（`Container/project.yml`），
它天然支持跨目录源码引用，且生成物是 `.xcodeproj`（本身不进仓库，属 CI 产物）。

产物不提交仓库（docs/12 §4.3 XC-4：xcframework 不进仓库）。

退出码：0 成功；1 写入失败。
"""

from __future__ import annotations

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IOS = os.path.join(ROOT, "platform", "ios")

# docs/12 §4.6
DEPLOYMENT_TARGET = "15.0"
SWIFT_VERSION = "5.0"

# docs/12 §4.2 关键 Build Settings（逐条转写）
BUILD_SETTINGS = {
    "IPHONEOS_DEPLOYMENT_TARGET": DEPLOYMENT_TARGET,
    "SWIFT_VERSION": SWIFT_VERSION,
    "ENABLE_BITCODE": "NO",                       # §4.5 Bitcode 已废弃
    "SKIP_INSTALL": "YES",                        # framework 通过 xcframework 分发
    "PRODUCT_NAME": "Recon",
    "TARGETED_DEVICE_FAMILY": "1",                # 仅 iPhone（1 = iPhone，2 = iPad）
    "SUPPORTS_MACCATALYST": "NO",
    "SUPPORTS_MAC_DESIGNED_FOR_IPHONE_IPAD": "NO",
    "SUPPORTS_UIKITFORMAC": "NO",
    "ONLY_ACTIVE_ARCH": "NO",                     # 归档需全架构
    "SWIFT_TREAT_WARNINGS_AS_ERRORS": "YES",      # 零警告标准（本项目铁律）
    "GCC_TREAT_WARNINGS_AS_ERRORS": "YES",
    "CLANG_ENABLE_OBJC_ARC": "YES",
    "CLANG_CXX_LANGUAGE_STANDARD": "c++17",       # docs/12 §1.1
    "CLANG_CXX_LIBRARY": "libc++",
    "ENABLE_USER_SCRIPT_SANDBOXING": "YES",
    "ENABLE_MODULE_VERIFIER": "NO",               # 第三方校准常误报，本项目无第三方
    "CODE_SIGN_STYLE": "Automatic",
    "DEAD_CODE_STRIPPING": "YES",
    "COPY_PHASE_STRIP": "YES",
}


def write(path: str, content: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    print(f"  写入 {os.path.relpath(path, ROOT)}")


def gen_xcconfig() -> None:
    lines = [
        "// Recon.xcconfig — iOS 构建设置（由 tools/gen_ios_project.py 生成）",
        "//",
        "// 契约：docs/12_构建与打包.md §4.2（Xcode Build Settings 关键项）、",
        "//       §4.5（Bitcode）、§4.6（最低部署版本 = 15.0）、§4.7（arm64 only）。",
        "//",
        "// ⚠️ 本文件由脚本生成，请不要手改 —— 改 tools/gen_ios_project.py 后重跑。",
        "//    理由：规格值需要可审查、可在 diff 中复核；手改会与生成器漂移。",
        "",
    ]
    for k, v in BUILD_SETTINGS.items():
        lines.append(f"{k} = {v}")
    lines.append("")
    lines.append("// docs/11 §1.6：零网络 —— 仅链接白名单系统框架。")
    lines.append("// 该列表只作声明；真正的校验由 tools/check_ios_contract.py 与 CI 的")
    lines.append("// otool -L 步骤执行（见 docs/12 §4.3 XC 校验项）。")
    lines.append("OTHER_LDFLAGS = $(inherited) -framework Foundation -framework UIKit "
                 "-framework AVFoundation -framework CoreMedia -framework CoreMotion "
                 "-framework Metal -framework MetalKit -framework ARKit -framework Photos")
    lines.append("")

    write(os.path.join(IOS, "Recon.xcconfig"), "\n".join(lines))


def gen_package_swift() -> None:
    content = f'''// swift-tools-version:5.9
//
// Package.swift — Recon iOS 侧依赖描述（由 tools/gen_ios_project.py 生成）
//
// 契约：docs/12_构建与打包.md §4.1（Swift 5.0 / Xcode 15.4）、
//       §4.6（最低部署 iOS {DEPLOYMENT_TARGET}）、§1.1 B-1（CMake 是唯一构建入口）。
//
// ── 边界（必读）────────────────────────────────────────────────────────
// 本 Package **不编译 `core/`**。核心库的编译入口是 CMake（B-1），产物为
// `ReconCore.xcframework`（docs/12 §4.3），由本 Package 以 `.binaryTarget`
// 引用。
//
// 它**也不编译 UI 层**。UI 源码在 `ui/ios/`，位于本包根目录（`platform/ios/`）
// 之外 —— SwiftPM 禁止 target 的源码位于包根之外，硬凑只能靠符号链接，那是
// 掩盖结构问题。UI + 桥接层的 App 容器由 `Container/project.yml`（XcodeGen，
// 在 macOS 上运行）生成，见 tools/gen_ios_project.py 文件头的说明。
//
// 因此本 Package 的职责是**单一且清晰**的：把 xcframework 变成一个可
// `import ReconCore` 的依赖。
//
// 缺产物时 SPM 报错而非静默跳过 —— 符合「不静默降级」的项目铁律。
//

import PackageDescription

let package = Package(
    name: "Recon",
    platforms: [
        .iOS(.v15),          // docs/12 §4.6
    ],
    products: [
        // 名字必须与 C ABI 头里的模块名、docs/12 §4.3 的产物名一致：
        // Swift 侧 `import ReconCore` 才能命中。
        .library(name: "ReconCore", targets: ["ReconCore"]),
    ],
    targets: [
        // 核心静态库（CMake 在 macOS 上构建后由 xcodebuild 合并为 xcframework）。
        // 路径相对包根：`platform/ios/build/ReconCore.xcframework`。
        .binaryTarget(
            name: "ReconCore",
            path: "build/ReconCore.xcframework"
        ),
    ]
)
'''
    write(os.path.join(IOS, "Package.swift"), content)


# XcodeGen 容器描述：把两个**物理分离**的源码树（ui/ios 与 platform/ios/Recon）
# 拢进一个 App target。由 macOS 侧的 CI 跑 `xcodegen generate` 生成 .xcodeproj。
PROJECT_YML = '''# project.yml — Recon iOS App 容器定义（XcodeGen）
#
# 契约：docs/12_构建与打包.md §4.2（混编配置）、§4.6（部署目标 15.0）。
#
# 为什么需要本文件：UI 层（ui/ios/）与桥接层（platform/ios/Recon/）在仓库里是
# **物理分离**的两个目录树，而 SwiftPM 不允许 target 源码位于包根之外，无法用
# Package.swift 把它们拢成一个可构建的 App。XcodeGen 支持任意路径的源码引用，
# 且生成物（.xcodeproj）不进仓库（属 CI 产物）。
#
# ── ⚠️ 生成命令必须带 --project-root 与 --project（一次真实事故，必读）──────
#   XcodeGen 解析 spec 中的相对路径，**默认基准是 spec 文件所在目录**
#   （即 platform/ios/Container/），而本文件里的路径全部是按**仓库根**写的。
#   若不显式指定，会同时坏掉两件事：
#     ① `sources`/`framework` 找不到源码与 xcframework（路径基准错了一层）；
#     ② `SRCROOT` 变成 Container/，于是下面所有 `$(SRCROOT)/core/include`
#        之类全部指向不存在的位置（SRCROOT = .xcodeproj 所在目录）。
#   正确调用（在**仓库根**执行；--project 与 --project-root 都指向仓库根）：
#
#     xcodegen generate --spec platform/ios/Container/project.yml \\
#       --project . --project-root .
#
#   这样 .xcodeproj 落在仓库根 → SRCROOT = 仓库根 → 下面所有路径与
#   `$(SRCROOT)/...` 同时成立。CI 见 .github/workflows/ios.yml 的 build-app job。
#
# ⚠️ 本文件由 tools/gen_ios_project.py 生成，请勿手改。

name: Recon
options:
  deploymentTarget:
    iOS: "15.0"
  createIntermediateGroups: true

configs:
  Debug: debug
  Release: release

settings:
  base:
    SWIFT_VERSION: "5.0"                       # docs/12 §4.1
    IPHONEOS_DEPLOYMENT_TARGET: "15.0"         # docs/12 §4.6
    TARGETED_DEVICE_FAMILY: "1"                # 仅 iPhone
    SWIFT_TREAT_WARNINGS_AS_ERRORS: "YES"      # 零警告标准
    GCC_TREAT_WARNINGS_AS_ERRORS: "YES"
    CLANG_CXX_LANGUAGE_STANDARD: "c++17"
    CLANG_CXX_LIBRARY: "libc++"
    CLANG_ENABLE_MODULES: "YES"
    ENABLE_BITCODE: "NO"                       # docs/12 §4.5
    ONLY_ACTIVE_ARCH: "NO"
    # 脚本阶段沙箱必须关掉（如实声明的例外，不是图省事）：
    #   「Compile Metal Shaders」phase 要调用 tools/build_metal_shaders.sh，
    #   该脚本会 `find shaders/metal/*.metal` 并写产物到 $(BUILT_PRODUCTS_DIR)。
    #   开启沙箱（Xcode 15+ 的默认）会将其限制在声明的 in/out 范围内，而脚本
    #   需要读仓库根的源码树 —— 表现为 "Sandbox: bash(...) deny file-read-data"。
    #   替代方案是在 phase 里逐文件声明 inputFiles，但脚本用 find 动态收集，
    #   手抄清单 = 第二事实来源（必然漂移，见 MEMORY.md）。故明确关闭。
    #   ⚠️ Recon.xcconfig 里该键为 YES，但 xcconfig **未接入** project.yml
    #      （见 configFiles 说明），故这里必须独立设置。
    ENABLE_USER_SCRIPT_SANDBOXING: "NO"
    # UI 层引用 C ABI：经 bridging header（docs/12 §4.2）。
    SWIFT_OBJC_BRIDGING_HEADER: "platform/ios/Recon/Recon-Bridging-Header.h"
    # core 的 C ABI 头搜索路径。
    #
    # ⚠️ 必须同时包含 `platform/common/include` 与 `shaders/common`：
    #   · 前者：`platform/ios/Recon/*.mm` 直接 `#include "recon_platform/xxx.h"`
    #     复用平台无关规则本体（DepthConverter / ImuRingBuffer / YuvConverter）。
    #   · 后者：`MetalComputeBackend.mm` `#include "gpu_algo.h"` 复用 GPU 参数块
    #     的**唯一事实来源**（否则要在 host 侧手抄一份，必然漂移）。
    #   缺任一项 → 真机构建报 "file not found"，而本机（Windows）**查不出**。
    #   这些 `$(SRCROOT)/...` 只在 SRCROOT = 仓库根时成立（见文件头的调用说明）。
    HEADER_SEARCH_PATHS:
      - "$(SRCROOT)/core/include"
      - "$(SRCROOT)/platform/ios/ReconCore/include"
      - "$(SRCROOT)/platform/common/include"
      - "$(SRCROOT)/shaders/common"

targets:
  Recon:
    type: application
    platform: iOS
    sources:
      # 两个物理分离的源码树，一次拢进来。
      - path: ui/ios/Recon/Sources
      - path: platform/ios/Recon
        excludes:
          - "Info.plist"
          - "Recon-Bridging-Header.h"   # 由 SWIFT_OBJC_BRIDGING_HEADER 指定
      - path: ui/ios/Recon/Resources    # locales/*.json
    settings:
      base:
        PRODUCT_BUNDLE_IDENTIFIER: "cn.qingfeng.recon"
        PRODUCT_NAME: Recon
        GENERATE_INFOPLIST_FILE: "NO"
        INFOPLIST_FILE: "platform/ios/Recon/Info.plist"
        CODE_SIGN_STYLE: Automatic
    dependencies:
      # 核心静态库：CMake 构建 + xcodebuild -create-xcframework 的产物。
      # 由 CI 在 xcodegen 之前放到此路径（docs/12 §4.3）。
      - framework: platform/ios/build/ReconCore.xcframework
        embed: false
        codeSign: false
      # 系统框架：用 XcodeGen 的显式 SDK 依赖，而不是靠 Swift `import` 的隐式
      # 自动链接 —— MetalComputeBackend.mm 是 ObjC++ 层，其 Metal/CoreVideo
      # 符号必须显式链接（Swift 的自动链接只覆盖 Swift 侧 import 的模块）。
      - sdk: Metal.framework
      - sdk: MetalKit.framework
      - sdk: CoreVideo.framework
      - sdk: AVFoundation.framework
      - sdk: CoreMedia.framework
      - sdk: CoreMotion.framework
      - sdk: ARKit.framework
      - sdk: Photos.framework
    # 「Compile Metal Shaders」Build Phase（docs/12 §4.4 的挂载表）。
    # 用 preBuildScripts → 位于「Compile Sources」之前，符合该表要求。
    # 脚本自己按 OUT_DIR(=BUILT_PRODUCTS_DIR/Contents/Resources) 定位产物，
    # 并由 `RECON_REQUIRE_METAL=1` 保证「缺工具链即硬失败」而**不伪造空产物**
    # （空 default.metallib 会被 makeDefaultLibrary() 当作「库在但无函数」，
    #  比「找不到库」更难查 —— 见 MEMORY.md「静默丢弃最难查」）。
    preBuildScripts:
      - name: Compile Metal Shaders
        script: |
          set -euo pipefail
          export RECON_REQUIRE_METAL=1
          # SDK 跟随当前构建平台：真机用 iphoneos、模拟器用 iphonesimulator。
          # 脚本内 `xcrun -sdk "$SDK" metal` 会按此选 SDK；写死 iphoneos 会让
          # 模拟器构建拿到错的 SDK（表现为链接/加载期问题，而非编译期报错）。
          export RECON_IOS_SDK="${PLATFORM_NAME}"
          # 部署目标：它决定 `-target` 三元组，进而决定 `__METAL_VERSION__`
          # （见 tools/build_metal_shaders.sh 的长论证）。**必须传** ——
          # 不传则脚本回落默认值，而默认值与 target 的 Build Settings 是
          # 两处事实来源，迟早漂移（MEMORY.md）。
          # ⚠️ 刻意**读 Xcode 注入的构建设置**而不是在此再写一遍 "15.0"：
          #    该变量与 target 的 IPHONEOS_DEPLOYMENT_TARGET 同源，
          #    改一处即全链路生效。为空时**不覆盖**（让脚本用自带默认值，
          #    并由脚本内的 `>= 15` 断言把关 —— fail-closed 而非静默降级）。
          if [ -n "${IPHONEOS_DEPLOYMENT_TARGET:-}" ]; then
            export RECON_IOS_DEPLOYMENT_TARGET="${IPHONEOS_DEPLOYMENT_TARGET}"
          fi
          export RECON_METALLIB_OUT_DIR="${BUILT_PRODUCTS_DIR}/${CONTENTS_FOLDER_PATH}/Resources"
          export RECON_METAL_AIR_DIR="${DERIVED_FILE_DIR}/metal-air"
          bash "${SRCROOT}/tools/build_metal_shaders.sh"
        basedOnDependencyAnalysis: false

# 显式 scheme：`xcodebuild -scheme Recon` 需要有共享 scheme。
# XcodeGen 会为 application target 自动生成 scheme，但**显式声明**才能保证
# scheme 名稳定（自动生成的命名规则随版本可能变），CI 里不能赌默认值。
schemes:
  Recon:
    build:
      targets:
        Recon: all
    run:
      config: Debug
    test:
      config: Debug
    profile:
      config: Release
    analyze:
      config: Debug
    archive:
      config: Release
'''


def gen_container() -> None:
    base = os.path.join(IOS, "Container")
    write(os.path.join(base, "project.yml"), PROJECT_YML)
    write(os.path.join(base, "README.md"), CONTAINER_README)


CONTAINER_README = '''# iOS App 容器

本目录不属于源码，而是**容器定义**：把仓库里**物理分离**的两个源码树
（`ui/ios/` 与 `platform/ios/Recon/`）拢成 Xcode 可构建的一个 App target。

## 为什么容器要单独放

- **SwiftPM 做不到**：SwiftPM 明令禁止 target 的源码位于包根之外。UI 层在
  `ui/ios/`，不在 `platform/ios/` 之内，无法被 `Package.swift` 直接包含。
- **`.pbxproj` 不该进仓库**：它是数百 KB 的不可审查 XML，任何手改都无法在
  diff 中复核；Xcode 版本升级还会导致格式漂移。
- **所以**：本仓库管「值」（`project.yml` 是纯文本、可审查、可 diff），
  由 macOS 侧的 XcodeGen 生成容器（`.xcodeproj`，CI 产物，不进仓库）。

## 生成方式（必须在 macOS 上）

**在仓库根目录**执行（两个 `--project*` 参数**不可省**）：

```bash
brew install xcodegen
cd <仓库根>
xcodegen generate --spec platform/ios/Container/project.yml \
  --project . --project-root .
```

### 为什么必须带 `--project . --project-root .`

XcodeGen 解析 spec 中相对路径的**默认基准是 spec 文件所在目录**
（即 `platform/ios/Container/`），而 `project.yml` 里的路径（`ui/ios/...`、
`platform/ios/build/ReconCore.xcframework`）全部是按**仓库根**写的。不指定会坏两件事：

1. `sources` / `framework` 的路径基准错一层 → 找不到源码与 xcframework；
2. `SRCROOT` 变成 `Container/` → `project.yml` 里所有
   `$(SRCROOT)/core/include` 之类的头文件搜索路径全部指向不存在的位置。

指定后 `.xcodeproj` 落在仓库根 → **SRCROOT = 仓库根** → 上述路径与
`$(SRCROOT)/...` 同时成立。生成物 `Recon.xcodeproj` 属 CI 产物，不进仓库。

## 前置依赖

`platform/ios/build/ReconCore.xcframework` 必须已存在（由 CMake + xcodebuild
生成，见 docs/12 §4.3）。**缺失时 xcodegen / Xcode 会报错，不会静默跳过**
—— 符合项目「不静默降级」铁律。

## 本文件的生成器

`tools/gen_ios_project.py`。改容器配置请改那个脚本，不要手改本目录下的文件。
'''



def main() -> int:
    print("生成 iOS 工程描述（契约：docs/12 §4）")
    print(f"  部署目标 {DEPLOYMENT_TARGET} · Swift {SWIFT_VERSION} · arm64 only")
    try:
        gen_xcconfig()
        gen_package_swift()
        gen_container()
    except OSError as e:
        print(f"错误：写入失败：{e}", file=sys.stderr)
        return 1
    print()
    print("下一步（必须在 macOS 上）：")
    print("  1) cmake -S . -B platform/ios/build/device -G Xcode \\")
    print("       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ios.cmake \\")
    print("       -DCMAKE_OSX_ARCHITECTURES=arm64")
    print("  2) cmake --build platform/ios/build/device --config Release")
    print("  3) 模拟器 slice 同上（加 -DRECON_IOS_SIMULATOR=ON "
          "-DCMAKE_OSX_ARCHITECTURES=\"arm64;x86_64\"）")
    print("  4) xcodebuild -create-xcframework ... → platform/ios/build/"
          "ReconCore.xcframework")
    print("  5) # 在**仓库根**执行（--project-root 不可省，见 project.yml 文件头）：")
    print("     xcodegen generate --spec platform/ios/Container/project.yml \\")
    print("       --project . --project-root .")
    return 0


if __name__ == "__main__":
    sys.exit(main())
