#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""
check_metallib_symbols.py —— 校验 default.metallib 覆盖 gpu_algo.h 声明的算法函数。

契约：docs/12_构建与打包.md §4.4（Metal 着色器编译与嵌入）、docs/00 §3.2
      （算法同源、两端分别编译）。

为什么要有本工具（而不是在 shell 里写几行）：
  同一判定有**两个消费点** ——
    ① tools/build_metal_shaders.sh 的步骤 3b（构建时自检：刚编出的库有没有漏函数）
    ② .github/workflows/ios.yml 的「校验 default.metallib」（CI 产物层独立校验）
  两份实现必然漂移（MEMORY.md 铁律：同一逻辑不写两遍）。故收敛到本工具，
  两个消费点都调它。

  ⚠️ 反面教材（本项目真实事故，勿重犯）：在 `set -euo pipefail` 下写
     `strings "$MB" | grep -qx "$fn"` **必然误判**。机理：grep -q 命中即退出并
     关闭读端 → 写端 strings 收到 SIGPIPE 退出 141 → pipefail 令整条管道非零
     → `if !` 成立 → 报告「找不到函数」，即使它就在输出的第一行。
     产物小于管道缓冲（64 KB）时侥幸不触发，metallib 一变大便必然炸。
     本工具改为「读文件 → 在内存里精确比对」，不经过管道，也不依赖系统
     strings 的默认最小长度（那会随平台变化）。

判据（两条都必须成立）：
  M-1  gpu_algo.h 的 RECON_GPU_ALGO(名字) 声明集合**非空**且能被解析出来。
       （解析式失效若被当作「没什么可查」= 静默通过，属最坏一类闸门缺陷，
         故显式失败。）
  M-2  decl 集合 ⊆ metallib 中的字符串集合（**精确**匹配，不匹配前缀）。

用法：
  python3 tools/check_metallib_symbols.py --metallib <path> [--repo-root <dir>]

退出码：0 = 全部通过；1 = 失败（含 M-1 解析失效）。
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# 《造一个只含可打印 ASCII 的串集合》所需的最小长度。
# `strings` 的默认最小长度就是 4；保持一致以免与既有闸门语义漂移。
MIN_STR_LEN = 4
_PRINTABLE = re.compile(rb"[\x20-\x7e]{%d,}" % MIN_STR_LEN)

# gpu_algo.h 里 RECON_GPU_ALGO 的**宏定义行**必须排除：
#   `#define RECON_GPU_ALGO(function_name) ...` 会被朴素正则当成一次声明。
#   脚本原实现用 re.sub 去掉 `^\s*#\s*define[^\n]*RECON_GPU_ALGO[^\n]*$`，
#   这里沿用同一策略（同一判据的同一处理方式）。
_DEFINE_LINE = re.compile(r"^\s*#\s*define[^\n]*RECON_GPU_ALGO[^\n]*$", re.M)
_DECL = re.compile(r"RECON_GPU_ALGO\(\s*(\w+)\s*\)\s*;")


def parse_declared_algorithms(repo_root: str) -> list[str]:
    """从 shaders/common/gpu_algo.h 解析声明的算法函数名（有序去重）。

    头文件缺失 → 抛 FileNotFoundError，由 main 转成清晰的闸门失败
    （不吞异常：少一个事实来源就是闸门失效，必须说出来）。
    """
    path = os.path.join(repo_root, "shaders", "common", "gpu_algo.h")
    if not os.path.isfile(path):
        raise FileNotFoundError(path)
    with open(path, encoding="utf-8") as f:
        text = f.read()
    body = _DEFINE_LINE.sub("", text)
    seen: dict[str, None] = {}
    for name in _DECL.findall(body):
        seen.setdefault(name, None)
    return list(seen)


def read_printable_strings(path: str) -> set[str]:
    """读出二进制文件里的可打印 ASCII 串集合（等价 strings -n 4，且不做精确长度校验）。"""
    with open(path, "rb") as f:
        data = f.read()
    out: set[str] = set()
    for m in _PRINTABLE.finditer(data):
        out.add(m.group(0).decode("ascii"))
    # `strings` 会把串按 NUL 等分隔切开；此处按字节切分已足够，
    # 但 metallib 里同名函数可能紧邻其它字符，故再补一次「按非可打印分隔」。
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="校验 default.metallib 覆盖 gpu_algo.h 声明的算法函数"
    )
    ap.add_argument("--metallib", required=True, help="default.metallib 路径")
    ap.add_argument(
        "--repo-root",
        default=".",
        help="仓库根（用于定位 shaders/common/gpu_algo.h），默认当前目录",
    )
    args = ap.parse_args(argv)

    if not os.path.isfile(args.metallib):
        print(f"[metal-symbols] FAIL：找不到产物 {args.metallib}")
        return 1
    if os.path.getsize(args.metallib) == 0:
        # 空产物会被 makeDefaultLibrary() 当成「库在但无函数」，比「找不到库」
        # 更难查 —— 故在这里就硬失败，不给它进入比对的机会。
        print(f"[metal-symbols] FAIL：产物为空 {args.metallib}")
        return 1

    try:
        declared = parse_declared_algorithms(args.repo_root)
    except FileNotFoundError as e:
        print(f"[metal-symbols] FAIL：找不到事实来源 {e}（--repo-root 是否正确？）")
        return 1
    # M-1：解析失效必须显式失败，不许静默当"无可查项"。
    if not declared:
        print(
            "[metal-symbols] FAIL：未能从 shaders/common/gpu_algo.h 解析出任何算法函数"
            "（提取式可能已失效）"
        )
        return 1

    present = read_printable_strings(args.metallib)

    missing = [n for n in declared if n not in present]
    if missing:
        print(f"[metal-symbols] FAIL：metallib 里找不到这些算法函数（共 {len(missing)} 个）：")
        for n in missing:
            print(f"    - {n}")
        print("  （源码可能写对了但未被编入；检查 build_metal_shaders.sh 的 SOURCES 收集）")
        found = sorted(x for x in present if x.startswith("recon_"))
        print(f"  产物中 recon_ 开头的串（{len(found)} 个）：")
        for n in found:
            print(f"    {n}")
        return 1

    print(f"[metal-symbols] OK：声明的 {len(declared)} 个算法函数在 metallib 中全部存在")
    return 0


if __name__ == "__main__":
    sys.exit(main())
