#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_toolchain_negative.py — check_toolchain.py 的负向注入自检。

铁律：**每条闸门判据必须能被负向注入证伪**。否则它可能是个恒真的假闸门
（这是本项目已反复踩过的坑：判据恒真 → 永远报 PASS → 出事时毫无提示）。

本脚本对 `tools/check_toolchain.py` 的每条判据各注入一个**真实缺陷**，
断言闸门**必须拒收**（非零退出），然后还原。

注入清单（一个判据至少一项）
----------------------------
  A-ndk      改 app.ndkVersion         → 期望：判据 A / B / E 之一触发
  A-agp      改 ext.agpRequired        → 期望：判据 A 触发
  A-kotlin   改 ext.kotlinRequired     → 期望：判据 A 触发
  A-cmake    改 app 的 cmake version   → 期望：判据 A 触发
  A-sdk      改 app 的 compileSdk      → 期望：判据 A 触发
  B-dup      令 ext 与 app 的 NDK 互异 → 期望：判据 B 触发（A 也会顺带触发）
  C-literal  plugins 块的 AGP 写成字面量 → 期望：判据 C 触发
  D-wrapper  改 wrapper 的 gradle 版本   → 期望：判据 A（Gradle 行）触发
  E-oldndk   把 NDK 降到 r27 值        → 期望：判据 E 触发
  E2-oldagp  把 AGP 降到 8.4.0         → 期望：判据 E2 触发
  F-floating wrapper 改成 gradle-latest  → 期望：判据 F 触发
  Z-empty    把 ndkRequired 整行删掉   → 期望：反空约束触发（判据失效即失败）

用法：python tools/verify_toolchain_negative.py [--root .]
退出码：0 = 全部被正确拒收；3 = 有注入未被拒收（闸门是假的）
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
GATE = HERE / "check_toolchain.py"


class Inject:
    """一次「改文件 → 跑闸门 → 还原」的注入实验。"""

    def __init__(self, name: str, rel: str, old: str, new: str,
                 expect_mention: tuple[str, ...] = ()) -> None:
        self.name = name
        self.rel = rel
        self.old = old
        self.new = new
        self.expect_mention = expect_mention

    def run(self, root: pathlib.Path) -> tuple[bool, str]:
        target = root / self.rel
        if not target.is_file():
            return False, f"锚点文件不存在：{self.rel}"
        original = target.read_text(encoding="utf-8")
        if self.old not in original:
            return False, (f"**注入锚点未命中**（{self.rel} 里找不到 {self.old!r}）"
                           f"—— 注入失效，本次实验无意义，判为失败")
        if original.count(self.old) != 1:
            return False, (f"注入锚点不唯一（出现 {original.count(self.old)} 次）："
                           f"{self.old!r}")
        try:
            target.write_text(original.replace(self.old, self.new), encoding="utf-8")
            proc = subprocess.run(
                [sys.executable, str(GATE), "--root", str(root)],
                capture_output=True, text=True, timeout=120)
            out = (proc.stdout or "") + (proc.stderr or "")
            if proc.returncode == 0:
                return False, f"闸门**未拒收**（rc=0）—— 该判据是假闸门！\n{out[-800:]}"
            if self.expect_mention:
                hit = [k for k in self.expect_mention if k in out]
                if not hit:
                    return False, (f"闸门拒收了（rc={proc.returncode}）但**未提到**"
                                   f"预期判据 {self.expect_mention}，"
                                   f"可能因别的判据偶发触发：\n{out[-800:]}")
                return True, f"已拒收（rc={proc.returncode}，命中 {hit}）"
            return True, f"已拒收（rc={proc.returncode}）"
        finally:
            target.write_text(original, encoding="utf-8")


INJECTS = [
    Inject("A-ndk", "platform/android/app/build.gradle",
           "ndkVersion '28.1.13356709'", "ndkVersion '28.2.13676358'",
           ("判据", "NDK")),
    Inject("A-agp", "platform/android/build.gradle",
           "agpRequired = '8.7.3'", "agpRequired = '8.6.0'", ("AGP",)),
    Inject("A-kotlin", "platform/android/build.gradle",
           "kotlinRequired = '2.0.21'", "kotlinRequired = '2.0.0'", ("Kotlin",)),
    Inject("A-cmake", "platform/android/app/build.gradle",
           "version '3.31.6'", "version '3.22.1'", ("CMake",)),
    Inject("A-sdk", "platform/android/app/build.gradle",
           "compileSdk 35", "compileSdk 36", ("compileSdk",)),
    Inject("B-dup", "platform/android/app/build.gradle",
           "ndkVersion '28.1.13356709'", "ndkVersion '28.0.13004108'", ("判据 B",)),
    Inject("C-literal", "platform/android/build.gradle",
           "id 'com.android.application' version \"${agpRequired}\" apply false",
           "id 'com.android.application' version '8.7.3' apply false",
           ("判据 C",)),
    Inject("D-wrapper", "platform/android/gradle/wrapper/gradle-wrapper.properties",
           "gradle-8.9-bin.zip", "gradle-8.7-bin.zip", ("Gradle",)),
    Inject("E-oldndk", "platform/android/build.gradle",
           "ndkRequired = '28.1.13356709'", "ndkRequired = '27.3.13750724'",
           ("判据 E",)),
    Inject("E2-oldagp", "platform/android/build.gradle",
           "agpRequired = '8.7.3'", "agpRequired = '8.4.0'", ("判据 E2",)),
    Inject("F-floating", "platform/android/gradle/wrapper/gradle-wrapper.properties",
           "gradle-8.9-bin.zip", "gradle-latest-bin.zip", ("判据 F",)),
    Inject("Z-empty", "platform/android/build.gradle",
           "ndkRequired = '28.1.13356709'", "_removed_by_negative_test_ = 0",
           ("反空约束",)),
]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".", help="仓库根目录")
    args = ap.parse_args(argv)

    root = pathlib.Path(args.root).resolve()

    # 基线：未注入时闸门必须 PASS
    base = subprocess.run([sys.executable, str(GATE), "--root", str(root)],
                          capture_output=True, text=True, timeout=120)
    if base.returncode != 0:
        print("[negative] 基线失败：未注入缺陷时闸门就不 PASS，先修闸门")
        print((base.stdout or "") + (base.stderr or ""))
        return 3
    print("[negative] 基线：闸门 PASS（未注入缺陷）✓\n")

    # 用临时副本做注入，避免任何意外写坏工作区（双保险：Inject 自身也还原）
    n_bad = 0
    with tempfile.TemporaryDirectory(prefix="recon-tc-neg-") as td:
        work = pathlib.Path(td) / "repo"
        # ⚠️ 忽略规则必须**精确**，不能用 `build*`（本脚本第一版的真实自伤）：
        #    `shutil.ignore_patterns` 是 fnmatch 匹配**每一项的名字**，而
        #    `platform/android/build.gradle` 的 basename 恰好是 `build.gradle`
        #    —— 被 `build*` 一并排除掉了。结果是临时副本里根本没有
        #    build.gradle，12 项注入全部以"锚点文件不存在"告败，
        #    看起来像"闸门坏了"，实际是**注入器自己**把被注入的文件删了。
        #    这正是 MEMORY.md「黑名单/白名单是手工清单、必然漂移」的又一实例。
        shutil.copytree(root, work, ignore=shutil.ignore_patterns(
            ".git", "build", "build_*", "build-*", "out", "dist", "third_party",
            ".workbuddy", "__pycache__", "*.spv", "*.o", "*.a"))
        for i, inj in enumerate(INJECTS, 1):
            ok, msg = inj.run(work)
            mark = "✓" if ok else "✗"
            print(f"  [{i:02d}] {mark} {inj.name:12s} —— {msg}")
            if not ok:
                n_bad += 1

        # 还原后（临时目录已被 copytree 的每轮 finally 还原）再验一次基线
        post = subprocess.run([sys.executable, str(GATE), "--root", str(work)],
                              capture_output=True, text=True, timeout=120)
        if post.returncode == 0:
            print("\n[negative] 还原后闸门重新 PASS ✓")
        else:
            print("\n[negative] 还原后闸门**仍失败** —— 注入还原不完整")
            n_bad += 1

    if n_bad:
        print(f"\n[negative] FAIL：{n_bad} / {len(INJECTS)} 项注入未被正确拒收")
        return 3
    print(f"\n[negative] PASS：{len(INJECTS)} 项注入全部被正确拒收"
          f"（check_toolchain.py 的每条判据均有效）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
