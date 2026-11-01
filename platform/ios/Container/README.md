# iOS App 容器

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
xcodegen generate --spec platform/ios/Container/project.yml   --project . --project-root .
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
