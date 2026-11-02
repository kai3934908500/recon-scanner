#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""verify_shell_pipefail_negative.py — 闸门 S 的负向注入（证明它非假闸门）。

================================================================================
为什么必须做负向注入
================================================================================
一条「永远不报错」的闸门在 CI 里看起来和「全部通过」完全一样 —— 本仓库把这类
东西统称**假闸门**，是最大的风险类别（见 LESSONS.md）。

故这里把**每一类真实事故**重新注入回代码，断言闸门确实拒收：

  S-1-a  android.yml 的 `{ yes || true; } | sdkmanager` 改回 `yes | sdkmanager`
         → 正是 CI #25 失败的那一行（build job 的 SDK 安装步骤）
  S-1-b  verify-16kb job 里的同一处（**两处都要注入**：只测一处不能证明
         闸门覆盖了另一处 —— 这正是「判据范围要跟上代码位置」的要求）
  S-2-a  `NM=$(find ... | head -1 || true)` 去掉 `|| true`
         → 真实事故形态；且该失败会**静默中止整个 run 块**
  S-2-b  `glslangValidator --version | head -3 || true` 去掉 `|| true`
         → 同一形态在不同 workflow（ios.yml）里的实例
  S-0    反空约束：把某个 workflow 暂时改名，使扫描面缩小
         → 闸门必须报「解析到的片段数不足」而不是静默通过

⚠️ 本脚本会**临时修改** `.github/workflows/*.yml` 再还原（try/finally 保证
   即使中途异常也还原）。

用法：python3 tools/verify_shell_pipefail_negative.py [--root .]
退出码：0 全部被正确拒收；1 有注入未被拒收（= 假闸门）。
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.dirname(HERE)  # ⚠️ 不依赖 cwd（ctest 的 cwd 是 build 目录）

GATE = os.path.join(HERE, "check_shell_pipefail.py")

IOS_YML = os.path.join(".github", "workflows", "ios.yml")
ANDROID_YML = os.path.join(".github", "workflows", "android.yml")

# (用例名, 说明, 期望输出关键词, [(相对路径, 原串, 替换串), ...])
INJECTIONS: list[tuple[str, str, str, list[tuple[str, str, str]]]] = [
    (
        "S-1-a-yes-sdkmanager-build-job",
        "把 build job 的 `{ yes || true; }` 改回 `yes |`（复现 CI #25）",
        "S-1",
        [(ANDROID_YML,
          "{ yes || true; } | sdkmanager --licenses > /dev/null\n"
          "          sdkmanager --install \\",
          "yes | sdkmanager --licenses > /dev/null\n"
          "          sdkmanager --install \\")],
    ),
    (
        "S-1-b-yes-sdkmanager-16kb-job",
        "把 verify-16kb job 的同一处改回 `yes |`（证明闸门覆盖了**另一**处）",
        "S-1",
        [(ANDROID_YML,
          "{ yes || true; } | sdkmanager --licenses > /dev/null\n"
          "          sdkmanager --install \"build-tools;35.0.0\"",
          "yes | sdkmanager --licenses > /dev/null\n"
          "          sdkmanager --install \"build-tools;35.0.0\"")],
    ),
    (
        "S-2-a-find-head-silent-abort",
        "去掉 `NM=$(find ... | head -1 || true)` 的 `|| true`"
        "（`set -e` 下会静默中止整个 run 块）",
        "S-2",
        [(ANDROID_YML,
          '| head -1 || true)',
          '| head -1)')],
    ),
    (
        "S-2-b-find-head-ios-app-dump",
        "去掉 ios.yml 里 `find build/app/Build/Products ... | head -20 || true` "
        "的守卫（同一形态在**另一个 workflow** 里的实例）",
        "S-2",
        [(IOS_YML,
          '| head -20 || true',
          '| head -20')],
    ),
]

# 反空约束注入：把 tools/*.sh 暂时搬走，看闸门是否报「片段数不足」。
EMPTY_CASE = (
    "S-0-anti-empty-shrunk-scan-surface",
    "把 tools/*.sh 暂时移走，使扫描面缩小 → 闸门必须报反空约束而非静默通过",
    "S0",
)


def slurp(p: str) -> str:
    with open(p, encoding="utf-8") as fh:
        return fh.read()


def run_gate(root: str) -> tuple[int, str]:
    proc = subprocess.run(
        [sys.executable, GATE, "--root", root],
        capture_output=True, text=True, cwd=root)
    return proc.returncode, proc.stdout + proc.stderr


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=DEFAULT_ROOT)
    args = ap.parse_args()
    root = os.path.abspath(args.root)

    if not os.path.isfile(GATE):
        print(f"[negative] 未找到闸门本体：{GATE}")
        return 1

    # 基线必须先通过 —— 否则「注入后失败」不能证明任何事。
    rc, out = run_gate(root)
    if rc != 0:
        print("[negative] 基线状态下闸门已失败，无法进行注入测试：")
        print(out[-2000:])
        return 1
    print("[negative] 基线：闸门 S 通过 ✓")

    fail = 0
    backups: list[tuple[str, str]] = []
    try:
        for name, desc, expect, edits in INJECTIONS:
            for rel, old, new in edits:
                path = os.path.join(root, rel)
                backups.append((path, slurp(path)))
                src = slurp(path)
                if src.count(old) < 1:
                    print(f"  ✗ {name} —— 注入锚点未命中（{rel}），"
                          f"说明**注入本身已失效**（不是闸门失效）")
                    fail = 1
                    break
                # 只替换**第一处**，避免把本来就要保留的另一处一起改掉。
                with open(path, "w", encoding="utf-8", newline="\n") as fh:
                    fh.write(src.replace(old, new, 1))
            else:
                rc2, out2 = run_gate(root)
                hit = expect in out2
                # 期望「闸门报错」且**报的正是预期的那一类**
                ok = (rc2 != 0) and hit
                mark = "✓" if ok else "✗"
                print(f"  {mark} {name} —— {'已拒收' if ok else '未被拒收'} "
                      f"(rc={rc2}, expect='{expect}' {'命中' if hit else '未命中'})"
                      f"  [{desc}]")
                if not ok:
                    fail = 1
                    print("      " + "\n      ".join(
                        out2.strip().splitlines()[-6:]))
            # 还原本案例的所有文件
            while backups:
                path, content = backups.pop()
                with open(path, "w", encoding="utf-8", newline="\n") as fh:
                    fh.write(content)

        # ---- 反空约束注入 -------------------------------------------------
        tools_dir = os.path.join(root, "tools")
        sh_files = [f for f in sorted(os.listdir(tools_dir)) if f.endswith(".sh")]
        stash = tempfile.mkdtemp(prefix="pipefail-neg-")
        moved: list[tuple[str, str]] = []
        try:
            for f in sh_files:
                src = os.path.join(tools_dir, f)
                dst = os.path.join(stash, f)
                shutil.move(src, dst)
                moved.append((src, dst))
            rc3, out3 = run_gate(root)
            name, desc, expect = EMPTY_CASE
            ok = (rc3 != 0) and (expect in out3)
            mark = "✓" if ok else "✗"
            print(f"  {mark} {name} —— {'已拒收' if ok else '未被拒收'} "
                  f"(rc={rc3}, expect='{expect}')  [{desc}]")
            if not ok:
                fail = 1
                print("      " + "\n      ".join(out3.strip().splitlines()[-6:]))
        finally:
            for src, dst in moved:
                shutil.move(dst, src)
            shutil.rmtree(stash, ignore_errors=True)

        # 最后再确认一次基线仍然通过（还原是否干净）
        rc4, out4 = run_gate(root)
        if rc4 != 0:
            print("[negative] 还原后基线失败 —— 注入脚本污染了工作区：")
            print(out4[-1500:])
            fail = 1
        else:
            print("[negative] 还原后基线：仍通过 ✓")
    finally:
        # 兜底还原（防止上面的 break / 异常留下脏文件）
        for path, content in backups:
            with open(path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(content)

    total = len(INJECTIONS) + 1
    if fail:
        print(f"\n::error::闸门 S 负向注入未全部通过（{total} 项）")
        return 1
    print(f"\n[negative] PASS：{total} 项注入全部被正确拒收"
          f"（闸门 S 的 S-1 / S-2 / S-0 均有效）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
