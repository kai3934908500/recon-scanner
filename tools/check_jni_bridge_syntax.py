#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""check_jni_bridge_syntax.py — Android JNI 桥接层的**语法-类型**闸门。

背景（本项目最贵的一类盲区）
---------------------------
`platform/android/app/src/main/cpp/*.cpp`（JNI 桥接层）只挂在根
`CMakeLists.txt` 的 `if(RECON_PLATFORM_ANDROID)` 分支里，而开发机没有 NDK
→ **桥接 C++ 从未被任何本机构建编译过**。后果：任何 C++ 语法/类型错误都只能
等 Android CI 真编译才发现，而且是**逐层暴露**——每修一层才露出下一层，
反馈周期以分钟计。

这不是理论担忧，是已发生的连续事故：

  · Android CI run #10（`native-lib.cpp:140`）
      error: assigning to 'recon_pipeline_branch' from incompatible type
             'unsigned int'
    根因：C 允许 `unsigned int` 隐式转枚举，**C++ 不允许**。该错误恰好只有
    C++ 编译器能发现，而这段代码此前只在 Android CI 被 C++ 编译过。

本闸门把这一层搬到**任何带 C++ 编译器的机器**（开发机 / ubuntu
static-contract job / macOS）上：用 `tools/jni_stub.h`（最小 JNI 头替身）
+ `-fsyntax-only` 做语法与类型检查。

为什么**不**并入 `check_android_contract.py`
-------------------------------------------
那份闸门的第一原则是「纯 Python 解析，**不 shell out 到工具**」（见其文件头
第 2 条），因为它要在**无 SDK / 无工具链**的机器上也能给出结论。
本闸门**必须**调编译器 —— 性质不同（与 `elf_load_align.py` 调 `readelf`、
以及「着色器一致性闸门之外另有独立 job 调 glslangValidator」同理），
故独立成脚本、独立能力点（`jni` / `neg-jni`）。

判据（每条都可被反向证伪；负向注入见 verify_jni_bridge_syntax_negative.py）
--------------------------------------------------------------------------
G-AND-7(a) **桩 ↔ 用面严格闭合**：`jni_stub.h` 声明的成员集合
           **恰好等于**桥接层实际调用的成员集合。
           · `stub - used` 非空 → 「**死声明**」→ 拒收。
             理由：桩在悄悄过期；下次有人真的用到那个成员时，会带着**未经
             核对的签名**通过检查（假绿）。这是本类闸门最真实的退化形态。
             该判定**带逐删实证**：把那个成员从桩里删掉重编译 —— 若仍通过，
             确认是死声明；若反而失败，说明**用面提取（正则）漏了它**，
             同样拒收（此时错的是闸门自己，不能悄悄放过）。
           · `used - stub` 非空 → 缺声明，编译必然失败，(b) 会兜住。
G-AND-7(b) **三个桥接 `.cpp` 全部真的过编译器**（`-fsyntax-only`）。
           这是「缺声明 / 类型错 / 签名过期」的**唯一实证手段**，也是本闸门
           存在的全部理由。
G-AND-7(c) **反空约束**：桥接文件数、桩成员数、用面成员数各有下限。
           防「扫描面塌缩 → 判据恒真通过」（MEMORY.md 明列的事故形态：
           把源文件语句删光后闸门反而全绿）。
G-AND-7(d) **fail-closed**：找不到任何 C++ 编译器 → **硬失败**，
           绝不静默跳过。「没跑」必须表现为**红**，不能表现为**绿**。

⚠️ 局限（如实登记，不静默）
--------------------------
本闸门用**替身头**而非真实 NDK 的 `jni.h`。因此：
  · 若替身某成员**签名写错**，检查可能给出与真机 NDK 不同的结论（假绿）。
    缓解：桩只声明桥接层**实际用到**的成员（空洞面小），且 (a) 的死声明判据
    逼它保持最新；签名核对仍须对着真实 NDK 头（见 jni_stub.h 的维护约定）。
  · `-fsyntax-only` **不做**代码生成与链接 → **不能**替代 Android CI 的
    真编译（它是「提前反馈」，不是「替代验证」）。

退出码
------
0 = 通过
1 = 用法/环境错误（无编译器、关键文件缺失）
3 = 契约违规
"""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
DEFAULT_ROOT = SCRIPT_DIR.parent

# 复用既有闸门的 C++ 注释剥离器（铁律「同一逻辑不写两遍」）。
# 它是状态机实现，会**保留字符串字面量里的 `//`** —— 本项目的桥接代码里有
# 含 `//` 的字符串（路径拼接），一刀切的正则会在那里出错。
sys.path.insert(0, str(SCRIPT_DIR))
from check_android_contract import strip_cpp_comments  # noqa: E402

# ---------------------------------------------------------------------------
# 反空下限（见 G-AND-7(c)）
# ---------------------------------------------------------------------------
# 这三个数不是"看起来差不多"，而是**当前真实值 + 安全余量**：
#   桥接文件 3 个（native-lib / platform-bridge / imu-bridge）
#   桩成员  22 个 / 用面 22 个（严格相等，见 (a)）
# 若将来真的删减桥接层，这几个数**应同步下调**（而不是让闸门报假红）；
# 但若它们被"顺手调小"来掩盖扫描面塌缩，那也是**显式的**代码改动 —— 这正是
# 反空约束想要的：把静默退化变成一次必须解释的改动。
MIN_BRIDGE_FILES = 3
MIN_STUB_MEMBERS = 20
MIN_USED_MEMBERS = 20

# 编译期 include 搜索路径。
# 前三个是仓库铁律（「头搜索路径双写」：core/include、platform/common/include、
# shaders/common —— 与 gen_ios_project.py、根 CMakeLists.txt 必须一致）；
# `tools` 用于找到 jni_stub.h。
INCLUDE_DIRS = ("tools", "core/include", "platform/common/include", "shaders/common")

CPP_STD = "-std=c++17"          # 与根 CMakeLists.txt 的 CMAKE_CXX_STANDARD 17 一致
WARN_FLAGS = ("-Wall", "-Wextra")   # 与根 CMakeLists.txt 的 target_compile_options 一致

# 编译器探测顺序：clang++ 优先（macOS/ubuntu CI），其后 g++ / c++ / cc。
# `cc` 放最后：它是 C 编译器的常见别名，靠扩展名分发到 C++ 前端。
COMPILER_CANDIDATES = ("clang++", "g++", "c++", "cc")

GATE = "G-AND-7"

# 用面提取：形如 `env->Foo(` 或 `ctx->env->Foo(`（把 env 存进结构体后再调）。
# ⚠️ 允许 `->` / `.` 组成的前缀链，但**最内层标识符必须是 `env`** —— 否则
#    `out->data()`、`v->resize()` 这类非 JNI 调用会被误捕（首版实测踩过）。
USED_MEMBER_RE = re.compile(
    r"(?<![A-Za-z0-9_])"
    r"(?:[A-Za-z_][A-Za-z0-9_]*\s*(?:->|\.)\s*)*"
    r"env\s*->\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(")

# 桩成员提取：`struct JNIEnv { ... };` 块内、形如 `RetType Name(args);` 的行。
# 取 `(` 之前**最后一个**标识符（返回类型在其前）。
STUB_MEMBER_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def read(path: pathlib.Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except (FileNotFoundError, IsADirectoryError, PermissionError):
        return ""


def rel(root: pathlib.Path, p: pathlib.Path) -> str:
    try:
        return p.relative_to(root).as_posix()
    except ValueError:
        return p.as_posix()


def find_compiler() -> str | None:
    for cand in COMPILER_CANDIDATES:
        found = shutil.which(cand)
        if found:
            return found
    return None


def stub_members(root: pathlib.Path) -> set[str]:
    """取 `jni_stub.h` 里 `struct JNIEnv` 的成员名集合。"""
    text = strip_cpp_comments(read(root / "tools" / "jni_stub.h"))
    m = re.search(r"struct\s+JNIEnv\s*\{(.*?)\n\};", text, re.S)
    if not m:
        return set()
    body = m.group(1)
    names: set[str] = set()
    for line in body.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        found = STUB_MEMBER_RE.findall(line)
        if found:
            names.add(found[-1])
    return names


def used_members(root: pathlib.Path, files: list[pathlib.Path]) -> set[str]:
    """扫桥接 `.cpp`（已剥注释），取实际调用的 JNIEnv 成员名集合。"""
    names: set[str] = set()
    for f in files:
        text = strip_cpp_comments(read(f))
        names.update(USED_MEMBER_RE.findall(text))
    return names


def compile_bridge(
    root: pathlib.Path,
    cc: str,
    files: list[pathlib.Path],
    stub_override: str | None = None,
    timeout: int = 120,
) -> list[tuple[str, str, int]]:
    """对每个桥接源文件跑 `-fsyntax-only`。

    `stub_override`：非 None 时，用它替换 `jni_stub.h` 的内容（用于死声明的
    逐删实证）。实现方式是在临时目录里放同名文件 + `jni.h` 转发，并把该目录
    放在 `-I` 最前 —— 因为 `#include "jni_stub.h"` 先查**当前文件所在目录**，
    再按 `-I` 顺序查，故同名文件会被优先命中。
    """
    results: list[tuple[str, str, int]] = []
    with tempfile.TemporaryDirectory(prefix="recon-jnistub-") as td:
        inc_dirs: list[str] = []
        if stub_override is not None:
            tdp = pathlib.Path(td)
            (tdp / "jni.h").write_text('#include "jni_stub.h"\n', encoding="utf-8")
            (tdp / "jni_stub.h").write_text(stub_override, encoding="utf-8")
            inc_dirs.append(tdp.as_posix())
        else:
            tdp = pathlib.Path(td)
            (tdp / "jni.h").write_text('#include "jni_stub.h"\n', encoding="utf-8")
            inc_dirs.append(tdp.as_posix())

        for d in INCLUDE_DIRS:
            inc_dirs.append(d)

        cmd_base = [cc, CPP_STD, "-fsyntax-only", *WARN_FLAGS]
        for d in inc_dirs:
            cmd_base += ["-I", d]

        for f in files:
            rel_f = rel(root, f)
            try:
                proc = subprocess.run(
                    [*cmd_base, rel_f],
                    cwd=str(root), capture_output=True, text=True, timeout=timeout)
                out = (proc.stdout or "") + (proc.stderr or "")
                results.append((rel_f, out, proc.returncode))
            except subprocess.TimeoutExpired:
                results.append((rel_f, f"编译超时（>{timeout}s）", 124))
            except OSError as exc:
                results.append((rel_f, f"无法执行编译器：{exc}", 127))
    return results


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Android JNI 桥接层的语法-类型闸门（jni_stub.h + -fsyntax-only）")
    ap.add_argument("--root", default="", help="仓库根目录（默认取脚本上级）")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    root = pathlib.Path(args.root).resolve() if args.root else DEFAULT_ROOT

    cpp_dir = root / "platform" / "android" / "app" / "src" / "main" / "cpp"
    stub = root / "tools" / "jni_stub.h"

    errors: list[str] = []
    notes: list[str] = []

    # ── 环境（缺失 = 无法执行，硬失败，不静默）──────────────────────────
    if not cpp_dir.is_dir():
        print(f"::error::[{GATE}] 桥接源目录不存在：{rel(root, cpp_dir)}")
        return 1
    if not stub.is_file():
        print(f"::error::[{GATE}] 桩头文件不存在：{rel(root, stub)}"
              f"（本闸门用它替代 NDK 的 jni.h）")
        return 1

    files = sorted(cpp_dir.glob("*.cpp"))

    # ── (c) 反空：文件数下限 ────────────────────────────────────────────
    if len(files) < MIN_BRIDGE_FILES:
        errors.append(
            f"[{GATE}(c)] 桥接 .cpp 只有 {len(files)} 个（下限 {MIN_BRIDGE_FILES}）"
            f"—— 扫描面塌缩，判据会退化为恒真通过。"
            f"若确实精简了桥接层，请**显式**下调 MIN_BRIDGE_FILES。")

    stub_set = stub_members(root)
    used_set = used_members(root, files)

    if len(stub_set) < MIN_STUB_MEMBERS:
        errors.append(
            f"[{GATE}(c)] 从 jni_stub.h 只解析出 {len(stub_set)} 个 JNIEnv 成员"
            f"（下限 {MIN_STUB_MEMBERS}）—— 解析失效或桩被删空。")
    if len(used_set) < MIN_USED_MEMBERS:
        errors.append(
            f"[{GATE}(c)] 从桥接源码只解析出 {len(used_set)} 个 JNIEnv 调用"
            f"（下限 {MIN_USED_MEMBERS}）—— 用面提取失效或源码被删空。")

    # ── (d) fail-closed：无编译器必须红 ────────────────────────────────
    cc = find_compiler()
    if cc is None:
        print(f"::error::[{GATE}(d)] 找不到任何 C++ 编译器"
              f"（试过：{', '.join(COMPILER_CANDIDATES)}）—— "
              f"本闸门**必须**调编译器，不能静默跳过。"
              f"请在 CI 镜像里提供 clang++ 或 g++（ubuntu/macOS runner 自带）。")
        return 1

    notes.append(f"[{GATE}] 编译器：{cc}")
    notes.append(f"[{GATE}] 桥接源文件 {len(files)} 个："
                 + ", ".join(rel(root, f) for f in files))

    # ── (b) 真编译 ──────────────────────────────────────────────────────
    results = compile_bridge(root, cc, files)
    for name, out, rc in results:
        if rc != 0:
            tail = "\n".join(out.strip().splitlines()[:25])
            errors.append(f"[{GATE}(b)] `{name}` 未通过编译器（rc={rc}）：\n{tail}")
        elif out.strip():
            # 有警告但通过：如实打印（不升级为错误 —— 根 CMakeLists 亦未开 -Werror，
            # 否则会出现「真构建绿、本闸门红」的假红）。
            notes.append(f"[{GATE}(b)] `{name}` 通过，但有告警：\n{out.strip()}")
        else:
            notes.append(f"[{GATE}(b)] `{name}` 通过（零告警）")

    # ── (a) 桩 ↔ 用面 严格闭合 ─────────────────────────────────────────
    missing = used_set - stub_set          # 用而未声明 → 编译应已失败
    dead = stub_set - used_set             # 声明而未用 → 死声明

    if missing:
        errors.append(
            f"[{GATE}(a)] 桥接层调用了桩里**未声明**的成员："
            f"{sorted(missing)} —— 请对着真实 NDK 的 jni.h 补齐签名"
            f"（见 jni_stub.h 维护约定），不要凭记忆写。")

    for name in sorted(dead):
        # 逐删实证：真的把它删掉，看编译是否仍然通过。
        text = read(stub)
        lines = text.splitlines(keepends=True)
        kept, removed = [], False
        for line in lines:
            if not removed and not line.lstrip().startswith("//") \
                    and re.search(rf"\b{re.escape(name)}\s*\(", line):
                removed = True
                continue
            kept.append(line)
        if not removed:
            errors.append(
                f"[{GATE}(a)] 判定 `{name}` 为死声明，但**未能在桩文本里定位到**"
                f"它的声明行 —— 提取逻辑与文本结构不一致（闸门自身失效）。")
            continue
        probe = compile_bridge(root, cc, files, stub_override="".join(kept))
        probe_failed = [n for n, _o, rc in probe if rc != 0]
        if probe_failed:
            # 删掉它反而编译失败 ⇒ 它其实**被用到了**，是用面正则漏了它。
            errors.append(
                f"[{GATE}(a)] 用面提取（正则）失效：把 `{name}` 从桩里删掉后，"
                f"{probe_failed} 编译失败 —— 说明它**真的被调用**，"
                f"而正则没扫到（USED_MEMBER_RE 需要更新）。")
        else:
            errors.append(
                f"[{GATE}(a)] 桩存在**死声明** `{name}`：删除它后三个桥接源"
                f"仍全部通过编译 —— 它从未被调用。请删掉这行声明"
                f"（桩只应声明被用到的成员，否则会带着未核对的签名悄悄过期）。")

    if not dead and not missing:
        notes.append(f"[{GATE}(a)] 桩 {len(stub_set)} 个成员 与 用面 "
                     f"{len(used_set)} 个成员**严格相等** ✓")

    for n in notes:
        print(n)
    for e in errors:
        print(f"::error::{e}")

    if errors:
        print(f"\n::error::[{GATE}] JNI 桥接语法闸门未通过（{len(errors)} 项）")
        return 3
    print(f"\nOK: [{GATE}] JNI 桥接语法闸门通过"
          f"（{len(files)} 个源文件过编译器；桩与用面严格闭合）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
