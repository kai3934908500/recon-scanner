// swift-tools-version:5.9
//
// Package.swift — Recon iOS 侧依赖描述（由 tools/gen_ios_project.py 生成）
//
// 契约：docs/12_构建与打包.md §4.1（Swift 5.0 / Xcode 15.4）、
//       §4.6（最低部署 iOS 15.0）、§1.1 B-1（CMake 是唯一构建入口）。
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
