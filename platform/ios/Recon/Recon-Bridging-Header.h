// SPDX-License-Identifier: BSD-3-Clause
//
// Recon-Bridging-Header.h — Swift → C 的单一桥接头。
//
// 契约：docs/12_构建与打包.md §4.2（Swift / C++ 混编配置）。
//
// 为什么只放 C 头、不放 C++ 头：
//   bridging header 是**编译器喂给每一个 Swift 文件**的隐式头。往里面塞 C++
//   头会让整个 Swift 编译单元退化成 ObjC++ 语境，并且把 C++ 类型直接泄进
//   Swift —— 破坏「C ABI 是唯一跨语言边界」（docs/00 §7.1）。
//   ObjC++ 平台实现（`.mm`，如 CaptureStateBridge.mm）**自行** `#include`
//   它需要的 C++ 头，不经由本文件。
//
// ── 与 docs/12 §4.2 的一处偏差（已登记，非疏漏）───────────────────────
// 该节示例含 `recon/types.h` 与 `recon/status.h` 两行。这两个文件**在仓库中
// 不存在**（实测 `core/include/recon/` 下无此二文件），且 `recon_c_api.h`
// 已刻意自包含全部 POD 类型与状态码。故此处只包含真实存在的那一个头。
// 详情与处置理由见同目录 `ReconCore/include/module.modulemap` 的注释。
//
// 与 module.modulemap 的分工（docs/12 §4.2 表）：
//   · module.modulemap —— 供 Swift 侧 `import ReconCore`，提供**类型可见性**；
//   · 本文件           —— 提供**顶层函数声明可见性**。
//   两者互补，缺一不可。
//
// ── Objective-C++ 桥类的可见性（必须，否则真机构建失败）─────────────────
//
// Swift 侧直接使用了两个 ObjC 类：`ReconCaptureStateMachine`（ScanSessionModel.swift）
// 与 `ReconImuRingBuffer` / `ReconDepthConverter`（FramePipeline.swift 等）。
// ObjC 类**只有**在 bridging header 里被 `#import` 才对该 target 的全部 Swift
// 文件可见 —— 仅把 `.mm` 加进 target 源码是不够的。
//
// 这两行是**必需的**，不是可选装饰：删掉后 Swift 会报
// "cannot find 'ReconCaptureStateMachine' in scope"，而本项目既有的静态闸门
// （G7-b 只校验 `#include` 指向的 C 头是否存在）**查不出**这类漏网 ——
// 该漏网已由 G7-e（本 header 必须 import 所有被 Swift 用到的桥头）补齐，见
// tools/check_ios_contract.py。
//
// ── ⚠️ 本文件**不得**写 `#pragma once`（Xcode 26 真编译实证，CI #33）────────
// 曾有一行 `#pragma once`（从初始提交就有，但**从未被真编译过** —— 见下）。
// Swift 编译器处理 bridging header 时是把它当作**翻译单元的主文件**编译
// （日志：`SwiftGeneratePch ... Compiling bridging header`，等价 `-x objective-c-header`），
// 此时 Clang 报 **`error: #pragma once in main file`** → 整个 App 构建失败。
//
// 为什么此前没暴露：iOS CI 的 `build-app` job 依赖 `build-xcframework`，
// 而 macos-26 遭遇平台容量故障（CI #29..#32 的 `build-xcframework` 四次 cancelled）
// → `build-app` 一直处于 **`skipped`**，**从未真编译过 App**。CI #33 首次跑起来
// 即报了这条。⇒ 同缺口 AM/AX：**「从未执行过」的代码，其错误只能等首次真跑**。
//
// 修法 = **删除**该行：bridging header 只经由 `SWIFT_OBJC_BRIDGING_HEADER`
// 被编译器作主文件处理，**不会**被别的文件 `#include`，故 include guard 本就无意义。

#include "recon/recon_c_api.h"

// ObjC++ 桥头（各自内部 `#include` 其需要的 C++ 头，不经由本文件传播 C++ 类型）。
#import "CaptureStateBridge.h"
#import "PlatformCaptureBridge.h"

// YUV→BGRA8 转换门面（docs/11 §693 IOS-AR-5）。
// 被 `FrameConverter.bgra8(from:)` 调用 —— 只有在此 `#import` 才对该 target 的
// 全部 Swift 文件可见（删掉后真机报 "cannot find 'ReconYuvConverter' in scope"，
// 由 G7-e 静态守护）。
#import "MetalComputeBackend.h"
