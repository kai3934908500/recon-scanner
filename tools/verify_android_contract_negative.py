#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""verify_android_contract_negative.py — 对 check_android_contract.py 做负向注入证伪。

为什么必须做
------------
本项目铁律：**每条闸门判据都必须做负向注入证伪**。理由不是形式主义 ——
本仓库真实发生过多次「闸门看起来在跑、其实恒真通过」：

  · `readelf -lW | awk '$1=="LOAD"{print $NF}'` 在工具缺失时匹配 0 行
    → 循环不执行 → **恒真通过**（`check_16kb.sh` 的判据来源）
  · `check_android_contract.py` 首版用 C 系 `//` 注释剥离器处理 CMake 的
    `#` 注释 → 注释里的 `)` 截断了 `add_library(...)` 解析 → **报假红**
  · 同首版把 `@style/Theme.Recon` 的正则写成 `[A-Za-z0-9_]*`（不含点）
    → 截成 `@style/Theme` → **同样是假红**

「跑起来了」与「判据有效」是两件事。本注入器对每条判据构造一个**必须被拒收**
的最小改动，断言两件事：
  ① 闸门返回非 0（拒收）；
  ② 拒收理由**命中该判据的标签**（防止「A 判据坏了由 B 判据兜住」的假通过）。

注入方式：把仓库拷到临时目录，在副本上做**单点**改写，跑闸门，再还原。
绝不触碰真实仓库。

用法
----
    python tools/verify_android_contract_negative.py          # 全跑
    python tools/verify_android_contract_negative.py -v       # 打印每步细节
"""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
GATE = REPO / "tools" / "check_android_contract.py"

# ⚠️ 排除模式必须**精确**，不能用 `build*` 通配 ——
#    `shutil.ignore_patterns` 是 fnmatch 匹配**每一项 basename**，
#    而 `platform/android/build.gradle` 的 basename 恰是 `build.gradle`，
#    `build*` 会把它一并排除 → 注入器自己把被注入文件删了，
#    外表像「闸门全挂了」。这是 verify_toolchain_negative.py 真实踩过的坑。
IGNORE = (".git", "build", "build_*", "build-*", "out", "dist",
          "third_party", ".workbuddy", "__pycache__",
          "*.spv", "*.o", "*.a", "*.so")


class Injection:
    """一次单点注入：改文件 → 断言闸门拒收且命中标签 → 还原。"""

    def __init__(self, name: str, expect_label: str, desc: str) -> None:
        self.name = name
        self.expect_label = expect_label
        self.desc = desc

    def apply(self, root: pathlib.Path) -> None:      # pragma: no cover
        raise NotImplementedError

    def undo(self, root: pathlib.Path) -> None:       # pragma: no cover
        raise NotImplementedError


class PatchInjection(Injection):
    """把 file 中 old 替换为 new（要求 old 唯一出现）。"""

    def __init__(self, name, expect_label, desc, file, old, new,
                 count: int = 1) -> None:
        super().__init__(name, expect_label, desc)
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

    def __init__(self, name, expect_label, desc, file) -> None:
        super().__init__(name, expect_label, desc)
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


def build_injections() -> list[Injection]:
    """构造全部注入项。每项都对应一个真实会发生的漂移。"""
    G = "platform/android/app/build.gradle"
    M = "platform/android/app/src/main/AndroidManifest.xml"
    C = "CMakeLists.txt"
    R = "platform/android/app/src/main/res"
    return [
        # ── G-AND-1：JNI 定义 target 未被 Gradle `targets` 覆盖 ──────────
        PatchInjection(
            "G-AND-1/targets-drop-jni",
            "G-AND-1",
            "从 `targets` 移除 recon_platform_android（缺口 R ② 的原始形态）",
            G,
            "targets 'recon_core', 'recon_platform_android'",
            "targets 'recon_core'"),
        PatchInjection(
            "G-AND-1/targets-missing-key",
            "G-AND-1",
            "整个删掉 `targets` 行（Gradle 缺省行为不可控）",
            G,
            "                targets 'recon_core', 'recon_platform_android'\n",
            ""),
        PatchInjection(
            "G-AND-1/targets-empty",
            "G-AND-1",
            "`targets` 改为空串（反空约束必须拒收）",
            G,
            "targets 'recon_core', 'recon_platform_android'",
            "targets ''"),

        # ── G-AND-2：loadLibrary 对应的 target 不产 .so ─────────────────
        PatchInjection(
            "G-AND-2/core-static",
            "G-AND-2",
            "把 recon_core 的 Android 形态改回 STATIC（缺口 A 的原始形态）",
            C,
            "  set(RECON_CORE_LIB_TYPE SHARED)",
            "  set(RECON_CORE_LIB_TYPE STATIC)"),
        PatchInjection(
            "G-AND-2/android-facade-static",
            "G-AND-2",
            "把 recon_platform_android 改回 STATIC",
            C,
            "add_library(recon_platform_android SHARED",
            "add_library(recon_platform_android STATIC) # SHARED"),
        PatchInjection(
            "G-AND-2/loadlibrary-typo",
            "G-AND-2",
            "kt 里 loadLibrary 名字拼错（target 不存在）",
            "platform/android/app/src/main/java/com/recon/scan/ReconApplication.kt",
            'System.loadLibrary("recon_core")',
            'System.loadLibrary("recon_cor")'),

        # ── G-AND-3：Manifest 引用不存在/类型不匹配的资源 ────────────────
        PatchInjection(
            "G-AND-3/missing-string",
            "G-AND-3",
            "Manifest 引用未定义的 @string",
            M,
            'android:label="@string/app_name"',
            'android:label="@string/app_nam"'),
        PatchInjection(
            "G-AND-3/mipmap-in-drawable",
            "G-AND-3",
            "把图标改到 drawable 类型（第四十二轮真实事故形态）",
            M,
            'android:icon="@mipmap/ic_launcher"',
            'android:icon="@drawable/ic_launcher"'),
        PatchInjection(
            "G-AND-3/style-dotted-name",
            "G-AND-3",
            "把主题名改错（同时验证带点号的资源名解析）",
            M,
            'android:theme="@style/Theme.Recon"',
            'android:theme="@style/Theme.Reconn"'),
        PatchInjection(
            "G-AND-3/class-missing",
            "G-AND-3",
            "Manifest 声明了不存在的 Application 类",
            M,
            'android:name=".ReconApplication"',
            'android:name=".ReconApplicationX"'),

        # ── G-AND-4：Kotlin import 的 androidx 分组未声明 ────────────────
        PatchInjection(
            "G-AND-4/dep-dropped-activity",
            "G-AND-4",
            "删掉 androidx.activity 依赖（缺口 R ③ 的原始形态）",
            G,
            "    implementation 'androidx.activity:activity-ktx:1.9.3'\n",
            ""),
        # ⚠️ 本项曾写成「只删 androidx.core:core 一行」—— 实测**闸门不报错，
        #    而这是正确行为**。原因（POM 实证，第四十三轮）：
        #    `androidx.core:core-ktx:1.13.1` 的 POM **明确声明**
        #    `androidx.core:core:1.13.1`（scope=compile），故 core 是传递获得的；
        #    只删显式那一行，`androidx.core` 分组仍被 core-ktx 覆盖。
        #    当时的「假绿」是**注入项假设错误**，不是闸门失效。
        #    故改为删掉**两条** core 声明（同一逻辑变更：不再直接依赖 core）。
        #    证据：dl.google.com/dl/android/maven2/androidx/core/core-ktx/1.13.1/
        #          core-ktx-1.13.1.pom
        PatchInjection(
            "G-AND-4/dep-dropped-core",
            "G-AND-4",
            "删掉两条 androidx.core 声明（core-ktx 与 core）",
            G,
            "    implementation 'androidx.core:core-ktx:1.13.1'\n"
            "    implementation 'androidx.core:core:1.13.1'\n",
            ""),
        PatchInjection(
            "G-AND-4/dep-dropped-lifecycle",
            "G-AND-4",
            "删掉 androidx.lifecycle 依赖（lifecycleScope 的承载者）",
            G,
            "    implementation 'androidx.lifecycle:lifecycle-runtime-ktx:2.8.7'\n",
            ""),

        # ── G-AND-5：扫描器失效（反假绿）────────────────────────────────
        DeleteFileInjection(
            "G-AND-5/res-missing",
            "G-AND-5",
            "res/ 整体缺失（闸门必须硬失败而非恒真通过）",
            f"{R}/values/themes.xml"),
        DeleteFileInjection(
            "env/cmake-missing",
            "env",
            "根 CMakeLists.txt 缺失 → 环境问题必须硬失败（rc=1）",
            C),
    ]


def run_gate(root: pathlib.Path, timeout: int = 120) -> tuple[int, str]:
    """在 root（副本仓库）里跑闸门，返回 (rc, 输出)。"""
    proc = subprocess.run(
        [sys.executable, str(root / "tools" / "check_android_contract.py")],
        cwd=str(root), capture_output=True, text=True, timeout=timeout)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="对 Android 契约闸门做负向注入证伪")
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

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="recon-andneg-"))
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
            elif inj.expect_label not in out:
                fail += 1
                failures.append(
                    f"{inj.name}：拒收了但**理由不命中** `{inj.expect_label}`"
                    f"（rc={rc}）—— 可能被别的判据兜住，属假通过")
                print(f"  ✗ {inj.name}：rc={rc} 但未命中 {inj.expect_label}")
                if args.verbose:
                    print("    ---- 输出 ----")
                    print("\n".join(out.splitlines()[:15]))
            else:
                ok += 1
                print(f"  ✓ {inj.name} → rc={rc}，命中 {inj.expect_label}")
                if args.verbose:
                    for ln in out.splitlines():
                        if inj.expect_label in ln:
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
