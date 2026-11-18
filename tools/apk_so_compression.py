#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""apk_so_compression.py — APK 内 `.so` 条目必须**未压缩**（ZIP_STORED）。

契约：docs/12_构建与打包.md §3.5（16 KB 页大小合规的必要条件）
登记：docs/14_验收测试.md 缺口 AY（其「.so 未压缩」子判据曾是误报型假闸门）

================================================================================
为什么需要它（这条判据曾经是**误报型假闸门（假红）**）
================================================================================
它原先内联在 `tools/check_16kb.sh` 里，写成：

    unzip -lv "$apk" | awk '$NF ~ /\\.so$/ && $3 == "Stored" {print $NF}' | wc -l

`unzip -lv` 的输出表头是：

    Length   Method    Size  Cmpr    Date    Time   CRC-32   Name
    ------  ------  ------- ---- ---------- ----- --------  ----
      8000  Stored     8000   0% ...                    big.so

「Method」是**第 2 列**（`$2`），`$3` 是 `Size` 列 —— 于是该 awk **恒不匹配**，
`Stored` 计数**恒为 0** → 只要 APK 里有 `.so`，就**恒判「被压缩」**（误报）。

CI #12 首次执行到这条断言时正是如此：报「.so 条目 15 个，其中 Stored 0 个」。
而**真实产物完全正确** —— 本机用 CI #12 的真实 APK 实测：`$3` 命中 0、
`$2` 命中 15（= `.so` 总数 15）。此前从未暴露的原因是 CI #11 在更靠前的
「检查 1」就失败，这条断言**从未被执行到**（被上游失败遮蔽）。

**修法（本脚本）**：不再解析 `unzip` 的**文本输出**（列位随版本可能变，硬编码
列位是脆的），改用 Python `zipfile` 直接读**每个条目的 `compress_type`** ——
这是结构性事实，不受任何输出格式影响。

================================================================================
判据（三条，全部落在**产物**上）
================================================================================
  1  每个 `.so` 条目的 `compress_type == ZIP_STORED`（0，未压缩）
     —— `useLegacyPackaging false` 是"未压缩"的必要条件；若它被改回 `true`，
        上面的 `zipalign -c -P 16` 可能**仍然通过**（它对齐的是 zip 局部头），
        但装到 Android 15+ 设备上仍会因 `.so` 被压缩而失败。
  2  **反空约束**：APK 内 `.so` 条目数为 0 → **报错**（"没东西可查" ≠ 通过；
        `abiFilters` / `jniLibs` 被改动时不得静默放过）。
  3  文件本身必须可读为 zip → 否则报错（不得静默跳过）。

退出码：0 全部通过；1 用法/读取错误；3 判据不满足。

用法：
    python3 tools/apk_so_compression.py <apk> [<apk> ...]
    python3 tools/apk_so_compression.py --self-test
"""

from __future__ import annotations

import argparse
import io
import os
import sys
import zipfile

KIND_STORED = "未压缩（Stored）"
KIND_DEFLATED = "已压缩（Deflated）"
KIND_OTHER = "其它压缩方式"


def classify(compress_type: int) -> tuple[bool, str]:
    """返回 (是否合规, 人类可读的方式名)。"""
    if compress_type == zipfile.ZIP_STORED:
        return True, KIND_STORED
    if compress_type == zipfile.ZIP_DEFLATED:
        return False, KIND_DEFLATED
    return False, f"{KIND_OTHER}({compress_type})"


def so_entries(apk_path: str) -> list[tuple[str, int]]:
    """列出 APK 内 `.so` 条目 → [(名字, compress_type), ...]。"""
    with zipfile.ZipFile(apk_path) as zf:
        return [(i.filename, i.compress_type) for i in zf.infolist()
                if i.filename.endswith(".so")]


def check_apk(apk_path: str, verbose: bool = True) -> tuple[bool, list[str]]:
    """判一条 APK。返回 (通过?, 说明行)。"""
    lines: list[str] = []
    try:
        entries = so_entries(apk_path)
    except (zipfile.BadZipFile, OSError) as exc:
        return False, [f"{apk_path}: 无法作为 zip 读取 —— {exc}"]

    if not entries:
        return False, [f"{apk_path}: 未找到任何 .so 条目 —— 判据的目标对象缺失，"
                       f"不得静默通过（abiFilters / jniLibs 配置被改动？）"]

    bad = [(n, ct) for n, ct in entries if not classify(ct)[0]]
    if verbose:
        if bad:
            for n, ct in bad:
                lines.append(f"{apk_path}: ❌ {n} 为 {classify(ct)[1]}")
        lines.append(f"{apk_path}: .so 条目 {len(entries)} 个，"
                     f"其中未压缩 {len(entries) - len(bad)} 个")
    return (not bad), lines


# ---------------------------------------------------------------------------
# 自检：用**合成 APK** 双态证伪（不依赖真实构建 / 不需要 Android SDK）
# ---------------------------------------------------------------------------
def _build_apk(path: str, so_types: dict[str, int]) -> None:
    payload = b"\x7fELF" + b"\x00" * 4092  # 内容无关紧要，只看 compress_type
    with zipfile.ZipFile(path, "w") as zf:
        for name, ctype in so_types.items():
            zf.writestr(zipfile.ZipInfo(name), payload, compress_type=ctype)


def self_test() -> int:
    import tempfile

    # ⚠️ 临时目录落 **E 盘**（本机铁律：产物/缓存不进 C 盘），
    #    优先环境变量，否则用仓库旁的 `_neg_tmp`。
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(here)
    tmp_base = os.environ.get("RECON_NEG_TMPDIR") or os.path.join(
        os.path.dirname(repo.rstrip("\\/")), "_neg_tmp")
    os.makedirs(tmp_base, exist_ok=True)

    ok_all = True
    with tempfile.TemporaryDirectory(prefix="apk-so-cmp-", dir=tmp_base) as td:
        # ---- 用例 1：全部 Stored → 通过（**真 APK 的形态**）--------------
        ok_apk = os.path.join(td, "ok.apk")
        _build_apk(ok_apk, {
            "lib/arm64-v8a/librecon_core.so": zipfile.ZIP_STORED,
            "lib/arm64-v8a/libc++_shared.so": zipfile.ZIP_STORED,
            "lib/armeabi-v7a/librecon_core.so": zipfile.ZIP_STORED,
        })
        passed, _ = check_apk(ok_apk, verbose=False)
        mark = "✓" if passed else "✗"
        print(f"  {mark} ok-all-stored        全部 Stored → 通过（期望通过）")
        ok_all &= passed

        # ---- 用例 2：有一个 Deflated → **拒收**（负向）--------------------
        bad_apk = os.path.join(td, "bad.apk")
        _build_apk(bad_apk, {
            "lib/arm64-v8a/librecon_core.so": zipfile.ZIP_STORED,
            "lib/arm64-v8a/librecon_platform_android.so": zipfile.ZIP_DEFLATED,
        })
        passed, _ = check_apk(bad_apk, verbose=False)
        mark = "✓" if not passed else "✗"
        print(f"  {mark} bad-one-deflated      单个 .so 被压缩 → 拒收（期望拒收）")
        ok_all &= (not passed)

        # ---- 用例 3：**全部** Deflated → 拒收（= useLegacyPackaging true 的形态）
        allz_apk = os.path.join(td, "allz.apk")
        _build_apk(allz_apk, {
            "lib/arm64-v8a/librecon_core.so": zipfile.ZIP_DEFLATED,
            "lib/arm64-v8a/libc++_shared.so": zipfile.ZIP_DEFLATED,
        })
        passed, _ = check_apk(allz_apk, verbose=False)
        mark = "✓" if not passed else "✗"
        print(f"  {mark} bad-all-deflated     全部 .so 被压缩（useLegacyPackaging true）"
              f"→ 拒收（期望拒收）")
        ok_all &= (not passed)

        # ---- 用例 4：**无 .so 条目** → 拒收（反空约束）--------------------
        noso_apk = os.path.join(td, "noso.apk")
        with zipfile.ZipFile(noso_apk, "w") as zf:
            zf.writestr("classes.dex", b"\x00" * 100)
        passed, _ = check_apk(noso_apk, verbose=False)
        mark = "✓" if not passed else "✗"
        print(f"  {mark} bad-no-so            无 .so 条目 → 拒收（反空约束）")
        ok_all &= (not passed)

        # ---- 用例 5：不是 zip → 拒收（不得静默跳过）----------------------
        notzip = os.path.join(td, "notzip.apk")
        with open(notzip, "wb") as fh:
            fh.write(b"this is not a zip at all")
        passed, _ = check_apk(notzip, verbose=False)
        mark = "✓" if not passed else "✗"
        print(f"  {mark} bad-not-zip          非 zip 文件 → 拒收（期望拒收）")
        ok_all &= (not passed)

        # ---- 用例 6：**混装**（部分 Stored / 部分 Deflated）→ 拒收 ---------
        mix_apk = os.path.join(td, "mix.apk")
        _build_apk(mix_apk, {
            "lib/arm64-v8a/librecon_core.so": zipfile.ZIP_STORED,
            "lib/armeabi-v7a/librecon_core.so": zipfile.ZIP_DEFLATED,
            "lib/x86_64/librecon_core.so": zipfile.ZIP_STORED,
        })
        passed, _ = check_apk(mix_apk, verbose=False)
        mark = "✓" if not passed else "✗"
        print(f"  {mark} bad-mixed            混装（部分压缩）→ 拒收（期望拒收）")
        ok_all &= (not passed)

    if ok_all:
        print("[self-test] PASS：6 项全部符合预期"
              "（含「单个被压缩」「全部被压缩」「无 .so」「非 zip」「混装」五个反例）")
        return 0
    print("[self-test] FAIL：存在不符合预期的用例 —— 判据不可信")
    return 3


def main() -> int:
    ap = argparse.ArgumentParser(description="APK 内 .so 必须未压缩（ZIP_STORED）")
    ap.add_argument("apks", nargs="*", help="APK 路径")
    ap.add_argument("--self-test", action="store_true", help="只跑合成 APK 双态自检")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    if not args.apks:
        print("用法：apk_so_compression.py <apk> [...] | --self-test", file=sys.stderr)
        return 1

    fail = 0
    for apk in args.apks:
        passed, lines = check_apk(apk)
        for ln in lines:
            print(("说明  " if passed else "::error::") + ln)
        if not passed:
            fail += 1
    if fail:
        print(f"[apk-so] FAIL：{fail} 个 APK 的 .so 未压缩判据未通过")
        return 3
    print(f"[apk-so] PASS：{len(args.apks)} 个 APK 的 .so 全部未压缩（ZIP_STORED）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
