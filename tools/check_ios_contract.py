#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""check_ios_contract.py — iOS 侧静态契约闸门（无需 macOS 即可运行）。

背景：iOS 的真机构建需要 macOS（Xcode 仅 macOS），Windows 上无法编译 iOS
二进制。因此本工具承担「编译前能查的全部静态约束」，把错误挡在提交之前，
而不是等 CI 上的 macOS runner 才暴露。

为什么这些检查在 macOS 上也值得保留：它们检查的是**规格一致性**，与编译器
无关 —— Xcode 不会因为你把 Liquid Glass 用在业务内容区、或漏了一个 locale 键
而报错，但两者都是规格违规。

十二重闸门
--------
G1 §6.1 locale 对等
   20 个 locale 文件齐备；源语言 zh-CN 的键集合必须被 en-US 完全覆盖；
   每个 locale 文件必须是合法 JSON；`_meta.rtl` 必须与 §6.1 表一致。

G2 §1.2.1 LG-2 玻璃表面白名单
   Liquid Glass 只允许出现在 GlassRole 白名单授权的系统级表面。
   实现方式：`glassSurface(_:)` 的第一实参必须是 GlassRole 的成员；
   且禁止在「业务内容区」视图（Screens/ 下非 App 文件的 body 直接调用）。

G3 U-X 禁令静态扫描
   禁止 Gradient / shadow / capsule 圆角 / 自定义动画 API 出现在 ui/ios/。
   同时禁止网络 API（00 R-LOCAL）。

G4 Info.plist 与 docs/11 §1.1/§1.2 权限对等
   必备键存在；禁止出现 §1.1 判定为「否」的键（如
   NSPhotoLibraryUsageDescription）。

G5 C ABI 覆盖对等
   `ReconCore.swift`（Swift 直接调 C ABI 的文件）须覆盖 native-lib.cpp 的
   调用面（与 tools/check_platform_parity.py 的第 4 关同源，此处针对 iOS
   单独复查，因为 iOS 新增了 UI 层的调用点）。

G6 L10nKey ↔ 源语言 JSON 键集合一致
   `L10nKey` 每个 case 的原始值都必须存在于 `zh-CN.json`。否则 UI 运行时
   会静默显示键名而非文案（本项目真实踩过的坑，见 gate_l10n_keys 注释）。

G7 Swift/C 混编接线完整性
   docs/12 §4.2 要求的 `module.modulemap` 与 `Recon-Bridging-Header.h` 存在；
   它们引用的每个 C 头在 `core/include/` 下真实存在；不得引用 C++ 头；
   `Package.swift` 不得引用包根之外的源码（SwiftPM 禁止）；
   `Container/project.yml` 存在且拢进了两个源码树。

G8 三级回退链可达性
   `用户语言 → en-US → zh-CN(源) → key 本身` 四级链路各环节可达；
   缺键必须显示键名而非空串（不静默空白）。

G9 UI 文案去硬编码
   UI 源码中不得出现未走 `L10n.s` 的中文/英文 UI 文案；
   且 `L10n.s("原文")` 的原文必须能**反查**到键（避免表外原文静默回退）。

G10 C ABI 边界完整性
   枚举成员显式赋值；固定长度数组声明个数；声明-定义闭合；空指针守卫；
   会话句柄并发保护。

G11 recon_status 逐值对等
   Swift `enum ReconStatus: Int32` 的 rawValue 集合必须与
   `core/include/recon/recon_c_api.h` 的 `recon_status` **逐值相等**。
   缺项会让 iOS 侧经 `ReconStatus(rawValue:)` 静默落回 `.internalError`。

G12 ReconStageMap 自洽
   `ui/ios/Recon/Sources/State/ReconStageMap.swift` 的三份手工数据
   （总段数 / 可达段集合 / 缺口段集合）必须满足
   `可达 ⊎ 缺口 = 1...总段数`（不相交、无越界、无遗漏）。

G16 未签名 IPA 打包链纪律
   `.github/workflows/ios.yml` 的 build-app job 必须真正走「device 归档 →
   手工 Payload 打包 → 产物结构校验」这条链，且：
     G16-a 归档目标必须是 device（`generic/platform=iOS`），不得是模拟器
           （模拟器 slice 不得进入交付包，docs/12 §4.7 XC-3）；
     G16-b 打包与校验必须**调用** `tools/stage_ios_ipa.py`（判据的单一实现），
           而不是在 YAML 里就地重写 `zip` 三行；
     G16-c 不得出现 `-exportArchive`（它强制签名，而 CI 无 Apple 凭据 ——
           看到它就说明有人把带签名流程混进了未签名 job）；
     G16-d 必须把 IPA 作为 artifact 上传（否则这条链白跑）。

G17 locale 资源落位纪律（folder reference + 与 Swift 查找方式交叉对账）
   `LocaleCatalog.loadAll` 用 `subdirectory:` 指定目录查找 locale JSON，
   故包内必须有同名目录层。XcodeGen 默认 `type: group` 会把资源**展平到
   bundle 根** → 20 个 locale 全部落入 `missingFiles`、UI 整体回退到键名，
   **且无任何编译/运行期报错**。故：
     G17-a 生成物 project.yml 里 locale 资源必须 `type: folder`，且路径
           以 `/locales` 结尾；
     G17-b 生成器源头 gen_ios_project.py 同禁（否则重生成即回退）；
     G17-c **交叉对账**：`Localization.swift` 的 `subdirectory:` 实参必须
           等于 folder reference 的最后一段目录名（改任一侧即失败）。
     G17-d **隐私清单的落位前提**：`PrivacyInfo.xcprivacy` 在
           `platform/ios/Recon/Resources/` 下，它必须落在**包根**。当前是靠
           该 source 的默认 `group` 类型**展平**实现的 —— 故该 source 一旦被
           改成 `type: folder`，隐私清单会落到
           `Recon.app/Recon/Resources/`，而 Apple 要求它在包根（上架校验项，
           ITMS 类错误）。本判据守住"不要为了统一而把资源 source 全改成
           folder"这条边界。

退出码：0 = 全部通过；1 = 有违规；2 = 工具自身错误（文件缺失等）。
"""

from __future__ import annotations

import json
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI_IOS = os.path.join(ROOT, "ui", "ios")
# iOS 侧 ObjC++ 桥接/宿主实现目录（与 ui/ios 权威 UI 源物理分离 —— 见 MEMORY.md
# 「iOS 侧权威 UI 源在 ui/ios/Recon/Sources/；platform/ios/ 只放桥接与工程配置」）。
IOS_PLATFORM = os.path.join(ROOT, "platform", "ios")
LOCALES = os.path.join(UI_IOS, "Recon", "Resources", "locales")
IOS_DIR = os.path.join(ROOT, "platform", "ios")

# docs/09 §6.1 的 20 个 locale（顺序与表一致）
EXPECTED_LOCALES = [
    "zh-CN", "en-US", "en-GB", "de", "fr-FR", "es", "pt-BR", "ru", "ua",
    "pl", "cs", "tr", "ar", "he", "ja", "ko", "zh-TW", "it", "nl", "th",
]
RTL_LOCALES = {"ar", "he"}
SOURCE_LOCALE = "zh-CN"
FALLBACK_LOCALE = "en-US"

# docs/11 §1.1：必须写入 Info.plist 的键
REQUIRED_PLIST_KEYS = [
    "NSCameraUsageDescription",
    "NSMotionUsageDescription",
    "NSPhotoLibraryAddUsageDescription",
]
# docs/11 §1.1 判定为「否」→ 不得出现
FORBIDDEN_PLIST_KEYS = [
    "NSPhotoLibraryUsageDescription",
    "NSMicrophoneUsageDescription",
    "NSLocationWhenInUseUsageDescription",
    "NSUserTrackingUsageDescription",
]

# docs/11 §1.6：iOS 二进制只允许出现的系统框架（其余即失败）
ALLOWED_IOS_FRAMEWORKS = {
    "Foundation", "UIKit", "AVFoundation", "CoreMedia", "CoreMotion",
    "CoreGraphics", "Metal", "MetalKit", "MetalPerformanceShaders",
    "QuartzCore", "ARKit", "Accelerate", "CoreML", "SwiftUI", "Photos",
    "UniformTypeIdentifiers", "CoreImage", "simd", "Dispatch",
    "Combine", "OSLog", "CoreVideo", "ImageIO",
}
# docs/11 §1.6：出现即失败
FORBIDDEN_IOS_FRAMEWORKS = {
    "CFNetwork", "Network", "Security", "WebKit",
}


class Report:
    def __init__(self) -> None:
        self.errors: list[str] = []
        self.warnings: list[str] = []
        self.notes: list[str] = []

    def err(self, gate: str, msg: str) -> None:
        self.errors.append(f"[{gate}] {msg}")

    def warn(self, gate: str, msg: str) -> None:
        self.warnings.append(f"[{gate}] {msg}")

    def note(self, msg: str) -> None:
        self.notes.append(msg)


def rel(p: str) -> str:
    return os.path.relpath(p, ROOT).replace("\\", "/")


def read_text(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def iter_swift_files() -> list[str]:
    out: list[str] = []
    for dirpath, _dirnames, filenames in os.walk(UI_IOS):
        for fn in filenames:
            if fn.endswith(".swift"):
                out.append(os.path.join(dirpath, fn))
    return sorted(out)


def iter_ios_platform_impl_files() -> list[str]:
    """iOS 侧 ObjC++ 桥接实现（`.mm`）。

    为什么不复用 `iter_swift_files()`：**平面缓冲处理真正发生的地方是 `.mm`**，
    不在 Swift。G14 最初只看 Swift，是因为当时 `GetBaseAddress` 误用发生在
    `ReconSessionController.swift`。引入 `MetalComputeBackend.mm` 之后，
    「用哪个 CVPixelBuffer 取址 API」这件事整体搬到了宿主编译单元里 ——
    若 G14 仍只看 Swift，新代码就**没有任何闸门在看**，正是本闸门注释里自己
    警告过的「判据没落在目标对象上」。故按目标对象扩展扫描范围。
    """
    out: list[str] = []
    for dirpath, _dirnames, filenames in os.walk(IOS_PLATFORM):
        for fn in filenames:
            if fn.endswith(".mm") or fn.endswith(".m"):
                out.append(os.path.join(dirpath, fn))
    return sorted(out)


def strip_comments_and_strings(text: str) -> str:
    """剥离注释与字符串字面量，避免把「注释里提到被禁 API」当成违规。

    与 tools/check_platform_parity.py 的 strip_comments 同源思路 —— 该工具
    此前因注释造成 3 处假阳性，这里直接沿用教训。
    """
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":                     # 行注释
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and nxt == "*":                     # 块注释
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == '"':                                    # 字符串字面量
            # 处理三引号
            if text[i:i + 3] == '"""':
                j = text.find('"""', i + 3)
                i = n if j < 0 else j + 3
                continue
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                if text[i] == "\n":
                    break
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def strip_comments_only(text: str) -> str:
    """只剥离注释，**保留字符串字面量**（G6 需要读 case 的原始值）。

    与 `strip_comments_and_strings` 的区别仅在字符串处理：那一个用于查「被禁
    API 名」，必须连字符串一起剥（否则 `"URLSession"` 这种字面量会假阳性）；
    这一个是查「键名字符串本身」，如果把字符串也剥了就没东西可查了。
    """
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == '"':
            # 字符串整体原样输出（含引号），处理转义与三引号
            if text[i:i + 3] == '"""':
                j = text.find('"""', i + 3)
                end = n if j < 0 else j + 3
                out.append(text[i:end])
                i = end
                continue
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"':
                    j += 1
                    break
                if text[j] == "\n":
                    break
                j += 1
            out.append(text[i:j])
            i = j
            continue
        out.append(c)
        i += 1
    return "".join(out)


# ===========================================================================
# G1 §6.1 locale 对等
# ===========================================================================

def gate_locales(rep: Report) -> None:
    gate = "G1/locale"
    if not os.path.isdir(LOCALES):
        rep.err(gate, f"locale 目录不存在：{rel(LOCALES)}")
        return

    present = {
        os.path.splitext(f)[0]
        for f in os.listdir(LOCALES)
        if f.endswith(".json")
    }
    missing = [l for l in EXPECTED_LOCALES if l not in present]
    extra = sorted(present - set(EXPECTED_LOCALES))
    if missing:
        rep.err(gate, f"§6.1 要求 20 个 locale，缺少：{missing}")
    if extra:
        rep.err(gate, f"出现 §6.1 未列出的 locale 文件：{extra}")

    tables: dict[str, dict] = {}
    for loc in EXPECTED_LOCALES:
        p = os.path.join(LOCALES, loc + ".json")
        if not os.path.isfile(p):
            continue
        try:
            doc = json.loads(read_text(p))
        except json.JSONDecodeError as e:
            rep.err(gate, f"{rel(p)} 不是合法 JSON：{e}")
            continue
        if not isinstance(doc, dict):
            rep.err(gate, f"{rel(p)} 顶层必须是 JSON 对象")
            continue
        tables[loc] = doc

        # RTL 标记必须与 §6.1 表一致
        meta = doc.get("_meta")
        if isinstance(meta, dict) and "rtl" in meta:
            want = loc in RTL_LOCALES
            if bool(meta["rtl"]) != want:
                rep.err(gate,
                        f"{rel(p)} 的 _meta.rtl={meta['rtl']}，但 §6.1 表为 {want}")

    # 源语言必须存在且非空
    src = tables.get(SOURCE_LOCALE)
    if not src:
        rep.err(gate, f"源语言 {SOURCE_LOCALE}.json 缺失或无法解析")
        return

    src_keys = {k for k in src if not k.startswith("_")}
    if len(src_keys) < 50:
        rep.err(gate, f"源语言 {SOURCE_LOCALE} 键数仅 {len(src_keys)}，疑似被截断")
    rep.note(f"源语言 {SOURCE_LOCALE} 键数：{len(src_keys)}")

    # 二级回退必须完整覆盖源语言
    fb = tables.get(FALLBACK_LOCALE)
    if fb:
        fb_keys = {k for k in fb if not k.startswith("_")}
        miss = sorted(src_keys - fb_keys)
        xtra = sorted(fb_keys - src_keys)
        if miss:
            rep.err(gate,
                    f"二级回退 {FALLBACK_LOCALE} 缺少 {len(miss)} 个源键：{miss[:8]}")
        if xtra:
            rep.err(gate,
                    f"二级回退 {FALLBACK_LOCALE} 多出 {len(xtra)} 个源中不存在的键：{xtra[:8]}")
        if not miss and not xtra:
            rep.note(f"二级回退 {FALLBACK_LOCALE} 与源语言键集合完全一致（{len(fb_keys)} 键）")

    # 其余 locale：允许 pending（用 _meta 声明），但必须显式声明
    pending, done = [], []
    for loc in EXPECTED_LOCALES:
        if loc in (SOURCE_LOCALE, FALLBACK_LOCALE):
            continue
        doc = tables.get(loc)
        if doc is None:
            continue
        meta = doc.get("_meta") if isinstance(doc.get("_meta"), dict) else {}
        status = meta.get("translation_status")
        has_keys = any(not k.startswith("_") for k in doc)
        if has_keys or status == "done":
            done.append(loc)
        elif status == "pending":
            pending.append(loc)
        else:
            rep.err(gate,
                    f"{loc}.json 既无译文键也未声明 _meta.translation_status=pending"
                    "（不许用空文件冒充完成）")
    rep.note(f"已完成翻译的 locale：{len(done) + 2}/20（含源与二级回退）")
    rep.note(f"待翻译（已显式声明 pending）：{len(pending)} 个 → {' '.join(pending) if pending else '无'}")


# ===========================================================================
# G2 §1.2.1 LG-2 玻璃表面白名单
# ===========================================================================

GLASS_ALLOWED_ROLES = {
    "navigationBar", "toolbar", "floatingAction", "sheet", "alertAccessory",
}
# LG-2 明列的业务内容区（这些视图的 body 不得直接挂玻璃）
BUSINESS_CONTENT_FILES = {
    "HomeView.swift", "CapabilityView.swift", "ScanView.swift",
    "RebuildView.swift", "ResultView.swift", "ParamsView.swift",
    "CalibrationView.swift", "ExportView.swift",
}


def gate_glass(rep: Report) -> None:
    gate = "G2/glass"
    swift = iter_swift_files()
    if not swift:
        rep.err(gate, "未找到任何 Swift 文件，ui/ios 疑似未创建")
        return

    call_re = re.compile(r"glassSurface\s*\(\s*\.\s*([A-Za-z_][A-Za-z0-9_]*)")
    seen_roles: set[str] = set()

    for path in swift:
        code = strip_comments_and_strings(read_text(path))
        for m in call_re.finditer(code):
            role = m.group(1)
            seen_roles.add(role)
            if role not in GLASS_ALLOWED_ROLES:
                rep.err(gate,
                        f"{rel(path)}: glassSurface(.{role}) 不在 LG-2 白名单内，"
                        f"允许值：{sorted(GLASS_ALLOWED_ROLES)}")

        # 业务内容区不得直接调用 glassSurface（LG-2）
        base = os.path.basename(path)
        if base in BUSINESS_CONTENT_FILES and call_re.search(code):
            rep.err(gate,
                    f"{rel(path)} 属业务内容区（LG-2），不得直接使用 Liquid Glass 表面")

    # 必须存在 GlassSurface.swift（否则玻璃能力来源不明）
    if not any(os.path.basename(p) == "GlassSurface.swift" for p in swift):
        rep.err(gate, "缺少 GlassSurface.swift —— 玻璃表面必须有单一来源")

    # 版本守卫（LG-3）：所有 #available 必须含 iOS 26.0
    for path in swift:
        code = strip_comments_and_strings(read_text(path))
        if "glassEffect" in code and "#available(iOS 26" not in code:
            rep.err(gate, f"{rel(path)} 使用了 glassEffect 但缺少 #available(iOS 26.0, *) 守卫（LG-3）")

    if seen_roles:
        rep.note(f"已使用的玻璃表面角色：{sorted(seen_roles)}")
    else:
        rep.note("尚未有任何 glassSurface 调用（组件已就位，界面待接入）")


# ===========================================================================
# G3 U-X 禁令 + 零网络
# ===========================================================================

BANNED_PATTERNS = [
    # (正则, 说明, U-X 编号)
    (r"\bLinearGradient\b", "LinearGradient", "U-X-1 渐变"),
    (r"\bRadialGradient\b", "RadialGradient", "U-X-1 渐变"),
    (r"\bAngularGradient\b", "AngularGradient", "U-X-1 渐变"),
    (r"\.shadow\s*\(", ".shadow(...)", "U-X-2 阴影卡片堆叠"),
    (r"\.elevation\b", ".elevation", "U-X-2 阴影卡片堆叠"),
    (r"\bCapsule\s*\(", "Capsule()", "U-X-3 圆角胶囊滥用"),
    (r"\.skeleton\b", ".skeleton", "U-X-8 骨架屏"),
    (r"ProgressView\s*\(\)", "ProgressView() 无限旋转", "U-X-8 无限旋转指示器"),
    (r"\.blur\s*\(", ".blur(...)", "U-X-5 毛玻璃"),
    (r"\bUIVisualEffectView\b", "UIVisualEffectView", "U-X-5 毛玻璃"),
    (r"withAnimation\s*\(", "withAnimation", "U-M-1 零自定义动画"),
    (r"\.transition\s*\(", ".transition(...)", "U-M-1 零自定义动画"),
    (r"\.rotationEffect\s*\(", ".rotationEffect(...)", "U-X-4/U-M-1 动效"),
    # 零网络（00 R-LOCAL / docs/11 §1.6）
    (r"\bURLSession\b", "URLSession", "R-LOCAL 零网络"),
    (r"\bNWConnection\b", "NWConnection", "R-LOCAL 零网络"),
    (r"\bCFStreamCreate", "CFStreamCreate*", "R-LOCAL 零网络"),
    (r"\bWKWebView\b", "WKWebView", "R-LOCAL 零网络"),
]

# 允许使用 Capsule 的例外：进度条自身（高 4，圆角=高/2，不在 U-X-3 语境）
CAPSULE_ALLOWED_FILES = {"Components.swift"}


def gate_banned(rep: Report) -> None:
    gate = "G3/ban"
    for path in iter_swift_files():
        code = strip_comments_and_strings(read_text(path))
        base = os.path.basename(path)
        for pattern, label, rule in BANNED_PATTERNS:
            if re.search(pattern, code):
                if label == "Capsule()" and base in CAPSULE_ALLOWED_FILES:
                    continue
                rep.err(gate, f"{rel(path)} 命中 {rule}：{label}")


# ===========================================================================
# G4 Info.plist 权限对等
# ===========================================================================

def gate_plist(rep: Report) -> None:
    gate = "G4/plist"
    plist = os.path.join(IOS_DIR, "Recon", "Info.plist")
    if not os.path.isfile(plist):
        rep.err(gate, f"缺少 {rel(plist)}")
        return
    text = read_text(plist)

    for key in REQUIRED_PLIST_KEYS:
        if f"<key>{key}</key>" not in text:
            rep.err(gate, f"docs/11 §1.1 要求写入但缺失：{key}")

    for key in FORBIDDEN_PLIST_KEYS:
        if f"<key>{key}</key>" in text:
            rep.err(gate, f"docs/11 §1.1 判定为「否」但出现了：{key}")

    # 零网络（§1.6）：不得声明 ATS 例外或本地网络用途
    if "<key>NSAppTransportSecurity</key>" in text:
        rep.err(gate, "不得声明 NSAppTransportSecurity（00 R-LOCAL 零网络）")
    if "<key>NSLocalNetworkUsageDescription</key>" in text:
        rep.err(gate, "不得声明 NSLocalNetworkUsageDescription（零网络）")

    # 隐私清单必须存在（§1.5）
    priv = os.path.join(IOS_DIR, "Recon", "Resources", "PrivacyInfo.xcprivacy")
    if not os.path.isfile(priv):
        rep.err(gate, f"缺少隐私清单：{rel(priv)}")
    else:
        ptext = read_text(priv)
        if "NSPrivacyTracking" not in ptext:
            rep.err(gate, "隐私清单缺少 NSPrivacyTracking 声明")
        else:
            m = re.search(r"<key>NSPrivacyTracking</key>\s*<(true|false)\s*/>", ptext)
            if m is None:
                rep.err(gate, "无法解析 NSPrivacyTracking 的布尔值（应为 <true/> 或 <false/>）")
            elif m.group(1) == "true":
                rep.err(gate, "NSPrivacyTracking 必须为 false（本产品不做任何跟踪）")

        # 隐私清单必须声明该 App 访问的原因码 API（docs/11 §1.5 列 4 项）。
        expected_apis = {
            "NSPrivacyAccessedAPICategoryFileTimestamp",
            "NSPrivacyAccessedAPICategoryUserDefaults",
            "NSPrivacyAccessedAPICategoryDiskSpace",
            "NSPrivacyAccessedAPICategorySystemBootTime",
        }
        missing_apis = sorted(a for a in expected_apis if a not in ptext)
        if missing_apis:
            rep.err(gate, f"隐私清单缺少 docs/11 §1.5 要求的原因码 API：{missing_apis}")


# ===========================================================================
# G5 C ABI 覆盖对等
# ===========================================================================

def abi_calls(text: str) -> set[str]:
    """提取 `recon_xxx(` 形式的**调用点**（带左括号，排除类型名与注释）。"""
    code = strip_comments_and_strings(text)
    return set(re.findall(r"\b(recon_[a-z_]+)\s*\(", code))


def gate_abi(rep: Report) -> None:
    gate = "G5/abi"
    swift_abi = os.path.join(IOS_DIR, "Recon", "ReconCore.swift")
    android_abi = os.path.join(ROOT, "platform", "android", "app", "src", "main",
                               "cpp", "native-lib.cpp")
    if not os.path.isfile(swift_abi):
        rep.err(gate, f"缺少 {rel(swift_abi)}")
        return
    if not os.path.isfile(android_abi):
        rep.warn(gate, f"缺少 {rel(android_abi)}，跳过对等比较")
        return

    a = abi_calls(read_text(android_abi))
    s = abi_calls(read_text(swift_abi))

    only_android = sorted(a - s)
    if only_android:
        rep.err(gate,
                f"以下 C ABI 在 native-lib.cpp 被调用但 ReconCore.swift 未覆盖："
                f"{only_android}")
    rep.note(f"iOS 直接调用的 C ABI 函数：{len(s)} 个")


# ===========================================================================
# G6 L10nKey ↔ 源语言 JSON 键集合一致
# ===========================================================================
#
# 为什么需要这一关（真实事故）：
#   `Localization.swift` 的 `L10nKey` 枚举一度有 17 个 case 的原始值写成了
#   JSON 里**不存在**的键（如 `msg.scan.move_too_fast` 而 JSON 里是
#   `msg.notice.move_too_fast`）。后果不是编译错误，而是：运行时三级回退
#   全部落空 → UI **静默显示键名**（`msg.scan.move_too_fast`）而不是文案。
#   这与本项目既往的「静默丢弃」「单测硬编码绕过注册表」属于同一类
#   **静默失效**，必须在 CI 层面永久挡住。
#
# 判据：
#   解析 `Localization.swift`，取 `L10nKey` 每个 case 的原始值（`= "..."`），
#   逐个检查是否存在于 `zh-CN.json`。任一缺失即失败。
#   反向（JSON 有而枚举没有）只做提示，因为 UI 层尚在逐屏接线过程中，
#   允许「文案先行、引用后到」；但**引用了不存在的文案**绝不允许。

L10N_KEY_SWIFT = os.path.join(
    UI_IOS, "Recon", "Sources", "Localization", "Localization.swift")


def parse_l10n_keys(text: str) -> list[str]:
    """从 `L10nKey` 枚举体里提取每个 case 的原始值。

    **只能剥注释、不能剥字符串** —— 键名就在字符串里。
    """
    code = strip_comments_only(text)
    # 注意 `L10nKey: String, CaseIterable {` —— 冒号后除 `String` 还可能有
    # 其它协议/泛型后缀，故用 `[^{]*` 吃掉而不是写死 `\s*\{`。
    m = re.search(r"enum\s+L10nKey\s*:\s*String\b[^{]*\{(.*?)\n\}", code, re.S)
    if not m:
        return []
    body = m.group(1)
    # `case foo = "bar"` —— 注释已被剥离，不会被注释里的引号干扰
    return re.findall(r"\bcase\s+\w+\s*=\s*\"([^\"]+)\"", body)


def gate_l10n_keys(rep: Report) -> None:
    gate = "G6/l10n-key"
    if not os.path.isfile(L10N_KEY_SWIFT):
        rep.err(gate, f"缺少 {rel(L10N_KEY_SWIFT)}")
        return
    src = os.path.join(LOCALES, f"{SOURCE_LOCALE}.json")
    if not os.path.isfile(src):
        rep.err(gate, f"缺少源语言文件 {rel(src)}")
        return

    with open(src, "r", encoding="utf-8") as f:
        source_table = json.load(f)
    source_keys = {k for k in source_table if not k.startswith("_")}

    used = parse_l10n_keys(read_text(L10N_KEY_SWIFT))
    if not used:
        rep.warn(gate, "未从 L10nKey 解析到任何 case 原始值（解析器可能失配）")
        return

    dupes = sorted({k for k in used if used.count(k) > 1})
    if dupes:
        rep.err(gate, f"L10nKey 存在重复原始值（两个 case 指向同一键）：{dupes}")

    missing = sorted(set(used) - source_keys)
    if missing:
        rep.err(gate,
                f"L10nKey 引用了 {SOURCE_LOCALE}.json 中不存在的键（UI 会静默"
                f"显示键名）：{missing}")

    unused = sorted(source_keys - set(used))
    rep.note(f"L10nKey 引用 {len(used)} 个键，源语言共 {len(source_keys)} 个键；"
             f"尚未被 L10nKey 引用 {len(unused)} 个（逐屏接线中，属预期）")


# ===========================================================================
# G7 Swift/C 混编接线完整性
# ===========================================================================
#
# 为什么需要这一关（真实缺口，三个一起）：
#   1. docs/12 §4.2 明确列出 `module.modulemap` 与 `Recon-Bridging-Header.h`
#      两个文件，但**它们当时都不存在** —— 文档说了、代码没做。
#   2. 那两个文件在文档里的示例还引用了 `recon/types.h` / `recon/status.h`，
#      而这两个头**在仓库里根本不存在**（`recon_c_api.h` 是刻意自包含的单头）。
#      照抄文档就会写出引用不存在文件的代码。
#   3. 初版 `Package.swift` 用 `sources: ["../Recon/Sources"]` 引用了**包根之外**
#      的路径 —— SwiftPM 明令禁止，`swift build` 会直接失败。
#
# 判据（全部可静态验证）：
#   G7-a module.modulemap 存在；其中 `header "..."` 的每一项都必须在
#        `core/include/` 下**真实存在**（防引用虚构头）。
#   G7-b Recon-Bridging-Header.h 存在；其 `#include "..."` 每一项同样必须
#        真实存在；且**不得**包含任何 C++ 头（`.h` 里出现 `namespace`/`class`
#        的项目头即视为 C++ 头，防破坏「C ABI 是唯一跨语言边界」）。
#   G7-c Package.swift 中不得出现 `sources:` 数组里带 `..` 的路径
#        （SwiftPM 会拒收包外源码，这是回归防线）。
#   G7-d Container/project.yml 存在，且两个源码树都被引用。

CORE_INCLUDE = os.path.join(ROOT, "core", "include")
MODULEMAP = os.path.join(IOS_DIR, "ReconCore", "include", "module.modulemap")
BRIDGING = os.path.join(IOS_DIR, "Recon", "Recon-Bridging-Header.h")
PACKAGE_SWIFT = os.path.join(IOS_DIR, "Package.swift")
CONTAINER_YML = os.path.join(IOS_DIR, "Container", "project.yml")

# C++ 头的判定关键词（出现在 `core/include/recon/*.h` 即认为它是 C++ 头）
CPP_HEADER_MARKERS = ("namespace ", "class ", "template<", "template <",
                      "std::", "constexpr")


def is_cpp_header(path: str) -> bool:
    """判断某个 `core/include/recon/*.h` 是否是 C++ 头。

    **必须先剥注释** —— 否则 `recon_c_api.h` 第 6 行的文档注释
    「POD-only：…不传 `std::` 容器、不传 C++ 类」会被当成"它用了 std::"，
    直接产生假阳性。本项目在 `check_platform_parity.py` 上因注释吃过 3 次
    假阳性（该工具已内置 `strip_comments`），这里同样处理。
    """
    try:
        text = read_text(path)
    except OSError:
        return False
    code = strip_comments_and_strings(text)
    return any(m in code for m in CPP_HEADER_MARKERS)


def check_header_exists(rep: Report, gate: str, inc: str, owner: str) -> None:
    """`inc` 形如 `recon/recon_c_api.h` —— 必须在 core/include 下真实存在。"""
    cand = os.path.join(CORE_INCLUDE, *inc.split("/"))
    if not os.path.isfile(cand):
        rep.err(gate, f"{owner} 引用了不存在的头 `{inc}`（应为 {rel(cand)}）")
        return
    if is_cpp_header(cand):
        rep.err(gate, f"{owner} 引用了 C++ 头 `{inc}` —— "
                      f"违反「C ABI 是唯一跨语言边界」（docs/00 §7.1）")


def gate_wiring(rep: Report) -> None:
    gate = "G7/wiring"

    # --- G7-a module.modulemap -----------------------------------------
    if not os.path.isfile(MODULEMAP):
        rep.err(gate, f"缺少 {rel(MODULEMAP)}（docs/12 §4.2 要求的混编入口）")
    else:
        text = read_text(MODULEMAP)
        if "module ReconCore" not in text:
            rep.err(gate, "module.modulemap 未声明 `module ReconCore`"
                          "（Swift 侧 `import ReconCore` 会失败）")
        headers = re.findall(r'header\s+"([^"]+)"', text)
        if not headers:
            rep.err(gate, "module.modulemap 未声明任何 header")
        for h in headers:
            check_header_exists(rep, gate, h, "module.modulemap")
        rep.note(f"module.modulemap 暴露 {len(headers)} 个头：{headers}")

    # --- G7-b bridging header ------------------------------------------
    if not os.path.isfile(BRIDGING):
        rep.err(gate, f"缺少 {rel(BRIDGING)}（docs/12 §4.2 要求的混编入口）")
    else:
        text = read_text(BRIDGING)
        incs = re.findall(r'#include\s+"([^"]+)"', text)
        if not incs:
            rep.err(gate, "Recon-Bridging-Header.h 未包含任何 C 头")
        for h in incs:
            check_header_exists(rep, gate, h, "Recon-Bridging-Header.h")

    # --- G7-c Package.swift 包外源码 ------------------------------------
    if not os.path.isfile(PACKAGE_SWIFT):
        rep.err(gate, f"缺少 {rel(PACKAGE_SWIFT)}")
    else:
        # ⚠️ 这里查的是「字符串字面量里的内容」（`.library(name: "ReconCore")`
        #    的 `"ReconCore"`、以及可能带 `..` 的路径串），所以**只能剥注释**；
        #    用 strip_comments_and_strings 会把字面量一起吃掉，导致漏检。
        raw = read_text(PACKAGE_SWIFT)
        code = strip_comments_only(raw)
        # 精确抓 `sources:` / `path:` 数组或字符串里的 `..`
        for m in re.finditer(r'(sources|path)\s*:\s*(\[[^\]]*\]|"[^"]*")', code):
            val = m.group(2)
            if ".." in val:
                rep.err(gate,
                        f"Package.swift 的 `{m.group(1)}` 引用了包根之外的路径 "
                        f"（SwiftPM 会拒收）：{val.strip()}")
        if "ReconCore" not in raw:
            rep.err(gate, "Package.swift 未声明 `ReconCore`（与 C ABI 模块名/"
                          "docs/12 §4.3 产物名不一致）")

    # --- G7-d 容器定义 --------------------------------------------------
    if not os.path.isfile(CONTAINER_YML):
        rep.err(gate, f"缺少 {rel(CONTAINER_YML)}（UI 与桥接层的 App 容器定义）")
    else:
        text = read_text(CONTAINER_YML)
        for needed in ("ui/ios/Recon/Sources", "platform/ios/Recon"):
            if needed not in text:
                rep.err(gate, f"Container/project.yml 未引用源码树 `{needed}`")

    # --- G7-e ObjC 桥类对 Swift 的可见性（真实缺口）-----------------------
    #
    # 为什么需要这一关（第三十五轮实测发现）：
    #   `ScanSessionModel.swift` 用了 `ReconCaptureStateMachine.shared()`，但
    #   `Recon-Bridging-Header.h` 里**没有** `#import "CaptureStateBridge.h"`。
    #   ObjC 类**只有**在 bridging header 里被 import 才对该 target 的全部
    #   Swift 文件可见 —— 仅把 `.mm` 加进 target 源码是不够的。
    #
    #   G7-b 只校验 `#include "..."` 指向的 **C 头是否存在**，因此**查不出**
    #   这类漏网：真机 Xcode 构建会报 "cannot find 'ReconCaptureStateMachine'
    #   in scope"，而本项目既有的全部静态闸门**会放行**。
    #
    # 判据（落在目标对象上，可负向注入证伪）：
    #   ① 扫描 `ui/ios/Recon/Sources/**/*.swift`，收集所有「形如 `Recon` 开头的
    #      ObjC 类名」的标识符使用；
    #   ② 对每个这样的标识符，在 `platform/ios/Recon/*.h` 里找哪个头声明了它
    #      （`@interface <Name>`）；
    #   ③ 该头必须被 `Recon-Bridging-Header.h` `#import`。
    #
    # 注意：**不能**简单地「把 platform/ios/Recon 下所有头都 import 了就放过」
    # —— 那等于没有判据。必须从「Swift 实际用了谁」出发反查，因此新增桥类却
    # 忘记接线时本关会立即报错。
    if not os.path.isfile(BRIDGING):
        pass  # G7-b 已报缺失。
    else:
        bridging_text = read_text(BRIDGING)
        imported = set(re.findall(r'#import\s+"([^"]+)"', bridging_text))
        # 收集 platform/ios/Recon 下每个头声明的 ObjC 类名。
        ios_recon_dir = os.path.dirname(BRIDGING)
        decl_to_header: dict[str, str] = {}
        if os.path.isdir(ios_recon_dir):
            for fn in sorted(os.listdir(ios_recon_dir)):
                if not fn.endswith(".h"):
                    continue
                htext = read_text(os.path.join(ios_recon_dir, fn))
                for cls in re.findall(r"@interface\s+(Recon\w+)", htext):
                    decl_to_header.setdefault(cls, fn)

        # 收集 Swift 源码里用到的 ObjC 桥类名。
        swift_dir = os.path.join(UI_IOS, "Recon", "Sources")
        used: dict[str, str] = {}  # 类名 → 首个使用它的 Swift 相对路径
        if decl_to_header and os.path.isdir(swift_dir):
            for root, _dirs, files in os.walk(swift_dir):
                for fn in files:
                    if not fn.endswith(".swift"):
                        continue
                    spath = os.path.join(root, fn)
                    code = strip_comments_and_strings(read_text(spath))
                    for cls in decl_to_header:
                        if re.search(r"\b" + re.escape(cls) + r"\b", code):
                            used.setdefault(cls, os.path.relpath(spath, ROOT))

        missing_imports = []
        for cls, header in sorted(decl_to_header.items()):
            if cls not in used:
                continue  # 该桥类无人使用（死码，另一类问题，不在此关裁决）。
            if header not in imported:
                missing_imports.append((cls, header, used[cls]))
        if missing_imports:
            for cls, header, where in missing_imports:
                rep.err(gate,
                        f"Swift（{where}）使用了 ObjC 桥类 `{cls}`，但其头 "
                        f"`{header}` 未被 {os.path.basename(BRIDGING)} `#import` "
                        f"—— 真机构建会报 cannot find '{cls}' in scope")
        else:
            rep.note(f"ObjC 桥类可见性：{len([c for c in decl_to_header if c in used])} "
                     f"个被 Swift 使用的桥类均已 import ✓")


# ===========================================================================
# G8 三级回退链可达性（Python 忠实移植 Swift 的 resolve()）
# ===========================================================================
#
# 为什么需要这一关：
#   §6.1 的回退链是 `用户语言 → en-US → zh-CN(源) → key 本身`。这条链**只靠
#   读代码看不出对错** —— 它依赖每个 locale 文件里**实际有哪些键**。例如：
#   某 locale 文件若把 `_meta` 写成对象而加载器按 `[String:String]` 解码，则
#   该 locale 整体加载失败（本项目真实发生过），回退链会**静默**多退一级。
#   因此必须按真实文件内容逐 (key × locale) 推演一遍。
#
# 判据：
#   · 对 L10nKey 的每个键，在**每个** locale 下都必须解析到「非键名」的文案。
#     解析到键名即意味着三级回退全部落空 —— 用户会看到
#     `msg.home.title` 这种字符串。这是硬失败。
#   · 逐 locale 报告「本 locale 自有键数 / 借 en-US 数 / 借 zh-CN 数 / 落空数」，
#     让「待翻译」是可度量的而不是感觉上的。

def load_locale_tables() -> tuple[dict[str, dict[str, str]], dict[str, str]]:
    """按 `LocaleCatalog.loadAll` 的语义加载。

    返回 `(表, 状态)`，其中 `状态[loc]` ∈ {"full", "pending", "missing", "empty"}：
      · `full`    —— 有文案；
      · `pending` —— 文件在、**显式**声明 `translation_status = "pending"`、
                     无 `msg.*` 键。这是**合法**的「待翻译」（docs/09 §6.1 允许，
                     且刻意不预填机器翻译）；
      · `empty`   —— 文件在、既无文案也无 pending 声明 → 违规（静默空壳）；
      · `missing` —— 文件不存在 → 违规。

    只取字符串值、忽略 `_meta`（与 Swift 的 JSONSerialization 实现同构）。
    """
    tables: dict[str, dict[str, str]] = {}
    status: dict[str, str] = {}
    for loc in EXPECTED_LOCALES:
        path = os.path.join(LOCALES, f"{loc}.json")
        if not os.path.isfile(path):
            tables[loc] = {}
            status[loc] = "missing"
            continue
        with open(path, "r", encoding="utf-8") as f:
            raw = json.load(f)
        table = {k: v for k, v in raw.items() if isinstance(v, str)}
        tables[loc] = table
        if table:
            status[loc] = "full"
            continue
        meta = raw.get("_meta")
        declared = isinstance(meta, dict) and \
            str(meta.get("translation_status", "")).lower() == "pending"
        status[loc] = "pending" if declared else "empty"
    return tables, status


def resolve_chain(tables: dict[str, dict[str, str]], key: str, loc: str) -> tuple[str, str]:
    """移植 Swift `LocaleCatalog.resolve`。返回 (结果, 来源)。

    来源 ∈ {"self", "en-US", "zh-CN", "missing"}。
    **必须与 Swift 实现保持同构** —— 这是本闸门的全部价值所在。
    """
    for src in (loc, FALLBACK_LOCALE, SOURCE_LOCALE):
        v = tables.get(src, {}).get(key)
        if v:                                  # Swift 侧判据是 `!v.isEmpty`
            return v, src
    return key, "missing"


def gate_fallback(rep: Report) -> None:
    gate = "G8/fallback"
    if not os.path.isfile(L10N_KEY_SWIFT):
        rep.err(gate, "缺少 Localization.swift，无法取键集合")
        return

    used = parse_l10n_keys(read_text(L10N_KEY_SWIFT))
    if not used:
        rep.err(gate, "未从 L10nKey 解析到键（解析器失配）")
        return

    tables, status = load_locale_tables()
    # 只有「文件不存在」或「文件在但既无文案也无 pending 声明」才是违规。
    # 显式声明 pending 的空文件是 §6.1 允许的合法待翻译态。
    bad = [l for l in EXPECTED_LOCALES if status[l] in ("missing", "empty")]
    if bad:
        rep.err(gate, f"以下 locale 文件缺失或是未声明的空壳："
                      f"{[(l, status[l]) for l in bad]}")
    _ = status  # 状态已用于上面的判定；明示以免被误读为未使用

    # 逐 locale 统计
    summary: list[str] = []
    for loc in EXPECTED_LOCALES:
        own = borrow_en = borrow_zh = 0
        holes: list[str] = []
        for k in used:
            _val, src = resolve_chain(tables, k, loc)
            if src == loc:            own += 1
            elif src == FALLBACK_LOCALE: borrow_en += 1
            elif src == SOURCE_LOCALE:   borrow_zh += 1
            else:                        holes.append(k)
        if holes:
            rep.err(gate, f"locale {loc} 下 {len(holes)} 个键三级回退全部落空"
                          f"（用户会看到键名）：{holes[:6]}"
                          f"{' …' if len(holes) > 6 else ''}")
        if loc not in (SOURCE_LOCALE, FALLBACK_LOCALE):
            summary.append(f"{loc}: 自有{own}/借英{borrow_en}/借中{borrow_zh}")

    translated = sum(1 for l in EXPECTED_LOCALES
                     if status[l] == "full" and l not in (SOURCE_LOCALE, FALLBACK_LOCALE))
    pending = [l for l in EXPECTED_LOCALES if status[l] == "pending"]
    rep.note(f"回退链可达性：{len(used)} 键 × {len(EXPECTED_LOCALES)} locale 全部"
             f"解析到文案（无落空）")
    rep.note(f"唤醒：已翻译 {translated + 2}/20（含源与二级回退）；"
             f"显式待翻译 {len(pending)} 个")
    rep.note(f"待翻译 locale 的覆盖分布：{'；'.join(summary[:6])}"
             f"{' …' if len(summary) > 6 else ''}")


# ===========================================================================
# G9 UI 文案去硬编码（视图层不得再出现未接线的中文）
# ===========================================================================
#
# 为什么需要这一关：
#   UI 层的文案一旦**硬编码在 Swift 源码里**，那 20 个 locale 文件就是死数据
#   —— 用户切到英文仍然看到中文。这类问题编译器和 Xcode 都不会报错。
#
#   本闸门在 G8 的「正向可达性」（键都能解析到文案）之外补上**反向闭合性**：
#   视图层出现的每一段中文，都必须经 `L10n.*` 取值。
#
# 判据：
#   `ui/ios/Recon/Sources/` 下代码区（已剥注释）的中文字符串字面量，必须
#   处于 `L10n.s(...)` / `L10n.t(...)` / `L10n.raw(...)` / `L10n.fill(...)`
#   调用之内，或命中 `NON_UI_COPY_ALLOWLIST`（开发者消息/数据，非界面文案）。
#
# 覆盖范围：Screens/ 全部；其余目录（Design/State/Platform）同样扫描 ——
# 因为它们也产出界面可见文本。

# 允许保留的中文字面量：**不是界面文案**，故不查表。
NON_UI_COPY_ALLOWLIST = {
    # 开发者断言消息（preconditionFailure，不会呈现给用户）。
    "非法色值字面量 '\\(hex)'：应为 #RRGGBB 或 #AARRGGBB",
    # Liquid Glass 降级说明（开发者诊断信息，经 GlassRole 暴露给调试面板，
    # 不属 §5 文案表；若将来要呈现给用户，须先入表）。
    "当前构建使用 Xcode 15.4 基线（iOS 17 SDK），无法编译 Liquid Glass 材质，界面使用系统半透明材质替代。",
    "当前系统版本低于 iOS 26，界面使用系统半透明材质替代 Liquid Glass。",
    # 演示/占位**数据**（文件路径与会话名），不是 UI 文案。
    "/Documents/陶瓷杯.glb",
    "陶瓷杯",
}

UI_SRC_DIR = os.path.join(UI_IOS, "Recon", "Sources")


def strip_comments_keep_strings(text: str) -> str:
    """剥注释、留字符串（与 wire_l10n.py 的 `transform_states` 同构）。"""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


L10N_CALL_BEFORE = re.compile(r"L10n\.(?:s|t|raw|fill|fill2)\s*\(\s*$")
STR_LIT = re.compile(r'"([^"\n]*)"')


def gate_hardcoded_copy(rep: Report) -> None:
    gate = "G9/hardcoded"
    if not os.path.isdir(UI_SRC_DIR):
        rep.err(gate, f"缺少 {rel(UI_SRC_DIR)}")
        return

    # 源语言表（供 G9-b 反查）。
    src_path = os.path.join(LOCALES, f"{SOURCE_LOCALE}.json")
    source_table: dict[str, str] = {}
    if os.path.isfile(src_path):
        with open(src_path, "r", encoding="utf-8") as f:
            source_table = {k: v for k, v in json.load(f).items()
                            if isinstance(v, str)}
    source_values = {v for v in source_table.values() if v}
    enum_keys = set(parse_l10n_keys(read_text(L10N_KEY_SWIFT))) \
        if os.path.isfile(L10N_KEY_SWIFT) else set()
    # case 名集合（供 G9-b-2 校验 `.枚举键.rawValue` 的写法）。
    enum_names: set[str] = set()
    if os.path.isfile(L10N_KEY_SWIFT):
        enum_names = set(re.findall(
            r"^\s*case\s+(\w+)\s*=",
            strip_comments_only(read_text(L10N_KEY_SWIFT)), re.M))

    violations: list[str] = []
    scanned = 0
    # G9-b 计数
    s_lits = 0
    unresolved = []
    key_args = 0
    t_checked = 0
    bad_keys = []
    for dirpath, _dn, filenames in os.walk(UI_SRC_DIR):
        for fn in sorted(filenames):
            if not fn.endswith(".swift"):
                continue
            # Localization.swift 自身含键名与实现，跳过。
            if fn == "Localization.swift":
                continue
            path = os.path.join(dirpath, fn)
            text = read_text(path)
            code = strip_comments_keep_strings(text)

            # --- G9-a 中文字面量必须处于 L10n 调用之内 ----------------------
            for m in STR_LIT.finditer(code):
                lit = m.group(1)
                if not re.search(r"[\u4e00-\u9fa5]", lit):
                    continue
                scanned += 1
                if L10N_CALL_BEFORE.search(code[:m.start()]):
                    continue
                if lit in NON_UI_COPY_ALLOWLIST:
                    continue
                violations.append(f"{rel(path)} :: {lit[:60]}")

            # --- G9-b-1 `L10n.s("原文")` 的原文必须能反查到键 ----------------
            #     反查不到意味着 `L10n.s` 会**原样返回中文**（见 Localization.swift
            #     `s(_:)` 的实现）→ 非中文用户看到整段中文，且**不报错**。
            #     这比「没接线」更隐蔽：源码里明明有 `L10n.` 调用，G9-a 会放过它。
            for m in re.finditer(r'L10n\.s\(\s*"((?:[^"\\]|\\.)*)"\s*\)', code):
                lit = m.group(1)
                s_lits += 1
                if lit not in source_values:
                    ln = code[:m.start()].count("\n") + 1
                    unresolved.append(f"{rel(path)}:{ln} :: {lit[:60]}")
                    continue
                # 歧义：同一原文对应多个键，且译文不同 → 反查只能二选一，
                # 必然有调用点被译错。必须改用 `L10n.t(.枚举键)`。
                cands = [k for k, v in source_table.items() if v == lit]
                if len(cands) > 1:
                    en_path = os.path.join(LOCALES, "en-US.json")
                    en = {}
                    if os.path.isfile(en_path):
                        with open(en_path, "r", encoding="utf-8") as f:
                            en = {k: v for k, v in json.load(f).items()
                                  if isinstance(v, str)}
                    if len({en.get(k, "") for k in cands}) > 1:
                        ln = code[:m.start()].count("\n") + 1
                        unresolved.append(
                            f"{rel(path)}:{ln} :: 歧义原文 {lit[:40]} "
                            f"（候选键 {sorted(cands)} 译文不同，须用 L10n.t 指定）")

            # --- G9-b-2 `L10n.fill/fill2/raw` 的键参数必须在枚举内 ----------
            #     接受两种形态：
            #       `L10n.fill(.fmtDepthTier, …)`          类型化重载（首选）
            #       `L10n.fill(.fmtDepthTier.rawValue, …)` 显式取值（等价）
            #     两者的 case 名都必须是 `L10nKey` 的真实成员。
            for m in re.finditer(
                    r'L10n\.(?:fill2?|raw)\(\s*(.+?)\s*,', code):
                arg = m.group(1).strip()
                em = re.match(r"\.(\w+)(?:\.rawValue)?$", arg)
                if em:
                    name = em.group(1)
                    if name not in enum_names:
                        bad_keys.append(
                            f"{rel(path)} :: .{name} 不在 L10nKey 中")
                    else:
                        key_args += 1
                    continue
                sm = re.match(r'"([^"]+)"$', arg)
                if sm:
                    key_args += 1
                    lit = sm.group(1)
                    # 明文键的**真正风险**不是「不在枚举里」，而是「表里没有」：
                    # 后者会让 `resolve` 走完三级回退 → 显示键名本身。
                    # G6 只看枚举声明，看不到调用点上的明文键，故这里必须自己查表。
                    if lit not in source_table:
                        bad_keys.append(
                            f"{rel(path)} :: 明文键 \"{lit}\" 不在 {SOURCE_LOCALE}.json"
                            f" 中（UI 会显示键名自身）")
                    elif lit not in enum_keys:
                        bad_keys.append(
                            f"{rel(path)} :: 明文键 \"{lit}\" 未收进 L10nKey 枚举"
                            f"（G6 覆盖不到该调用点，请改用 .枚举键.rawValue）")
                    continue

            # --- G9-b-3 `L10n.t(.name)` 的 case 必须是 L10nKey 真实成员 -----
            #
            # 为什么必须补这一关（2026-10-31 实测假闸门）：
            #   G6 校验的是「**已声明**的 case → JSON 键存在」；
            #   G9-b-2 校验的是 `fill/raw` 的键参数；
            #   但 **`L10n.t(.名字)` 的 `名字` 是否真的在枚举里，此前无人校验**。
            #   实测 `ParamsView` 接线过程中产生 62 个 `.xxx` 引用指向**不存在**的
            #   case —— Swift 会编译失败，而整条闸门链报「13/13 通过」。
            #   这正是本项目反复出现的「假闸门」：判据没有落在目标对象上。
            #
            # 判据落在「引用 → 声明」方向：每个 `L10n.t(.name)` 的 name
            # 必须是 `L10nKey` 的成员。
            for m in re.finditer(r"L10n\.t\(\s*\.(\w+)", code):
                t_name = m.group(1)
                t_checked += 1
                if t_name not in enum_names:
                    ln = code[:m.start()].count("\n") + 1
                    bad_keys.append(
                        f"{rel(path)}:{ln} :: .{t_name} 不在 L10nKey 中"
                        f"（Swift 编译失败；G6/G9 此前覆盖不到此方向）")

    for v in violations:
        rep.err(gate, f"视图层存在未接线的中文文案（不会随语言切换）：{v}")
    for u in unresolved:
        rep.err(gate, f"L10n.s 的原文无法精确反查到键（非中文用户会看到中文）：{u}")
    for b in bad_keys:
        rep.err(gate, f"格式串键参数不合格（G6 覆盖不到）：{b}")

    rep.note(f"UI 文案去硬编码：扫描 {scanned} 段中文，未接线 {len(violations)} 段"
             f"（白名单 {len(NON_UI_COPY_ALLOWLIST)} 段非文案）；"
             f"L10n.s 反查 {s_lits} 处，不可反查/歧义 {len(unresolved)} 处；"
             f"格式串键 {key_args} 个，不合格 {len(bad_keys)} 个；"
             f"L10n.t 枚举引用 {t_checked} 处，越界 {len(bad_keys)} 处")


# G10 C ABI 边界完整性（类型标签 / 长度上界 / 空输入 / 并发唯一性）
# ===========================================================================
#
# 为什么需要这一关：
#   G5 只验「Swift 调用的每个 recon_* 符号都在头文件里声明过」。但**签名对了
#   不代表边界安全**。本轮的真实缺陷有三类，G5 一个都挡不住：
#
#   (a) 类型标签（`recon_export_format` 之类的枚举）在头文件里**没有**
#       `= 0` 的显式值 → Swift 侧 `0` 与 C 侧某值可能错位，导出格式被静默换掉。
#   (b) 长度参数没有上界断言 → 传入 `size > UINT32_MAX` 时内部截断，
#       缓冲被短写却返回成功（静默丢数据）。
#   (c) 空输入（`count == 0` / `data == NULL`）未定义 → 空网格导出时崩溃。
#   (d) 并发：同一个会话句柄被两个线程同时进入 → 数据竞争。
#
#   这四类都不会让编译失败、也不会让单测失败（单测总是传合法小数据），
#   属「单测孤岛测不到」的静默失效。
#
# 判据（全部只需文本分析，不需编译）：
#   G10-a：头文件中每个 `typedef enum` / `enum recon_*` 块里，若存在
#          `RECON_*_INVALID = 0` 之类的哨兵，则**每个**枚举成员都必须显式赋值。
#   G10-b：`recon_c_api.h` 里出现 `uint32_t n_*` / `uint32_t *_count` 形态的
#          计数参数时，函数体内（`core/` 或 `platform/common/` 的实现）必须
#          存在对应的上界断言（`RECON_MAX_` 常量或 `<` 比较）。
#   G10-c：`recon_c_api.h` 里的每个 `recon_*` 导出函数，其实现必须处理
#          `count == 0` 或空指针（`if (!ptr)` / `== nullptr`）。
#   G10-d：会话句柄（`recon_session*`）的并发进入必须有互斥（`std::mutex` /
#          `std::atomic`）。

#: 允许在头文件里「不显式赋值」的枚举说明（无哨兵的纯计数枚举）。
ENUM_EXPLICIT_EXEMPT = set()


def gate_abi_boundary(rep: Report) -> None:
    gate = "G10/abi-boundary"
    # **必须基于 ROOT 拼绝对路径**：本脚本既被开发者从仓库根直接调用，
    # 也被 ctest 从 `build_cmake/` 调用 —— 用相对路径在后者下会「找不到文件」。
    # （第一版就是这样在 ctest 里失败的：`缺少 core\include\recon\recon_c_api.h`。）
    header = os.path.join(ROOT, "core", "include", "recon", "recon_c_api.h")
    if not os.path.isfile(header):
        rep.err(gate, f"缺少 {header}")
        return

    text = strip_comments_and_strings(read_text(header))
    raw = read_text(header)

    # --- G10-a 枚举成员显式赋值 -------------------------------------------
    enum_blocks = re.findall(r"enum\s+(?:recon_\w+)?\s*\{([^}]*)\}", raw)
    checked = 0
    unassigned = []
    for block in enum_blocks:
        items = [it.strip() for it in block.split(",") if it.strip()]
        # 只关心有哨兵语义的枚举：至少一个成员显式 = 0
        if not any(re.search(r"=\s*0\b", it) for it in items):
            continue
        checked += 1
        for it in items:
            name = re.match(r"([A-Za-z_]\w*)", it)
            if not name:
                continue
            if not re.search(r"=\s*[-\w]", it):
                unassigned.append(name.group(1))
    if unassigned:
        rep.err(gate, f"含哨兵的枚举存在未显式赋值的成员（跨语言 ABI 会错位）："
                      f"{unassigned}")

    # --- G10-b 长度类参数的契约必须可静态验证 -----------------------------
    #     第一版这里查的是「计数参数是否有上界常量」，实测**恒不触发** ——
    #     因为本 API 的计数全部在结构体里（`imu_count` 等），函数签名里根本
    #     没有 `uint32_t n_*` 参数。「恒不报错」的判据等于没有判据（假闸门），
    #     已弃用。
    #
    #     真实风险在这里：`recon_calibrate_scale` 的 `const double* detected_corners`
    #     是**固定长度数组**（8 个 double），却没有配套计数参数。这类参数一旦
    #     写入个数与实现预期不符，会造成越界读 —— 而 C ABI 是语言边界，
    #     Swift 侧无从校验。唯一能静态守住的契约就是**注释里写明元素个数**。
    #
    #     判据：函数签名里每个「指向算术类型的裸指针」参数（`const double*`、
    #     `float*` 等），必须要么有配套计数参数（含 `count`/`size`/`len` 的
    #     参数名），要么其**紧邻文档注释**里出现「N 个」形式的数量声明。
    impl_text = ""
    for d in (os.path.join(ROOT, "core", "src"),
              os.path.join(ROOT, "platform", "common", "src")):
        if not os.path.isdir(d):
            continue
        for dp, _dn, fns in os.walk(d):
            for fn in fns:
                if fn.endswith((".c", ".cc", ".cpp")):
                    impl_text += read_text(os.path.join(dp, fn))
    impl_code = strip_comments_and_strings(impl_text)

    # 取「前置注释 + 单个函数声明」为一组（用 raw 才能读到注释）。
    # **注意捕获组要包住函数声明本体**：第一版写成
    #   `((?:/\*\*.*?\*/\s*)*)RECON_API[^;]*;`
    # —— 捕获组只包了注释，`g` 里没有参数列表 → 裸指针数恒为 0（空判据）。
    decl_groups = [m.group(0) for m in re.finditer(
        r"(?:(?:/\*\*.*?\*/|//[^\n]*)\s*)*RECON_API[^;]*;", raw, re.S)]
    undocumented = []
    checked_ptrs = 0
    for g in decl_groups:
        doc = g
        pm0 = re.search(r"\(([^)]*)\)", g, re.S)
        if not pm0:
            continue
        params = pm0.group(1)
        # 配套计数参数？只看参数列表。
        has_counter = bool(re.search(
            r"\b\w*(?:count|size|len|num)_?\w*\b", params))
        for pm in re.finditer(
                r"const\s+(?:double|float|int32_t|uint32_t|uint8_t)\s*\*\s*(\w+)",
                params):
            checked_ptrs += 1
            name = pm.group(1)
            if has_counter:
                continue
            if re.search(r"\d\s*个", doc):
                continue
            undocumented.append(name)
    if undocumented:
        rep.err(gate,
                "固定长度数组参数未在注释里声明元素个数（越界读无从校验）："
                f"{sorted(set(undocumented))}")

    # --- G10-c 空输入处理 --------------------------------------------------
    # 头文件里声明的每个导出函数，其名必须能在实现里找到；
    # 且实现中至少出现一次空指针守卫（全校级即可 —— 逐函数的严格判据
    # 需要解析 C，超出本闸门「静态可验证」的边界，此处如实标注为弱判据）。
    #     注意用 `(recon_\w+)` 捕获**纯函数名** —— 第一版写成
    #     `\brecon_\w+\s*\(`（无捕获组）→ `findall` 返回整个匹配，
    #     结果集合里全是带尾随 `(` 的名字（`recon_create(`），与定义集
    #     永远不相交 → 21 个函数全被误报为「没实现」。
    exported = set(re.findall(r"\b(recon_\w+)\s*\(", text))
    # --- G10-c 声明与定义闭合（禁止「声明了没实现」）------------------------
    #     `recon_c_api.h` 里 `RECON_API` 声明的每个函数，其**定义**必须能在
    #     实现源文件里找到（函数名后跟 `(`，且不是以 `;` 结尾的声明）。
    #     缺定义只会在**链接期**报错 —— 而本项目 iOS 侧是静态库，
    #     链接错误会拖到 App 打包阶段才暴露，反馈链极长。
    #
    #     （原先这里只数了一遍「实现里有没有空指针守卫」，那是个「恒真」的
    #       弱判据 —— 项目实现里到处都有守卫，永远不报错。已弃用。）
    c_api_impl = os.path.join(ROOT, "core", "src", "util", "c_api.cpp")
    if os.path.isfile(c_api_impl):
        impl_raw = read_text(c_api_impl)
        # 定义：行首（可有返回类型/属性）出现 `recon_xxx(`，且该行不以 `;` 结束。
        defined = set()
        for dm in re.finditer(r"^[A-Za-z_][^\n;]*?\b(recon_\w+)\s*\(", impl_raw, re.M):
            line_end = impl_raw.find("\n", dm.start())
            line = impl_raw[dm.start():line_end if line_end > 0 else len(impl_raw)]
            if line.rstrip().endswith(";"):
                continue
            defined.add(dm.group(1))
        missing_def = sorted(exported - defined)
        if missing_def:
            rep.err(gate, "C ABI 声明了但**没有实现**（链接期才会失败）："
                          f"{missing_def}")
    else:
        rep.err(gate, f"缺少 C ABI 实现 {rel(c_api_impl)}")

    if exported:
        guards = len(re.findall(r"!\s*\w+\s*\)|==\s*nullptr|!=\s*nullptr", impl_code))
        if guards == 0:
            rep.err(gate, "C ABI 实现中没有任何空指针守卫（空网格输入会崩溃）")
        rep.note(f"C ABI 边界：enum 块 {checked} 个（成员显式赋值检查通过）、"
                 f"导出函数 {len(exported)} 个、实现空指针守卫 {guards} 处；"
                 f"固定长度数组参数 {checked_ptrs} 个（个数声明已核）")

    # --- G10-d 会话并发保护 ------------------------------------------------
    session_hdr = os.path.join(ROOT, "platform", "common", "include")
    mutex_found = False
    if os.path.isdir(session_hdr):
        for dp, _dn, fns in os.walk(session_hdr):
            for fn in fns:
                if fn.endswith((".h", ".hpp")):
                    t = read_text(os.path.join(dp, fn))
                    if "std::mutex" in t or "std::atomic" in t:
                        mutex_found = True
    if "recon_session" in text and not mutex_found:
        rep.warn(gate, "未在平台层头文件中发现 std::mutex / std::atomic —— "
                       "会话句柄的跨线程访问保护需要在适配层显式确认")


# ===========================================================================
# G11 recon_status 逐值对等（Swift 枚举 ↔ C ABI 头文件）
# ===========================================================================
#
# 为什么需要这一关（真实事故）：
#   `platform/ios/Recon/ReconCore.swift` 的 `enum ReconStatus: Int32` 一度只列了
#   14 项（ok=0..noCapability=9、采集 100..104、内存 1100/1101），而
#   `core/include/recon/recon_c_api.h` 的 `recon_status` 实有 **47 项**。
#   缺的 33 项（200-203 同步 / 300-302 VIO / 400-403 SfM / 500-502 MVS /
#   600-602 深度 / 700-702 网格 / 800-802 后处理 / 900-903 标定 / 1000-1002 导出）
#   在 Swift 侧走 `ReconStatus(rawValue:)` 一律返回 nil → 全部落回 `.internalError`。
#   后果：**所有网格/后处理/标定/导出错误被静默错报成「内部错误」**，
#   且 docs/09 §5.5 的「失败时必须列出对应错误码」无法满足。
#   这与本项目既往的「静默丢弃」「单测硬编码绕过注册表」同类 —— 都是**静默失效**。
#
# 判据（锚定「目标对象」而非代理指标）：
#   解析头文件 `recon_status` 的 (枚举名, 值) 对，以及 Swift `ReconStatus` 的
#   (case 名, rawValue) 对，比较二者的**值集合**。任一方向不相等即失败：
#     · 头文件有而 Swift 无 → 该错误码在 iOS 侧会被错报（本闸门要拦的正是它）；
#     · Swift 有而头文件无 → 引用了一个不存在的错误码（更严重，会读到野值）。
#   同时校验文档化的**条数**（47），防止头文件被删项而无人察觉。
#
# 注意：C 枚举与 Swift 枚举的**成员名不同**（RECON_ERR_MESH_POISSON_FAILED
#    vs meshPoissonFailed），故只比较**数值**，不比较名字 —— 比较名字会
#    把「命名风格差异」误报成错误码缺失（弱判据/假阳性）。

C_API_HEADER = os.path.join(ROOT, "core", "include", "recon", "recon_c_api.h")
SWIFT_RECON_CORE = os.path.join(IOS_DIR, "Recon", "ReconCore.swift")
EXPECTED_STATUS_COUNT = 47


def parse_c_status_values(text: str) -> dict[str, int]:
    """解析 `typedef enum recon_status { ... }` 的 (成员名, 值)。

    · 只认**显式赋值**（`FOO = 123`），与 G10-a 的约定一致；
    · `RECON_OK = 0` 也带 `= 0`，故无需特判 0；
    · 注释先剥离，避免注释里的 `= 100` 之类干扰。
    """
    code = strip_comments_only(text)
    m = re.search(r"typedef\s+enum\s+recon_status\s*\{(.*?)\}\s*recon_status\s*;",
                  code, re.S)
    if not m:
        # 兼容 `typedef enum { ... } recon_status;` 的匿名形式
        m = re.search(r"typedef\s+enum\s*\{(.*?)\}\s*recon_status\s*;", code, re.S)
    if not m:
        return {}
    out: dict[str, int] = {}
    for line in m.group(1).split(","):
        mm = re.match(r"\s*(RECON_\w+)\s*=\s*(\d+)\s*$", line)
        if mm:
            out[mm.group(1)] = int(mm.group(2))
    return out


def parse_swift_status_values(text: str) -> dict[str, int]:
    """解析 Swift `enum ReconStatus: Int32 { case foo = 100 ... }`。

    只认 `case <name> = <digits>`；Swift 侧**不允许**省略 rawValue
    （若省略，rawValue 由编译器推导，静态不可知 —— 那是另一种静默失效，
    故本闸门对「无 rawValue 的 case」直接判失败，而不是忽略）。
    """
    code = strip_comments_only(text)
    m = re.search(r"enum\s+ReconStatus\s*:\s*Int32\s*\{(.*?)\n\}",
                  code, re.S)
    if not m:
        return {}
    body = m.group(1)
    out: dict[str, int] = {}
    implicit: list[str] = []
    for cm in re.finditer(r"\bcase\s+(\w+)\s*(?:=\s*(\d+))?", body):
        name, val = cm.group(1), cm.group(2)
        if val is None:
            implicit.append(name)
        else:
            out[name] = int(val)
    # 无 rawValue 的 case 单独上报（由调用方决定如何报错）
    out["__implicit__"] = implicit  # type: ignore[assignment]
    return out


def gate_status_parity(rep: Report) -> None:
    gate = "G11/status-parity"
    if not os.path.isfile(C_API_HEADER):
        rep.err(gate, f"缺少 {rel(C_API_HEADER)}")
        return
    if not os.path.isfile(SWIFT_RECON_CORE):
        rep.err(gate, f"缺少 {rel(SWIFT_RECON_CORE)}")
        return

    c_map = parse_c_status_values(read_text(C_API_HEADER))
    if not c_map:
        rep.err(gate, f"{rel(C_API_HEADER)} 未解析出任何 recon_status 显式赋值")
        return

    swift_raw = parse_swift_status_values(read_text(SWIFT_RECON_CORE))
    implicit = swift_raw.pop("__implicit__", []) if "__implicit__" in swift_raw else []
    swift_map: dict[str, int] = swift_raw  # type: ignore[assignment]

    if implicit:
        rep.err(gate, f"ReconStatus 存在无显式 rawValue 的 case（rawValue 静态不可知）："
                      f"{implicit}")

    c_vals = set(c_map.values())
    s_vals = set(swift_map.values())

    missing_in_swift = sorted(c_vals - s_vals)
    extra_in_swift = sorted(s_vals - c_vals)

    if missing_in_swift:
        detail = ", ".join(
            f"{v}({next(k for k, vv in c_map.items() if vv == v)})"
            for v in missing_in_swift)
        rep.err(gate, f"C ABI 有但 Swift ReconStatus 缺 {len(missing_in_swift)} 项"
                      f"（这些错误码在 iOS 侧会被静默错报）：{detail}")
    if extra_in_swift:
        detail = ", ".join(
            f"{v}({next(k for k, vv in swift_map.items() if vv == v)})"
            for v in extra_in_swift)
        rep.err(gate, f"Swift ReconStatus 有但 C ABI 无 {len(extra_in_swift)} 项"
                      f"（引用了不存在的错误码）：{detail}")

    if len(c_vals) != EXPECTED_STATUS_COUNT:
        rep.warn(gate, f"recon_status 条数 {len(c_vals)} ≠ 文档基准 {EXPECTED_STATUS_COUNT} "
                       f"—— 若确为契约变更，请同步本文件 EXPECTED_STATUS_COUNT 与 docs/01 §4")

    if not missing_in_swift and not extra_in_swift:
        rep.note(f"recon_status 逐值对等：C ABI {len(c_vals)} 项 ↔ Swift {len(s_vals)} 项 ✓")


# ===========================================================================
# G12 ReconStageMap 内部一致性（reachable ⊔ gaps ⊔ 段数）
# ===========================================================================
#
# 为什么需要这一关：
#   `ui/ios/Recon/Sources/State/ReconStageMap.swift` 同时承载三份**手工维护**的
#   数据：`uiSegmentCount`（总段数）、`reachableSegments`（可达段集合）、
#   `contractGaps`（缺口段号）。它们之间**必须**满足恒等式：
#
#       reachableSegments  ⊎  {gap.segment}  =  1...uiSegmentCount
#
#   （⊎ 为不相交并集。）三者一旦漂移，UI 会显示错误的「阶段 N/14」——
#   例如往 `contractGaps` 加一项却忘从 `reachableSegments` 删除，
#   就会出现「既可达又记为缺口」的自相矛盾。
#   该项目文件里虽有 `selfCheck_gapsMatchUnreachable()` 运行期断言，但
#   **运行期断言只在 App 真跑时触发**，而 iOS 当前无法在本机编译 ——
#   必须在**源码层**再守一道（与 G8「三级回退链」同一思路：静态可查的
#   就不要留到运行期）。
#
# 判据（三条，缺一不可）：
#   G12-a  `contractGaps` 的段号集合 ⊆ (1...uiSegmentCount)，且无重复；
#   G12-b  `reachableSegments` 的段号集合 ⊆ (1...uiSegmentCount)，且无重复；
#   G12-c  `reachableSegments` 与 `contractGaps` 段号集合**不相交**，
#          且二者的并集**恰好等于** 1...uiSegmentCount（无遗漏、无越界）。
#
# 说明：本判据是**文本级**的（解析 Swift 字面量），不是类型检查；
#   它锚定的是「三份数据的集合关系」这一**目标对象**，而非「有没有调用
#   selfCheck 函数」这种代理指标 —— 后者恒真（函数存在即可通过），
#   是典型的假闸门。

STAGE_MAP_SWIFT = os.path.join(
    UI_IOS, "Recon", "Sources", "State", "ReconStageMap.swift")


def _parse_int_set(text: str, pattern: str) -> list[int]:
    """把形如 `[1, 2, 3]` 的 Swift 数组字面量里的整数取出来（保序，含重复）。"""
    m = re.search(pattern, text, re.S)
    if not m:
        return []
    return [int(x) for x in re.findall(r"\d+", m.group(1))]


def gate_stage_map(rep: Report) -> None:
    gate = "G12/stage-map"
    if not os.path.isfile(STAGE_MAP_SWIFT):
        rep.err(gate, f"缺少 {rel(STAGE_MAP_SWIFT)}")
        return

    text = strip_comments_only(read_text(STAGE_MAP_SWIFT))

    # uiSegmentCount
    m_count = re.search(r"uiSegmentCount\s*=\s*(\d+)", text)
    if not m_count:
        rep.err(gate, "未找到 `uiSegmentCount` 的字面量定义")
        return
    total = int(m_count.group(1))

    # reachableSegments: Set<Int> = [ ... ]
    reachable = _parse_int_set(
        text, r"reachableSegments\s*:\s*Set<Int>\s*=\s*\[(.*?)\]")
    if not reachable:
        rep.err(gate, "未从 `reachableSegments` 解析到段号（解析器失配）")
        return

    # contractGaps: [(segment: Int, ...)] = [ (5, "VIO(2)", ...), ... ]
    m_gaps = re.search(r"contractGaps\s*:[^=]*=\s*\[(.*?)\n\s*\]", text, re.S)
    if not m_gaps:
        rep.err(gate, "未从 `contractGaps` 解析到列表（解析器失配）")
        return
    gaps = [int(x) for x in re.findall(r"\(\s*(\d+)\s*,", m_gaps.group(1))]

    universe = set(range(1, total + 1))
    reach = set(reachable)
    gaps_s = set(gaps)

    # --- G12-a 缺口段号合法且无重复 ---
    if len(gaps) != len(gaps_s):
        rep.err(gate, f"contractGaps 段号有重复：{sorted(gaps)}")
    bad_gaps = sorted(gaps_s - universe)
    if bad_gaps:
        rep.err(gate, f"contractGaps 段号越界（不在 1..{total}）：{bad_gaps}")

    # --- G12-b 可达段号合法且无重复 ---
    if len(reachable) != len(reach):
        rep.err(gate, f"reachableSegments 段号有重复：{sorted(reachable)}")
    bad_reach = sorted(reach - universe)
    if bad_reach:
        rep.err(gate, f"reachableSegments 段号越界（不在 1..{total}）：{bad_reach}")

    # --- G12-c 二者不相交且并集 = 全集 ---
    overlap = sorted(reach & gaps_s)
    if overlap:
        rep.err(gate, f"段号同时出现在 reachableSegments 与 contractGaps"
                      f"（既说可达又说缺口，自相矛盾）：{overlap}")
    union = reach | gaps_s
    missing = sorted(universe - union)
    if missing:
        rep.err(gate, f"段 {missing} 既不在 reachableSegments 也不在 contractGaps"
                      f"（{total} 段未被完整覆盖）")

    if not (overlap or missing or bad_gaps or bad_reach):
        rep.note(f"阶段映射自洽：可达 {len(reach)} 段 + 缺口 {len(gaps_s)} 段 "
                 f"= 总 {total} 段，互斥且完备 ✓")


# ===========================================================================
# G14 平台像素缓冲处理纪律（缺口 J 的守护闸门）
# ===========================================================================
#
# 为什么需要这一关（2026-10-31 第三十四轮实测发现的缺口 J）：
#
#   `ARFrame.capturedImage` 在真机上是 **`420YpCbCr8BiPlanarFullRange`**
#   的**双平面**缓冲。对这类缓冲，`CVPixelBufferGetBaseAddress(buf)` **返回
#   NULL** —— 必须用 `CVPixelBufferGetBaseAddressOfPlane(buf, i)`。
#
#   旧实现（`ReconSessionController.pushFrameToCore`）正是用了
#   `CVPixelBufferGetBaseAddress(capturedImage)` 后 `guard let base = ... else
#   { return }` —— 于是**静默丢弃 100% 的帧**，`pushedFrames` 恒为 0，
#   而 UI 状态显示「采集中」。**没有任何闸门能发现它**，因为：
#     · 它不违反 G3（禁令）/ G5（ABI 覆盖）/ G7（接线完整性）；
#     · 它不产生编译错误（API 存在，只是对该格式返回 NULL）；
#     · 它不产生运行期错误（`guard` 就是正常控制流）。
#
#   这是本项目「假闸门」教训的第 N 次重演：**判据没有落在目标对象上**。
#   目标对象是「平台层如何处理平面格式缓冲」，而此前没有任何闸门在看这件事。
#
# 判据（四条，各自可负向注入证伪）：
#
#   G14-a  **平面缓冲上禁止用 `GetBaseAddress`**。判据必须落在**调用点**上，
#          而不是「文件里是否出现过某个词」—— 第一版正是按文件判的，于是把
#          `FrameConverter` 对**深度图**（`DepthFloat32`，**单平面**）的
#          `GetBaseAddress` 调用误报成违规（假阳性）。修正后的判据：
#          仅当 `GetBaseAddress(<含 capturedImage 的表达式>)` 时才违规 ——
#          即**实参**指明了这是一个可能为平面格式的缓冲。
#   G14-b  **`GetPlaneCount` 必须被检查**：任何调用
#          `CVPixelBufferGetBaseAddressOfPlane` 的文件，必须同时出现
#          `CVPixelBufferGetPlaneCount`（不检查平面数就取 plane 1 会越界）。
#   G14-c  **深度换算必须锁格式**：调用 `CVPixelBufferGetWidth` 处理深度时
#          必须校验 `kCVPixelFormatType_DepthFloat32`（否则字节布局是猜的）。
#
# G14-d  **桥接层的 include 纪律**：`platform/ios/Recon/*.mm` 引用
#          `recon_platform/*.h`（平台无关规则本体）与 `gpu_algo.h`（GPU 参数块
#          唯一事实来源）时，必须靠**头搜索路径**，不得写 `../..` 相对路径 ——
#          后者要人工数 `..` 层数（易错），且 CMake 与 Xcode 两个消费者的
#          相对基准不同。缺头搜索路径时真机报 "file not found"，
#          而本机（Windows）**查不出**。
#
# 负向注入见 `tools/verify_ios_contract_negative.py --case platform-plane-api`
# 与 `--case bridge-include-path`。
# ===========================================================================

# 可能为**平面格式**（多平面）的缓冲约定命名：出现这些名字的实参才受 G14-a 约束。
# 这是**显式清单**而非启发式：将来新增平面缓冲来源时，必须在此登记 ——
# 否则闸门会漏，而不是误报（漏比误报更容易被忽视，故用清单逼出显式决定）。
PLANAR_BUFFER_HINTS = ("capturedImage", "pixelBuffer", "yuvBuffer")


def gate_platform_plane_discipline(rep: Report) -> None:
    gate = "G14/平台平面缓冲"

    call_checked = 0
    plane_base_files = 0
    mm_checked = 0

    # 扫描范围：Swift（UI 层）+ ObjC++（桥接层）。见 iter_ios_platform_impl_files
    # 的说明 —— 平面处理已搬到 .mm，只看 Swift 就是判据没落在目标对象上。
    targets = [(p, "swift") for p in iter_swift_files()]
    targets += [(p, "objc") for p in iter_ios_platform_impl_files()]

    for path, kind in targets:
        raw_text = read_text(path)                     # G14-d 用（含字符串）
        code = strip_comments_and_strings(raw_text)     # G14-a/b/c 用（去噪）
        relpath = rel(path)

        if kind == "objc":
            mm_checked += 1

        uses_plane_base = "CVPixelBufferGetBaseAddressOfPlane(" in code
        uses_plane_count = "CVPixelBufferGetPlaneCount(" in code

        # --- G14-a：逐个**调用点**检查 `GetBaseAddress(<平面缓冲实参>)` -------
        for m in re.finditer(r"CVPixelBufferGetBaseAddress\(([^)]*)\)", code):
            arg = m.group(1)
            if arg.strip().startswith("OfPlane"):
                continue
            call_checked += 1
            if not any(h in arg for h in PLANAR_BUFFER_HINTS):
                continue
            ln = code[: m.start()].count("\n") + 1
            rep.err(gate,
                    f"{relpath}:{ln} 对可能是**平面格式**的缓冲（实参 `{arg.strip()}`）"
                    f"调用 `CVPixelBufferGetBaseAddress` —— 该 API 对 "
                    f"`420YpCbCr8BiPlanar` 返回 **NULL**，配合 `guard else {{ return }}` "
                    f"会**静默丢弃全部帧**（缺口 J）。必须改用 "
                    f"`GetBaseAddressOfPlane`，或经 `FrameConverter` 统一转换。")

        # --- G14-b：取平面地址必须校验平面数 ---------------------------------
        if uses_plane_base:
            plane_base_files += 1
            if not uses_plane_count:
                ln = code.index("CVPixelBufferGetBaseAddressOfPlane(")
                rep.err(gate,
                        f"{relpath}:{code[:ln].count(chr(10)) + 1} 调用 "
                        f"`GetBaseAddressOfPlane` 但未检查 `CVPixelBufferGetPlaneCount` "
                        f"—— 不校验平面数就取 plane 1 存在越界风险。")

        # --- G14-c：深度换算的格式锁 -----------------------------------------
        if "kCVPixelFormatType_DepthFloat32" in code:
            call_checked += 1
            if "CVPixelBufferGetPixelFormatType(" not in code:
                rep.err(gate,
                        f"{relpath} 使用 `kCVPixelFormatType_DepthFloat32` 但未调用 "
                        f"`CVPixelBufferGetPixelFormatType` —— 深度字节布局必须是"
                        f"**校验过的事实**，不能是假设（缺口 J 同类）。")

        # --- G14-d：桥接层不得用 `../..` 相对路径引用仓库内公共头 ------------
        #
        # ⚠️ 本判据**必须扫原始文本**，不能用上面那个
        #    `strip_comments_and_strings()` 的结果 —— 它会把**字符串字面量**
        #    一并剥掉，而 include 的路径**恰恰在字符串里**：
        #        #include "gpu_algo.h"
        #                  ^^^^^^^^^^^^ 被剥掉 → 正则永远匹配不到 → **假闸门**。
        #    首版就是这么写的，被 G14-d 自己的负向注入抓出来（rc=0 未拒收）。
        #
        #    这里按行处理：① 跳过块注释区间；② 去掉行尾 `//` 注释；
        #    ③ 要求 `#include` 出现在**行首**（注释里提到的 include 因此不会误报，
        #       因为 `// #include ...` 的 `#` 不在行首）。
        if kind == "objc":
            in_block_comment = False
            for lineno, raw_line in enumerate(raw_text.splitlines(), 1):
                line = raw_line
                if in_block_comment:
                    end = line.find("*/")
                    if end < 0:
                        continue
                    line = line[end + 2:]
                    in_block_comment = False
                # 去掉块注释的剩余部分（同行 `/* ... */` 可能多段）
                out_line = []
                rest = line
                while True:
                    start = rest.find("/*")
                    if start < 0:
                        out_line.append(rest)
                        break
                    out_line.append(rest[:start])
                    tail = rest[start + 2:]
                    end = tail.find("*/")
                    if end < 0:
                        in_block_comment = True
                        break
                    rest = tail[end + 2:]
                line = "".join(out_line)
                line = re.sub(r"//.*$", "", line)     # 行尾注释
                m = re.match(r'\s*#\s*include\s+"([^"]*\.\./[^"]*)"', line)
                if m:
                    rep.err(gate,
                            f"{relpath}:{lineno} 用**相对路径** `{m.group(1)}` "
                            f"引用仓库内公共头 —— 必须靠头搜索路径（见 "
                            f"tools/gen_ios_project.py 的 HEADER_SEARCH_PATHS 与 "
                            f"CMakeLists.txt 的 target_include_directories）。"
                            f"数 `..` 层数易错，且 CMake 与 Xcode 的相对基准不同；"
                            f"缺搜索路径时真机报 file not found，本机查不出。")

    if not any(gate in e for e in rep.errors):
        rep.note(f"平台平面缓冲纪律：逐调用点检查 {call_checked} 处、"
                 f"取平面地址文件 {plane_base_files} 个、ObjC++ 桥接文件 "
                 f"{mm_checked} 个，全部合规 ✓")


# ===========================================================================
# G13 ReconStatus → L10nKey 映射穷举性
# ===========================================================================
#
# 为什么需要这一关：
#   `ui/ios/Recon/Sources/State/ReconSessionController.swift` 的
#   `ReconStatusPresentation.messageKey(for:)` 是一个 **`ReconStatus` 上的
#   switch**。Swift 会强制 switch 穷举（漏 case 是编译错误）—— 但那要求**能编译**，
#   而 iOS 个本机无法编译。一旦有人给 `ReconStatus` 加一项却忘了在此 switch 补
#   分支，后果是「该错误码落到通用失败」或（更糟）编译在 mac 上才失败。
#
#   本闸门把「语法上的穷举」在**文本层**复算一遍：switch 里出现的 `.caseName`
#   集合必须**等于** `enum ReconStatus` 的 case 名集合。
#
# 判据：解析 `ReconStatus` 的 case 名集合 S；解析 `messageKey(for:)` 函数体里
#   出现的 `case .xxx` / `, .yyy` 形态的标识符集合 M；要求 M ⊇ S
#   （用 ⊇ 而非 =，因为 switch 分支里可能出现 `nil`、`.ok` 等；但要
#   **每个 S 成员都出现过**）。
#
# 说明：文本级解析无法处理「多个 case 合并写成一行、跨行」的边界，故实现里
#   用「在函数体内全局 findall `.标识符`」—— 对本文件足够（switch 极简单）。
#   如实标注其为**弱于编译器**的判据，但比「什么都不查」强，且能在 CI 秒级暴露。

CONTROLLER_SWIFT = os.path.join(
    UI_IOS, "Recon", "Sources", "State", "ReconSessionController.swift")


def gate_status_mapping(rep: Report) -> None:
    gate = "G13/status-map"
    if not os.path.isfile(SWIFT_RECON_CORE):
        rep.err(gate, f"缺少 {rel(SWIFT_RECON_CORE)}")
        return
    if not os.path.isfile(CONTROLLER_SWIFT):
        # 接线层尚未建立时不报错（逐屏接线中），只提示。
        rep.warn(gate, f"缺少 {rel(CONTROLLER_SWIFT)}，跳过错误码映射穷举检查")
        return

    # 1) ReconStatus 的 case 名集合
    core = strip_comments_only(read_text(SWIFT_RECON_CORE))
    m_enum = re.search(r"enum\s+ReconStatus\s*:\s*Int32\s*\{(.*?)\n\}", core, re.S)
    if not m_enum:
        rep.err(gate, "未从 ReconCore.swift 解析到 recon_status 枚举")
        return
    status_names = set(re.findall(r"\bcase\s+(\w+)\s*=", m_enum.group(1)))

    # 2) messageKey(for:) 函数体里出现的 .标识符
    ctrl = strip_comments_only(read_text(CONTROLLER_SWIFT))
    m_fn = re.search(r"func\s+messageKey\s*\(for\s+\w+\s*:\s*ReconStatus\)[^{]*\{",
                     ctrl)
    if not m_fn:
        rep.err(gate, "未找到 `ReconStatusPresentation.messageKey(for:)`")
        return
    # 用大括号配平取出函数体。
    i = ctrl.find("{", m_fn.end() - 1)
    depth = 0
    body_start = i
    while i < len(ctrl):
        if ctrl[i] == "{":
            depth += 1
        elif ctrl[i] == "}":
            depth -= 1
            if depth == 0:
                break
        i += 1
    body = ctrl[body_start:i]

    referenced = set(re.findall(r"\.(\w+)", body))
    missing = sorted(status_names - referenced)
    if missing:
        rep.err(gate, f"ReconStatus 有 {len(missing)} 个 case 未在 messageKey(for:) "
                      f"中被引用（新增错误码时漏改映射）：{missing}")
    if status_names:
        rep.note(f"错误码映射穷举：{len(status_names)} 个 ReconStatus case "
                 f"全部在 messageKey(for:) 中出现 ✓")


# ===========================================================================
# G15 iOS bundle 落位纪律（非本地化资源在包根，无 `Contents/` 一层）
# ===========================================================================
#
# 为什么需要这一关（2026-11-01 实测发现的真实缺口）：
#
#   iOS 的 bundle 是**扁平**结构 —— 非本地化资源位于**包根**
#   （`Recon.app/default.metallib`），**不存在** `Contents/` 这一层。
#   Apple《Bundle Structures》原文：iOS bundle
#   "uses a relatively flat structure with few extraneous directories"，
#   非本地化资源 "in the top-level bundle directory"；
#   而 macOS 的 bundle 才是 `MyApp.app/Contents/Resources/` 那种层级结构。
#
#   `tools/gen_ios_project.py` 生成 Build Phase 时曾写
#       export RECON_METALLIB_OUT_DIR="${BUILT_PRODUCTS_DIR}/${CONTENTS_FOLDER_PATH}/Resources"
#   该表达式在 iOS 上会展开到 `Recon.app/Recon.app/Resources`（扁平 bundle 里
#   `CONTENTS_FOLDER_PATH` 取到的最后一段就是 bundle 名自身）。
#   而 `MTLDevice.makeDefaultLibrary()` 与 `newDefaultLibraryWithBundle:error:`
#   **都只在包根找** `default.metallib`（Apple Developer Forums thread/52287、
#   多份 Apple 示例一致）。
#
#   后果是最难查的那一类：**着色器编得过、CI 全绿、产物也在包里，但运行期
#   永远加载不到** → `MetalComputeBackend.MetalProbe()` 诚实报
#   "找不到 default.metallib"，GPU 路径永久降级为 CPU 回退。没有任何编译期
#   信号、也没有任何运行期崩溃。
#
# 判据（落在**生成物与生成器**上，不落脚本自述）：
#   G15-a  `platform/ios/Container/project.yml` 里凡是把产物放进
#          `$(BUILT_PRODUCTS_DIR)/...` 的落位表达式，**不得**出现
#          `${CONTENTS_FOLDER_PATH}`（iOS 上该变量会引入不存在的 `Contents`
#          层级）。正确变量是 `${UNLOCALIZED_RESOURCES_FOLDER_PATH}`
#          ——它的语义正是「非本地化资源目录」，由 Xcode 按平台展开
#          （iOS = 包根，macOS = Contents/Resources），两端都对且无需分支。
#   G15-b  生成器 `tools/gen_ios_project.py` 里同样不得再出现该错误表达式
#          —— 否则"手改生成物"会被下一次 `gen_ios_project.py` 覆盖回去，
#          而判据只盯生成物就会漏掉真正的源头（**判据必须落在源头**）。
#   G15-c  `cmake/Licenses.cmake` 的 iOS 嵌入位置不得写成
#          `Recon.app/Contents/Resources`（同一扁平结构问题）。
#   G15-d  **文档面交叉对账**（2026-11-01 补的第二类漂移）—— 声明了许可清单
#          iOS 落位的文档（docs/*.md）里，若**同一行**同时出现
#          `THIRD_PARTY_LICENSES` 与 `Recon.app/Contents/`，即报错。
#          ⚠️ 判据刻意收紧到「同行共现」：`docs/12` 的 dSYM 路径
#          （`*.dSYM/Contents/Resources/DWARF/...`）与作为反例引用的 macOS
#          路径里的 `Contents/` **都是正确的**，不得误伤。它守的是
#          「代码改对了、文档留在错的」这一方向（G15-a/b/c 守的是反方向）。
#
# 负向注入见 `tools/verify_ios_contract_negative.py --case bundle-layout`。
# ===========================================================================

def gate_bundle_layout(rep: Report) -> None:
    gate = "G15/bundle-layout"
    checked = 0

    # --- G15-a：生成物（project.yml）--------------------------------------
    container_yml = os.path.join(IOS_DIR, "Container", "project.yml")
    if not os.path.isfile(container_yml):
        rep.err(gate, f"缺少 {rel(container_yml)}")
    else:
        text = read_text(container_yml)
        # 只查"落位表达式"所在的**代码行**（去掉注释行）：
        # 上面那段论证的注释里**刻意**提到 `${CONTENTS_FOLDER_PATH}`（作为
        # 反例说明），若连带注释一起判就会误报 —— 这正是"判据范围要跟上
        # 代码位置"的要求。故先剥掉整行注释。
        for lineno, raw in enumerate(text.splitlines(), 1):
            line = raw.split("#", 1)[0]
            if "CONTENTS_FOLDER_PATH" in line:
                rep.err(gate,
                        f"{rel(container_yml)}:{lineno} 落位表达式用了 "
                        f"`${{CONTENTS_FOLDER_PATH}}` —— iOS 是扁平 bundle，"
                        f"该变量会引入不存在的 `Contents` 层级，产物将无法被 "
                        f"`makeDefaultLibrary()` 加载（运行期静默降级）。"
                        f"应改用 `${{UNLOCALIZED_RESOURCES_FOLDER_PATH}}`。")
            if "BUILT_PRODUCTS_DIR" in line:
                checked += 1

    # --- G15-b：生成器（源头）---------------------------------------------
    gen = os.path.join(ROOT, "tools", "gen_ios_project.py")
    if os.path.isfile(gen):
        for lineno, raw in enumerate(read_text(gen).splitlines(), 1):
            line = raw.split("#", 1)[0]
            if "CONTENTS_FOLDER_PATH" in line:
                rep.err(gate,
                        f"{rel(gen)}:{lineno} 生成器里仍有 "
                        f"`CONTENTS_FOLDER_PATH` 落位表达式 —— 它是生成物的**源头**，"
                        f"只改生成物会被下一次重生成覆盖回错误值。")

    # --- G15-c：许可清单嵌入位置 ------------------------------------------
    lic = os.path.join(ROOT, "cmake", "Licenses.cmake")
    if os.path.isfile(lic):
        for lineno, raw in enumerate(read_text(lic).splitlines(), 1):
            line = raw.split("#", 1)[0]
            if re.search(r'"Recon\.app/Contents/', line):
                rep.err(gate,
                        f"{rel(lic)}:{lineno} 许可清单嵌入位置写成 "
                        f"`Recon.app/Contents/...` —— iOS 无 `Contents/` 层，"
                        f"设备上该目录不存在。应写 `Recon.app`（包根）。")

    # --- G15-d：文档面的交叉对账（2026-11-01 补的第二类漂移）--------------
    #
    # 为什么需要（与 G15-a/b/c **方向相反**的同一类问题）：
    #   G15-a/b/c 守的是「代码写错」，修法是「改代码」。但实测发现
    #   `docs/12 §2.7` 的嵌入位置表**仍旧**写着
    #       iOS 放 `Recon.app/Contents/Resources/THIRD_PARTY_LICENSES.txt`
    #   （`docs/10` 的 LIC-5 同错），而 `cmake/Licenses.cmake` 早已是
    #   `Recon.app`（包根）。即：**代码改对了、文档留在错的** ——
    #   与 G15 初建时「文档写错、代码跟着错」是同一枚硬币的两面，
    #   共同根因都是**同一事实有两处来源、且两者之间没有对账**。
    #
    #   ⚠️ 判据设计（避免误伤，这是本关最易写错处）：
    #     ① **不得**用「文件里出现 `Contents/` 就报错」—— 
    #        `docs/12:1197` 的 dSYM 路径 `<...>.a.dSYM/Contents/Resources/DWARF/`
    #        里的 `Contents/` **是正确的**（那是 dSYM 包自己的结构，与 iOS
    #        App bundle 无关）；`docs/12:1307`/`docs/10` 中作为反例被引用的
    #        macOS 路径同样合法。
    #     ② 故判据收紧为「**同一行**里，既出现许可清单文件名
    #        `THIRD_PARTY_LICENSES` 又出现 `Recon.app/Contents/`」——
    #        只有"把许可清单放进 App bundle 的 Contents"这一种写法会被命中，
    #        其余提及 `Contents/` 的上下文一律放过。
    #     ③ **豁免缺口登记行**：docs/14 的缺口表里，登记行的职责就是
    #        **引用当初写错的那个值**（如缺口 AN 行必须写出
    #        `Recon.app/Contents/Resources/THIRD_PARTY_LICENSES.txt` 才能说明
    #        漂移是什么）。若把登记行也判进来，等于「禁止记录 bug」—— 那是
    #        判据**范围过宽**，不是文档错。故按行首编号模式 `| A<大写字母> |`
    #        显式跳过（**只跳登记行，不跳整份文档**：docs/14 的散文段落仍在
    #        扫描范围内；注入项 G15-d-doc14-prose 专门证明这个豁免是**窄**的）。
    #     ④ 扫描面 = `docs/*.md` 全文（成本可忽略）。
    _GAP_ROW = re.compile(r"^\|\s*A[A-Z](-[0-9]+)?\s*\|")
    for doc in sorted(glob.glob(os.path.join(ROOT, "docs", "*.md"))):
        for lineno, raw in enumerate(read_text(doc).splitlines(), 1):
            if "THIRD_PARTY_LICENSES" not in raw:
                continue
            if _GAP_ROW.match(raw):
                continue  # 缺口登记行：职责就是引用错误值，豁免（见 ③）
            if re.search(r"Recon\.app/Contents/", raw):
                rep.err(gate,
                        f"{rel(doc)}:{lineno} 文档把 iOS 许可清单落位写成 "
                        f"`Recon.app/Contents/...`，而 `cmake/Licenses.cmake` 的 "
                        f"`RECON_LICENSES_EMBED_DEST` 是 `Recon.app`（包根）—— "
                        f"**代码与文档两份事实来源已漂移**。iOS bundle 无 "
                        f"`Contents/` 一层，请改文档为 `Recon.app/THIRD_PARTY_LICENSES.txt`。")

    if not any(gate in e for e in rep.errors):
        n_docs = len(glob.glob(os.path.join(ROOT, "docs", "*.md")))
        rep.note(f"iOS bundle 落位纪律：检查落位表达式 {checked} 处、"
                 f"生成器与 Licenses.cmake 各 1 处、文档面 {n_docs} 份（许可清单"
                 f"与 `Contents/` 同行交叉对账），均未出现 iOS 上不存在的 "
                 f"`Contents/` 层级 ✓")


# ===========================================================================
# G16 未签名 IPA 打包链纪律
# ===========================================================================
#
# 为什么需要这一关（2026-11-01 补，与 G15 同源的一类风险）：
#
#   G15 管的是「metallib 落位」这一个**事实**，而把产物交到用户手上的是
#  构造它之后的一整条链：`xcodebuild archive` → `Payload/` → `zip` → artifact。
#   这条链有三处只有"读 workflow 全文"才能发现的风险，且都不会让任何编译
#   或测试变红：
#
#     · **归档到了模拟器**：`-destination 'generic/platform=iOS Simulator'`
#       能编过、能产出 `.app`，但那是 x86_64/arm64 的模拟器 slice ——
#       XC-3 明文禁止它进入提包，而它照样能被打进 IPA 并"看起来成功"。
#       真实事故形态：用户拿到 IPA 却装不上（架构不符），而 CI 全绿。
#     · **把打包逻辑就地写进 YAML**：`mkdir Payload && cp -R ... && zip`
#       表面上等价，但它把「IPA 结构判据」的实现体变成了 YAML 里的几行
#       shell —— 与 `tools/stage_ios_ipa.py` 成为**两处事实来源**（必然漂移），
#       且 YAML 里没有地方能读回 IPA 做校验（判据落不到目标对象上）。
#     · **混入 `-exportArchive`**：它**强制**要 exportOptions plist 且强制
#       签名 —— CI 上必然失败（无凭据）。更糟的是有人在本地手工改成
#       "先 exportArchive 再上传"，于是 CI 与手工路径分叉，而 CI 那段仍写着
#       未签名开关，形成自相矛盾的记录。
#
#   这三条都不是"代码 bug"，而是**流程纪律**；纪律只能靠闸门守。
#   判据落在 `.github/workflows/ios.yml` 的**具体行**上，不落注释自述
#   （注释里刻意提到 `-exportArchive`、模拟器 destination 作为反例说明，
#    故必须先剥掉行注释再判 —— 与 G15-a 同一处理）。
#
# 负向注入见 `tools/verify_ios_contract_negative.py` 的 G16-a/b/c/d 四项。
# ===========================================================================

def gate_ipa_pipeline(rep: Report) -> None:
    gate = "G16/ipa-pipeline"
    wf = os.path.join(ROOT, ".github", "workflows", "ios.yml")
    if not os.path.isfile(wf):
        rep.err(gate, f"缺少 {rel(wf)}")
        return

    # 只留下 build-app job 的正文（其后到文件末即该 job；本项目该 job 是
    # 文件里最后一个，故用「从 `build-app:` 到末尾」切片是安全的）。
    # ⚠️ 若将来在它之后追加了新 job，这段切片会连带把新 job 判进来 ——
    #   故显式断言「切片里确实含归档命令」，否则 fail-closed。
    raw = read_text(wf)
    marker = "\n  build-app:"
    idx = raw.find(marker)
    if idx < 0:
        rep.err(gate, f"{rel(wf)} 里找不到 `build-app:` job（本闸门的目标对象）")
        return
    body = raw[idx:]

    # 剥掉整行注释（保留缩进结构），再逐行判 —— 注释里刻意提到反例。
    code_lines = [ln.split("#", 1)[0] for ln in body.splitlines()]
    code = "\n".join(code_lines)
    # 行号（相对整个文件）便于定位
    base_line = raw[:idx].count("\n") + 1

    def line_of(needle: str) -> int:
        for i, ln in enumerate(code_lines):
            if needle in ln:
                return base_line + i + 1
        return -1

    # --- 先确认这条链**真的存在**（否则下面全是"没东西可查所以通过"）-------
    has_archive = "xcodebuild archive" in code
    if not has_archive:
        rep.err(gate,
                "build-app job 里没有 `xcodebuild archive` —— 未签名 IPA 链"
                "不存在（或已被删）。若这是有意的，请同步删除本闸门与 "
                "docs/12 §4.8 的对应小节，不要让判据对着空气判定。")
        return

    # --- G16-a：归档目标必须是 device ------------------------------------
    # ⚠️ 判据写成「archive 那一段里不得出现 iphonesimulator / iOS Simulator」，
    #    而不是「必须出现 generic/platform=iOS」—— 后者能被"顺便写个注释"骗过，
    #    且本 job 里**确实存在**另一条 simulator 构建（那是模拟器校验轮，
    #    合法）。故取"archive 命令及其续行"这一段来判。
    arch_idx = code.find("xcodebuild archive")
    # 续行：从 archive 起连续的行（以 `\` 结尾或缩进继续），到第一个空行/新 step
    tail = code[arch_idx:].splitlines()
    arch_block: list[str] = []
    for ln in tail:
        if ln.strip() == "" and arch_block:
            break
        arch_block.append(ln)
        if "-destination" in ln and "\\" not in ln:
            # destination 之后还有几行开关，继续收几行
            continue
        if len(arch_block) > 30:
            break
    arch_text = "\n".join(arch_block)
    if "generic/platform=iOS Simulator" in arch_text or "iphonesimulator" in arch_text:
        rep.err(gate,
                f"{rel(wf)}:{line_of('xcodebuild archive')} 归档目标是**模拟器** "
                f"—— 模拟器 slice 不得进入交付包（docs/12 §4.7 XC-3）。"
                f"应写 `-destination 'generic/platform=iOS'`。")
    elif "generic/platform=iOS" not in arch_text:
        rep.err(gate,
                f"{rel(wf)}:{line_of('xcodebuild archive')} 归档命令里找不到 "
                f"`-destination 'generic/platform=iOS'` —— 无法确认归档到了设备。")
    # 未签名三开关必须齐
    for key in ("CODE_SIGNING_ALLOWED=NO", "CODE_SIGNING_REQUIRED=NO",
                'CODE_SIGN_IDENTITY=""'):
        if key not in arch_text:
            rep.err(gate,
                    f"{rel(wf)}:{line_of('xcodebuild archive')} 归档命令缺少 "
                    f"`{key}` —— 未签名链会在有凭据的机器上变成签名产物的"
                    f"另一形态（判据与产物不匹配）。")

    # --- G16-b：打包/校验必须调用单一实现 ---------------------------------
    if "tools/stage_ios_ipa.py" not in code:
        rep.err(gate,
                f"{rel(wf)} 的 build-app job 没有调用 "
                f"`tools/stage_ios_ipa.py` —— IPA 结构与打包的判据必须在**一处**"
                f"实现（否则与脚本成为两处事实来源，必然漂移，且判据落不到产物上）。")
    else:
        # ⚠️ 反向判据：既然调了脚本，就**不得**在 YAML 里另写一份 zip 打包。
        #    （曾担心"有人既调脚本又补一段 cp"——那正是两处事实来源。）
        for pat, why in ((re.compile(r"\bzip\s+-r\b"), "就地写 `zip -r` 打包"),
                         (re.compile(r"mkdir\s+-p\s+Payload"), "就地建 `Payload/`")):
            m = pat.search(code)
            if m:
                rep.err(gate,
                        f"{rel(wf)} 出现{why} —— 打包逻辑应只在 "
                        f"`tools/stage_ios_ipa.py` 里有一份实现，YAML 只负责调用。")

    # --- G16-c：不得出现 exportArchive（强制签名）-------------------------
    if "-exportArchive" in code:
        rep.err(gate,
                f"{rel(wf)}:{line_of('-exportArchive')} 出现 `-exportArchive` —— "
                f"它强制要求 exportOptions plist 且**强制签名**，CI 无 Apple "
                f"凭据必然失败。未签名路径只走 Payload 手工打包。")

    # --- G16-d：IPA 必须上传为 artifact -----------------------------------
    if not re.search(r"name:\s*Recon-unsigned\.ipa", code):
        rep.err(gate,
                f"{rel(wf)} 未把 `Recon-unsigned.ipa` 上传为 artifact —— "
                f"这条链产出的包没有出口等于白跑（用户要的是能下载的包）。")

    if not any(gate in e for e in rep.errors):
        rep.note("未签名 IPA 链纪律：device 归档 + 调用 stage_ios_ipa.py + "
                 "无 exportArchive + 产物上传 artifact，四项齐备 ✓")


# ===========================================================================
# G17 locale 资源落位纪律（folder reference + 与 Swift 查找方式交叉对账）
# ===========================================================================
#
# 为什么需要这一关（2026-11-01 补，与 G15 同源的第三例「静默失效」）：
#
#   `Localization.swift` 的 `LocaleCatalog.loadAll` 用
#       bundle.url(forResource: locale.rawValue, withExtension: "json",
#                  subdirectory: "locales")
#   查找文案 —— 它**要求包内存在 `locales/` 这一层目录**。
#
#   而 XcodeGen 的 target source 若用默认的 `type: group`（黄色组），资源会被
#   **展平到 bundle 根**（group 只保留工程内的逻辑分组，不保留磁盘层级；
#   `folder` 才是蓝色文件夹引用、整棵子树原样拷入）。
#   于是 20 个 json 落到 `Recon.app/zh-CN.json` 之类位置，`subdirectory:`
#   一个都匹配不上 → `LocaleCatalog` 把 20 个 locale **全部**记入
#   `missingFiles` → 所有界面回退显示键名（如 `msg.export.title`）。
#
#   为什么此前无人发现：**编译、链接、签名、安装、启动全部正常**，UI 也不崩
#   —— 只是所有文案变成键名。属本项目第三次踩到的同一形态（前两次：缺口 J/P
#   的 RGB 帧被静默丢弃、G15 的 metallib 落位）。这三例的共同教训是：
#   **凡是"运行时按路径查找资源"的代码，都必须有一条闸门把「代码查找方式」与
#   「包内实际落位」对账** —— 只看工程配置、或只看代码，都看不出来。
#
# 判据（三条，G17-c 是真正抓得住漂移的那条）：
#   G17-a  生成物 `project.yml` 的 locale 资源条目必须带 `type: folder`，
#          且路径最后一段为 `locales`。
#   G17-b  生成器 `tools/gen_ios_project.py` 同禁（源头；否则重生成即回退）。
#   G17-c  **交叉对账**：从 `Localization.swift` 解析出的 `subdirectory:` 实参
#          必须等于该 folder reference 目录的最后一段。任一侧被改（代码改成
#          不再带 subdirectory，或路径改成别的目录名）都立即失败。
#          —— 这条是"判据落在目标对象上"的落地：判据不是"有没有写 type: folder"，
#             而是"包的落位与代码的查找**是不是同一件事**"。
#
# 产物面的对应判据是 `tools/stage_ios_ipa.py` 的 **P-8**（读回 IPA 验证
# `Payload/Recon.app/<subdir>/<locale>.json` 齐全）。二者一静（本闸门）一动
# （读产物），互补而非重复。
#
# 负向注入见 `tools/verify_ios_contract_negative.py` 的 G17-a/b/c 三项。
# ===========================================================================

def gate_locale_resource_layout(rep: Report) -> None:
    gate = "G17/locale-layout"

    # --- G17-c 的右侧：Swift 的查找方式（先解析，供 a/b 共用）--------------
    swift = os.path.join(ROOT, "ui", "ios", "Recon", "Sources",
                         "Localization", "Localization.swift")
    subdir: str | None = None
    if not os.path.isfile(swift):
        rep.err(gate, f"缺少 {rel(swift)}（无法交叉对账 locale 查找方式）")
    else:
        m = re.search(r'subdirectory:\s*"([^"]*)"', read_text(swift))
        if not m:
            rep.err(gate,
                    f"{rel(swift)} 里找不到 `subdirectory: \"...\"` —— locale 的"
                    f"查找方式已变（或改成非字面量）。本闸门无法交叉对账，"
                    f"须同步更新它，或改回带 subdirectory 的查找。")
        elif not m.group(1):
            rep.err(gate,
                    f"{rel(swift)} 的 `subdirectory:` 为空字符串 —— 那就不再要求"
                    f"包内有独立目录层，与 folder reference 的落位不符。")
        else:
            subdir = m.group(1)

    def check_source(path: str, label: str) -> None:
        if not os.path.isfile(path):
            rep.err(gate, f"缺少 {rel(path)}（{label}）")
            return
        text = read_text(path)
        # 找 `- path: .../locales` 这一条，取出其紧随的 `type:`。
        m = re.search(
            r'-\s*path:\s*(\S+?)\s*#?[^\n]*\n((?:[ \t]+[^\n]*\n)*?)(?:[ \t]*-|$)',
            text)
        # 上面这条对"下一个条目"的边界不稳，改用逐行状态机（更可读也更稳）。
        lines = text.splitlines()
        found_path: str | None = None
        found_type: str | None = None
        for i, raw in enumerate(lines):
            s = raw.strip()
            if s.startswith("- path:") and s.split(":", 1)[1].strip().rstrip("/").endswith("/locales"):
                found_path = s.split(":", 1)[1].strip().split()[0].rstrip("/")
                # 向下最多 3 行找同级的 type:
                for j in range(i + 1, min(i + 4, len(lines))):
                    sj = lines[j].strip()
                    if sj.startswith("- path:") or sj.startswith("settings:"):
                        break
                    if sj.startswith("type:"):
                        found_type = sj.split(":", 1)[1].strip()
                        break
                break
            # 兼容朴素写法：路径就是 `.../Resources`（缺 locales 段）
            if s.startswith("- path:") and s.split(":", 1)[1].strip().rstrip("/").endswith("/Resources"):
                found_path = s.split(":", 1)[1].strip().split()[0].rstrip("/")
                break
        if found_path is None:
            rep.err(gate,
                    f"{rel(path)}（{label}）里找不到 locale 资源条目"
                    f"（`- path: .../locales`）—— 若资源声明被删，则包内不会"
                    f"有任何 locale，UI 全部回退到键名。")
            return
        if found_type != "folder":
            rep.err(gate,
                    f"{rel(path)}（{label}）的 locale 资源 `{found_path}` 未声明 "
                    f"`type: folder`（实际 type={found_type!r}）—— XcodeGen 默认的 "
                    f"`type: group` 会把资源**展平到 bundle 根**，"
                    f"`subdirectory: \"{subdir or 'locales'}\"` 将全部匹配不上。")
        if subdir is not None and os.path.basename(found_path) != subdir:
            rep.err(gate,
                    f"{rel(path)}（{label}）的 locale 目录名为 "
                    f"`{os.path.basename(found_path)}`，而 {rel(swift)} 的 "
                    f"`subdirectory:` 是 `{subdir}` —— 二者不一致，包内落位与"
                    f"代码查找不是同一件事。")

    # --- G17-a：生成物 ----------------------------------------------------
    check_source(os.path.join(IOS_DIR, "Container", "project.yml"), "生成物")
    # --- G17-b：生成器源头 ------------------------------------------------
    check_source(os.path.join(ROOT, "tools", "gen_ios_project.py"), "生成器源头")

    # --- G17-d：隐私清单的落位前提（不得把 platform/ios/Recon 改成 folder）--
    #   它与 G17-a 的诉求**方向相反**：locales 要 folder 才不丢目录层，
    #   PrivacyInfo 要靠 group 展平才落到包根。两条同时成立才是当前正确状态。
    for path, label in ((os.path.join(IOS_DIR, "Container", "project.yml"), "生成物"),
                        (os.path.join(ROOT, "tools", "gen_ios_project.py"), "生成器源头")):
        if not os.path.isfile(path):
            continue
        lines = read_text(path).splitlines()
        for i, raw in enumerate(lines):
            s = raw.strip()
            if s.startswith("- path:") and s.split(":", 1)[1].strip().rstrip("/") == "platform/ios/Recon":
                for j in range(i + 1, min(i + 4, len(lines))):
                    sj = lines[j].strip()
                    if sj.startswith("- path:") or sj.startswith("excludes:") \
                            or sj.startswith("settings:"):
                        break
                    if sj == "type: folder":
                        rep.err(gate,
                                f"{rel(path)}（{label}）把 `platform/ios/Recon` 声明为 "
                                f"`type: folder` —— 那会让它与它下面的 "
                                f"`Resources/PrivacyInfo.xcprivacy` 一起保留目录层，"
                                f"隐私清单落到 `Recon.app/Recon/Resources/` 而非**包根**"
                                f"（docs/11 §1.5 要求包根；上架校验项）。"
                                f"该 source 必须保持默认 group 以展平到包根。")
                break

    # --- G17-c 已在 check_source 内完成交叉对账 ----------------------------
    if not any(gate in e for e in rep.errors):
        rep.note(f"locale 资源落位纪律：locales 为 folder reference，与 "
                 f"Localization.swift 的 subdirectory=\"{subdir}\" 一致，"
                 f"且 platform/ios/Recon 保持 group（PrivacyInfo 落包根）✓")


# ---------------------------------------------------------------------------
# G18 iOS 系统 API 幻觉清单
# ---------------------------------------------------------------------------
#
# ── 为什么需要这条（2026-11-01 CI #20 的真实事故）────────────────────────
#
# `CameraCapture.swift` 写了 `motion.maximumGyroUpdateInterval` 去算 IMU 硬件
# 上限。该成员**不存在**（`CMMotionManager` 只有 `gyroUpdateInterval`，且它是
# 调用方写入的期望间隔，不是硬件上限）。结果：
#
#     error: value of type 'CMMotionManager' has no member 'maximumGyroUpdateInterval'
#     ** BUILD FAILED **   （退出码 65，Swift 编译）
#
# 而**同时** G1..G17 全部通过、build-xcframework 与 Vulkan 两个 job 全绿 ——
# （G18 正是为这条事故补的回归防线，见下方 gate_system_api_denylist）
# 因为它不是「契约不一致」，是「调用了一个不存在的 API」。**只有类型检查器
# 能发现**，而本机无 macOS/Swift，故这类错误此前零覆盖（与 #111 Swift 调用
# 图闸门缺失同属检测盲区）。
#
# ── 本闸门能做什么 / 不能做什么（**边界必须如实说明，不得夸大**）────────
#
#   能：拦住「**已被记录的**幻觉成员」—— 即下方 DENYLIST 里逐条列出的名字，
#       每一条都有明确的「正确替代」。
#   不能：拦住**未知**的幻觉 API。它不是类型检查器，没有 SDK 符号表。
#
#   所以本闸门的定位是**回归防线**（防同一个错再犯），不是**完备性证明**。
#   真正的完备防线仍是 macOS CI 上的 xcodebuild 真编译 —— 本闸门只把一部分
#   低级错误提前到 ubuntu 静态 job 上，让它失败得更快、更好归因。
#
#   反过来说：本闸门**不替代** G16/build-app 的真编译。若哪天有人以为
#   「G18 绿了就说明 iOS 能编」，那是误读 —— 故在此显式写明。
#
# ── 为什么用「逐条列出」而不是「模糊匹配 maximum* 前缀」──────────────────
#   模糊匹配会误伤**真实存在**的 API（如 `CMMotionManager.deviceMotionUpdateInterval`、
#   `maximumNumberOfLines` 一类 UIKit 属性），造成假阳性。宁可窄而准。
_SYSTEM_API_DENYLIST: list[tuple[str, str, str]] = [
    # (幻觉成员名, 所在类型, 正确替代/说明)
    ("maximumGyroUpdateInterval", "CMMotionManager",
     "该成员不存在。CMMotionManager 无任何查询硬件最大采样率的接口；"
     "gyroUpdateInterval 是**调用方写入**的期望值。iOS 侧该字段应写 0（未知）"),
    ("maximumDeviceMotionUpdateInterval", "CMMotionManager",
     "该成员不存在（同上）。docs/11 §3.1.5 曾引用此名，已一并修正"),
    ("maximumAccelerometerUpdateInterval", "CMMotionManager",
     "该成员不存在（同上）"),
]


def gate_system_api_denylist(rep: Report) -> None:
    gate = "G18/system-api-denylist"
    files = iter_swift_files() + iter_ios_platform_impl_files()
    hits = 0
    for path in files:
        raw = read_text(path)
        # 剥注释与字符串：注释里会**刻意**提到这些错名字（说明"这里曾写错"），
        # 那不是违规 —— 与 G3 的处置一致。
        code = strip_comments_and_strings(raw)
        for member, owner, advice in _SYSTEM_API_DENYLIST:
            if member not in code:
                continue
            # 取出现处的行号（在**原文**里找，便于人看）
            for i, ln in enumerate(raw.splitlines(), 1):
                if member in ln and not ln.lstrip().startswith("//") \
                        and not ln.lstrip().startswith("*"):
                    rep.err(gate,
                            f"{rel(path)}:{i} 使用了**不存在**的系统 API "
                            f"`{owner}.{member}`。{advice}。"
                            f"（真实 CI 会在这里以 `has no member` 编译失败）")
                    hits += 1
    if hits == 0:
        rep.note(f"系统 API 幻觉清单：{len(files)} 个 iOS 源文件已扫，"
                 f"未出现 {len(_SYSTEM_API_DENYLIST)} 个已登记幻觉成员 ✓"
                 f"（注意：本闸门只覆盖**已登记**的名字，不是类型检查器）")


def main() -> int:
    rep = Report()
    if not os.path.isdir(UI_IOS):
        print(f"错误：找不到 {rel(UI_IOS)}", file=sys.stderr)
        return 2

    gate_locales(rep)
    gate_glass(rep)
    gate_banned(rep)
    gate_plist(rep)
    gate_abi(rep)
    gate_l10n_keys(rep)
    gate_wiring(rep)
    gate_fallback(rep)
    gate_hardcoded_copy(rep)
    gate_abi_boundary(rep)
    gate_status_parity(rep)
    gate_stage_map(rep)
    gate_status_mapping(rep)
    gate_platform_plane_discipline(rep)
    gate_bundle_layout(rep)
    gate_ipa_pipeline(rep)
    gate_locale_resource_layout(rep)
    gate_system_api_denylist(rep)

    print("=" * 68)
    print("iOS 契约闸门（tools/check_ios_contract.py）")
    print("=" * 68)
    for n in rep.notes:
        print(f"  说明  {n}")
    print()
    if rep.warnings:
        for w in rep.warnings:
            print(f"  警告  {w}")
        print()
    if rep.errors:
        for e in rep.errors:
            print(f"  失败  {e}")
        print()
        print(f"结论：{len(rep.errors)} 项违规 —— 闸门未通过。")
        return 1

    print("结论：G1 locale / G2 玻璃白名单 / G3 禁令 / G4 权限 / G5 ABI / "
          "G6 键集合 / G7 混编接线 / G8 回退链 / G9 去硬编码 / "
          "G10 ABI 边界 / G11 错误码逐值对等 / G12 阶段映射自洽 / "
          "G13 错误码映射穷举 / G14 平台平面缓冲纪律 / "
          "G15 iOS bundle 落位纪律 / G16 未签名 IPA 链纪律 / "
          "G17 locale 资源落位纪律 / G18 系统 API 幻觉清单 全部通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
