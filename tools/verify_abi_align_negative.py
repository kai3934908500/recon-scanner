#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""verify_abi_align_negative.py — 「按 ABI 分别判页对齐」这条判据的负向注入。

登记：docs/14_验收测试.md 缺口 AY（假闸门 + armeabi-v7a 被抬到 16 KB）。
CMake 根因侧的静态判据是 G-AND-8（tools/check_android_contract.py）。

================================================================================
为什么需要它（这条判据曾经是**假闸门**）
================================================================================
`tools/check_16kb.sh` 的「检查 3」（armeabi-v7a 必须保持 4 KB）原先写成：

    if `elf_load_align.py --min-align 0x4000` 失败(rc=3); then 判为通过; fi

即**靠"失败"反推"是 4 KB"**。这个反推在逻辑上不成立：

  · armeabi-v7a 目录里混有**第三方预编译库**（libarcore_sdk_c/jni、
    libc++_shared）—— 它们不受本仓库 CMake 链接参数控制，实测本来就是
    0x1000；
  · 于是 `all(a >= 0x4000)` 恒为假 → rc 恒 3 → **恒判"符合 ✓"。**

后果：CI #11 的真实产物里，**本案自建**的 librecon_core.so /
librecon_platform_android.so 在 armeabi-v7a 下被抬到了 0x4000（16 KB），
与 docs/12 §3.2「armeabi-v7a 保持 4 KB、明确不做 16 KB 对齐」相悖 ——
而假闸门把真缺陷放过去了整整一路。

本脚本的作用：把「假闸门的那两种退化形态」重新注入，断言新判据确实拒收。

  注入 1（判据方向）：把检查 3 改回「用 `--min-align 0x4000` 的失败反推」
    → 在真实缺陷产物上，旧判据恒判通过 ⇒ 断言脚本**不再报错**。
    这正面证明：旧写法确实是零检测力的假闸门，新写法才有检测力。

  注入 2（范围收敛）：让筛选本案自建 .so 的白名单失效（前缀改成永不匹配的值）
    → 检查 3 会走到「未产出本案自建的 .so」分支而**静默跳过**。
    断言脚本因此不再报错 ⇒ 证明白名单是判据的**必要组成**，不是可选装饰。

  注入 3（CMake 根因）：把顶层 CMakeLists.txt 的 ABI 分支拆掉（恢复无条件
    `add_link_options`）→ 断言静态闸门（check_android_contract.py 的
    G-AND-8）拒收。这证明**根因侧**也有机器看守，不只靠产物侧。

  注入 4（反空）：把检查 3 整段删掉 → 断言脚本报「未找到检查 3」。

  注入 5（apk .so 未压缩判据 —— 误报型假闸门，CI #12 首次执行暴露）：
    原内联写法用 `unzip -lv | awk '$3 == "Stored"'`，而 Method 列是**第 2 列**
    → 计数恒 0 → **恒判「被压缩」**（真实 APK 里 15 个 .so 全是 Stored）。
    该判定已抽成 `tools/apk_so_compression.py`（读 `zipfile.compress_type`，
    不解析文本输出）。本注入把「未压缩判定」改成恒真/恒假，断言新判据的
    `--self-test` 会拒收 —— 证明该自检不是摆设。

⚠️ 注入把**合成 ELF** 当作判据的目标对象（`--build-dir` 指向临时树），
   因此本脚本**不需要 Android SDK / NDK / 真实构建**，任何机器可跑。
   合成 ELF 的 p_align 写成真实缺陷的形态（32 位自建 = 0x4000）。

用法：python3 tools/verify_abi_align_negative.py [--root .]
退出码：0 全部被正确拒收；1 有注入未被拒收（= 判据仍是假闸门）。
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

CHECK16 = os.path.join("tools", "check_16kb.sh")
ANDROID_CONTRACT = os.path.join("tools", "check_android_contract.py")
APK_SO_COMPRESSION = os.path.join("tools", "apk_so_compression.py")
ROOT_CMAKE = "CMakeLists.txt"

GATE_TAG = "G-AND-8"


# ---------------------------------------------------------------------------
# 构造「真实缺陷形态」的产物树
# ---------------------------------------------------------------------------
def _make_so(path: str, aligns: list[int]) -> None:
    """用 elf_load_align 自己的构造器造合成 ELF（保证与解析器同源，不引入第三方）。"""
    import importlib.util

    spec = importlib.util.spec_from_file_location(
        "_recon_elfalign", os.path.join(HERE, "elf_load_align.py"))
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(mod._make_elf64(aligns))


def build_defect_tree(base: str) -> str:
    """造一棵 build 树：64 位自建=0x4000（合规）；32 位自建=0x4000（**真实缺陷**）。

    ⚠️ 树的根必须命名 `build` —— `check_16kb.sh --build-dir <APP_DIR>` 会去找
    `<APP_DIR>/build`（见该脚本第 70-77 行）。故本函数返回的是 **APP_DIR**
    （即 `build` 的父目录）。
    """
    app_dir = os.path.join(base, "platform", "android", "app")
    lib = os.path.join(app_dir, "build", "intermediates", "merged_native_libs",
                       "release", "mergeReleaseNativeLibs", "out", "lib")
    # 64 位：合规
    for abi in ("arm64-v8a", "x86_64"):
        _make_so(os.path.join(lib, abi, "librecon_core.so"), [0x4000, 0x4000])
        _make_so(os.path.join(lib, abi, "librecon_platform_android.so"),
                 [0x4000, 0x4000])
    # 32 位自建：**缺陷**（应为 0x1000）
    for name in ("librecon_core.so", "librecon_platform_android.so"):
        _make_so(os.path.join(lib, "armeabi-v7a", name), [0x4000, 0x4000])
    # 32 位第三方预编译：本来就是 0x1000（不该参与判定）
    for name in ("libarcore_sdk_c.so", "libarcore_sdk_jni.so", "libc++_shared.so"):
        _make_so(os.path.join(lib, "armeabi-v7a", name), [0x1000, 0x1000])
    return app_dir


def run_check16(root: str, build_dir: str) -> tuple[int, str]:
    """跑 check_16kb.sh。

    ⚠️ 两个与 `check_16kb.sh` 实现耦合的细节（**不满足就会静默跑空**）：
      · 该脚本开头 `cd "${ROOT}"`（`ROOT`= 本仓库根），故 `--build-dir` 若给
        绝对路径必须用**正斜杠**形式（Windows 上 `C:\\...` 混反斜杠会让
        `find` 匹配不到）；
      · `--build-dir <X>` 实际扫的是 `<X>/build`，故调用方给的应是 APP_DIR。
    """
    bd = build_dir.replace("\\", "/")
    proc = subprocess.run(
        ["bash", os.path.join(root, CHECK16), "--build-dir", bd,
         "--expect-so-only"],
        capture_output=True, text=True, cwd=root)
    return proc.returncode, proc.stdout + proc.stderr


def run_android_contract(root: str) -> tuple[int, str]:
    """跑 Android 静态契约闸门。

    ⚠️ `check_android_contract.py` **没有** `--root` 参数 —— 它从脚本自身位置
    推导仓库根（`REPO = parent(HERE)`）。故这里直接跑，不做 cwd/参数注入，
    也不可假想它支持 `--root`（真实踩过：传 `--root` 会 rc=2 用法错误，
    表现为「注入未被拒收」的假红）。
    """
    proc = subprocess.run(
        [sys.executable, os.path.join(root, ANDROID_CONTRACT)],
        capture_output=True, text=True, cwd=root)
    return proc.returncode, proc.stdout + proc.stderr


def run_apk_self_test(root: str) -> tuple[int, str]:
    """跑 `apk_so_compression.py --self-test`（合成 APK 双态自检）。"""
    proc = subprocess.run(
        [sys.executable, os.path.join(root, APK_SO_COMPRESSION), "--self-test"],
        capture_output=True, text=True, cwd=root)
    return proc.returncode, proc.stdout + proc.stderr


def slurp(p: str) -> str:
    with open(p, encoding="utf-8") as fh:
        return fh.read()


def restore(p: str, content: str) -> None:
    with open(p, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(content)


# ---------------------------------------------------------------------------
# 注入定义
# ---------------------------------------------------------------------------
# 语义统一：**每个注入都使判据失效**（注入 → 32 位越界不再被拒收）。
# 这正面证明「这条判据是检测力的来源」：拆掉它，缺陷就漏过去。
# (用例名, 说明, 目标文件, 原串, 替换串, 判据失效的标志子串)
CHECK16_CASES: list[tuple[str, str, str, str, str, str]] = [
    (
        "AA-1-revert-to-failure-inference",
        "把检查 3 改回「用 --min-align 0x4000 的失败反推 4 KB」"
        "（= CI #11 时那版零检测力的假闸门）",
        CHECK16,
        '  rc32=$?\n'
        '  set -e\n'
        '  case "${rc32}" in\n'
        '    0) note "armeabi-v7a 自建 .so 全部 <= 0x1000 → 4 KB 口径符合 docs/12 §3.2 ✓" ;;\n'
        '    3) bad "armeabi-v7a 的**自建** .so 出现 > 0x1000 的 LOAD 段 —— 疑似把"$',
        '  rc32=$?\n'
        '  set -e\n'
        '  if [ "${rc32}" -eq 3 ]; then rc32=0; fi\n'
        '  case "${rc32}" in\n'
        '    0) note "armeabi-v7a 自建 .so 全部 <= 0x1000 → 4 KB 口径符合 docs/12 §3.2 ✓" ;;\n'
        '    3) bad "armeabi-v7a 的**自建** .so 出现 > 0x1000 的 LOAD 段 —— 疑似把"$',
        "armeabi-v7a 的**自建** .so 出现",
    ),
    (
        "AA-2-whitelist-neutered",
        "让「只判本案自建 .so」的白名单失效（前缀改成永不匹配的值）"
        "→ 检查 3 会静默跳过（证明白名单是判据的必要组成）",
        CHECK16,
        'RECON_OWN_PREFIXES=("librecon_")',
        'RECON_OWN_PREFIXES=("__never_matches__")',
        "armeabi-v7a 的**自建** .so 出现",
    ),
    (
        "AA-3-check3-degraded-to-report",
        "把检查 3 的**判定**退化成 report-only（只打印不判，"
        "= 闸门悄悄变成日志）→ 判据失效（不再拒收）",
        CHECK16,
        '  "${PY}" "${SCRIPT_DIR}/elf_load_align.py" --no-cross-check \\\n'
        '      --max-align 0x1000 "${sos32[@]}"',
        '  "${PY}" "${SCRIPT_DIR}/elf_load_align.py" --no-cross-check --report \\\n'
        '      "${sos32[@]}" || true',
        "armeabi-v7a 的**自建** .so 出现",
    ),
]

# CMake 根因侧：拆掉 ABI 分支（恢复无条件挂载）→ 静态闸门 G-AND-8 拒收。
CMAKE_CASES: list[tuple[str, str, str, str, str, str]] = [
    (
        "AA-4-cmake-unconditional-align",
        "顶层 CMakeLists.txt 恢复「无条件 add_link_options」"
        "（= CI #11 那个把 32 位抬到 16 KB 的写法）",
        ROOT_CMAKE,
        "  if(_recon_abi IN_LIST RECON_16KB_ABIS)",
        "  if(TRUE)",
        GATE_TAG,
    ),
]

# APK 内 `.so` 未压缩判据的自检（CI #12 暴露的**误报型假闸门**）。
# 语义与上面几组**不同**：这里注入的是**判据本体**（apk_so_compression.py），
# 而「拒收」的动作由它的 `--self-test` 承担 —— 故断言是：
#   **注入后自检必须失败**（rc≠0 且输出含 `FAIL`）。
# 这正面证明该自检**不是摆设**：判据一坏，自检当场抓住。
#
# (用例名, 说明, 原串, 替换串, 期望「出现」的子串)
APK_CASES: list[tuple[str, str, str, str, str]] = [
    (
        "AA-5-apk-so-classifier-neutered",
        "把「是否未压缩」的判定改成**恒真**（= CI #12 那版错列位 awk 的"
        "本质：无论实际如何都判不出「被压缩」）→ 自检必须失败",
        "    if compress_type == zipfile.ZIP_STORED:\n"
        "        return True, KIND_STORED\n"
        "    if compress_type == zipfile.ZIP_DEFLATED:\n"
        "        return False, KIND_DEFLATED\n"
        "    return False, f\"{KIND_OTHER}({compress_type})\"",
        "    return True, KIND_STORED",
        "FAIL",
    ),
    (
        "AA-6-apk-so-antiempty-removed",
        "删掉**反空约束**（无 .so 条目时不再报错）→ 自检必须失败"
        "（证明反空约束是判据的必要组成，不是可选装饰）",
        "    if not entries:\n"
        "        return False, [f\"{apk_path}: 未找到任何 .so 条目 —— 判据的目标对象缺失，\"\n"
        "                       f\"不得静默通过（abiFilters / jniLibs 配置被改动？）\"]",
        "    if not entries:\n"
        "        return True, []",
        "FAIL",
    ),
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=DEFAULT_ROOT)
    args = ap.parse_args()
    root = os.path.abspath(args.root)

    if not os.path.isfile(os.path.join(root, CHECK16)):
        print(f"[negative] 未找到判据本体：{os.path.join(root, CHECK16)}")
        return 1

    fail = 0
    # 判据的**目标对象**用合成 ELF（无需 Android SDK）。
    # ⚠️ 临时树必须落 **E 盘**（本机铁律：所有产物/缓存不进 C 盘）。
    #    优先用环境变量 `RECON_NEG_TMPDIR`，否则落仓库旁的 `_neg_tmp`。
    tmp_base = os.environ.get("RECON_NEG_TMPDIR") or os.path.join(
        os.path.dirname(root.rstrip("\\/")), "_neg_tmp")
    os.makedirs(tmp_base, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="recon-abi-align-neg-", dir=tmp_base)
    try:
        build_dir = build_defect_tree(tmp)

        # ---- 基线：真实缺陷形态必须被拒收（否则后面注入无意义）------------
        rc, out = run_check16(root, build_dir)
        hit = "armeabi-v7a 的**自建** .so 出现" in out
        if rc != 3 or not hit:
            print("[negative] 基线状态下「32 位自建 16 KB」**未被拒收** —— "
                  "判据本身有问题，注入测试无意义：")
            print(out[-2000:])
            return 1
        print("[negative] 基线：32 位自建 16 KB 被正确拒收 ✓")

        # ---- 注入 1..3：产物侧判据 ---------------------------------------
        # 统一语义：注入后判据**失效**（32 位越界不再被拒收）。
        # 这证明该判据是检测力的真实来源；若注入后仍被拒收，说明"拒收"来自
        # 别处（比如本脚本自己的基线断言），判据的贡献未被隔离。
        backups: list[tuple[str, str]] = []
        try:
            for name, desc, rel, old, new, expect in CHECK16_CASES:
                path = os.path.join(root, rel)
                src = slurp(path)
                backups.append((path, src))
                if src.count(old) < 1:
                    print(f"  ✗ {name} —— 注入锚点未命中（{rel}），"
                          f"说明**注入本身已失效**（不是判据失效）")
                    fail = 1
                    continue
                restore(path, src.replace(old, new, 1))

                rc2, out2 = run_check16(root, build_dir)
                hit2 = expect in out2
                ok = not hit2  # 期望：注入后**不再**报该子串
                mark = "✓" if ok else "✗"
                print(f"  {mark} {name} —— "
                      f"{'判据失效（注入后不再拒收）' if ok else '仍被拒收（注入无效？）'}"
                      f"  [{desc}]")
                if not ok:
                    fail = 1
                    print("      " + "\n      ".join(out2.strip().splitlines()[-6:]))
                restore(path, backups.pop()[1])
        finally:
            while backups:
                path, content = backups.pop()
                restore(path, content)

        # ---- 注入 4：CMake 根因侧（静态闸门） ----------------------------
        for name, desc, rel, old, new, expect in CMAKE_CASES:
            path = os.path.join(root, rel)
            src = slurp(path)
            if src.count(old) < 1:
                print(f"  ✗ {name} —— 注入锚点未命中（{rel}）")
                fail = 1
                continue
            restore(path, src.replace(old, new, 1))
            try:
                rc3, out3 = run_android_contract(root)
                hit3 = expect in out3
                ok = (rc3 != 0) and hit3
                mark = "✓" if ok else "✗"
                print(f"  {mark} {name} —— {'已拒收' if ok else '未被拒收'} "
                      f"(rc={rc3}, expect='{expect}' {'命中' if hit3 else '未命中'})"
                      f"  [{desc}]")
                if not ok:
                    fail = 1
                    print("      " + "\n      ".join(out3.strip().splitlines()[-6:]))
            finally:
                restore(path, src)

        # ---- 基线：apk_so_compression.py 的自检必须先通过 ---------------
        # 若自检本身就不通过，后面的注入断言无意义（无法区分「注入生效」与
        # 「判据本来就坏」）。
        if not os.path.isfile(os.path.join(root, APK_SO_COMPRESSION)):
            print(f"[negative] 未找到判据本体：{os.path.join(root, APK_SO_COMPRESSION)}")
            fail = 1
        else:
            rca, outa = run_apk_self_test(root)
            if rca != 0:
                print("[negative] 基线：apk_so_compression.py --self-test **未通过**"
                      " —— 判据本身有问题，注入测试无意义：")
                print(outa[-1500:])
                fail = 1
            else:
                print("[negative] 基线：apk 内 .so 未压缩自检通过（6/6）✓")

            # ---- 注入 5..6：APK .so 未压缩判据 ---------------------------------
            # 语义：注入**判据本体**，断言它的 `--self-test` **失败**（rc≠0）。
            # = 判据一坏，自检当场抓住 ⇒ 自检不是摆设。
            backups_a: list[tuple[str, str]] = []
            try:
                for name, desc, old, new, expect in APK_CASES:
                    path = os.path.join(root, APK_SO_COMPRESSION)
                    src = slurp(path)
                    backups_a.append((path, src))
                    if src.count(old) < 1:
                        print(f"  ✗ {name} —— 注入锚点未命中（{APK_SO_COMPRESSION}），"
                              f"说明**注入本身已失效**（不是判据失效）")
                        fail = 1
                        continue
                    restore(path, src.replace(old, new, 1))

                    rcb, outb = run_apk_self_test(root)
                    hit_b = expect in outb
                    ok = (rcb != 0) and hit_b
                    mark = "✓" if ok else "✗"
                    print(f"  {mark} {name} —— "
                          f"{'自检失败（注入后判据被抓住）' if ok else '自检仍通过（注入无效？）'}"
                          f"(rc={rcb})  [{desc}]")
                    if not ok:
                        fail = 1
                        print("      " + "\n      ".join(outb.strip().splitlines()[-6:]))
                    restore(path, backups_a.pop()[1])
            finally:
                while backups_a:
                    path, content = backups_a.pop()
                    restore(path, content)

            # 还原后自检必须重新通过
            rcc, outc = run_apk_self_test(root)
            if rcc != 0:
                print("[negative] 还原后 apk 自检失败 —— 注入脚本污染了工作区：")
                print(outc[-1200:])
                fail = 1

        # ---- 还原后基线 ------------------------------------------------
        rc4, out4 = run_check16(root, build_dir)
        hit4 = "armeabi-v7a 的**自建** .so 出现" in out4
        if rc4 != 3 or not hit4:
            print("[negative] 还原后基线失败 —— 注入脚本污染了工作区：")
            print(out4[-1500:])
            fail = 1
        else:
            print("[negative] 还原后基线：仍正确拒收 ✓")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if fail:
        print("\n[negative] FAIL：存在未被正确拒收的注入（判据可能是假闸门）")
        return 1
    print("\n[negative] PASS：全部注入均被正确拒收 —— "
          "「按 ABI 分别判页对齐」这条判据有真实检测力")
    return 0


if __name__ == "__main__":
    sys.exit(main())
