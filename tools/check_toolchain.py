#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_toolchain.py — Android 工具链版本一致性闸门（B-1 的配套裁决器）。

背景
----
`platform/android/build.gradle` 的注释与 `docs/12_构建与打包.md` §3.4.2 **双重**
声明：「三个版本必须一致，由 tools/check_toolchain.py 在 CI 复核」：

    NDK 28.1.13356709 (r28b) / AGP 8.7.3 / Gradle 8.9

但该脚本**从来不存在** —— 属 MEMORY.md 明列的「设计意图未落实」类：
断言被写在注释里、没有任何机器读它。故本脚本是它的**首个实现**。

它要防的具体事故
----------------
三个版本分散在**四处**互不引用的位置，改一处而漏另一处时**没有任何编译期信号**：

    ① `platform/android/build.gradle`  → `ext.ndkRequired / agpRequired`
    ② `platform/android/app/build.gradle` → `android.ndkVersion` + 断言
    ③ `platform/android/app/build.gradle` → `externalNativeBuild.cmake.version`
                                            （Android SDK 里要装的 CMake）
    ④ `platform/android/gradle/wrapper/gradle-wrapper.properties`
                                       → `distributionUrl` 里的 Gradle 版本
    ⑤ `platform/android/build.gradle`  → `plugins { id 'com.android.application'
                                            version "${agpRequired}" }`（同源，须一致）

漂移后果：
  · NDK 改了而 ext 没改 → `app/build.gradle` 的 `if (ndkVersion != ndkRequired)`
    在 Gradle **配置期**抛 GradleException，CI 报错但**本地若不跑 Gradle 就看不见**；
  · AGP 改了而 Gradle 没跟着升 → AGP 8.7.3 要求 Gradle ≥ 8.9，
    报错形如 `Minimum supported Gradle version is 8.9` —— 与真正的根因距离很远；
  · NDK 降到 r28 以下 → **静默产出 4 KB 页对齐的 .so**，Google Play 上架被拒，
    而构建本身是"成功"的（这正是 docs/12 §3.5 立硬门禁的原因）。

判据（每条都能被反向证伪）
--------------------------
  A. 三处声明值必须与 docs/12 §3.1「CI 锁定版本」表**逐条相等**；
     证伪：把 app/build.gradle 的 ndkVersion 改一个字符 → 立即失败。
  B. `ext.ndkRequired` 与 `app.ndkVersion` 必须一致（同一事实两个副本）；
  C. `ext.agpRequired` 与 `plugins` 块里的 `version "${agpRequired}"` 引用 **同源**
     —— 若 plugins 块改成字面量，即出现第二事实来源，报警；
  D. Gradle wrapper 的 `distributionUrl` 版本必须 == `docs/12 §3.1` 的 Gradle 锁定版；
  E. NDK 不得低于 r28 下限（`28.0.13004108`）—— 独立于 A 的红线；
  F. `distributionUrl` 必须 pin 到具体版本（出现 `-latest` / 无版本号 → 失败），
     因为**不可复现的构建**与「docs/12 §1.1 确定性构建」直接冲突。

反空约束
--------
若任一提取正则**一个值都没取到**，脚本必须**失败**而不是"无差异即通过"
—— 正则失效会让整条闸门退化成恒真（本项目已吃过同类亏）。

退出码
------
0 = 一致；1 = 用法/文件缺失；3 = 检测到不一致（漂移）
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

# ---------------------------------------------------------------------------
# docs/12 §3.1「CI 锁定版本」表的机器可读副本
# ---------------------------------------------------------------------------
# ⚠️ 这是**期望值**，不是从被检文件回读的值。改动它等同于改契约，
#    必须同步改 docs/12 §3.1 的表（本脚本文件头即登记这一点）。
EXPECTED = {
    "ndk": "28.1.13356709",
    "agp": "8.7.3",
    "gradle": "8.9",
    "kotlin": "2.0.21",
    "cmake": "3.31.6",
    "compile_sdk": "35",
    "min_sdk": "24",
    "target_sdk": "35",
}

# NDK 硬下限（docs/12 §3.1：低于 r28 一律拒绝构建 —— 16 KB 页对齐）
NDK_MIN = "28.0.13004108"


class Report:
    def __init__(self) -> None:
        self.fails: list[str] = []
        self.notes: list[str] = []

    def fail(self, msg: str) -> None:
        self.fails.append(msg)

    def note(self, msg: str) -> None:
        self.notes.append(msg)


def read(path: pathlib.Path) -> str:
    if not path.is_file():
        raise FileNotFoundError(str(path))
    return path.read_text(encoding="utf-8", errors="replace")


def ver_tuple(s: str) -> tuple[int, ...]:
    """把 '8.9' / '28.1.13356709' 变成可比较的元组（不足位补 0）。"""
    parts = re.findall(r"\d+", s)
    return tuple(int(p) for p in parts) if parts else (0,)


def find_one(pattern: str, text: str, what: str, rep: Report, origin: str) -> str | None:
    """取**恰好一个**匹配；0 个或 ≥2 个都算失败（反空约束 + 反歧义）。"""
    hits = re.findall(pattern, text, re.M)
    if len(hits) == 0:
        rep.fail(f"{origin}：未能提取「{what}」—— 正则 `{pattern}` 零命中，"
                 f"判据已失效（反空约束）")
        return None
    if len(hits) > 1:
        uniq = sorted(set(hits))
        if len(uniq) > 1:
            rep.fail(f"{origin}：「{what}」出现多个**不同**值 {uniq} —— "
                     f"出现第二事实来源，必须收敛为一处")
            return None
        rep.note(f"{origin}：「{what}」出现 {len(hits)} 次但值一致（{uniq[0]}），"
                 f"容忍；建议收敛")
    return hits[0]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Android 工具链版本一致性闸门")
    ap.add_argument("--root", default=".", help="仓库根目录")
    args = ap.parse_args(argv)

    root = pathlib.Path(args.root).resolve()
    rep = Report()

    top_gradle = root / "platform/android/build.gradle"
    app_gradle = root / "platform/android/app/build.gradle"
    wrapper = root / "platform/android/gradle/wrapper/gradle-wrapper.properties"
    docs12 = root / "docs/12_构建与打包.md"

    for p in (top_gradle, app_gradle, wrapper, docs12):
        if not p.is_file():
            print(f"[toolchain] FAIL：缺少文件 {p}", file=sys.stderr)
            return 1

    t_gradle = read(top_gradle)
    a_gradle = read(app_gradle)
    w_props = read(wrapper)
    d12 = read(docs12)

    # -------------------------------------------------------------------
    # ① 顶层 build.gradle 的 ext 块
    # -------------------------------------------------------------------
    ext_ndk = find_one(r"ndkRequired\s*=\s*['\"]([^'\"]+)['\"]",
                       t_gradle, "ext.ndkRequired", rep, "platform/android/build.gradle")
    ext_agp = find_one(r"agpRequired\s*=\s*['\"]([^'\"]+)['\"]",
                       t_gradle, "ext.agpRequired", rep, "platform/android/build.gradle")
    ext_kotlin = find_one(r"kotlinRequired\s*=\s*['\"]([^'\"]+)['\"]",
                          t_gradle, "ext.kotlinRequired", rep,
                          "platform/android/build.gradle")
    ext_compile = find_one(r"compileSdkRequired\s*=\s*(\d+)",
                           t_gradle, "ext.compileSdkRequired", rep,
                           "platform/android/build.gradle")
    ext_min = find_one(r"minSdkRequired\s*=\s*(\d+)",
                       t_gradle, "ext.minSdkRequired", rep,
                       "platform/android/build.gradle")
    ext_target = find_one(r"targetSdkRequired\s*=\s*(\d+)",
                          t_gradle, "ext.targetSdkRequired", rep,
                          "platform/android/build.gradle")

    # ①b plugins 块的 AGP 版本必须**引用** ext，而不是写死字面量（判据 C）
    agp_plugin_literal = re.search(
        r"id\s+['\"]com\.android\.application['\"]\s+version\s+['\"](\d[^'\"]*)['\"]",
        t_gradle)
    agp_plugin_ref = re.search(
        r"id\s+['\"]com\.android\.application['\"]\s+version\s+[\"']\$\{agpRequired\}[\"']",
        t_gradle)
    if agp_plugin_literal:
        rep.fail("platform/android/build.gradle：plugins 块把 AGP 版本写成了**字面量** "
                 f"'{agp_plugin_literal.group(1)}' —— 与 ext.agpRequired 形成第二事实"
                 "来源（判据 C）。必须写成 version \"${agpRequired}\"")
    elif not agp_plugin_ref:
        rep.fail("platform/android/build.gradle：未能找到 com.android.application 插件"
                 "声明（判据 C 失效，反空约束）")

    # -------------------------------------------------------------------
    # ② app/build.gradle 的 ndkVersion 与 CMake 版本
    # -------------------------------------------------------------------
    app_ndk = find_one(r"ndkVersion\s*['\"]([^'\"]+)['\"]",
                       a_gradle, "android.ndkVersion", rep,
                       "platform/android/app/build.gradle")
    # app/build.gradle 里 ndkVersion 会出现两次（defaultConfig 外的声明 + 断言里
    # 的引用）。上面 find_one 已容忍「多次出现但值一致」。
    app_cmake = find_one(r"version\s*['\"](\d+\.\d+\.\d+)['\"]",
                         a_gradle, "externalNativeBuild.cmake.version", rep,
                         "platform/android/app/build.gradle")
    app_compile = find_one(r"compileSdk\s+(\d+)", a_gradle, "compileSdk", rep,
                           "platform/android/app/build.gradle")
    app_min = find_one(r"minSdk\s+(\d+)", a_gradle, "minSdk", rep,
                       "platform/android/app/build.gradle")
    app_target = find_one(r"targetSdk\s+(\d+)", a_gradle, "targetSdk", rep,
                          "platform/android/app/build.gradle")

    # -------------------------------------------------------------------
    # ③ Gradle wrapper
    # -------------------------------------------------------------------
    dist_url = find_one(r"distributionUrl\s*=\s*(\S+)", w_props,
                        "distributionUrl", rep,
                        "platform/android/gradle/wrapper/gradle-wrapper.properties")
    gradle_ver = None
    if dist_url:
        m = re.search(r"gradle-(\d+\.\d+(?:\.\d+)?)-", dist_url)
        if not m:
            rep.fail("gradle-wrapper.properties：distributionUrl 未能解析出 Gradle 版本"
                     f"（值：{dist_url}）—— 判据 D 失效。"
                     "必须形如 .../gradle-8.9-bin.zip")
        else:
            gradle_ver = m.group(1)
        # 判据 F：不得用 -latest / 浮动版本
        if re.search(r"gradle-(latest|\d+\.\d+\+)", dist_url):
            rep.fail(f"gradle-wrapper.properties：distributionUrl 使用了浮动版本"
                     f"（{dist_url}）—— 与 docs/12 §1.1 确定性构建冲突（判据 F）")

    # -------------------------------------------------------------------
    # ④ docs/12 §3.1 表与脚本 EXPECTED 的一致性
    # -------------------------------------------------------------------
    # 只做「关键项在表里出现」的轻量核对：表里必须逐条列出这些锁定值。
    for label, val in (("NDK", EXPECTED["ndk"]), ("AGP", EXPECTED["agp"]),
                       ("Gradle", EXPECTED["gradle"])):
        if val not in d12:
            rep.fail(f"docs/12 §3.1 表里未出现 {label} 锁定值 {val} —— "
                     "文档与脚本 EXPECTED 已漂移，二者必须同源")

    # -------------------------------------------------------------------
    # 汇总比对
    # -------------------------------------------------------------------
    pairs: list[tuple[str, str | None, str]] = [
        ("NDK (ext.ndkRequired)", ext_ndk, EXPECTED["ndk"]),
        ("NDK (app.ndkVersion)", app_ndk, EXPECTED["ndk"]),
        ("AGP", ext_agp, EXPECTED["agp"]),
        ("Kotlin", ext_kotlin, EXPECTED["kotlin"]),
        ("Gradle (wrapper)", gradle_ver, EXPECTED["gradle"]),
        ("CMake (SDK 内安装版)", app_cmake, EXPECTED["cmake"]),
        ("compileSdk (ext)", ext_compile, EXPECTED["compile_sdk"]),
        ("compileSdk (app)", app_compile, EXPECTED["compile_sdk"]),
        ("minSdk (ext)", ext_min, EXPECTED["min_sdk"]),
        ("minSdk (app)", app_min, EXPECTED["min_sdk"]),
        ("targetSdk (ext)", ext_target, EXPECTED["target_sdk"]),
        ("targetSdk (app)", app_target, EXPECTED["target_sdk"]),
    ]
    for name, got, want in pairs:
        if got is None:
            continue          # find_one 已登记失败
        if got != want:
            rep.fail(f"{name}：实际 '{got}' != docs/12 §3.1 锁定值 '{want}'（判据 A）")
        else:
            rep.note(f"{name} = {got} ✓")

    # 判据 B：同一事实的两个副本必须一致
    if ext_ndk is not None and app_ndk is not None and ext_ndk != app_ndk:
        rep.fail(f"NDK 版本两处不一致：ext.ndkRequired='{ext_ndk}' vs "
                 f"app.ndkVersion='{app_ndk}'（判据 B）—— app/build.gradle 的"
                 "GradleException 断言会在配置期抛出，但本地不跑 Gradle 看不见")

    # 判据 E：NDK 不得低于 r28
    for label, v in (("ext.ndkRequired", ext_ndk), ("app.ndkVersion", app_ndk)):
        if v and ver_tuple(v) < ver_tuple(NDK_MIN):
            rep.fail(f"{label}='{v}' 低于 r28 下限 {NDK_MIN}（判据 E）—— "
                     "16 KB 页对齐将失效（docs/12 §3.1/§3.5）")

    # 判据 E2：AGP ≥ 8.5.1 是 16 KB 产物支持的门槛
    if ext_agp and ver_tuple(ext_agp) < ver_tuple("8.5.1"):
        rep.fail(f"AGP='{ext_agp}' 低于 8.5.1 下限（判据 E2）—— "
                 "不支持 16 KB 页大小构建产物（docs/12 §3.1）")

    # -------------------------------------------------------------------
    # 输出
    # -------------------------------------------------------------------
    for n in rep.notes:
        print(f"说明  {n}")

    if rep.fails:
        print("")
        for f in rep.fails:
            print(f"::error::{f}")
        print(f"\n[toolchain] FAIL：{len(rep.fails)} 项工具链版本不一致")
        return 3

    print(f"\n[toolchain] PASS：Android 工具链版本一致"
          f"（NDK {EXPECTED['ndk']} / AGP {EXPECTED['agp']} / "
          f"Gradle {EXPECTED['gradle']} / Kotlin {EXPECTED['kotlin']} / "
          f"CMake {EXPECTED['cmake']}）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
