#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""check_android_contract.py — Android 侧静态契约闸门（无需 JDK/SDK/NDK）。

背景
----
Android 的真构建需要 JDK + Android SDK + NDK（外加 Gradle 首次要联网拉依赖），
在开发机上通常不可用；而**本仓库的 Android 链长期从未被任何编译器验证过**
（只有 `ios.yml`，没有任何 Android CI job，见 docs/14 缺口 R）。于是下列漂移
全部长期潜伏、且在「桌面 ctest 28/28 全绿」的掩护下看起来一切正常：

  · `targets` 漏列 JNI 薄壳     → `librecon_core.so` 内**零 JNI 符号**
  · CMake target 全是 STATIC    → 根本没有 `.so` 可 `loadLibrary`
  · `recon_shaders_vulkan` 是孤儿 custom target → SPIR-V 永不生成
  · Kotlin 用了未声明依赖的包    → Kotlin 编译期 `Unresolved reference`
  · Manifest 引用了不存在的资源  → AAPT 资源链接期失败

这些**都不是**「编译器会告诉你」的那类错误 —— 它们要么在运行期才炸
（`UnsatisfiedLinkError`），要么在另一个语言/另一个工具的构建阶段才炸。
本闸门把「编译前、纯静态、任何机器可跑」的那部分全部锁死。

与既有闸门的分工（避免重复实现 —— 铁律「同一逻辑不写两遍」）
------------------------------------------------------------
`tools/check_platform_parity.py` 已经覆盖：
  · 闸门 9   JNI 声明 ↔ 实现闭合（Kotlin `external fun` ↔ `Java_*` 符号）
  · 闸门 9-b 每个 JNI `.cpp` 已注册进根 CMakeLists 的 `recon_platform_android`
本闸门**不重复**这两项，而是补上它**结构上查不到**的剩余部分：

G-AND-1  JNI 定义所属 target ↔ Gradle `targets` 覆盖
         `recon_platform_android` 里定义了 JNI 符号，则 `build.gradle` 的
         `externalNativeBuild.cmake.targets` **必须**列出它。
         （闸门 9-b 只查 CMakeLists 注册，不查 Gradle `targets` —— 而
           决定「哪些 .so 进 APK」的恰恰是 `targets`。这正是缺口 R ②。）

G-AND-2  `loadLibrary("X")` ↔ CMake 里存在**产出 `.so` 的 target X**
         Kotlin 每个 `System.loadLibrary("name")` 都要求名为 `libname.so` 的
         动态库真实存在，即 CMake 里对应 target 的库类型必须是 SHARED
         （或 MODULE）。STATIC 下无 `.so` → 真机启动第一步即
         `UnsatisfiedLinkError: dlopen failed: library "libX.so" not found`。
         （第四十三轮**致命缺口 A**：全仓库 target 皆 STATIC。）

G-AND-3  Manifest 资源引用 ↔ `res/` 真实存在
         `@mipmap/ic_launcher` / `@style/Theme.Recon` / `@string/app_name` /
         `@color/...` / `@drawable/...` 每一项都要在 `res/` 下有定义，
         且**资源类型目录必须匹配**（`@mipmap/x` 不能放在 `drawable/` 下 ——
         AAPT 按类型目录名解析，这是第四十二轮真实踩过的坑）。
         另查 `android:name=".Xxx"`（相对类名）在 Kotlin 源码里存在。

G-AND-4  Kotlin `import` ↔ Gradle `dependencies` 声明
         源码实际 import 的 `androidx.*` 分组必须在依赖里被声明。
         漏声明的表现是 Kotlin 编译期 `Unresolved reference`，而文档片段
         与源码**双双漂移**过（第四十二轮缺口 R ③⑥）。

         ⚠️ **已知局限（如实登记，非静默）**：本判据只做**文本级**比对 ——
         读 `build.gradle` 里**写了哪些坐标**，不看 Maven 的**传递解析**结果。
         于是有两种情形它判不了：
           · **假阴**：某分组其实由传递依赖提供（未显式声明），本判据会报
             「缺声明」。例：`androidx.core:core-ktx:1.13.1` 的 POM **明确
             声明**了 `androidx.core:core:1.13.1`（scope=compile），故 core
             其实是传递获得的。第四十三轮据此**撤回**了一条错误断言
             （曾以为「core-ktx 不传递 core」，属事实错误，已勘误）。
           · **假阳**：显式声明了某 artifact，但源码已不再 import 它
             （无用依赖）。本判据不报 —— 那需要 `./gradlew dependencies`
             的真实解析图，本机无 JDK 时不可得。
         要消除这两种局限，唯一办法是在**有 JDK 的 CI** 里跑
         `./gradlew :app:dependencies --configuration releaseCompileClasspath`
         并把解析图与源码 import 做闭环比对。那属 `android.yml` 的职责，
         不在本静态闸门的可达范围内。

G-AND-5  SHARED/SOURCES 与 HAL/JNI 一致性 + 资源完整性反空约束
         每个 `loadLibrary` 名字对应的 target 都要在 CMakeLists 里真实定义
         （防「名字拼错」与「target 被改名」）；
         且断言 `res/` 下 `mipmap`/`values` 目录非空（防「闸门因找不到文件
         而恒真通过」—— MEMORY.md「判据可被工具覆盖即等同于可被绕过」）。

G-AND-6  薄壳转发路径的上级层数（`src/main/cpp/CMakeLists.txt`）
         转发壳用 `${CMAKE_CURRENT_LIST_DIR}/../../.../` 指到仓库根。层数写错
         → `RECON_REPO_ROOT` 落到 `<repo>/platform`（无 CMakeLists.txt）
         → `CMakeLists.txt:17` 触发 FATAL_ERROR → Gradle
         `:app:configureCMakeRelease[<abi>]` **失败**。
         ⚠️ 这类错误的隐蔽性：**桌面构建完全不受影响**（仓库根有 CMakeLists.txt，
         ctest 全绿），是「只有 CI 能发现」的一类 —— 第四十九轮实测：
         Android CI 从 #3 到 #6 **四次全部**倒在这里，同一期间 iOS 侧闸门
         + ctest 三十余项全绿。判据做两层：(a) 禁 `CMAKE_SOURCE_DIR`；
         (b) **真的做路径解析**（不是数 `../` 的文本比对，那会随写法漂移而失锚）。

包含的**反假绿**设计（本项目反复踩过的坑，逐条都对应真实事故）
--------------------------------------------------------------
1. **反空约束**：每个判据在「匹配数为 0」时**失败**，而非静默跳过。
   `check_16kb.sh` 与 `elf_load_align.py` 都因这一点做过专门处理；
   本闸门在收集端（`_require_nonempty`）统一实现。
2. **不依赖工具输出**：纯 Python 解析 XML/Gradle/CMake 文本，不 shell out
   到 `aapt` / `nm` / `readelf`（那些在无 SDK 的机器上不存在，一旦缺失就会
   让「判据」退化成「永远为真」）。
3. **判据与判定同处**：收集函数只返回事实，「是否违规」的判定只有一个入口
   （`judge_*`），避免「收集处放宽、判定处严格」这类双标准。

退出码
------
0 = 全部通过
1 = 用法错误 / 关键文件缺失（环境问题，非契约违规）
3 = 检测到契约违规
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
import xml.etree.ElementTree as ET

REPO = pathlib.Path(__file__).resolve().parent.parent
ANDROID = REPO / "platform" / "android"
APP = ANDROID / "app"
GRADLE = APP / "build.gradle"
MANIFEST = APP / "src" / "main" / "AndroidManifest.xml"
RES = APP / "src" / "main" / "res"
KOTLIN_SRC = APP / "src" / "main" / "java"
CPP_SRC = APP / "src" / "main" / "cpp"
ROOT_CMAKE = REPO / "CMakeLists.txt"

# Android 资源类型目录 → XML 标签名。
# AAPT 按**目录类型名**解析 `@type/name`，故目录与类型必须严格对应。
RES_TAG_FOR_DIR = {
    "values": None,          # values/ 下按子标签名定类型（string/color/style/...）
    "mipmap": "mipmap",
    "drawable": "drawable",
    "layout": "layout",
    "xml": "xml",
    "anim": "anim",
    "raw": "raw",
    "font": "font",
}

# values/ 下哪些子标签算「资源类型」定义。
VALUES_TAGS = {
    "string", "color", "style", "dimen", "bool", "integer",
    "string-array", "integer-array", "array", "attr", "declare-styleable",
}

# Kotlin `import androidx.foo.bar.Baz` → 需要的 Maven 坐标前缀。
# 只登记「源码可能用到、且不同 artifact 承载」的映射；未登记的分组不报错
# （避免把闸门变成一张必然漂移的手写清单 —— MEMORY.md 明列的高频坑）。
# 映射依据：每个 androidx 分组归哪个 artifact，见 AndroidX 官方分组页。
ANDROIDX_GROUP_TO_ARTIFACT = {
    "androidx.activity": ("androidx.activity:activity-ktx", "androidx.activity:activity"),
    "androidx.core": ("androidx.core:core-ktx", "androidx.core:core"),
    "androidx.lifecycle": ("androidx.lifecycle:lifecycle-runtime-ktx",
                           "androidx.lifecycle:lifecycle-runtime"),
    "androidx.fragment": ("androidx.fragment:fragment-ktx", "androidx.fragment:fragment"),
    "androidx.compose": ("androidx.compose",),   # 前缀匹配，见 _match
    "androidx.camera": ("androidx.camera",),
    "androidx.appcompat": ("androidx.appcompat:appcompat",),
}


# ---------------------------------------------------------------------------
# 基础读取 / 解析工具
# ---------------------------------------------------------------------------
def read(path: pathlib.Path) -> str:
    """读文本；不存在则返回空串（调用方用 _require() 判定环境问题）。"""
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except (FileNotFoundError, IsADirectoryError, PermissionError):
        return ""


def strip_cpp_comments(text: str) -> str:
    """去掉 // 与 /* */ 注释（避免把注释里的示例当真实代码）。

    ⚠️ 必须保留字符串字面量内的 //（如 `"jni://"`）。本项目的桥接代码里
    存在含 `//` 的字符串（路径拼接），故按状态机扫描而非正则一刀切。
    """
    out = []
    i, n = 0, len(text)
    state = "code"          # code | line | block | sq | dq
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line"
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block"
                i += 2
                continue
            if c == '"':
                state = "dq"
            elif c == "'":
                state = "sq"
            out.append(c)
            i += 1
            continue
        if state == "line":
            if c == "\n":
                state = "code"
                out.append(c)
            i += 1
            continue
        if state == "block":
            if c == "*" and nxt == "/":
                state = "code"
                i += 2
                continue
            if c == "\n":
                out.append(c)   # 保留行号
            i += 1
            continue
        # sq / dq：字符串内不做注释识别
        if c == "\\" and i + 1 < n:
            out.append(c)
            out.append(nxt)
            i += 2
            continue
        if (state == "dq" and c == '"') or (state == "sq" and c == "'"):
            state = "code"
        out.append(c)
        i += 1
    return "".join(out)


def strip_kotlin_comments(text: str) -> str:
    """去掉 Kotlin 的 // 与 /* */ 注释（Kotlin 支持嵌套块注释）。"""
    out = []
    i, n = 0, len(text)
    depth = 0
    state = "code"          # code | line | block | str
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line"
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block"
                depth = 1
                i += 2
                continue
            if c == '"':
                state = "str"
            out.append(c)
            i += 1
            continue
        if state == "line":
            if c == "\n":
                state = "code"
                out.append(c)
            i += 1
            continue
        if state == "block":
            if c == "/" and nxt == "*":
                depth += 1
                i += 2
                continue
            if c == "*" and nxt == "/":
                depth -= 1
                i += 2
                if depth == 0:
                    state = "code"
                continue
            if c == "\n":
                out.append(c)
            i += 1
            continue
        if c == "\\" and i + 1 < n:
            out.append(c)
            out.append(nxt)
            i += 2
            continue
        if c == '"':
            state = "code"
        out.append(c)
        i += 1
    return "".join(out)


def strip_groovy_comments(text: str) -> str:
    """去掉 Groovy 的 // 与 /* */ 注释（保留字符串字面量）。"""
    return strip_cpp_comments(text)     # Groovy 字符串规则对本用途足够


def strip_cmake_comments(text: str) -> str:
    """去掉 CMake 的 `#` 行注释。

    ⚠️ 教训（本闸门首版真实 bug）：CMake 的注释符是 `#`，**不是** `//`。
    首版误用 `strip_cpp_comments` 处理 CMakeLists.txt，于是注释里的中文括号
    `（...）` 与 `(...)` 一起进了「代码」流；而 `add_library(...)` 的解析用
    `[^)]*` 抓参数，**第一个在注释里出现的 `)` 就把参数截断了** ——
    导致 `recon_platform_android` 整个 target 没被解析出来，
    G-AND-1 报「推断不出承载 JNI 的 target」（假红），
    G-AND-2 报「无同名 target」（也是假红）。
    这类「判据自己的解析器有 bug → 报假红/假绿」正是 MEMORY.md 反复强调的坑：
    **每条判据都得靠负向注入 + 基线实跑双向证伪**，不能只看它「跑起来了」。
    """
    out = []
    for line in text.splitlines(keepends=True):
        # 行内 # 之后全为注释（CMake 无字符串内 # 需保留的场景）
        idx = line.find("#")
        out.append(line if idx < 0 else line[:idx] + "\n")
    return "".join(out)


class Report:
    def __init__(self) -> None:
        self.errors: list[str] = []
        self.notes: list[str] = []

    def err(self, gate: str, msg: str) -> None:
        self.errors.append(f"[{gate}] {msg}")

    def note(self, msg: str) -> None:
        self.notes.append(msg)


def rel(p: pathlib.Path) -> str:
    try:
        return str(p.relative_to(REPO)).replace("\\", "/")
    except ValueError:
        return str(p).replace("\\", "/")


def _require(rep: Report, path: pathlib.Path, why: str) -> bool:
    """环境问题（文件不存在）→ 记 error 并返回 False。

    与「契约违规」区分开：文件缺失是本闸门**无法执行**，必须硬失败，
    绝不能静默当作「没有违规」—— 这是本项目「不静默降级」铁律的落点。
    """
    if not path.exists():
        rep.err("env", f"关键文件不存在：{rel(path)}（{why}）")
        return False
    return True


# ---------------------------------------------------------------------------
# 收集：事实提取（只返回事实，不做判定）
# ---------------------------------------------------------------------------
JNI_SYMBOL_RE = re.compile(r"Java_com_recon_scan_([A-Za-z0-9]+)_([A-Za-z0-9_]+)")


def collect_jni_symbols() -> dict[str, set[str]]:
    """扫 cpp/ 下所有 .cpp，返回 {类名: {方法名}}。

    与 check_platform_parity.py 的闸门 9 同源（那里做声明↔实现闭合）；
    此处只用来推断「这些符号定义在哪个 target 的源文件里」。
    """
    found: dict[str, set[str]] = {}
    for cpp in sorted(CPP_SRC.glob("*.cpp")):
        text = strip_cpp_comments(read(cpp))
        for cls, meth in JNI_SYMBOL_RE.findall(text):
            found.setdefault(cls, set()).add(meth)
    return found


def collect_cmake_targets() -> dict[str, dict]:
    """解析根 CMakeLists.txt 的 add_library，返回 {target: {type, sources}}。

    只做**文本级**解析（本项目 CMakeLists 的 add_library 一律是单行或多行
    字面清单），并显式处理 `SHARED`/`STATIC`/`MODULE` —— 这正是缺口 A 的
    判据来源。

    ⚠️ 必须用 `strip_cmake_comments`（`#` 注释），不能用 C 系 `//` 版本 ——
    否则注释里的 `)` 会截断 `add_library(...)` 的参数采集（见该函数 docstring
    记录的首版真实 bug）。
    """
    text = strip_cmake_comments(read(ROOT_CMAKE))
    targets: dict[str, dict] = {}

    # 逐块解析：从 `add_library(` 起，按括号配平找到匹配的 `)`。
    # 用配平而非 `[^)]*`，因为 sources 里有 `${...}` 与可能的嵌套调用。
    for m in re.finditer(r"add_library\s*\(", text):
        start = m.end()
        depth, i, n = 1, start, len(text)
        while i < n and depth:
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
            i += 1
        if depth:
            continue                    # 括号不配平 → 跳过该处（异常输入）
        body = text[start:i - 1]
        toks = body.split()
        if not toks:
            continue
        name = toks.pop(0)
        # 变量形态：`add_library(recon_core ${RECON_CORE_LIB_TYPE} ...)`
        # → 该 token 不是类型关键字，先原样保留，稍后由变量回读修正。
        lib_type = "STATIC"             # CMake 默认（BUILD_SHARED_LIBS 未设时）
        if toks and toks[0] in ("STATIC", "SHARED", "MODULE", "OBJECT",
                                "INTERFACE"):
            lib_type = toks.pop(0)
        targets[name] = {"type": lib_type, "sources": toks}

    # 变量形态回读：`set(RECON_CORE_LIB_TYPE SHARED|STATIC)` 位于
    # `if(RECON_PLATFORM_ANDROID)` 分支内 → Android 下取分支内的那个取值。
    blk = re.search(r"if\(RECON_PLATFORM_ANDROID\)(.*?)\nendif\(\)", text, re.S)
    scope = blk.group(1) if blk else text
    var = re.search(r"set\(RECON_CORE_LIB_TYPE\s+(SHARED|STATIC)\)", scope)
    if var and "recon_core" in targets:
        targets["recon_core"]["type"] = var.group(1)
    # 兜底：若变量在 if 之外定义（形态重构），也用全局唯一取值。
    if "recon_core" in targets and targets["recon_core"]["type"] == "STATIC":
        gvar = re.search(r"set\(RECON_CORE_LIB_TYPE\s+(SHARED|STATIC)\)", text)
        if gvar:
            targets["recon_core"]["type"] = gvar.group(1)
    return targets


def collect_gradle_targets() -> list[str] | None:
    """从 build.gradle 取 externalNativeBuild.cmake.targets 的清单。

    返回 None 表示**该键不存在** —— 调用方必须把它当作违规（缺 targets 时
    Gradle 会打进全部 target，看似「更宽松」，实则不可控且与文档契约不符）。
    """
    text = strip_groovy_comments(read(GRADLE))
    m = re.search(r"\btargets\s+([^\n]*)$", text, re.M)
    if not m:
        return None
    raw = m.group(1)
    return re.findall(r"['\"]([^'\"]+)['\"]", raw)


def collect_gradle_dependencies() -> list[str]:
    """取 dependencies { implementation/... '坐标:版本' } 的坐标清单。"""
    text = strip_groovy_comments(read(GRADLE))
    blk = re.search(r"\bdependencies\s*\{(.*)\}", text, re.S)
    if not blk:
        return []
    deps: list[str] = []
    for m in re.finditer(r"^\s*(?:implementation|api|compileOnly|runtimeOnly|"
                         r"debugImplementation|releaseImplementation)\s+"
                         r"['\"]([^'\"]+)['\"]", blk.group(1), re.M):
        deps.append(m.group(1))
    return deps


def collect_load_library() -> list[tuple[str, str]]:
    """扫 Kotlin 源，返回 [(库名, 出现位置)]。

    `System.loadLibrary("recon_core")` 要求 `librecon_core.so` 存在。
    同时识别 `const val` 间接形式（本项目是直接字面量，仍留出扩展位）。
    """
    out: list[tuple[str, str]] = []
    for kt in sorted(KOTLIN_SRC.rglob("*.kt")):
        text = strip_kotlin_comments(read(kt))
        for m in re.finditer(r'loadLibrary\(\s*"([^"]+)"\s*\)', text):
            line = text[:m.start()].count("\n") + 1
            out.append((m.group(1), f"{rel(kt)}:{line}"))
    return out


MANIFEST_REF_RE = re.compile(r'@(mipmap|drawable|string|style|color|dimen|'
                             r'integer|bool|layout|xml|font|raw|anim)/'
                             r'([A-Za-z_][A-Za-z0-9_.]*)')
MANIFEST_ANDROID_NAME_RE = re.compile(
    r'android:name\s*=\s*"\.([A-Za-z_][A-Za-z0-9_.]*)"')


def collect_manifest_refs() -> list[tuple[str, str, int]]:
    """返回 Manifest 里全部 `@type/name` 引用 → [(type, name, lineno)]。"""
    text = read(MANIFEST)
    refs: list[tuple[str, str, int]] = []
    for m in MANIFEST_REF_RE.finditer(text):
        line = text[:m.start()].count("\n") + 1
        refs.append((m.group(1), m.group(2), line))
    return refs


def collect_res_definitions() -> tuple[set[tuple[str, str]], dict[str, int]]:
    """扫描 res/，返回 ({(type, name)}, {type: 定义条数})。

    `type` 是 AAPT 语义的资源类型（不是目录名）：
      · `values/` 下的子标签决定类型（`<string name=x>` → ("string","x")）
      · `mipmap-*/` 下的**文件名**决定类型（`ic_launcher.png` → ("mipmap","ic_launcher")）
      · `drawable-*/` 同理
    这样 `@mipmap/ic_launcher` 放在 `drawable/` 下会被正确判定为**未定义**
    —— 这正是第四十二轮真实踩过的坑（AAPT: resource mipmap/ic_launcher not found）。
    """
    defs: set[tuple[str, str]] = set()
    counts: dict[str, int] = {}
    if not RES.exists():
        return defs, counts
    for d in sorted(RES.iterdir()):
        if not d.is_dir():
            continue
        # 目录名去掉 -qualifier 后缀（mipmap-anydpi-v26 → mipmap）
        base = d.name.split("-")[0]
        if base == "values":
            for xml in sorted(d.glob("*.xml")):
                try:
                    root = ET.parse(xml).getroot()
                except ET.ParseError:
                    continue
                for child in root:
                    tag = child.tag
                    if tag not in VALUES_TAGS:
                        continue
                    nm = child.get("name")
                    if not nm:
                        continue
                    # <string-array> 的 AAPT 类型是 array
                    aapt_type = {"string-array": "array",
                                 "integer-array": "array"}.get(tag, tag)
                    defs.add((aapt_type, nm))
                    counts[aapt_type] = counts.get(aapt_type, 0) + 1
        else:
            for f in sorted(d.iterdir()):
                if not f.is_file():
                    continue
                stem = f.name.rsplit(".", 1)[0]
                # xml 资源（如 mipmap/ic_launcher.xml）与位图同规则
                defs.add((base, stem))
                counts[base] = counts.get(base, 0) + 1
    return defs, counts


def collect_kotlin_imports() -> set[str]:
    """收集 Kotlin 源码里的 `import androidx.*` / `import com.google.*` 全名。"""
    out: set[str] = set()
    for kt in sorted(KOTLIN_SRC.rglob("*.kt")):
        text = strip_kotlin_comments(read(kt))
        for m in re.finditer(r"^\s*import\s+((?:androidx|com\.google)"
                             r"[A-Za-z0-9_.]*)", text, re.M):
            out.add(m.group(1))
    return out


def collect_kotlin_classes() -> set[str]:
    """收集 Kotlin 顶层类名（用于校验 Manifest 的 `android:name=".Xxx"`）。"""
    out: set[str] = set()
    for kt in sorted(KOTLIN_SRC.rglob("*.kt")):
        text = strip_kotlin_comments(read(kt))
        for m in re.finditer(r"^\s*(?:open\s+|abstract\s+|sealed\s+|final\s+|"
                             r"data\s+|enum\s+)?class\s+([A-Za-z_][A-Za-z0-9_]*)",
                             text, re.M):
            out.add(m.group(1))
    return out


# ---------------------------------------------------------------------------
# 判定：唯一入口（收集与判定分离，避免双标准）
# ---------------------------------------------------------------------------
def judge_g1_jni_target_covered(
    rep: Report, jni: dict[str, set[str]], gradle_targets: list[str] | None
) -> None:
    """G-AND-1：定义 JNI 符号的 target 必须出现在 Gradle `targets` 里。"""
    if gradle_targets is None:
        rep.err("G-AND-1", "build.gradle 的 externalNativeBuild.cmake 缺少 "
                           "`targets` 键。缺省时 Gradle 会打进全部 target，"
                           "但契约（docs/12 §3.4.1）要求显式声明。")
        return
    if not gradle_targets:
        rep.err("G-AND-1", "`targets` 解析为空 —— 判据无法执行"
                           "（反空约束：拒绝在零数据上通过）")
        return
    # 「定义 JNI 符号」的 target：扫 cpp/ 下 .cpp，看它是否属于某 target 的 sources
    cmake = collect_cmake_targets()
    owner_names: set[str] = set()
    for tname, info in cmake.items():
        for src in info["sources"]:
            # 只关心 Android JNI 源（platform/android/.../cpp/*.cpp）
            if "platform/android/app/src/main/cpp/" in src.replace("\\", "/"):
                owner_names.add(tname)
    if not owner_names:
        rep.err("G-AND-1", "未能从根 CMakeLists.txt 推断出任何承载 Android JNI "
                           "源文件的 target —— 判据失效，拒绝通过")
        return
    missing = sorted(owner_names - set(gradle_targets))
    if missing:
        rep.err(
            "G-AND-1",
            f"根 CMakeLists.txt 里 target {missing} 承载了 "
            f"{sum(len(v) for v in jni.values())} 个 JNI 符号定义，"
            f"但 build.gradle 的 `targets` 只列出 {gradle_targets}。"
            "Gradle 只把 `targets` 里列出的 target 的 .so 打进 APK ——"
            "漏列的那个 .so 会被 Gradle **静默丢弃**，运行期首调其 JNI 方法即 "
            "`java.lang.UnsatisfiedLinkError: No implementation found for ...`。"
            "（docs/14 缺口 R ② 的真实事故，静态闸门此前结构上查不出）")
    else:
        rep.note(f"G-AND-1 JNI 定义 target {sorted(owner_names)} 均已在 "
                 f"Gradle `targets` 中声明 ✓")


def judge_g2_loadlibrary_is_shared(
    rep: Report, load_calls: list[tuple[str, str]], cmake: dict[str, dict]
) -> None:
    """G-AND-2：`loadLibrary("X")` 对应的 CMake target X 必须产出 `.so`。"""
    if not load_calls:
        rep.err("G-AND-2", "未在任何 Kotlin 源中找到 `System.loadLibrary(...)` ——"
                           "判据无法执行（反空约束）。若确实移除了加载逻辑，"
                           "请同步删除本判据。")
        return
    bad: list[str] = []
    for lib, where in load_calls:
        info = cmake.get(lib)
        if info is None:
            bad.append(f"{where} 加载 `{lib}`，但根 CMakeLists.txt 中"
                       f"**无同名 target**（target 被改名或拼错）")
            continue
        if info["type"] not in ("SHARED", "MODULE"):
            bad.append(
                f"{where} 加载 `{lib}`，但该 target 类型是 {info['type']}，"
                f"**不产出 .so**。`loadLibrary` 只认动态库文件，"
                f"STATIC 下真机启动第一步即 "
                f"`UnsatisfiedLinkError: dlopen failed: "
                f"library \"lib{lib}.so\" not found`")
    if bad:
        for b in bad:
            rep.err("G-AND-2", b)
        rep.err("G-AND-2", "修法：Android 平台下把该 target 改为 SHARED"
                           "（桌面/iOS 维持 STATIC，见 CMakeLists.txt 的 "
                           "RECON_CORE_LIB_TYPE 注释）")
    else:
        rep.note(f"G-AND-2 全部 {len(load_calls)} 处 loadLibrary 对应的 target "
                 f"均为动态库形态 ✓")


def judge_g3_manifest_refs(
    rep: Report, refs: list[tuple[str, str, int]],
    defs: set[tuple[str, str]], classes: set[str],
) -> None:
    """G-AND-3：Manifest 的 `@type/name` 与 `android:name=".Xxx"` 均须可解析。"""
    if not refs:
        rep.err("G-AND-3", "AndroidManifest.xml 中未解析到任何 `@type/name` 资源引用"
                           " —— 判据无法执行（反空约束）")
    else:
        missing = [(t, n, ln) for (t, n, ln) in refs if (t, n) not in defs]
        for t, n, ln in missing:
            rep.err("G-AND-3",
                    f"AndroidManifest.xml:{ln} 引用 `@{t}/{n}`，但 res/ 下"
                    f"**没有该类型该名字的资源**。"
                    f"（AAPT 按资源**类型目录名**解析：`@mipmap/x` 必须落在 "
                    f"`mipmap*/` 下，放在 `drawable/` 下无效 —— "
                    f"第四十二轮真实事故 `resource mipmap/ic_launcher not found`）")
        if not missing:
            rep.note(f"G-AND-3 Manifest 的 {len(refs)} 处资源引用均可在 res/ 解析 ✓")

    app_names = []
    text = read(MANIFEST)
    for m in MANIFEST_ANDROID_NAME_RE.finditer(text):
        line = text[:m.start()].count("\n") + 1
        app_names.append((m.group(1), line))
    if not app_names:
        rep.err("G-AND-3", "Manifest 未解析到任何 `android:name=\".Xxx\"` 相对类名"
                           " —— 判据无法执行（反空约束）")
    else:
        bad_cls = [(c, ln) for (c, ln) in app_names if c not in classes]
        for c, ln in bad_cls:
            rep.err("G-AND-3",
                    f"AndroidManifest.xml:{ln} 声明 `android:name=\".{c}\"`，"
                    f"但 Kotlin 源码中**无此类**（相对类名会解析为 "
                    f"`<applicationId>.{c}`）→ 运行期 "
                    f"`ClassNotFoundException`")
        if not bad_cls:
            rep.note(f"G-AND-3 Manifest 的 {len(app_names)} 处相对类名均存在 ✓")


def _match_artifact(group: str, deps: list[str]) -> bool:
    """判断 androidx 分组 group 是否被 deps 里某条声明覆盖。"""
    cands = ANDROIDX_GROUP_TO_ARTIFACT.get(group)
    if not cands:
        return True     # 未登记的分组不判错（避免手工清单漂移）
    for c in cands:
        for d in deps:
            dg = d.split(":")[0] + (":" + d.split(":")[1] if d.count(":") >= 1 else "")
            if d.startswith(c) or dg == c or dg.startswith(c):
                return True
    return False


def judge_g4_kotlin_deps(rep: Report, imports: set[str], deps: list[str]) -> None:
    """G-AND-4：Kotlin import 的 androidx 分组须在 dependencies 声明。"""
    if not imports:
        rep.err("G-AND-4", "Kotlin 源码中未解析到任何 androidx/com.google import"
                           " —— 判据无法执行（反空约束）")
        return
    if not deps:
        rep.err("G-AND-4", "build.gradle 的 dependencies 解析为空"
                           " —— 判据无法执行（反空约束）")
        return
    groups: dict[str, list[str]] = {}
    for imp in imports:
        parts = imp.split(".")
        # androidx.activity.result.contract.X → 取前三段（androidx.<lib>.<sub>）
        # androidx.core.app.X → androidx.core
        for take in (3, 2):
            g = ".".join(parts[:take])
            if g in ANDROIDX_GROUP_TO_ARTIFACT:
                groups.setdefault(g, []).append(imp)
                break
    missing = {g: v for g, v in groups.items() if not _match_artifact(g, deps)}
    if missing:
        for g, v in sorted(missing.items()):
            rep.err("G-AND-4",
                    f"Kotlin 源码 import 了 `{g}.*`（如 {v[0]}），但 "
                    f"build.gradle 的 dependencies 未声明承载它的 artifact "
                    f"（应为 {ANDROIDX_GROUP_TO_ARTIFACT[g]}）→ Kotlin 编译期 "
                    f"`Unresolved reference`。当前依赖：{deps}")
    else:
        rep.note(f"G-AND-4 {len(groups)} 个 androidx 分组均已在 dependencies 声明 ✓")


def judge_g5_res_nonempty(rep: Report, counts: dict[str, int]) -> None:
    """G-AND-5：res/ 的关键类型必须有定义（反假绿）。

    若 `mipmap` 一个都没扫到，而 Manifest 确实引用了 `@mipmap/...`，
    那么 G-AND-3 会失败 —— 但如果扫描器本身失效（目录不存在 / 解析全失败），
    G-AND-3 的「missing」会是「全部都是 missing」，看起来也像违规。
    为区分「真的缺资源」与「扫描器坏了」，此处对**已知必须存在**的类型做
    非空断言，让「扫描器失效」以独立、可归因的方式失败。
    """
    if not RES.exists():
        rep.err("G-AND-5", f"{rel(RES)} 目录不存在")
        return
    required = ("mipmap", "color", "style", "string")
    for t in required:
        if counts.get(t, 0) == 0:
            rep.err("G-AND-5",
                    f"res/ 下未扫描到任何 `{t}` 类型资源（或扫描器失效）。"
                    f"当前统计：{dict(sorted(counts.items()))}。"
                    f"这会让 G-AND-3 的判定失去意义。")
    if not rep.errors:
        rep.note(f"G-AND-5 res/ 资源统计：{dict(sorted(counts.items()))} ✓")


# ---------------------------------------------------------------------------
# G-AND-6：转发壳（src/main/cpp/CMakeLists.txt）的上级路径层数
# ---------------------------------------------------------------------------
def judge_g6_forwarder_path(rep: Report) -> None:
    """G-AND-6：薄壳转发路径的 `../` 层数必须真的指到仓库根。

    为什么必须**实测**而不是「数一数对不对」：这条路径错了的表现是
    Gradle 的 `:app:configureCMakeRelease[<abi>]` 失败（`CMakeLists.txt:17`
    的 FATAL_ERROR），而**桌面构建完全不受影响**（仓库根有 CMakeLists.txt，
    ctest 照样全绿）—— 属「只有 CI 能发现」的一类。实测：Android CI 从 #3
    到 #6 **四次全部**倒在这里，而同一期间 iOS 侧闸门 + ctest 三十余项全绿。

    判据分两层（都做，缺一不可）：
      (a) **静态**：不许出现 `${CMAKE_SOURCE_DIR}`（转发壳的「自己在哪里」
          只有一个正确答案：`CMAKE_CURRENT_LIST_DIR`；前者语义是「顶层源
          目录」，被 `add_subdirectory` 引入即漂移）。
      (b) **实测**：把路径片段与 `CPP_SRC` 拼接后**真的做路径解析**
          （`pathlib` 的 `..` 折叠），断言结果 == 仓库根，且该处确有
          `CMakeLists.txt`。只比对「写了几个 `../`」是文本判据，路径一改
          就会失锚；实测等价于 CMake 的 `get_filename_component(... ABSOLUTE)`。
    """
    forwarder = CPP_SRC / "CMakeLists.txt"
    if not _require(rep, forwarder, "G-AND-6：Android 薄壳转发文件"):
        return
    text = strip_cmake_comments(read(forwarder))

    # (a) CMAKE_SOURCE_DIR 禁令
    if "CMAKE_SOURCE_DIR" in text:
        rep.err("G-AND-6",
                f"{rel(forwarder)} 使用了 `CMAKE_SOURCE_DIR` —— 转发壳必须用 "
                f"`CMAKE_CURRENT_LIST_DIR`（前者语义是「顶层源目录」，"
                f"被 add_subdirectory 引入即漂移）。")

    # (b) 实测路径解析
    m = re.search(
        r"\$\{CMAKE_CURRENT_LIST_DIR\}((?:/\.\.)+)/", text)
    if not m:
        rep.err("G-AND-6",
                f"{rel(forwarder)} 中未找到 `${{CMAKE_CURRENT_LIST_DIR}}/../.../` "
                f"形态的上级路径 —— 判据失锚（路径写法已变），请同步更新本判据。")
        return

    ups = m.group(1).count("..")
    # ⚠️ 必须 `lstrip("/")` 再拼接：`Path("a/b") / "/../.."` 在 Windows 上会把
    #    以分隔符开头的参数当作**根相对路径**（`\../../..`）→ 丢掉前缀 `a/b`，
    #    直接解析到盘根 → 判据对正确与错误代码**一律报错**（假红）。
    #    本行由「注入 5 级与 6 级都报错」的负向注入实测暴露并修正。
    resolved = (CPP_SRC / m.group(1).lstrip("/")).resolve()
    want = REPO.resolve()
    if resolved != want:
        rep.err("G-AND-6",
                f"{rel(forwarder)} 的上级路径解析到 `{resolved}`，"
                f"但仓库根是 `{want}`（当前 {ups} 个 `../`）。"
                f"CMake 会在 `CMakeLists.txt:17` 触发 FATAL_ERROR → "
                f"`configureCMakeRelease` 失败。")
        return
    if not (resolved / "CMakeLists.txt").is_file():
        rep.err("G-AND-6", f"解析到的 `{resolved}` 下没有 CMakeLists.txt。")
        return

    rep.note(f"G-AND-6 转发壳上级路径实测指到仓库根（{ups} 个 `../`）✓")


# ---------------------------------------------------------------------------
# 入口
# ---------------------------------------------------------------------------
def run(gates: set[str] | None = None) -> int:
    rep = Report()
    for p, why in ((GRADLE, "Gradle 构建脚本；本闸门的核心输入"),
                   (MANIFEST, "Manifest 资源与组件声明"),
                   (ROOT_CMAKE, "target 定义与库形态")):
        _require(rep, p, why)
    if rep.errors:
        for e in rep.errors:
            print(f"::error::{e}")
        return 1

    want = gates or {"1", "2", "3", "4", "5", "6"}

    jni = collect_jni_symbols()
    cmake = collect_cmake_targets()
    gradle_targets = collect_gradle_targets()
    load_calls = collect_load_library()

    if "1" in want:
        judge_g1_jni_target_covered(rep, jni, gradle_targets)
    if "2" in want:
        judge_g2_loadlibrary_is_shared(rep, load_calls, cmake)
    if "3" in want or "5" in want:
        defs, counts = collect_res_definitions()
        refs = collect_manifest_refs()
        classes = collect_kotlin_classes()
        if "3" in want:
            judge_g3_manifest_refs(rep, refs, defs, classes)
        if "5" in want:
            judge_g5_res_nonempty(rep, counts)
    if "4" in want:
        judge_g4_kotlin_deps(rep, collect_kotlin_imports(),
                             collect_gradle_dependencies())
    if "6" in want:
        judge_g6_forwarder_path(rep)

    for n in rep.notes:
        print(f"[android-contract] {n}")
    if rep.errors:
        for e in rep.errors:
            print(f"::error::{e}")
        print(f"[android-contract] FAIL：{len(rep.errors)} 项违规")
        return 3
    print("[android-contract] PASS：G-AND-1..6 全部通过")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Android 侧静态契约闸门（无需 JDK/SDK/NDK）")
    ap.add_argument("--gates", default="",
                    help="只跑指定判据，逗号分隔（1,2,3,4,5,6）。默认全跑。")
    args = ap.parse_args()
    gates = {g.strip() for g in args.gates.split(",") if g.strip()}
    if gates and not gates <= {"1", "2", "3", "4", "5", "6"}:
        print(f"::error::未知判据：{sorted(gates - {'1','2','3','4','5','6'})}")
        return 1
    return run(gates or None)


if __name__ == "__main__":
    sys.exit(main())
