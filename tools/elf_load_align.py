#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""elf_load_align.py — 读 ELF 的 PT_LOAD 段 `p_align`，判断是否满足 16 KB 页对齐。

为什么单独成一个文件（而不是把 awk 塞进 shell）
------------------------------------------------
`docs/12_构建与打包.md` §3.5 给的示例用
    readelf -lW "$so" | awk '$1=="LOAD" {print $NF}'
取 `Align` 列。这条链有一个**结构性的假绿风险**：`readelf` 一旦不在 PATH、
或输出格式变了（GNU readelf / llvm-readelf 的列宽与字段名都不同），
`awk` 会匹配到 **0 行** → 循环体一次都不进 → `fail` 保持 0 → **闸门报通过**。
（本项目已多次踩到「提取正则零命中 → 判据退化成恒真」这一类。）

故本文件把「解析」与「判定」放在**同一处实现**，并强制两条反空约束：
  · 解析出的 LOAD 段数必须 ≥ 1，否则**失败**而不是跳过；
  · 读到的 p_align 不得为 0（LOAD 段的 align=0 意味着无对齐保证）。

可被证伪（`--self-test`）
------------------------
`--self-test` 现场构造 ELF64 文件（真实 ELF 头 + 真实 program header 布局），
把 p_align 分别写成 0x1000 / 0x4000 / 混合值，断言判定结果**恰好**符合预期。
⇒ 解析器与比较逻辑**不是恒真的**：把阈值判断写反、或只取第一个 LOAD 段、
或按字段宽度硬编码偏移，自检立刻失败。

与 `readelf` 的交叉验证
-----------------------
若 PATH 上存在 `llvm-readelf` 或 `readelf`，本工具会对同一文件**再算一遍**并
比对结果；不一致即失败（两种独立实现互证）。这让「手工解析的偏移写错」
这类错误无法通过。缺 readelf 时**降级为提示**（不是失败）—— 因为本工具
自足，readelf 只是佐证。

用法
----
  # 判据：这些 .so 的每个 LOAD 段 p_align 必须 >= 0x4000
  python tools/elf_load_align.py --min-align 0x4000 lib/*.so

  # 报告模式：只打印，不判定（用于 armeabi-v7a 的 4 KB 现状留档）
  python tools/elf_load_align.py --report lib/armeabi-v7a/*.so

  # 自检（造合成 ELF，证伪解析与判定）
  python tools/elf_load_align.py --self-test

退出码：0 通过；1 用法错误；3 判据不满足
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import struct
import subprocess
import sys

PT_LOAD = 1


class ElfError(Exception):
    pass


def parse_load_aligns(path: pathlib.Path) -> list[int]:
    """返回该 ELF 里**全部** PT_LOAD 段的 p_align（按出现顺序）。"""
    data = path.read_bytes()
    if len(data) < 64:
        raise ElfError(f"{path}: 文件小于 64 字节，不可能是 ELF")

    if data[0:4] != b"\x7fELF":
        raise ElfError(f"{path}: 魔数不是 \\x7fELF（这不是 ELF 文件）")

    ei_class = data[4]        # 1 = 32 位，2 = 64 位
    ei_data = data[5]         # 1 = 小端，2 = 大端

    if ei_data == 1:
        end = "<"
    elif ei_data == 2:
        end = ">"
    else:
        raise ElfError(f"{path}: EI_DATA={ei_data} 非法")

    if ei_class == 2:         # ELF64
        e_phoff = struct.unpack_from(end + "Q", data, 32)[0]
        e_phentsize = struct.unpack_from(end + "H", data, 54)[0]
        e_phnum = struct.unpack_from(end + "H", data, 56)[0]
        p_type_off, p_type_fmt = 0, end + "I"
        # p_align 在 ELF64 program header 的偏移 0x30（48）
        p_align_off, p_align_fmt = 48, end + "Q"
        ph_min = 56
    elif ei_class == 1:       # ELF32
        e_phoff = struct.unpack_from(end + "I", data, 28)[0]
        e_phentsize = struct.unpack_from(end + "H", data, 42)[0]
        e_phnum = struct.unpack_from(end + "H", data, 44)[0]
        p_type_off, p_type_fmt = 0, end + "I"
        # p_align 在 ELF32 program header 的偏移 0x1C（28）
        p_align_off, p_align_fmt = 28, end + "I"
        ph_min = 32
    else:
        raise ElfError(f"{path}: EI_CLASS={ei_class} 非法")

    if e_phentsize < ph_min:
        raise ElfError(f"{path}: e_phentsize={e_phentsize} < {ph_min}，"
                       f"program header 布局异常")
    if e_phnum == 0:
        raise ElfError(f"{path}: e_phnum=0 —— 没有 program header，"
                       f"对共享库而言不正常（解析可能已失效）")
    if e_phoff + e_phnum * e_phentsize > len(data):
        raise ElfError(f"{path}: program header 表越界"
                       f"（phoff={e_phoff} n={e_phnum} entsize={e_phentsize} "
                       f"filesz={len(data)}）")

    aligns: list[int] = []
    for i in range(e_phnum):
        base = e_phoff + i * e_phentsize
        p_type = struct.unpack_from(p_type_fmt, data, base + p_type_off)[0]
        if p_type != PT_LOAD:
            continue
        p_align = struct.unpack_from(p_align_fmt, data, base + p_align_off)[0]
        aligns.append(int(p_align))
    return aligns


def readelf_aligns(path: pathlib.Path) -> list[int] | None:
    """用外部 readelf 独立算一遍（交叉验证）。工具缺失返回 None。"""
    tool = None
    for cand in ("llvm-readelf", "readelf"):
        if shutil.which(cand):
            tool = cand
            break
    if tool is None:
        return None
    try:
        proc = subprocess.run([tool, "-lW", str(path)],
                              capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    if proc.returncode != 0:
        return None
    out: list[int] = []
    for line in proc.stdout.splitlines():
        f = line.split()
        if f and f[0] == "LOAD" and len(f) >= 2:
            try:
                out.append(int(f[-1], 16))
            except ValueError:
                continue
    return out or None


# ---------------------------------------------------------------------------
# 自检：构造合成 ELF，证伪解析与判定
# ---------------------------------------------------------------------------
def _make_elf64(aligns: list[int]) -> bytes:
    """构造一个最小但**布局真实**的 ELF64：ELF 头 + N 个 PT_LOAD program header。"""
    phnum = len(aligns)
    ehsize, phentsize = 64, 56
    phoff = ehsize
    total = phoff + phnum * phentsize
    buf = bytearray(b"\x00" * total)

    # e_ident
    buf[0:4] = b"\x7fELF"
    buf[4] = 2        # ELFCLASS64
    buf[5] = 1        # ELFDATA2LSB
    buf[6] = 1        # EV_CURRENT
    buf[7] = 0        # ELFOSABI_SYSV
    struct.pack_into("<H", buf, 16, 3)      # e_type = ET_DYN
    struct.pack_into("<H", buf, 18, 183)    # e_machine = EM_AARCH64
    struct.pack_into("<I", buf, 20, 1)      # e_version
    struct.pack_into("<Q", buf, 32, phoff)  # e_phoff
    struct.pack_into("<H", buf, 52, ehsize)  # e_ehsize
    struct.pack_into("<H", buf, 54, phentsize)  # e_phentsize
    struct.pack_into("<H", buf, 56, phnum)   # e_phnum

    for i, al in enumerate(aligns):
        base = phoff + i * phentsize
        struct.pack_into("<I", buf, base + 0, PT_LOAD)   # p_type
        struct.pack_into("<I", buf, base + 4, 5)         # p_flags = R|X
        struct.pack_into("<Q", buf, base + 8, 0)         # p_offset
        struct.pack_into("<Q", buf, base + 16, 0)        # p_vaddr
        struct.pack_into("<Q", buf, base + 24, 0)        # p_paddr
        struct.pack_into("<Q", buf, base + 32, 4096)     # p_filesz
        struct.pack_into("<Q", buf, base + 40, 4096)     # p_memsz
        struct.pack_into("<Q", buf, base + 48, al)       # p_align
    return bytes(buf)


def _judge(aligns: list[int], min_align: int) -> bool:
    """与 run() 里的判定**同一逻辑**（下方 run 也调它，避免两处实现漂移）。"""
    if not aligns:
        raise ElfError("未读到任何 LOAD 段 —— 判据会退化成恒真，必须失败")
    if any(a == 0 for a in aligns):
        return False
    return all(a >= min_align for a in aligns)


def self_test() -> int:
    import tempfile

    cases = [
        ("ok-16k",          [0x4000],            0x4000, True),
        ("ok-16k-multi",    [0x4000, 0x4000],    0x4000, True),
        ("bad-4k",          [0x1000],            0x4000, False),
        ("bad-second-load", [0x4000, 0x1000],    0x4000, False),  # 抓「只看第一个」
        ("bad-mixed32",     [0x1000, 0x4000],    0x4000, False),
        ("bad-zero-align",  [0x4000, 0x0],       0x4000, False),
        ("ok-larger",       [0x10000],           0x4000, True),
        ("ok-4k-mode",      [0x1000],            0x1000, True),
        ("bad-4k-mode",     [0x800],             0x1000, False),
    ]
    n_bad = 0
    with tempfile.TemporaryDirectory(prefix="recon-elfalign-") as td:
        for name, aligns, thresh, want_ok in cases:
            p = pathlib.Path(td) / f"{name}.so"
            p.write_bytes(_make_elf64(aligns))
            try:
                got_aligns = parse_load_aligns(p)
            except ElfError as e:
                print(f"  ✗ {name:16s} 解析失败：{e}")
                n_bad += 1
                continue
            if got_aligns != aligns:
                print(f"  ✗ {name:16s} 解析结果 {[hex(a) for a in got_aligns]} "
                      f"!= 写入值 {[hex(a) for a in aligns]}")
                n_bad += 1
                continue
            ok = _judge(got_aligns, thresh)
            mark = "✓" if ok == want_ok else "✗"
            if ok != want_ok:
                n_bad += 1
            print(f"  {mark} {name:16s} aligns={[hex(a) for a in aligns]} "
                  f"阈值={hex(thresh)} → {'通过' if ok else '失败'}"
                  f"（期望{'通过' if want_ok else '失败'}）")

        # 反空约束：空的 program header 表必须**失败**而不是"无 LOAD 即通过"
        p = pathlib.Path(td) / "no-ph.so"
        buf = bytearray(_make_elf64([0x4000]))
        struct.pack_into("<H", buf, 56, 0)      # e_phnum = 0
        p.write_bytes(bytes(buf))
        try:
            parse_load_aligns(p)
            print("  ✗ no-program-header  竟然解析成功（反空约束失效）")
            n_bad += 1
        except ElfError:
            print("  ✓ no-program-header  被正确拒绝（反空约束生效）")

        # 非 ELF 文件必须被拒绝（否则会把任意文件当"无 LOAD"放过）
        p = pathlib.Path(td) / "not-elf.bin"
        p.write_bytes(b"MZ" + b"\x00" * 200)
        try:
            parse_load_aligns(p)
            print("  ✗ not-elf            竟然解析成功（魔数校验失效）")
            n_bad += 1
        except ElfError:
            print("  ✓ not-elf            被正确拒绝（魔数校验生效）")

    if n_bad:
        print(f"\n[self-test] FAIL：{n_bad} 项不符合预期")
        return 3
    print(f"\n[self-test] PASS：{len(cases) + 2} 项全部符合预期"
          f"（含「混合对齐」「第二个 LOAD 掉到 4 KB」两个关键反例）")
    return 0


def run(files: list[str], min_align: int, report_only: bool,
        cross_check: bool) -> int:
    if not files:
        print("::error::未指定任何 .so 文件 —— 判据没有目标对象", file=sys.stderr)
        return 1

    misses: list[str] = []
    n_checked = 0
    fails = 0
    for f in files:
        p = pathlib.Path(f)
        if not p.is_file():
            misses.append(f)
            continue
        try:
            aligns = parse_load_aligns(p)
            if not aligns:
                raise ElfError("未读到任何 LOAD 段")
        except ElfError as e:
            print(f"::error::{e}")
            fails += 1
            continue
        n_checked += 1

        pretty = ", ".join(hex(a) for a in aligns)

        if cross_check:
            alt = readelf_aligns(p)
            if alt is None:
                print(f"提示  {p.name}: 无 readelf 可交叉验证（本工具的解析自足，"
                      f"readelf 仅作佐证）")
            elif alt != aligns:
                print(f"::error::{p.name}: 本工具解析 {pretty} 与 readelf 输出 "
                      f"{', '.join(hex(a) for a in alt)} 不一致 —— 至少一侧的"
                      f"偏移/格式理解有误")
                fails += 1
                continue
            else:
                print(f"说明  {p.name}: 与 readelf 交叉验证一致（{pretty}）")

        if report_only:
            print(f"报告  {p.name}: LOAD p_align = {pretty}")
            continue

        ok = _judge(aligns, min_align)
        if ok:
            print(f"说明  {p.name}: ✅ LOAD p_align = {pretty}（>= {hex(min_align)}）")
        else:
            print(f"::error::{p.name}: ❌ LOAD p_align = {pretty}，"
                  f"存在 < {hex(min_align)} 的段（16 KB 页对齐不满足，"
                  f"docs/12 §3.5）")
            fails += 1

    if misses:
        # 缺文件**不是**静默通过：调用方本应给出确定存在的路径
        print(f"::error::以下文件不存在：{misses} —— "
              f"判据的目标对象缺失（构建未跑？路径写错？）")
        fails += 1

    if n_checked == 0:
        print("::error::一个 .so 都没能成功检查 —— 判据已失效（反空约束）")
        return 3

    print(f"[elf-align] 检查 {n_checked} 个文件，失败 {fails} 个")
    return 3 if fails else 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="ELF LOAD 段 16 KB 页对齐检查")
    ap.add_argument("files", nargs="*", help="待检查的 .so 路径")
    ap.add_argument("--min-align", default="0x4000",
                    help="要求的最小 p_align（默认 0x4000 = 16 KB）")
    ap.add_argument("--report", action="store_true",
                    help="只报告不判定（用于 armeabi-v7a 的现状留档）")
    ap.add_argument("--no-cross-check", action="store_true",
                    help="不与 readelf 交叉验证")
    ap.add_argument("--self-test", action="store_true", help="自检（证伪解析与判定）")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    try:
        min_align = int(args.min_align, 0)
    except ValueError:
        print(f"用法错误：--min-align 无法解析为整数：{args.min_align}",
              file=sys.stderr)
        return 1

    return run(args.files, min_align, args.report, not args.no_cross_check)


if __name__ == "__main__":
    sys.exit(main())
