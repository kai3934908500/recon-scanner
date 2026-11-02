#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_abi_drift_negative.py — check 类 ABI 漂移闸门（tools/abi_drift_check.py）的负向注入自检。

铁律：**每条闸门判据必须能被负向注入证伪**。否则它可能是个恒真的假闸门
（本项目已反复踩过的坑：判据恒真 → 永远报 PASS → 出事时毫无提示）。

本脚本对 `tools/abi_drift_check.py` 的每条判据各注入一个**真实缺陷**，
断言闸门**必须拒收**（非零退出），然后还原。

注入清单（一个判据至少一项）
----------------------------
  锚点表格行    改 docs/01 的「函数总数 | **37** | **23**」→ 期望：检查 2 触发
  锚点结构体数  改 docs/01 的「真实头文件定义 **12 个**」→ 期望：检查 3b 触发
  散文函数数    改 docs/01 的「（23 函数）」→ 期望：检查 2b 触发（缺口 AQ）
  散文 only-real 改 docs/01 的「only-real 9」→ 期望：检查 2b 触发
  散文锚点复述  改 docs/01 的「声明的 23/37/交集 14/独占 23」→ 期望：检查 2b 触发
  散文失锚      删 docs/01 的「only-real N」整段表述 → 期望：反空约束触发
  新增函数      往头文件 fuzz 一个新 `recon_*` 函数 → 期望：检查 1 触发
  删除函数      删头文件里一个已有函数 → 期望：检查 1（基线消失）触发
  新增错误码    往头文件加一个新 `RECON_ERR_*` → 期望：检查 4 触发

用法：python tools/verify_abi_drift_negative.py [--root .]
退出码：0 = 全部被正确拒收；3 = 有注入未被拒收（闸门是假的）
"""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
GATE = HERE / "abi_drift_check.py"
# 仓库根 = 本脚本所在目录的父目录。**不能依赖 cwd** —— ctest 的工作目录是
# build_cmake/，用相对 "." 会让闸门「本地手跑绿、ctest 里找不到头文件」
# （2026-11-02 ctest 实测踩到；LESSONS §C 已记过同类教训）。
DEFAULT_ROOT = HERE.parent


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
                           f" —— 说明注入本身失效，不是闸门有效")
        mutated = original.replace(self.old, self.new, 1)
        try:
            target.write_text(mutated, encoding="utf-8")
            proc = subprocess.run(
                [sys.executable, str(GATE), "--root", str(root)],
                capture_output=True, text=True, encoding="utf-8", errors="replace")
        finally:
            target.write_text(original, encoding="utf-8")

        if proc.returncode == 0:
            return False, "闸门未拒收（退出码 0）→ 假闸门"
        if self.expect_mention:
            blob = (proc.stdout or "") + (proc.stderr or "")
            if not any(k in blob for k in self.expect_mention):
                return False, (f"已拒收但**报错文案不含期望关键词** {self.expect_mention}；"
                               f"实际输出：{blob.strip()[:200]}")
        return True, f"已拒收（rc={proc.returncode}）"


def injections() -> list[Inject]:
    doc01 = "docs/01_接口契约.md"
    header = "core/include/recon/recon_c_api.h"
    return [
        # --- 检查 2：锚点表格行 -------------------------------------------------
        Inject("anchor-func-table", doc01,
               "| 函数总数 | **37** | **23** |",
               "| 函数总数 | **37** | **99** |",
               expect_mention=("锚点声明的真实头文件函数总数",)),

        # --- 检查 3b：锚点结构体数 ---------------------------------------------
        Inject("anchor-struct-total", doc01,
               "真实头文件定义 **12 个** `recon_*` 结构体",
               "真实头文件定义 **99 个** `recon_*` 结构体",
               expect_mention=("锚点声明的真实头文件结构体总数",)),

        # --- 检查 2b：散文计数（缺口 AQ） --------------------------------------
        Inject("prose-func-paren", doc01,
               "> **裁定：以实际头文件（23 函数）为准。**",
               "> **裁定：以实际头文件（99 函数）为准。**",
               expect_mention=("散文",)),

        Inject("prose-only-real", doc01,
               "**分歧部分（only-doc 23 与 only-real 9）没有任何下游消费者**",
               "**分歧部分（only-doc 23 与 only-real 88）没有任何下游消费者**",
               expect_mention=("散文",)),

        Inject("prose-anchor-repeat", doc01,
               "本文锚点声明的 23/37/交集 14/独占 23 与实测不符",
               "本文锚点声明的 99/37/交集 14/独占 23 与实测不符",
               expect_mention=("散文",)),

        # --- 检查 2b 的反空约束：删掉一整类表述 → 必须报「失锚」而非静默通过 ---
        Inject("prose-unanchor-only-real", doc01,
               "**分歧部分（only-doc 23 与 only-real 9）没有任何下游消费者**",
               "**分歧部分更详细的分项见下表。**",
               expect_mention=("未能取到这些类别",)),

        # --- 检查 1：函数集合漂移 ----------------------------------------------
        Inject("new-func", header,
               "RECON_API recon_status recon_get_preview_mesh(",
               "RECON_API recon_status recon_fuzz_new_thing(\n"
               "    void* context, uint32_t x, recon_mesh_info* out);\n"
               "RECON_API recon_status recon_get_preview_mesh(",
               expect_mention=("新函数",)),

        # --- 检查 4：错误码漂移 ------------------------------------------------
        Inject("new-error-code", header,
               "RECON_OK = 0,",
               "RECON_OK = 0,\nRECON_ERR_FUZZ_NEW = 9999,",
               expect_mention=("错误码",)),
    ]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="ABI 漂移闸门负向注入自检")
    ap.add_argument("--root", default=str(DEFAULT_ROOT),
                    help="仓库根目录（默认 = 本脚本父目录，不依赖 cwd）")
    args = ap.parse_args(argv)
    root = pathlib.Path(args.root).resolve()

    # 先确认基线：干净状态下闸门必须 PASS（否则负向注入无意义）。
    base = subprocess.run([sys.executable, str(GATE), "--root", str(root)],
                          capture_output=True, text=True, encoding="utf-8",
                          errors="replace")
    if base.returncode != 0:
        print("[negative] 基线状态下闸门已失败 —— 请先修好闸门再跑负向注入",
              file=sys.stderr)
        print((base.stdout or "") + (base.stderr or ""), file=sys.stderr)
        return 3

    cases = injections()
    failed: list[str] = []
    for i, inj in enumerate(cases, 1):
        ok, msg = inj.run(root)
        mark = "✓" if ok else "✗"
        print(f"  [{i}] {mark} {inj.name}  —— {msg}")
        if not ok:
            failed.append(inj.name)

    # 还原后闸门必须重新 PASS（证明注入未污染工作区）。
    back = subprocess.run([sys.executable, str(GATE), "--root", str(root)],
                          capture_output=True, text=True, encoding="utf-8",
                          errors="replace")
    if back.returncode != 0:
        print("[negative] 还原后闸门仍失败 —— 注入未正确还原！", file=sys.stderr)
        return 3
    print("\n[negative] 还原后闸门重新 PASS ✓")

    if failed:
        print(f"\n[negative] FAIL：{len(failed)} 项注入未被拒收 {failed}",
              file=sys.stderr)
        return 3
    print(f"\n[negative] PASS：{len(cases)} 项注入全部被正确拒收"
          f"（检查 1 / 2 / 2b / 3b / 4 均有效）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
