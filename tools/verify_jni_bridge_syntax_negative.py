#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""verify_jni_bridge_syntax_negative.py — 对 check_jni_bridge_syntax.py 做负向注入证伪。

为什么必须做
------------
本项目铁律：**每条闸门判据都必须做负向注入证伪**。「跑起来了」与「判据有效」
是两件事 —— 本仓库真实发生过多次「闸门恒真通过」（见
verify_android_contract_negative.py 文件头列出的三个实例）。

本注入器对每条 G-AND-7 判据构造一个**必须被拒收**的最小改动，断言：
  ① 闸门返回非 0（拒收）；
  ② 拒收理由**命中该判据的标签**（防「A 判据坏了由 B 判据兜住」的假通过）。

**最要紧的一项是 `G-AND-7b/uint-to-enum`** —— 它把 Android CI run #10 的真实
错误原样还原（`cfg.forced_branch = 0xFFFFFFFFu;`），借此证明：**如果这个闸门
早存在，那次 CI 失败会在开发机上被提前拦住**。这是本闸门存在的全部理由，
也是它的负向注入里唯一一个「有真实 CI 日志对应」的项。

注入方式：把仓库拷到临时目录，在副本上做**单点**改写，跑闸门，再还原。
绝不触碰真实仓库。

用法
----
    python tools/verify_jni_bridge_syntax_negative.py          # 全跑
    python tools/verify_jni_bridge_syntax_negative.py -v       # 打印每步细节
    python tools/verify_jni_bridge_syntax_negative.py --only dead-stub
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO = SCRIPT_DIR.parent
GATE = SCRIPT_DIR / "check_jni_bridge_syntax.py"

# 与 verify_android_contract_negative.py 同一份精确排除表（勿用 `build*` 通配：
# 它会把 `platform/android/build.gradle` 一并排除 —— 那里已有实测记录）。
IGNORE = (".git", "build", "build_*", "build-*", "out", "dist",
          "third_party", ".workbuddy", "__pycache__",
          "*.spv", "*.o", "*.a", "*.so")

CPP = "platform/android/app/src/main/cpp"
STUB = "tools/jni_stub.h"
GATE_LABEL = "G-AND-7"


class Injection:
    """一次单点注入：改文件 → 断言闸门拒收且命中标签 → 还原。

    `expect_text`：除标签外还要求输出命中该**子串**。只用标签断言是不够的 ——
    多个判据共享 `G-AND-7` 前缀，若「(a) 的断言坏了」而「(b) 恰好也报错」，
    只看标签会判为通过（属假通过）。故一律给出**该判据特有的措辞**。
    """

    def __init__(self, name: str, expect_label: str, desc: str,
                 expect_text: str = "") -> None:
        self.name = name
        self.expect_label = expect_label
        self.expect_text = expect_text
        self.desc = desc

    def apply(self, root: pathlib.Path) -> None:      # pragma: no cover
        raise NotImplementedError

    def undo(self, root: pathlib.Path) -> None:       # pragma: no cover
        raise NotImplementedError


class PatchInjection(Injection):
    """把 file 中 old 替换为 new（要求 old 出现 count 次）。"""

    def __init__(self, name, expect_label, desc, file, old, new,
                 count: int = 1, expect_text: str = "") -> None:
        super().__init__(name, expect_label, desc, expect_text)
        self.file = file
        self.old = old
        self.new = new
        self.count = count
        self._backup: str | None = None

    def _path(self, root: pathlib.Path) -> pathlib.Path:
        return root / self.file

    def apply(self, root: pathlib.Path) -> None:
        p = self._path(root)
        txt = p.read_text(encoding="utf-8")
        n = txt.count(self.old)
        if n != self.count:
            raise AssertionError(
                f"[{self.name}] 锚点出现 {n} 次，期望 {self.count} 次："
                f"{self.old!r}（文件 {self.file}）")
        self._backup = txt
        p.write_text(txt.replace(self.old, self.new), encoding="utf-8")

    def undo(self, root: pathlib.Path) -> None:
        if self._backup is not None:
            self._path(root).write_text(self._backup, encoding="utf-8")
            self._backup = None


class DeleteFileInjection(Injection):
    """删除一个文件（用于「关键文件缺失 → 必须硬失败」这类判据）。"""

    def __init__(self, name, expect_label, desc, file,
                 expect_text: str = "") -> None:
        super().__init__(name, expect_label, desc, expect_text)
        self.file = file
        self._backup: str | None = None

    def apply(self, root: pathlib.Path) -> None:
        p = root / self.file
        if not p.exists():
            raise AssertionError(f"[{self.name}] 待删文件不存在：{self.file}")
        self._backup = p.read_text(encoding="utf-8")
        p.unlink()

    def undo(self, root: pathlib.Path) -> None:
        if self._backup is not None:
            (root / self.file).write_text(self._backup, encoding="utf-8")
            self._backup = None


class MultiPatchInjection(Injection):
    """在多个文件上各做一处替换（用于必须**同时**改两处才成立的注入）。

    例：「证明用面正则失效时闸门会拒收而非静默放过」需要
      ① 在一个**未使用**的桩成员上制造真实调用点（用正则可扫的写法，
         使该成员不再"死"）；
      ② 另一个地方用**正则扫不到**的写法调用它 —— 但这样就矛盾了。
    故本类用于另一类情形：把「扫描面」与「实际用面」解耦，构造
    「正则认为某成员未使用、而编译器认为它被使用了」的**不一致状态**。
    """

    def __init__(self, name, expect_label, desc, edits,
                 expect_text: str = "") -> None:
        """edits: [(file, old, new, count), ...]"""
        super().__init__(name, expect_label, desc, expect_text)
        self.edits = edits
        self._backups: list[tuple[str, str]] = []

    def apply(self, root: pathlib.Path) -> None:
        self._backups = []
        for file, old, new, count in self.edits:
            p = root / file
            txt = p.read_text(encoding="utf-8")
            n = txt.count(old)
            if n != count:
                raise AssertionError(
                    f"[{self.name}] 锚点出现 {n} 次，期望 {count} 次："
                    f"{old!r}（文件 {file}）")
            self._backups.append((file, txt))
            p.write_text(txt.replace(old, new), encoding="utf-8")

    def undo(self, root: pathlib.Path) -> None:
        for file, txt in self._backups:
            (root / file).write_text(txt, encoding="utf-8")
        self._backups = []


def build_injections() -> list[Injection]:
    """构造全部注入项。每项都对应一个真实会发生的漂移。"""
    NL = f"{CPP}/native-lib.cpp"
    return [
        # ── (b) 真编译：把 CI #10 的真实错误原样还原 ────────────────────
        # ⚠️ 这是全套注入里唯一有**真实 CI 日志**对应的一项。
        #    还原 `static_cast<recon_pipeline_branch>(0xFFFFFFFFu)` 为
        #    `0xFFFFFFFFu`（CI run #10 的写法）→ C++ 必须拒收。
        #    若这条注入不被拒收，说明本闸门**拦不住它本应拦的那一类错误**。
        PatchInjection(
            "G-AND-7b/uint-to-enum",
            GATE_LABEL,
            "还原 Android CI run #10 的真实错误：`unsigned int` 隐式转枚举",
            NL,
            "cfg.forced_branch = static_cast<recon_pipeline_branch>(0xFFFFFFFFu);",
            "cfg.forced_branch = 0xFFFFFFFFu;",
            expect_text="未通过编译器"),

        # 另一个纯语法错（证明 (b) 不只会拦"类型"这一类）。
        PatchInjection(
            "G-AND-7b/syntax-error",
            GATE_LABEL,
            "桥接源里制造纯语法错误（括号不配对）",
            NL,
            "  return static_cast<jint>(recon_abi_version());",
            "  return static_cast<jint>(recon_abi_version();",
            expect_text="未通过编译器"),

        # 缺声明（用而未在桩里声明）→ (b) 报编译失败，(a) 亦报 missing。
        # ⚠️ 断言 expect_text 取 **(a) 的措辞**，以证明 (a) 独立生效，
        #    而不是被 (b) 兜住。
        PatchInjection(
            "G-AND-7a/undeclared-member",
            GATE_LABEL,
            "桥接层调用桩里未声明的 JNI 成员（模拟 NDK 新增 API 而未补桩）",
            NL,
            "if (env->ExceptionCheck()) return false;\n  }\n  return true;\n}\n\nbool CopyShortArray",
            "if (env->ExceptionOccurred()) return false;\n  }\n  return true;\n}\n\nbool CopyShortArray",
            expect_text="未声明"),

        # ── (a) 死声明判定：往桩里塞一个从未被调用的成员 ────────────────
        # 这正是「桩悄悄过期」的形态：本闸门必须拒收，并指出该成员是死的。
        PatchInjection(
            "G-AND-7a/dead-stub-member",
            GATE_LABEL,
            "向桩里加一个从未被调用的成员（死声明）→ 必须被逐删实证拒收",
            STUB,
            "    void     DeleteLocalRef(jobject);",
            "    void     DeleteLocalRef(jobject);\n"
            "    jboolean IsSameObject(jobject, jobject);",
            expect_text="死声明"),

        # ── (a) 用面提取失效 → 必须**拒收**（不是静默放过）──────────────
        # 判据 (a) 是「桩成员集合 == 用面集合」。若用面提取（正则）漏扫了某个
        # **确实被调用**的成员，它就会被误判为"死声明"。闸门对此的处理是
        # **逐删实证**：把它从桩里删掉重编译 —— 若编译**失败**，说明它其实
        # 被用到，于是报「用面提取（正则）失效」并拒收。
        #
        # 本注入构造出这个状态：给桩加一个成员，并在源码里**用别名引用**调用它
        #   `JNIEnv& _jni_alias = *env; _jni_alias.IsInstanceOf(...);`
        #   —— 最内层标识符是 `_jni_alias` 而非 `env`，正则扫不到；
        #      而 C++ 里经引用调用成员函数**完全合法**，编译通过。
        # 期望：闸门报「用面提取（正则）失效」并拒收（rc=3），
        #       而**不是**静默通过、也**不是**错报成死声明。
        MultiPatchInjection(
            "G-AND-7a/used-but-unscanned",
            GATE_LABEL,
            "桩成员被**别名引用**真实调用而正则扫不到 → 逐删实证必须识破并"
            "报「用面提取失效」（证明 (a) 的这一支不是恒真）",
            [
                (STUB,
                 "    void     DeleteLocalRef(jobject);",
                 "    void     DeleteLocalRef(jobject);\n"
                 "    jboolean IsInstanceOf(jobject, jclass);",
                 1),
                (NL,
                 "bool CopyByteArray(JNIEnv* env, jbyteArray src, "
                 "std::vector<uint8_t>* out) {\n  if (src == nullptr) return true;",
                 "bool CopyByteArray(JNIEnv* env, jbyteArray src, "
                 "std::vector<uint8_t>* out) {\n"
                 "  { JNIEnv& _jni_alias = *env; "
                 "(void)_jni_alias.IsInstanceOf(nullptr, nullptr); }\n"
                 "  if (src == nullptr) return true;",
                 1),
            ],
            expect_text="用面提取"),

        # ── (c) 反空：扫描面塌缩 ────────────────────────────────────────
        # 删掉一个桥接源文件（3→2）→ 文件数反空约束必须触发。
        DeleteFileInjection(
            "G-AND-7c/shrink-bridge-files",
            GATE_LABEL,
            "删掉一个桥接源文件（3→2）→ 文件数反空约束必须触发",
            f"{CPP}/platform-bridge.cpp",
            expect_text="扫描面塌缩"),

        # ── (d) fail-closed：关键文件缺失 ───────────────────────────────
        # 桩不存在 → 必须硬失败（rc=1），不能静默跳过。
        DeleteFileInjection(
            "G-AND-7d/stub-missing",
            GATE_LABEL,
            "删掉 jni_stub.h → 环境缺失必须硬失败（不静默跳过）",
            STUB,
            expect_text="桩头文件不存在"),
    ]


def run_gate(root: pathlib.Path, timeout: int = 300) -> tuple[int, str]:
    """在 root（副本仓库）里跑闸门，返回 (rc, 输出)。"""
    proc = subprocess.run(
        [sys.executable, str(root / "tools" / "check_jni_bridge_syntax.py"),
         "--root", str(root)],
        cwd=str(root), capture_output=True, text=True, timeout=timeout)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="对 JNI 桥接语法闸门做负向注入证伪")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--only", default="", help="只跑名字含该子串的注入项")
    args = ap.parse_args()

    if not GATE.exists():
        print(f"::error::闸门不存在：{GATE}")
        return 1

    injections = build_injections()
    if args.only:
        injections = [i for i in injections if args.only in i.name]
    if not injections:
        print("::error::没有匹配的注入项")
        return 1

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="recon-jnineg-"))
    work = tmp / "repo"
    ok = fail = 0
    failures: list[str] = []
    try:
        shutil.copytree(REPO, work, ignore=shutil.ignore_patterns(*IGNORE),
                        symlinks=True)

        # --- 前置：副本上基线必须 PASS（否则注入结果无意义）-------------
        base_rc, base_out = run_gate(work)
        if base_rc != 0:
            print("::error::副本基线未 PASS，注入结果不可信：")
            print(base_out[-3000:])
            return 1
        print(f"[nav] 副本基线 PASS（{work}）")

        for inj in injections:
            inj.apply(work)
            try:
                rc, out = run_gate(work)
            finally:
                inj.undo(work)

            if rc == 0:
                fail += 1
                failures.append(
                    f"{inj.name}：闸门**未拒收**（rc=0）—— 判据形同虚设。"
                    f"说明：{inj.desc}")
                print(f"  ✗ {inj.name}：闸门未拒收（假绿）")
                continue

            hit_label = inj.expect_label in out
            hit_text = (not inj.expect_text) or (inj.expect_text in out)
            if not hit_label or not hit_text:
                fail += 1
                why = []
                if not hit_label:
                    why.append(f"未命中标签 `{inj.expect_label}`")
                if not hit_text:
                    why.append(f"未命中该判据特有措辞 `{inj.expect_text}`")
                failures.append(
                    f"{inj.name}：拒收了（rc={rc}）但 " + "、".join(why)
                    + " —— 可能被别的判据兜住，属假通过")
                print(f"  ✗ {inj.name}：rc={rc} 但 " + "、".join(why))
                if args.verbose:
                    print("    ---- 输出 ----")
                    print("\n".join(out.splitlines()[:20]))
            else:
                ok += 1
                print(f"  ✓ {inj.name} → rc={rc}，命中 {inj.expect_label}"
                      + (f" / `{inj.expect_text}`" if inj.expect_text else ""))
                if args.verbose:
                    for ln in out.splitlines():
                        if inj.expect_label in ln or (
                                inj.expect_text and inj.expect_text in ln):
                            print(f"      {ln[:160]}")

        # --- 收尾：还原后基线必须重新 PASS -----------------------------
        final_rc, final_out = run_gate(work)
        if final_rc != 0:
            fail += 1
            failures.append("还原后基线未 PASS —— 注入器未能干净还原，"
                            "说明某处 undo 不完整")
            print("::error::还原后基线未 PASS")
        else:
            print("[nav] 还原后基线重新 PASS ✓")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    if failures:
        print(f"::error::负向注入证伪失败 {fail} 项 / 共 {ok + fail} 项")
        for f in failures:
            print(f"  · {f}")
        return 3
    print(f"[nav] 全部 {ok} 项注入均被正确拒收 ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())
