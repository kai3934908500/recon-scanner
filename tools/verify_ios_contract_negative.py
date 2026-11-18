#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_ios_contract_negative.py — check_ios_contract.py 的**负向注入**自检。

为什么需要这个文件
------------------
本项目反复吃过的坑（见 .workbuddy/memory/MEMORY.md）：

    「恒不触发」的判据等于没有判据（假闸门）。
    每写一条闸门判据，必须做一次**负向注入**证明它真的会拒收；
    否则无法区分「通过」与「根本没检查」。

本脚本针对 `tools/check_ios_contract.py` 中的判据做注入：

  G6 L10nKey ↔ zh-CN.json 键集合
      · 给 L10nKey 加一个 JSON 里没有的 case → 闸门须报「引用了不存在的键」
  G8 三级回退链可达性
      · 从 zh-CN.json 删一个已被引用的键          → 闸门须报「三级回退全部落空」
  G9 UI 文案去硬编码
      · 在视图里写一个无法反查的中文 L10n.s 原文  → 闸门须报「无法精确反查到键」
  G11 recon_status 逐值对等
      · 从 Swift 侧删一个错误码   → 闸门须报「有但 Swift 缺」
      · 从 C ABI 侧删一个错误码   → 闸门须报「Swift 有但 C ABI 无」
      · 给 Swift 一个无 rawValue 的 case → 闸门须报「rawValue 静态不可知」
  G12 ReconStageMap 自洽
      · 让一个段号同时出现在可达集与缺口集 → 闸门须报「自相矛盾」
      · 从可达集删一个段号（造成遗漏）     → 闸门须报「未被完整覆盖」
      · 把总段数改小（造成越界）           → 闸门须报「段号越界」
  G13 ReconStatus → L10nKey 映射穷举
      · 从 `messageKey(for:)` 删一个 case 引用 → 闸门须报「未在 ... 中被引用」
  G14 平台平面缓冲纪律
      · 对平面格式缓冲（capturedImage）用 `GetBaseAddress` → 闸门须报「静默丢弃全部帧」
      · 深度换算不校验 `kCVPixelFormatType_DepthFloat32`         → 闸门须报「未调用」
  G18 iOS 系统 API 幻觉清单
      · 把 imuMaxRate 的诚实写 0 改回不存在的 maximumGyroUpdateInterval
        → 闸门须报「不存在」（**复现 CI #20 的真实失败**）
      · 使用不存在的 maximumDeviceMotionUpdateInterval
      · 把 MTKView 正确的 depthStencilPixelFormat 改回不存在的 depthPixelFormat
        → 闸门须报「不存在」（**复现 CI #24 的真实失败**；同时覆盖
        `platform/ios/**/*.swift` 这一此前不在视野内的目录）
  G21 Swift/Clang 编译可见缺陷（CI #33 首次真编译暴露）
      · 把 `#pragma once` 加回 bridging header
        → 闸门须报「#pragma once」（**复现 CI #33 的真实失败之一**）
      · 把两参数 onChange 改回单参数 `{ t in`
        → 闸门须报「单参数」（**复现 CI #33 的真实失败之二**）
      · **反向**：把合法的两参数写法换个形参名 → 断言闸门**不报错**
        （防判据自身假红 —— G21 第一版实测踩到过）

⚠️ 本脚本会**临时修改** `platform/ios/Recon/ReconCore.swift`、
   `platform/ios/Recon/WireframeRenderer.swift`、
   `ui/ios/Recon/Sources/State/ReconStageMap.swift`、
   `ui/ios/Recon/Sources/Localization/Localization.swift`、
   `ui/ios/Recon/Resources/locales/zh-CN.json`、以及任一视图 `.swift` 再还原。
   安全措施：
     · 每次注入前先记录原文，还原用原文写回（不依赖任何 git）；
     · 任一注入失败（闸门没拒收）→ 退出码 3，CI 视为失败。

用法
----
    python tools/verify_ios_contract_negative.py
    python tools/verify_ios_contract_negative.py --keep-going

退出码
------
  0 = 全部注入都被正确拒收（闸门有效）
  1 = 环境/用法错误
  3 = 有注入**未被拒收** → 存在假判据
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GATE = os.path.join(ROOT, "tools", "check_ios_contract.py")
SWIFT_CORE = os.path.join(ROOT, "platform", "ios", "Recon", "ReconCore.swift")
STAGE_MAP = os.path.join(ROOT, "ui", "ios", "Recon", "Sources", "State",
                         "ReconStageMap.swift")
CONTROLLER = os.path.join(ROOT, "ui", "ios", "Recon", "Sources", "State",
                          "ReconSessionController.swift")
C_API_HEADER = os.path.join(ROOT, "core", "include", "recon", "recon_c_api.h")
LOCALIZATION = os.path.join(ROOT, "ui", "ios", "Recon", "Sources", "Localization",
                            "Localization.swift")
ZH_JSON = os.path.join(ROOT, "ui", "ios", "Recon", "Resources", "locales",
                       "zh-CN.json")
EN_JSON = os.path.join(ROOT, "ui", "ios", "Recon", "Resources", "locales",
                       "en-US.json")
CAPABILITY_VIEW = os.path.join(ROOT, "ui", "ios", "Recon", "Sources", "Screens",
                               "CapabilityView.swift")
FRAME_PIPELINE = os.path.join(ROOT, "ui", "ios", "Recon", "Sources", "Platform",
                              "FramePipeline.swift")
FRAME_CONVERTER = os.path.join(ROOT, "ui", "ios", "Recon", "Sources", "Platform",
                               "FrameConverter.swift")
BRIDGING_HEADER = os.path.join(ROOT, "platform", "ios", "Recon",
                               "Recon-Bridging-Header.h")
# iOS 侧 YUV→BGRA8 转换的 ObjC++ 实现（G14 的**目标对象**：平面取址纪律
# 与桥接层 include 纪律真正的发生地）。
METAL_BACKEND = os.path.join(ROOT, "platform", "ios", "Recon",
                             "MetalComputeBackend.mm")
# G15 的三个目标对象：生成物（project.yml）、生成器源头（gen_ios_project.py）、
# 以及许可清单嵌入位置（Licenses.cmake）。三者都要能各自拒收 —— 只改生成物
# 会被下一次重生成覆盖回错误值（判据必须落在源头）。
CONTAINER_YML = os.path.join(ROOT, "platform", "ios", "Container", "project.yml")
GEN_IOS_PROJECT = os.path.join(ROOT, "tools", "gen_ios_project.py")
LICENSES_CMAKE = os.path.join(ROOT, "cmake", "Licenses.cmake")
# G15-d 的目标对象：**文档面**的许可清单 iOS 落位（与代码面方向相反的漂移）。
DOCS_10 = os.path.join(ROOT, "docs", "10_许可与合规.md")
DOCS_12 = os.path.join(ROOT, "docs", "12_构建与打包.md")
DOCS_14 = os.path.join(ROOT, "docs", "14_验收测试.md")
# G16 的目标对象：未签名 IPA 打包链所在的 workflow，以及其判据实现本体。
IOS_WORKFLOW = os.path.join(ROOT, ".github", "workflows", "ios.yml")
# G18 的目标对象：装配 recon_probe_data 的地方（幻觉 API 的实际发生地），
# 以及线框渲染器（CI #24 的 depthPixelFormat 幻觉发生地）。
CAMERA_CAPTURE = os.path.join(ROOT, "ui", "ios", "Recon", "Sources", "Platform",
                              "CameraCapture.swift")
WIREFRAME_RENDERER = os.path.join(ROOT, "platform", "ios", "Recon",
                                  "WireframeRenderer.swift")
# G20 的目标对象：根 CMakeLists.txt 的 iOS 部署守卫（此前引用了一个从未定义的
# 变量 IOS_DEPLOYMENT_TARGET，使整条守卫恒假 —— 2026-11-02 发现并修正）。
ROOT_CMAKELISTS = os.path.join(ROOT, "CMakeLists.txt")
# G21 的目标对象：bridging header（`#pragma once` 的真实发生地）与两处
# `onChange` 调用点（单参数 `perform:` 形式的真实发生地）。
CALIBRATION_VIEW = os.path.join(ROOT, "ui", "ios", "Recon", "Sources", "Screens",
                                "CalibrationView.swift")
REBUILD_VIEW = os.path.join(ROOT, "ui", "ios", "Recon", "Sources", "Screens",
                            "RebuildView.swift")
# G22 的目标对象：许可清单的写入链（生成物 phase + 生成器源头 + 落位脚本）
# 与读取端（许可页 Swift）。
LICENSES_VIEW = os.path.join(ROOT, "ui", "ios", "Recon", "Sources", "Screens",
                             "LicensesView.swift")
STAGE_LICENSES_SH = os.path.join(ROOT, "tools", "stage_licenses.sh")


def slurp(path: str) -> str:
    with open(path, "r", encoding="utf-8") as fh:
        return fh.read()


def spit(path: str, text: str) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def run_gate() -> tuple[int, str]:
    proc = subprocess.run([sys.executable, GATE], cwd=ROOT,
                          capture_output=True, text=True)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


# ---------------------------------------------------------------------------
# 注入定义：每项 = (编号, 说明, 期望失败信息片段, [(路径, 原文片段, 替换片段)])
# ---------------------------------------------------------------------------
INJECTIONS: list[tuple[str, str, str, list[tuple[str, str, str]]]] = [
    # --- G6：L10nKey 引用了 zh-CN.json 里不存在的键 ------------------------
    (
        "G6-a-key-not-in-json",
        "给 L10nKey 加一个 zh-CN.json 里没有的 case（枚举写了不存在的键）",
        "引用了 zh-CN.json 中不存在的键",
        [(LOCALIZATION,
          "    case homeNotRebuilt         = \"msg.home.not_rebuilt\"\n",
          "    case homeNotRebuilt         = \"msg.home.not_rebuilt\"\n"
          "    case negInjectedGhostKey    = \"msg.home.__negative_injection__\"\n")],
    ),
    # --- G8：三级回退链可达性（源语言与二级回退都缺 → 用户看到键名）--------
    #     注意：只删 zh-CN 是不够的 —— 回退链第二级 en-US 仍能命中，
    #     所以必须**两级都删**才能构造出「三级全落空」。这本身就说明
    #     「回退链可达性」这条判据测的是**链**而不是单表存在性。
    (
        "G8-a-fallback-hole",
        "从 zh-CN.json 与 en-US.json 同时删掉一个已被引用的键（三级回退全部落空）",
        "三级回退全部落空",
        [(ZH_JSON,
          "  \"msg.home.not_rebuilt\": \"未完成重建\",\n",
          ""),
         (EN_JSON,
          "  \"msg.home.not_rebuilt\": \"Not rebuilt\",\n",
          "")],
    ),
    # --- G1：locale 完成度判据（v1.3 新增 4 条 _meta 一致性 + 1 条废止「有键即 done」）
    #     背景：docs/09 §5.9 预言的**恒真型假闸门**（缺口 BB 同型）。v1.2 的
    #     `has_keys = any(...)` 判据在 18 个 locale 写入 32 个机器翻译键后会输出
    #     「已完成翻译 20/20」而真实自有覆盖率仅 ≈9.2%。以下注入既证明**新判据**
    #     会拒收，也证明**旧判据确实不能**达成（G1-e 反向坐实）。
    (
        "G1-a-mt-claims-done",
        "机器翻译 locale 却把 translation_status 改成 done（冒充人工完成）",
        "机器翻译**不得**声称已完成人工翻译",
        [(os.path.join(ROOT, "ui", "ios", "Recon", "Resources", "locales",
                       "de.json"),
          "\"translation_status\": \"pending\"",
          "\"translation_status\": \"done\"")],
    ),
    (
        "G1-b-pending-with-unlabeled-keys",
        "pending locale 预填了未标注机器翻译的键（「有键即完成」型假闸门）",
        "未标注机器翻译的键",
        [(os.path.join(ROOT, "ui", "ios", "Recon", "Resources", "locales",
                       "de.json"),
          "\"machine_translated\": true,",
          "\"machine_translated\": false,")],
    ),
    (
        "G1-c-mt-without-scope",
        "标注 machine_translated 却删掉 machine_translated_new_keys（无法界定范围）",
        "无法界定机器翻译范围",
        [(os.path.join(ROOT, "ui", "ios", "Recon", "Resources", "locales",
                       "de.json"),
          "\"machine_translated_new_keys\": [",
          "\"machine_translated_new_keys_unused\": [")],
    ),
    (
        "G1-d-mt-bogus-key",
        "machine_translated_new_keys 引用一个本 locale 不存在的键",
        "引用了本 locale 不存在的键",
        [(os.path.join(ROOT, "ui", "ios", "Recon", "Resources", "locales",
                       "de.json"),
          "\"machine_translated_new_keys\": [\n",
          "\"machine_translated_new_keys\": [\n    \"msg.settings.__ghost__\",\n")],
    ),
    # 说明：G1-a..d 分别对应 gate_locales 中 (a)(b)(c)(d) 四个分支，各有一条注入
    # 证伪。`done` 分支（人工翻译完成的 locale）由**基线**覆盖 —— 基线 rc=0 证明
    # 该分支在 zh-CN/en-US 上不误报。此处**故意不**构造「有键但撤下机器翻译标注」
    # 的 __NO_ERROR__ 反向注入：那恰恰是闸门**应当**拒收的冒充场景，不是合法写法。
    # --- G9：UI 文案无法反查（非中文用户会看到中文）------------------------
    (
        "G9-a-unresolvable-source",
        "在视图里写一个源语言表反查不到的中文 L10n.s 原文",
        "无法精确反查到键",
        [(CAPABILITY_VIEW,
          "        .background(ReconColor.bg.page)\n"
          "        .navigationTitle(L10n.t(.capabilityTitle))\n",
          "        .background(ReconColor.bg.page)\n"
          "        .navigationTitle(L10n.s(\"负向注入用的表外中文\"))\n")],
    ),
    (
        "G11-a-swift-missing",
        "Swift 侧删掉一个错误码（C ABI 有而 Swift 缺）",
        "但 Swift ReconStatus 缺",
        [(SWIFT_CORE,
          "    case meshPoissonFailed = 700\n",
          "")],
    ),
    (
        "G11-b-cabi-extra",
        "C ABI 侧删掉一个错误码（Swift 有而 C ABI 无——引用了不存在的错误码）",
        "但 C ABI 无",
        [(C_API_HEADER,
          "RECON_ERR_MEMORY_LIMIT_REACHED = 1100,\n"
          "    RECON_ERR_MEMORY_BLOCK_TOO_LARGE = 1101\n} recon_status;",
          "RECON_ERR_MEMORY_LIMIT_REACHED = 1100\n} recon_status;")],
    ),
    (
        "G11-c-swift-implicit-raw",
        "Swift 侧出现无显式 rawValue 的 case（rawValue 静态不可知）",
        "无显式 rawValue",
        [(SWIFT_CORE,
          "    case syncTimeNonmonotonic = 200\n",
          "    case syncTimeNonmonotonic\n")],
    ),
    (
        "G12-a-overlap",
        "同一段号既在 reachableSegments 又在 contractGaps（自相矛盾）",
        "自相矛盾",
        [(STAGE_MAP,
          "public static let reachableSegments: Set<Int> = [1, 2, 3, 4, 6, 9, 12, 14]",
          "public static let reachableSegments: Set<Int> = [1, 2, 3, 4, 5, 9, 12, 14]")],
    ),
    (
        "G12-b-missing",
        "从 reachableSegments 删一个段号，且不失补到 contractGaps（造成遗漏）",
        "未被完整覆盖",
        [(STAGE_MAP,
          "public static let reachableSegments: Set<Int> = [1, 2, 3, 4, 6, 9, 12, 14]",
          "public static let reachableSegments: Set<Int> = [1, 2, 3, 4, 6, 9, 14]")],
    ),
    (
        "G12-c-out-of-range",
        "把总段数改小，令缺口段号越界",
        "段号越界",
        [(STAGE_MAP,
          "public static let uiSegmentCount = 14",
          "public static let uiSegmentCount = 10")],
    ),
    (
        "G13-a-mapping-missing",
        "从 messageKey(for:) 删掉一个 case 引用（新增错误码时漏改映射）",
        "未在 messageKey(for:) 中被引用",
        [(CONTROLLER,
          "        case .cancelled:                return .rebuildCancelled        // R-13\n",
          "")],
    ),
    # --- G9-b-3：`L10n.t(.名字)` 指向不存在的枚举成员（2026-10-31 补的假闸门）--
    #     此前 G6 只校验「已声明的 case → JSON 键存在」，而「引用 → 是否声明」
    #     无人看管。实测 ParamsView 接线时产生 62 个越界引用，全链仍报通过。
    (
        "G9-b-3-t-references-ghost-case",
        "在视图里写一个 L10n.t(.不存在成员)，令 Swift 编译失败",
        "不在 L10nKey 中",
        [(CAPABILITY_VIEW,
          "        .navigationTitle(L10n.t(.capabilityTitle))\n",
          "        .navigationTitle(L10n.t(.capabilityTitleGhostInjected))\n")],
    ),
    # --- G14：平台平面缓冲纪律（2026-10-31 第三十四轮补的缺口 J 守护）------
    #     目标：`CVPixelBufferGetBaseAddress` 对 ARKit 的 YUV **平面**缓冲返回
    #     NULL，配合 `guard else { return }` 会静默丢弃 100% 的帧。该缺陷此前
    #     不触发任何闸门（不违反 G3/G5/G7，不产生编译或运行期错误）。
    (
        "G14-a-planar-base-address",
        "对平面格式缓冲（capturedImage）用 GetBaseAddress，触发静默丢帧路径",
        "静默丢弃全部帧",
        [(FRAME_PIPELINE,
          "let depth = packet.depthMap.flatMap { FrameConverter.depthMillimeters(from: $0) }",
          "_ = CVPixelBufferGetBaseAddress(packet.pixelBuffer)\n"
          "        let depth = packet.depthMap.flatMap { FrameConverter.depthMillimeters(from: $0) }")],
    ),
    (
        "G14-c-depth-format-unchecked",
        "深度换算不校验 CVPixelBufferGetPixelFormatType（字节布局靠猜）",
        "但未调用 `CVPixelBufferGetPixelFormatType`",
        [(FRAME_CONVERTER,
          "        let format = CVPixelBufferGetPixelFormatType(depthMap)\n",
          "        let format = kCVPixelFormatType_DepthFloat32\n")],
    ),
    # --- G7-e：ObjC 桥类对 Swift 的可见性（2026-10-31 第三十五轮补的真实缺口）--
    #     实测发现：ScanSessionModel.swift 用了 ReconCaptureStateMachine，但
    #     Recon-Bridging-Header.h 没 import CaptureStateBridge.h → 真机 Xcode
    #     构建会报 cannot find in scope，而既有 G7-b 只查 C 头是否存在、放行。
    (
        "G7-e-bridge-not-imported",
        "从 bridging header 删掉 CaptureStateBridge.h 的 import"
        "（Swift 用到的桥类不可见）",
        "未被 Recon-Bridging-Header.h `#import`",
        [(BRIDGING_HEADER,
          '#import "CaptureStateBridge.h"\n',
          "")],
    ),
    (
        "G7-e-newbridge-not-imported",
        "从 bridging header 删掉 PlatformCaptureBridge.h 的 import"
        "（FramePipeline 用到 ReconImuRingBuffer）",
        "未被 Recon-Bridging-Header.h `#import`",
        [(BRIDGING_HEADER,
          '#import "PlatformCaptureBridge.h"\n',
          "")],
    ),
    # --- G14-a（ObjC++ 目标对象）：桥接层对平面缓冲用 GetBaseAddress ----------
    #     判据的**目标对象**在 2026-10-31 第三十七轮从 Swift 扩展到了 .mm ——
    #     因为平面处理整体搬进了宿主编译单元。若闸门仍只看 Swift，新代码就
    #     没有任何闸门在看（正是闸门注释里警告的「判据没落在目标对象上」）。
    #     本注入证明扩展后的判据确实覆盖 .mm。
    (
        "G14-a-objc-planar-base-address",
        "在 MetalComputeBackend.mm 里对 capturedImage 改用 GetBaseAddress"
        "（平面缓冲 → 静默丢帧路径）",
        "静默丢弃全部帧",
        [(METAL_BACKEND,
          "    const uint8_t *y_base =\n"
          "        (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(capturedImage, 0);\n",
          "    const uint8_t *y_base =\n"
          "        (const uint8_t *)CVPixelBufferGetBaseAddress(capturedImage);\n")],
    ),
    # --- G14-d（新增判据）：桥接层不得用 `../..` 相对路径引用仓库内公共头 ------
    #     第三十七轮新增。相对路径要人工数 `..` 层数（易错），且 CMake 与
    #     Xcode 两个消费者的相对基准不同；缺头搜索路径时真机报 file not found，
    #     而本机（Windows）查不出。判据落在 include 指令实参上。
    (
        "G14-d-bridge-relative-include",
        "在 MetalComputeBackend.mm 里把 gpu_algo.h 改成 `../..` 相对路径",
        "用**相对路径**",
        [(METAL_BACKEND,
          '#include "gpu_algo.h"\n',
          '#include "../../shaders/common/gpu_algo.h"\n')],
    ),
    # --- G15：iOS bundle 落位纪律（2026-11-01 补的真实缺口）----------------
    #     目标：iOS 是**扁平** bundle，非本地化资源在包根，无 `Contents/` 一层。
    #     而 `MTLDevice.makeDefaultLibrary()` 只在包根找 default.metallib。
    #     写成 `${CONTENTS_FOLDER_PATH}/Resources` 会落到 `Recon.app/Recon.app/Resources`
    #     → 编得过、CI 全绿、运行期永久加载不到（GPU 路径静默降级）。
    #     故**生成物**与**生成器源头**两处都要注入：只盯生成物会漏掉
    #     「下次重生成又改回去」这条路径。
    #     G15-d 注入的是**反方向**：代码侧早已对，而 docs/12 §2.7 与 docs/10
    #     LIC-5 的文档面把 iOS 许可清单落位留在 `Contents/Resources/`。
    #     两条注入（docs/12 表行、docs/10 LIC-5 行）共同证明 G15-d 的
    #     「同行共现」判据有效，且不会误伤 dSYM 路径里的合法 `Contents/`。
    (
        "G15-a-container-contents-layer",
        "在生成的 project.yml 里把 metallib 落位改回 ${CONTENTS_FOLDER_PATH}/Resources"
        "（iOS 上该层级不存在 → 运行期加载不到）",
        "iOS 是扁平 bundle",
        [(CONTAINER_YML,
          'export RECON_METALLIB_OUT_DIR='
          '"${BUILT_PRODUCTS_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}"',
          'export RECON_METALLIB_OUT_DIR='
          '"${BUILT_PRODUCTS_DIR}/${CONTENTS_FOLDER_PATH}/Resources"')],
    ),
    (
        "G15-b-generator-contents-layer",
        "在生成器 gen_ios_project.py 源头里恢复 ${CONTENTS_FOLDER_PATH} 落位"
        "（判据必须落在源头，否则重生成即回退）",
        "生成器里仍有",
        [(GEN_IOS_PROJECT,
          'export RECON_METALLIB_OUT_DIR='
          '"${BUILT_PRODUCTS_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}"',
          'export RECON_METALLIB_OUT_DIR='
          '"${BUILT_PRODUCTS_DIR}/${CONTENTS_FOLDER_PATH}/Resources"')],
    ),
    (
        "G15-c-licenses-contents-layer",
        "在 Licenses.cmake 里把 iOS 许可清单嵌入位置改回 Recon.app/Contents/Resources"
        "（iOS 无该层级，设备上目录不存在）",
        "iOS 无 `Contents/` 层",
        [(LICENSES_CMAKE,
          '  set(RECON_LICENSES_EMBED_DEST "Recon.app")\n',
          '  set(RECON_LICENSES_EMBED_DEST "Recon.app/Contents/Resources")\n')],
    ),
    #     G15-d 守的是**反方向**：代码已是对的、文档留在错的。
    #     注入必须落在「许可清单 + Recon.app/Contents/ 同行共现」上 ——
    #     这正是判据的命中条件（dSYM 路径与 macOS 反例里的 Contents/ 不共现，
    #     故不会被误伤；本项也顺便证明了这一点）。
    (
        "G15-d-doc-licenses-contents-layer",
        "在 docs/12 的嵌入位置表里把 iOS 许可清单落位改回 Recon.app/Contents/"
        "Resources（代码已改为包根，文档是第二事实来源）",
        "两份事实来源已漂移",
        [(DOCS_12,
          "| 嵌入位置 | iOS：`Recon.app/THIRD_PARTY_LICENSES.txt`（**包根**，见下方警示）；"
          "Android：`assets/THIRD_PARTY_LICENSES.txt`；",
          "| 嵌入位置 | iOS：`Recon.app/Contents/Resources/THIRD_PARTY_LICENSES.txt`；"
          "Android：`assets/THIRD_PARTY_LICENSES.txt`；")],
    ),
    (
        "G15-d-doc10-licenses-contents-layer",
        "在 docs/10 的 LIC-5 行里同样把 iOS 许可清单落位写回 Contents/Resources",
        "两份事实来源已漂移",
        [(DOCS_10,
          "| LIC-5 | 许可文本**不得**被压缩成不可读形式。iOS 侧放 "
          "`Recon.app/THIRD_PARTY_LICENSES.txt`（**包根**",
          "| LIC-5 | 许可文本**不得**被压缩成不可读形式。iOS 侧放 "
          "`Recon.app/Contents/Resources/THIRD_PARTY_LICENSES.txt`（**包根**")],
    ),
    #     G15-d 的**范围证明**：豁免只该覆盖「缺口登记行」（行首 `| AN |` 之类，
    #     其职责就是引用错误值）。本项注入到 docs/14 的**散文行**上 ——
    #     若豁免写宽了（比如整份 docs/14 都跳过），本项就不会被拒收。
    (
        "G15-d-doc14-prose-not-exempt",
        "在 docs/14 的**散文段落**（非缺口登记行）里写入许可清单 + "
        "Recon.app/Contents/ 同行共现 —— 证明登记行豁免是**窄**的",
        "两份事实来源已漂移",
        [(DOCS_14,
          "> **共同根因**（与本项目既往教训同构）：判据没有落在**目标对象**上。\n",
          "> **共同根因**：iOS 许可清单放 `Recon.app/Contents/Resources/"
          "THIRD_PARTY_LICENSES.txt`，判据没有落在**目标对象**上。\n")],
    ),
    # --- G16：未签名 IPA 打包链纪律（2026-11-01 补）------------------------
    #     目标：`.github/workflows/ios.yml` 的 build-app job。
    #     四条各自独立，故必须**分四项注入**（当场把某一条写错才知它是否真被看管）。
    #     ⚠️ 注入片段必须落在**代码行**上，不能落进注释 —— G16 会先剥行注释再判
    #        （注释里刻意提到 `-exportArchive` 与模拟器 destination 作反例）。
    (
        "G16-a-archive-to-simulator",
        "把 device 归档改成模拟器归档（模拟器 slice 混进交付包，XC-3）",
        "归档目标是**模拟器**",        [(IOS_WORKFLOW,
          "            -destination 'generic/platform=iOS' \\\n"
          "            -archivePath build/ios/Recon.xcarchive \\",
          "            -destination 'generic/platform=iOS Simulator' \\\n"
          "            -archivePath build/ios/Recon.xcarchive \\")],
    ),
    (
        "G16-b-bypass-staging-script",
        "在 YAML 里就地写 zip 打包，绕过 tools/stage_ios_ipa.py"
        "（判据变成两处事实来源，且落不到产物上）",
        "打包逻辑应只在",
        [(IOS_WORKFLOW,
          "          python3 tools/stage_ios_ipa.py stage \\\n",
          "          mkdir -p build/ios/Payload\n"
          "          cp -R build/ios/Recon.app build/ios/Payload/\n"
          "          (cd build/ios && zip -r Recon-unsigned.ipa Payload)\n"
          "          python3 tools/stage_ios_ipa.py stage \\\n")],
    ),
    (
        "G16-c-exportarchive-signed-path",
        "在未签名 job 里混入 -exportArchive（强制签名，CI 无凭据必然失败）",
        "出现 `-exportArchive`",
        [(IOS_WORKFLOW,
          "          xcodebuild archive \\\n",
          "          xcodebuild -exportArchive -archivePath build/ios/Recon.xcarchive \\\n"
          "          xcodebuild archive \\\n")],
    ),
    (
        "G16-d-ipa-not-uploaded",
        "删掉 IPA 的 artifact 上传（链路没出口 = 白跑）",
        "没有出口",
        [(IOS_WORKFLOW,
          "          name: Recon-unsigned.ipa\n",
          "          name: Recon-unsigned-renamed.ipa\n")],
    ),
    # --- G17：locale 资源落位纪律（2026-11-01 补的第三例「静默失效」）------
    #     目标：`type: group` 把 locales 展平到包根 → subdirectory 查找全落空，
    #     而编译/链接/启动全绿、UI 只是显示键名。
    #     a/b 分打生成物与生成器源头；c 打**交叉对账**（改 Swift 的查找方式）。
    (
        "G17-a-container-group-not-folder",
        "把生成物里的 locale 资源从 folder reference 改回默认 group"
        "（资源被展平到 bundle 根，20 个 locale 全部加载不到）",
        "未声明 `type: folder`",
        [(CONTAINER_YML,
          "      - path: ui/ios/Recon/Resources/locales\n"
          "        type: folder\n",
          "      - path: ui/ios/Recon/Resources/locales\n")],
    ),
    (
        "G17-b-generator-group-not-folder",
        "在生成器源头里去掉 type: folder（判据必须落在源头，否则重生成即回退）",
        "生成器源头",
        [(GEN_IOS_PROJECT,
          "      - path: ui/ios/Recon/Resources/locales\n"
          "        type: folder\n",
          "      - path: ui/ios/Recon/Resources/locales\n")],
    ),
    (
        "G17-c-subdir-crosscheck-mismatch",
        "把 Swift 的 locale 查找目录从 locales 改成 Resources"
        "（代码查找与包内落位不再指向同一目录）",
        "二者不一致",
        [(LOCALIZATION,
          'subdirectory: "locales"',
          'subdirectory: "Resources"')],
    ),
    (
        "G17-d-platform-source-folder-breaks-privacy",
        "把 platform/ios/Recon 声明为 type: folder（会让 PrivacyInfo.xcprivacy "
        "落到 Recon.app/Recon/Resources/ 而非包根）",
        "隐私清单落到",
        [(CONTAINER_YML,
          "      - path: platform/ios/Recon\n"
          "        excludes:\n",
          "      - path: platform/ios/Recon\n"
          "        type: folder\n"
          "        excludes:\n")],
    ),
    # --- G18：iOS 系统 API 幻觉清单（回归防线）-----------------------------
    #   注入「当初真实犯过的那个错」：把诚实写 0 改回幻觉 API 调用。
    #   这条注入的价值就是**复现 CI #20 的原始失败**。
    (
        "G18-a-hallucinated-motion-member",
        "把 imuMaxRate 的诚实写 0 改回使用不存在的 "
        "CMMotionManager.maximumGyroUpdateInterval（复现 CI #20 的真实错误）",
        "不存在",
        [(CAMERA_CAPTURE,
          "        let imuMaxRate: Int32 = 0\n",
          "        let motion = CMMotionManager()\n"
          "        let imuMaxRate: Int32 = "
          "Int32(1.0 / motion.maximumGyroUpdateInterval)\n")],
    ),
    (
        "G18-b-hallucinated-devicemotion-member",
        "使用另一个不存在的成员 maximumDeviceMotionUpdateInterval",
        "不存在",
        [(CAMERA_CAPTURE,
          "        let imuMaxRate: Int32 = 0\n",
          "        let motion = CMMotionManager()\n"
          "        let imuMaxRate: Int32 = "
          "Int32(1.0 / motion.maximumDeviceMotionUpdateInterval)\n")],
    ),
    # 2026-11-02 CI #24（#114 线框渲染）：复现「MTKView.depthPixelFormat」这条
    # 真编译幻觉。注入 = 把正确的 `depthStencilPixelFormat` 改回错的
    # `depthPixelFormat` —— 正是 CI 当场报 `cannot find in scope` 的那一行。
    (
        "G18-c-hallucinated-mtkview-depth-format",
        "把 MTKView 正确的 depthStencilPixelFormat 改回不存在的 "
        "depthPixelFormat（复现 CI #24 的真实错误）",
        "不存在",
        [(WIREFRAME_RENDERER,
          "        desc.depthAttachmentPixelFormat = depthStencilPixelFormat\n",
          "        desc.depthAttachmentPixelFormat = depthPixelFormat\n")],
    ),
    # --- G19：Swift 访问级别自洽 -------------------------------------------
    # ① 复现 CI #24 的原始错误：把顶层 public 的 PreviewMesh 降为 internal，
    #    而 `public private(set) var previewMesh: PreviewMesh?` 仍引用它。
    (
        "G19-a-public-exposes-internal-type",
        "把 PreviewMesh 从 public 降为 internal，而 ReconSessionController 的 "
        "public 属性仍引用它（复现 CI #24 的真实错误）",
        "非 public",
        [(SWIFT_CORE, "public struct PreviewMesh {", "struct PreviewMesh {")],
    ),
    # --- G20：构建部署守卫引用的变量必须真实存在 ---------------------------
    # 复现 2026-11-02 发现的**真实假闸门**：根 CMakeLists.txt 的 iOS 守卫写作
    # `if(IOS_DEPLOYMENT_TARGET AND ... < 15.0)`，而该变量全仓从未定义 →
    # `if("" AND ...)` 恒假 → 守卫**从未触发**。注入 = 把正确的
    # `CMAKE_OSX_DEPLOYMENT_TARGET` 换回不存在的 `IOS_DEPLOYMENT_TARGET`。
    (
        "G20-a-undefined-deployment-var",
        "把 CMakeLists 部署守卫的变量换成从未定义的 IOS_DEPLOYMENT_TARGET"
        "（复现 2026-11-02 发现的真实假闸门）",
        "从未被定义",
        [(ROOT_CMAKELISTS,
          "  if(CMAKE_OSX_DEPLOYMENT_TARGET AND\n"
          "     CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 17.0)",
          "  if(IOS_DEPLOYMENT_TARGET AND\n"
          "     IOS_DEPLOYMENT_TARGET VERSION_LESS 17.0)")],
    ),
    # --- G21：Swift/Clang 编译可见缺陷（CI #33 首次真编译暴露的两类）---------
    #   ① 复现 CI #33 的原始错误之一：把 `#pragma once` 加回 bridging header
    #      （真编译报 `error: #pragma once in main file`）。
    (
        "G21-a-pragma-once-in-bridging-header",
        "把 `#pragma once` 加回 bridging header（复现 CI #33 的真实错误）",
        "#pragma once",
        [(BRIDGING_HEADER,
          "#include \"recon/recon_c_api.h\"\n",
          "#pragma once\n\n#include \"recon/recon_c_api.h\"\n")],
    ),
    #   ② 复现 CI #33 的原始错误之二：把两参数 onChange 改回单参数
    #      （真编译报 `'onChange(of:perform:)' was deprecated in iOS 17.0`，
    #        零警告标准 → 升为 `error:`）。
    (
        "G21-b-single-param-onchange",
        "把两参数 onChange 改回单参数 `{ t in`（复现 CI #33 的真实错误）",
        "单参数",
        [(CALIBRATION_VIEW,
          "            .onChange(of: referenceType) { _, t in\n",
          "            .onChange(of: referenceType) { t in\n")],
    ),
    #   ③ **判据自身的假红防线**（本闸门第一版实测踩到）：若 `.onChange(of:)`
    #      的实参表达式里含逗号（如 `controller.progress?.uiSegment` 那种跨行
    #      写法外，还有多实参写法），用「懒惰匹配 `)` 再匹配 `{`」的正则会
    #      **回溯**到文档后面某个 `) { d in` → 对**已修好的两参数写法**误报。
    #      本注入把正确写法**再行数加长**（闭包形参换名 + 引入第二处），断言
    #      基线仍 PASS —— 即「合法写法不得被误判」。
    (
        "G21-c-legal-two-param-must-pass",
        "把合法两参数写法换个形参名（`{ _, seg in` → `{ _, newSeg in`）—— "
        "断言闸门**不报错**（防判据自身假红）",
        "__NO_ERROR__",
        [(REBUILD_VIEW,
          "        .onChange(of: controller.progress?.uiSegment) { _, seg in\n"
          "            if let seg { lastStageIndex = seg }\n",
          "        .onChange(of: controller.progress?.uiSegment)"
          " { _, newSeg in\n"
          "            if let newSeg { lastStageIndex = newSeg }\n")],
    ),
    # --- G22：许可清单必须真的进 iOS App 包（2026-11-18 实测发现的真实阻塞缺口）---
    #   背景：U-13 许可清单页读**包根**的 THIRD_PARTY_LICENSES.txt，但该文件
    #   此前**从不进包**（`install(... DESTINATION "Recon.app")` 只在 CMake
    #   install 阶段生效，而 iOS 交付链走 xcodegen + xcodebuild，从不跑
    #   `cmake --install`），→ 真机永远读到空、编译期与 G1..G21 全绿。
    #   四条注入分别打生成物 / 生成器源头 / 读取端交叉对账 / 落位脚本 fail-closed。
    (
        "G22-a-container-phase-removed",
        "把生成物 project.yml 里的 `Stage Third-Party Licenses` phase 删掉"
        "（许可清单从不进包 → 许可页真机读到空，而编译期全绿）",
        "许可清单**从不进 App 包**",
        [(CONTAINER_YML,
          "      - name: Stage Third-Party Licenses\n",
          "")],
    ),
    (
        "G22-b-generator-phase-removed",
        "在生成器源头 gen_ios_project.py 里去掉该 phase（判据必须落在源头，"
        "否则重生成即回退）",
        "源头里没有 `Stage Third-Party Licenses` phase",
        [(GEN_IOS_PROJECT,
          "    postBuildScripts:\n"
          "      - name: Stage Third-Party Licenses\n",
          "    # NEGATIVE INJECTION: phase removed\n"
          "    __disabled_postBuildScripts__:\n"
          "      - name: Not-Stage-Licenses\n")],
    ),
    (
        "G22-c-reader-subdir-mismatch",
        "把读取端 LicensesView.swift 的查找方式改成带 subdirectory（读端指向"
        "子目录、写端放包根 → 两端漂移，真机读不到）",
        "带了 `subdirectory:`",
        [(LICENSES_VIEW,
          "        guard let url = Bundle.main.url(forResource: "
          "\"THIRD_PARTY_LICENSES\",\n"
          "                                       withExtension: \"txt\"),\n",
          "        guard let url = Bundle.main.url(forResource: "
          "\"THIRD_PARTY_LICENSES\",\n"
          "                                       withExtension: \"txt\",\n"
          "                                       subdirectory: \"Resources\"),\n")],
    ),
    (
        "G22-d-stage-script-not-fail-closed",
        "把落位脚本的 fail-closed 契约去掉（`REQUIRE` 不再绑定 "
        "`RECON_REQUIRE_LICENSES` → 缺清单时产空文件冒充「依赖为零」）",
        "不是 fail-closed",
        [(STAGE_LICENSES_SH,
          'REQUIRE="${RECON_REQUIRE_LICENSES:-0}"\n',
          'REQUIRE="0"\n')],
    ),
]


def main() -> int:
    ap = argparse.ArgumentParser(
        description="check_ios_contract.py 的负向注入自检"
                    "（G6 / G7 / G8 / G9 / G11 / G12 / G13 / G14 / G15 / G16 / "
                    "G17 / G18 / G19 / G20 / G21 / G22）")
    ap.add_argument("--keep-going", action="store_true",
                    help="全部跑完再汇总（默认也是全跑，此项保留语义）")
    args = ap.parse_args()
    _ = args

    for p in (GATE, SWIFT_CORE, STAGE_MAP, CONTROLLER, C_API_HEADER,
              LOCALIZATION, ZH_JSON, EN_JSON, CAPABILITY_VIEW,
              FRAME_PIPELINE, FRAME_CONVERTER, BRIDGING_HEADER, METAL_BACKEND,
              CONTAINER_YML, GEN_IOS_PROJECT, LICENSES_CMAKE, IOS_WORKFLOW,
              ROOT_CMAKELISTS, CALIBRATION_VIEW, REBUILD_VIEW,
              LICENSES_VIEW, STAGE_LICENSES_SH):
        if not os.path.isfile(p):
            print(f"[negative] 环境错误：找不到 {p}", file=sys.stderr)
            return 1

    print("=" * 68)
    print("iOS 契约闸门负向注入自检（tools/verify_ios_contract_negative.py）")
    print("=" * 68)

    # 先确认「无注入时闸门是 PASS」—— 否则后面的"拒收"可能只是基线本来就红。
    base_rc, base_out = run_gate()
    if base_rc != 0:
        print("[negative] 基线失败：未注入任何缺陷时闸门就报错，无法进行负向验证。")
        print(base_out)
        return 1
    print("[negative] 基线：闸门 PASS（未注入缺陷）✓")
    print("")

    failures: list[str] = []
    for idx, (code, desc, expect, edits) in enumerate(INJECTIONS, 1):
        originals: dict[str, str] = {}
        for path, _, _ in edits:
            if path not in originals:
                originals[path] = slurp(path)

        applied = False
        try:
            for path, old, new in edits:
                text = slurp(path)
                if old not in text:
                    failures.append(f"{code}: 注入片段在 {os.path.basename(path)} "
                                    f"中找不到，注入无效（需修正本脚本）")
                    break
                spit(path, text.replace(old, new, 1))
                applied = True

            if not applied and edits:
                continue      # 已记失败

            rc, out = run_gate()
            # `__NO_ERROR__` = 断言「**合法写法不得被误判**」的**反向**注入：
            # 用来证明判据自己不会假红（本闸门 G21 第一版实测踩到过 —— 见
            # INJECTIONS 里 G21-c 的说明）。
            if expect == "__NO_ERROR__":
                if rc == 0:
                    print(f"  [{idx:02d}] ✓ {code}  —— 合法写法未被误判（rc=0）")
                else:
                    failures.append(
                        f"{code}: **合法写法被误判为违规**（rc={rc}）→ 判据"
                        f"自身有假红，必须修正判据。 意图：{desc}\n{out}")
                    print(f"  [{idx:02d}] ✗ {code}  —— 合法写法被误判（判据假红）")
            elif rc == 0:
                failures.append(f"{code}: **闸门未拒收**（退出码 0）→ 该判据是假闸门。"
                                f" 意图：{desc}")
                print(f"  [{idx:02d}] ✗ {code}  —— 未被拒收！（假闸门）")
            elif expect not in out:
                failures.append(f"{code}: 闸门拒收了（rc={rc}），但失败信息里"
                                f"找不到预期片段「{expect}」。实际输出见下：\n{out}")
                print(f"  [{idx:02d}] ✗ {code}  —— 拒收了但信息不符预期")
            else:
                print(f"  [{idx:02d}] ✓ {code}  —— 已拒收（rc={rc}）")
        finally:
            for path, text in originals.items():
                spit(path, text)

    # --- 收尾：确认还原后闸门重新 PASS ---
    rc_after, out_after = run_gate()
    print("")
    if rc_after != 0:
        failures.append("还原后闸门仍失败 —— 还原不完整！输出：\n" + out_after)
        print("[negative] ✗ 还原后闸门仍失败（还原不完整）")
    else:
        print("[negative] 还原后闸门重新 PASS ✓")

    print("")
    if failures:
        print(f"[negative] FAIL：{len(failures)} 项")
        for f in failures:
            print("  ✗ " + f)
        return 3

    print(f"[negative] PASS：{len(INJECTIONS)} 项注入全部被正确拒收"
          f"（闸门 G6 / G8 / G9 / G11 / G12 / G13 / G14 / G15 / G16 / G17 / "
          f"G18 / G19 / G20 / G21 / G22 均有效）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
