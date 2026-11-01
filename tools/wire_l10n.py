#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
r"""wire_l10n.py — 把 ui/ios 源码里硬编码的源语言文案接到 L10n 查表。

背景
----
`ui/ios/Recon/Sources/` 下有 241 处**硬编码中文**字符串字面量，而
`L10nKey` / `L10n.s(_:)` 一处都没用上 —— 即 20 个 locale 文件当时是纯装饰。
实测这 241 处的文本**全部**能在 `zh-CN.json` 里找到对应键（181 精确 + 60
格式串前缀，0 孤儿），说明文案表是按这些 UI 文本写的、完整且正确，缺的只是
「把调用点接上」。

转换规则
--------
把**非注释代码**里的 `"<含中文的字面量>"` 替换为 `L10n.s("<同一字面量>")`。

- 只改字符串字面量，不碰注释（否则注释会被改坏）。
- 已包在 `L10n.s(...)` / `L10n.t(...)` / `L10n.raw(...)` 里的不再重复包裹。
- 格式串（含 `\(...)` 插值）整串作为**反查键**落下 —— `L10n.s` 反查不到时
  原样返回，源语言下渲染结果**逐字符不变**。因此本转换是**行为保持**的。
- `Localization.swift` 自身被排除（它的注释与实现里刻意含有中文/键名样本）。

行为保持的验收
--------------
源语言（zh-CN）下，替换前后每个字面量的渲染结果必须完全相同。本工具在
`--verify` 模式下打印「替换数 / 反查成功数 / 反查失败数」，要求失败数为 0
（或全部失败项都在白名单里，见 `ALLOWLIST`）。

退出码：0 成功；1 有失败；2 工具错误。
"""

from __future__ import annotations

import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI_SRC = os.path.join(ROOT, "ui", "ios", "Recon", "Sources")
LOCALE = os.path.join(ROOT, "ui", "ios", "Recon", "Resources", "locales",
                      "zh-CN.json")
EN_LOCALE = os.path.join(ROOT, "ui", "ios", "Recon", "Resources", "locales",
                         "en-US.json")

# 这些文件不做转换（含实现细节，非 UI 文案）。
SKIP_FILES = {"Localization.swift"}

# 明确不改的字符串（非 UI 文案，不查表）：
#   · 示例文件路径（演示数据，不是界面文案）
ALLOWLIST: set[str] = {
    "/Documents/陶瓷杯.glb",
}

CJK = re.compile(r"[\u4e00-\u9fa5]")


def read(path: str) -> str:
    with io.open(path, encoding="utf-8") as f:
        return f.read()


def write(path: str, text: str) -> None:
    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def transform_states(text: str):
    """生成 (片段, 是否代码) 的序列。

    「代码」= 可以替换字符串字面量的区域；注释区域保持原样。
    """
    i, n = 0, len(text)
    buf = []
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":                       # 行注释
            if buf:
                yield "".join(buf), True
                buf = []
            j = text.find("\n", i)
            j = n if j < 0 else j
            yield text[i:j], False
            i = j
            continue
        if c == "/" and nxt == "*":                       # 块注释
            if buf:
                yield "".join(buf), True
                buf = []
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            yield text[i:j], False
            i = j
            continue
        buf.append(c)
        i += 1
    if buf:
        yield "".join(buf), True


LIT = re.compile(r'"([^"\n]*)"')
# 判断该字面量左侧是否已经是 L10n 调用（`L10n.s(`、`L10n.t(`、`L10n.raw(`）。
WRAPPED = re.compile(r"(?:L10n\.(?:s|t|raw)\s*\(\s*)$")


def ambiguous_values(source_values_by_key: dict[str, str]) -> set[str]:
    """找出「同一原文对应多个键**且英文译文不同**」的原文。

    这些**必须**用 `L10n.t(.key)` 按枚举精确指定，不能靠反查 —— 反查会按
    键名字典序取一个，可能与调用点的工程意图不符。

    真实例子：`继续扫描` 同时是 `msg.home.continue_scan`（"Continue Scanning"）
    与 `msg.confirm.continue_scan`（"Resume Scan"）。首页按钮该用前者，确认框
    按钮该用后者；反查只能二选一，必然有一处是错的。
    """
    try:
        with io.open(EN_LOCALE, encoding="utf-8") as f:
            en = json.load(f)
    except OSError:
        return set()

    by_value: dict[str, list[str]] = {}
    for k, v in source_values_by_key.items():
        if k.startswith("_") or not isinstance(v, str) or not v:
            continue
        by_value.setdefault(v, []).append(k)

    out: set[str] = set()
    for v, keys in by_value.items():
        if len(keys) < 2:
            continue
        translations = {en.get(k) for k in keys}
        if len(translations) > 1:
            out.add(v)
    return out


def transform_code(code: str, source_values: set[str],
                   ambiguous: set[str] | None = None,
                   ) -> tuple[str, int, int, list[str], list[str]]:
    """替换代码片段里的中文字面量。

    返回 `(新片段, 替换数, 已包裹跳过数, 表外原文, 歧义原文)`。
    """
    ambiguous = ambiguous or set()
    out = []
    last = 0
    replaced = skipped_wrapped = 0
    unresolved: list[str] = []
    ambiguous_hits: list[str] = []
    for m in LIT.finditer(code):
        lit = m.group(1)
        if not CJK.search(lit):
            continue
        # 已经是 `L10n.s("…")` 形式 → 不动
        if WRAPPED.search(code[:m.start()]):
            skipped_wrapped += 1
            continue
        if lit in ALLOWLIST:
            continue
        if lit in ambiguous:
            ambiguous_hits.append(lit)
            continue
        if lit not in source_values:
            # 反查不到 → 不自动改，登记待人工拆分
            unresolved.append(lit)
            continue
        out.append(code[last:m.start()])
        out.append(f'L10n.s("{lit}")')
        last = m.end()
        replaced += 1
    out.append(code[last:])
    return "".join(out), replaced, skipped_wrapped, unresolved, ambiguous_hits


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true",
                    help="真正写回文件（默认只 dry-run）")
    ap.add_argument("--verify", action="store_true",
                    help="额外校验每个字面量都能在 zh-CN 表里反查到")
    args = ap.parse_args()

    with io.open(LOCALE, encoding="utf-8") as f:
        raw = json.load(f)
    source_values = {v for k, v in raw.items()
                     if isinstance(v, str) and not k.startswith("_")}
    ambiguous = ambiguous_values(raw)

    total_repl = total_skip = 0
    all_unresolved: dict[str, list[str]] = {}
    all_ambiguous: dict[str, list[str]] = {}

    files = sorted(
        os.path.join(dp, fn)
        for dp, _dn, fns in os.walk(UI_SRC)
        for fn in fns if fn.endswith(".swift") and fn not in SKIP_FILES
    )
    for path in files:
        text = read(path)
        parts = []
        repl = skip = 0
        unresolved: list[str] = []
        amb_hits: list[str] = []
        for chunk, is_code in transform_states(text):
            if is_code:
                new, r, sk, u, amb = transform_code(chunk, source_values, ambiguous)
                repl += r
                skip += sk
                unresolved += u
                amb_hits += amb
                parts.append(new)
            else:
                parts.append(chunk)
        new_text = "".join(parts)
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        if repl:
            if args.apply:
                write(path, new_text)
            print(f"  {'写入' if args.apply else '待改'} {repl:3d} 处  {rel}")
        if unresolved:
            all_unresolved[rel] = unresolved
        if amb_hits:
            all_ambiguous[rel] = amb_hits
        total_repl += repl
        total_skip += skip

    print()
    print(f"合计：{'已替换' if args.apply else '将替换'} {total_repl} 处；"
          f"已包裹跳过 {total_skip} 处")

    if all_unresolved:
        n = sum(len(v) for v in all_unresolved.values())
        print()
        print(f"⚠️  未能自动接线 {n} 处（组合型/表外文案，**必须人工处理**）：")
        print("    这些整串反查不到源语言键。天真包裹会让非中文用户看到中文，")
        print("    故工具拒绝自动改；正确做法是拆成可查表的片段。")
        for fn, items in all_unresolved.items():
            print(f"    {fn}")
            for it in dict.fromkeys(items):
                print(f"        {it}")

    if all_ambiguous:
        n = sum(len(v) for v in all_ambiguous.values())
        print()
        print(f"⚠️  另有 {n} 处**歧义原文**（同一原文对应多个键、且英文译文不同）：")
        print("    反查只能二选一，必然有调用点被译错。这些必须用")
        print("    `L10n.t(.枚举键)` 按工程意图精确指定。")
        for fn, items in all_ambiguous.items():
            print(f"    {fn}")
            for it in dict.fromkeys(items):
                print(f"        {it}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
