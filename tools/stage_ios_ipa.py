#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: BSD-3-Clause
"""
stage_ios_ipa.py —— 把归档出来的 Recon.app 打成**未签名 IPA** 并校验产物结构。

契约：docs/12_构建与打包.md §4.8（签名 / App Store Connect / TestFlight）
      docs/12 §4.7（设备架构：arm64 only，XC-3「模拟器 slice 不进提审包」）
      docs/12 §4.4（Metal 着色器编译与嵌入 —— metallib 必须在包根）
      docs/11 §1.5（PrivacyInfo.xcprivacy 必须存在于 Recon.app 根目录）

────────────────────────────────────────────────────────────────────────────
为什么需要本文件（而不是把三条 shell 命令写进 YAML）
────────────────────────────────────────────────────────────────────────────
`xcodebuild -exportArchive` 是 Apple **唯一官方**的「archive → IPA」途径，但它
要求 `-exportOptionsPlist` 且**强制走签名** —— 没有 Apple 开发者证书时直接失败。
本项目 CI 上**没有也不应该有** Apple 凭据（零网络策略 + 「CI 只做未签名构建」
的分工），故必须走**手工打包**这条未签名路径：

    mkdir -p Payload
    cp -R Recon.app Payload/
    zip -r Recon-unsigned.ipa Payload

把这三条直接写进 YAML 有两个结构性问题：

  ① **判据落不到目标对象上**。YAML 里没有任何地方「读回 IPA 并验证结构」，
     于是「包里到底对不对」没有任何机器在看。而这恰恰是最需要看的一类：
     本项目已两次踩到「编译过、CI 绿、产物也在包里，但运行期永远加载不到」
     —— metallib 曾在 iOS 扁平 bundle 上落到 `Recon.app/Recon.app/Resources/`
     （缺口见 G15），locale 曾因资源被展平而全表缺失。这两类**都没有编译期
     信号**，只有「读产物」才能发现。
  ② **`zip` 在 Windows 上不存在**。本项目的开发机是 Windows（无 Xcode / 无
     `zip` / 无 `lipo`），而铁律要求**每条判据都必须能负向注入证伪** ——
     证伪必须在开发机上跑得起来。故打包与校验都收敛到本文件，纯标准库实现。

────────────────────────────────────────────────────────────────────────────
判据（全部落在 **IPA 与 App 二进制** 上，不落脚本自述）
────────────────────────────────────────────────────────────────────────────
  P-1  ZIP 顶层结构合法：恰好一个 `Payload/`，其下恰好一个 `*.app`；
       不得出现 `__MACOSX/`（macOS 的 `zip` 会塞进来，真机上无用且增大体积）。
       ⇒ 也顺带证明「IPA 就是 ZIP 容器 + Payload/<Name>.app」这条契约。
  P-2  `Payload/<Name>.app/Info.plist` 存在且可被 plistlib 解析
       （archive 产物是 binary plist），且 `CFBundleExecutable` /
       `CFBundleIdentifier` 非空。
  P-3  主可执行文件是 **Mach-O**，架构集合**恰好 = {arm64}**，
       **不得含 x86_64**（含它即等于把模拟器 slice 打进了包 —— XC-3）。
       自研 Mach-O 头解析（含 FAT），并在 `lipo` 可用时**交叉验证**。
  P-4  `default.metallib` **存在且非空** —— 这是缺口 G15 修法在**产物**上的
       验证。编译期无法发现它落错位置，只有读包能发现。
  P-5  包内**不得存在 `Contents/`** —— iOS 是扁平 bundle。
  P-6  `CFBundleIdentifier` 与 `project.yml` 的 `PRODUCT_BUNDLE_IDENTIFIER`
       一致（两份事实来源对账；改了 project.yml 却忘了重新生成 / 归档时，
       这里立刻抓到）。
  P-7  包内**不得存在 `_CodeSignature/`** —— 这是「未签名」的正向定义。
       若它出现，说明 `CODE_SIGNING_ALLOWED=NO` 没生效（命令被改过）。
  P-8  **locale 落位与代码查找方式一致**：解析 `Localization.swift` 的
       `loadAll(...)` 用哪个 `subdirectory:`，就要求包内对应目录下存在
       **磁盘上全部 20 个 `<locale>.json`**。
       ⚠️ 这条是本文件最有价值的一条：XcodeGen 默认 `type: group` 会把资源
       **展平到 bundle 根**，而代码按 `subdirectory: "locales"` 查找 ——
       不匹配时 20 个 locale 全部落进 `LocaleCatalog.missingFiles`，
       UI 全部回退到键名，**且无任何报错**（与本项目既往「静默失效」同构）。
  P-9  `PrivacyInfo.xcprivacy` 位于**包根**（docs/11 §1.5 / docs/12 §4.8）。

────────────────────────────────────────────────────────────────────────────
可被证伪（`--self-test`）
────────────────────────────────────────────────────────────────────────────
`--self-test` 现场合成一个**结构正确的** `.app`（真实 Mach-O64 头、binary
plist、metallib、20 个 locale json、PrivacyInfo），打成 IPA 断言 P-1..P-9 全过；
然后**逐条**破坏一个事实，断言**恰好对应的那条判据**拒收。
⇒ 每条判据都不是恒真的：把判定写反 / 漏掉某条 / 正则零命中（退化成"没东西
   可查所以通过"）都会立刻被自检抓到。

用法
────
  # 归档后打包并校验（CI 主路径）
  python3 tools/stage_ios_ipa.py stage \
      --app build/ios/Recon.xcarchive/Products/Applications/Recon.app \
      --out build/ios/Recon-unsigned.ipa

  # 只校验一个已有 IPA
  python3 tools/stage_ios_ipa.py verify --ipa build/ios/Recon-unsigned.ipa

  # 自检（合成产物 + 逐条证伪）
  python3 tools/stage_ios_ipa.py --self-test

退出码：0 通过；1 用法/环境错误；3 判据不满足。
"""

from __future__ import annotations

import argparse
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT_YML = os.path.join(ROOT, "platform", "ios", "Container", "project.yml")
LOCALIZATION_SWIFT = os.path.join(
    ROOT, "ui", "ios", "Recon", "Sources", "Localization", "Localization.swift")
LOCALES_DIR = os.path.join(ROOT, "ui", "ios", "Recon", "Resources", "locales")

# Mach-O 头魔数（按各自字节序读出的整数值）
MH_MAGIC_64 = 0xFEEDFACF      # 小端 64 位
MH_MAGIC = 0xFEEDFACE         # 小端 32 位
MH_CIGAM_64 = 0xCFFAEDFE      # 大端 64 位
MH_CIGAM = 0xCEFAEDFE         # 大端 32 位
FAT_MAGIC = 0xCAFEBABE        # 大端 FAT（含 32 位与 64 位 fat_arch）
FAT_MAGIC_64 = 0xCAFEBABF

# CPU 类型（cputype 字段；64 位 ABI 的高位标记必须带上）
CPU_ARM64 = 0x0100000C
CPU_X86_64 = 0x01000007
CPU_I386 = 0x00000007
CPU_ARM = 0x0000000C
CPU_NAMES = {
    CPU_ARM64: "arm64",
    CPU_X86_64: "x86_64",
    CPU_I386: "i386",
    CPU_ARM: "arm",
}


class MachOError(Exception):
    """不是 Mach-O，或格式不被本解析器支持（fail-closed，绝不静默跳过）。"""


# ===========================================================================
# Mach-O 架构解析（自研；可被 --self-test 证伪；lipo 可用时交叉验证）
# ===========================================================================
def parse_macho_cputypes(data: bytes) -> list[int]:
    """返回 Mach-O 文件里出现的 CPU 类型列表（FAT 会返回全部 slice）。

    ⚠️ 解析失败必须**抛错**而不是返回空列表：空列表会被上层当成「没有架构
    约束」而静默放行 —— 那正是"判据退化成恒真"的形态（本项目在
    tools/elf_load_align.py 里已为同一类问题写明反空约束）。
    """
    if len(data) < 8:
        raise MachOError("文件过短，不是 Mach-O")

    be32 = struct.unpack(">I", data[0:4])[0]
    le32 = struct.unpack("<I", data[0:4])[0]

    # --- 单架构 Mach-O -----------------------------------------------------
    if le32 in (MH_MAGIC_64, MH_MAGIC):
        # 头布局（小端）：magic, cputype, cpusubtype, filetype, ncmds, ...
        return [struct.unpack_from("<I", data, 4)[0]]
    if be32 in (MH_CIGAM_64, MH_CIGAM):
        return [struct.unpack_from(">I", data, 4)[0]]

    # --- FAT（多架构）------------------------------------------------------
    if be32 in (FAT_MAGIC, FAT_MAGIC_64):
        fat_arch_size = 32 if be32 == FAT_MAGIC_64 else 20
        nfat = struct.unpack_from(">I", data, 4)[0]
        if nfat == 0:
            raise MachOError("FAT 头声明 0 个架构")
        need = 8 + nfat * fat_arch_size
        if len(data) < need:
            raise MachOError(
                f"FAT 头声明 {nfat} 个架构，但文件不足 {need} 字节")
        out: list[int] = []
        for i in range(nfat):
            off = 8 + i * fat_arch_size
            out.append(struct.unpack_from(">I", data, off)[0])
        return out

    raise MachOError(f"未识别的魔数（前 4 字节：{data[0:4].hex()}）")


def macho_archs(path: str) -> set[str]:
    """→ 架构名集合。非 Mach-O 或未知 cputype 一律抛错。"""
    with open(path, "rb") as fh:
        head = fh.read(4096)
    types = parse_macho_cputypes(head)
    names: set[str] = set()
    for t in types:
        if t not in CPU_NAMES:
            raise MachOError(f"未知 cputype 0x{t:08X}")
        names.add(CPU_NAMES[t])
    return names


def lipo_archs(path: str) -> set[str] | None:
    """若 PATH 上有 `lipo`，用它独立算一遍（两种实现互证）。

    返回 None 表示 lipo 不可用（**降级为提示，不是失败** —— 本工具自足，
    lipo 只是佐证。与 tools/elf_load_align.py 对 readelf 的做法一致。
    """
    if shutil.which("lipo") is None:
        return None
    try:
        out = subprocess.run(
            ["lipo", "-archs", path],
            check=True, capture_output=True, text=True, timeout=30)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError):
        return None
    return set(out.stdout.split())


# ===========================================================================
# 源码真源解析（判据必须与**代码事实**同源，不引入第二份清单）
# ===========================================================================
def parse_bundle_id_from_project_yml(path: str = PROJECT_YML) -> str:
    """从 project.yml（生成物）取 PRODUCT_BUNDLE_IDENTIFIER。"""
    text = _read_text(path)
    m = re.search(r'^\s*PRODUCT_BUNDLE_IDENTIFIER:\s*"?([^"\s#]+)"?\s*$',
                  text, re.MULTILINE)
    if not m:
        raise MachOError("project.yml 里找不到 PRODUCT_BUNDLE_IDENTIFIER")
    return m.group(1)


def parse_locales_subdirectory(path: str = LOCALIZATION_SWIFT) -> str:
    """从 Localization.swift 的 `loadAll` 取 `subdirectory:` 实参。

    fail-closed：解析不到就抛错。否则「改了查找方式」会让本判据悄悄失去
    目标（判据范围没跟上代码位置）—— 那属于假闸门。
    """
    text = _read_text(path)
    m = re.search(r'subdirectory:\s*"([^"]*)"', text)
    if not m:
        raise MachOError(
            f"{os.path.relpath(path, ROOT)} 里找不到 `subdirectory: \"...\"` —— "
            f"locale 查找方式已变（或改成了非字面量），须同步更新本判据")
    return m.group(1)


def disk_locale_files(dir_path: str = LOCALES_DIR) -> list[str]:
    """磁盘真源：`Resources/locales/` 下的全部 `<locale>.json`。"""
    if not os.path.isdir(dir_path):
        raise MachOError(f"找不到 locale 目录 {os.path.relpath(dir_path, ROOT)}")
    names = sorted(n for n in os.listdir(dir_path) if n.endswith(".json"))
    if not names:
        raise MachOError(
            f"{os.path.relpath(dir_path, ROOT)} 下 0 个 .json —— "
            f"解析式零命中，判据会退化成恒真")
    return names


def _read_text(path: str) -> str:
    with open(path, "r", encoding="utf-8") as fh:
        return fh.read()


# ===========================================================================
# IPA 打包
# ===========================================================================
def zip_dir_to_ipa(app_dir: str, out_ipa: str, app_name: str) -> None:
    """Payload/<app_name> ← app_dir，再整包 zip。

    用标准库 zipfile 而不是外部 `zip`：开发机（Windows）没有 `zip`，而判据的
    负向注入必须在开发机上能跑（铁律）。ZIP 路径一律用 `/`（ZIP 规范），
    目录条目带尾斜杠，权限位由 ZipInfo.from_file 从磁盘 st_mode 带上
    （`ZipFile.write` 内部即用它）—— App 主可执行文件的可执行位不能丢。
    """
    out_dir = os.path.dirname(os.path.abspath(out_ipa))
    os.makedirs(out_dir, exist_ok=True)
    payload = os.path.join(out_dir, "_payload_stage", "Payload")
    if os.path.isdir(os.path.join(out_dir, "_payload_stage")):
        shutil.rmtree(os.path.join(out_dir, "_payload_stage"))
    os.makedirs(payload, exist_ok=True)
    dest_app = os.path.join(payload, app_name)
    # symlinks=True：.app 内可能有符号链接（如框架的 Versions/Current）；
    # copystat 保留权限位（可执行位是 iOS 启动的必要条件）。
    shutil.copytree(app_dir, dest_app, symlinks=True)

    tmp_ipa = out_ipa + ".tmp"
    with zipfile.ZipFile(tmp_ipa, "w", zipfile.ZIP_DEFLATED) as zf:
        base = os.path.join(out_dir, "_payload_stage")
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames.sort()
            arc_root = os.path.relpath(dirpath, base).replace(os.sep, "/")
            if arc_root != ".":
                zf.write(dirpath, arc_root + "/")
            for fn in sorted(filenames):
                full = os.path.join(dirpath, fn)
                arc = (fn if arc_root == "." else f"{arc_root}/{fn}")
                zf.write(full, arc)
    os.replace(tmp_ipa, out_ipa)
    shutil.rmtree(os.path.join(out_dir, "_payload_stage"), ignore_errors=True)


# ===========================================================================
# 校验（P-1 .. P-9）—— 判据全部落在 IPA 与 App 二进制上
# ===========================================================================
class Result:
    def __init__(self) -> None:
        self.errors: list[str] = []
        self.notes: list[str] = []

    def err(self, code: str, msg: str) -> None:
        self.errors.append(f"[{code}] {msg}")

    def note(self, msg: str) -> None:
        self.notes.append(msg)


def verify_ipa(ipa_path: str, rep: Result) -> None:
    if not os.path.isfile(ipa_path):
        rep.err("P-0", f"IPA 不存在：{ipa_path}")
        return
    if not zipfile.is_zipfile(ipa_path):
        rep.err("P-1", f"{ipa_path} 不是合法 ZIP —— IPA 必须是 ZIP 容器")
        return

    with zipfile.ZipFile(ipa_path) as zf:
        names = [n for n in zf.namelist() if n and not n.endswith("/")]
        dirs = [n for n in zf.namelist() if n.endswith("/")]

        # ---- P-1 顶层结构 -------------------------------------------------
        tops = {n.split("/", 1)[0] for n in names}
        if "__MACOSX" in tops:
            rep.err("P-1", "包内出现 `__MACOSX/` —— 打包用了 macOS 的 `zip`，"
                           "会塞入 AppleDouble 垃圾条目（真机无用、徒增体积）")
        tops.discard("__MACOSX")
        if tops != {"Payload"}:
            rep.err("P-1", f"ZIP 顶层应为恰好 `Payload/`，实际为 {sorted(tops)}")
            return
        app_dirs = sorted({n.split("/")[1] for n in names
                           if n.startswith("Payload/") and "/" in n[len("Payload/"):]})
        if len(app_dirs) != 1:
            rep.err("P-1", f"`Payload/` 下应恰好一个 .app，实际 {app_dirs}")
            return
        app_name = app_dirs[0]
        if not app_name.endswith(".app"):
            rep.err("P-1", f"`Payload/` 下的实体 `{app_name}` 不以 .app 结尾")
            return
        root = f"Payload/{app_name}"
        rep.note(f"IPA 结构：Payload/{app_name}（{len(names)} 个文件）")

        def has(rel: str) -> bool:
            return f"{root}/{rel}" in names

        # ---- P-5 扁平 bundle（先判，因为它决定后续路径直觉）-------------
        if any(n.startswith(f"{root}/Contents/") for n in names):
            rep.err("P-5", f"包内出现了 `{root}/Contents/` —— iOS 是**扁平** "
                           f"bundle，该层级不存在（macOS 才有）")

        # ---- P-7 未签名 ---------------------------------------------------
        if any(n.startswith(f"{root}/_CodeSignature/") for n in names):
            rep.err("P-7", f"包内出现 `{root}/_CodeSignature/` —— 这不是未签名 "
                           f"产物，说明 CODE_SIGNING_ALLOWED=NO 未生效")

        # ---- P-2 Info.plist -----------------------------------------------
        plist_name = f"{root}/Info.plist"
        if plist_name not in names:
            rep.err("P-2", f"缺少 `{plist_name}`")
            return
        try:
            info = plistlib.loads(zf.read(plist_name))
        except Exception as exc:  # noqa: BLE001 — 任何解析失败都是判据失败
            rep.err("P-2", f"`{plist_name}` 无法解析为 plist：{exc}")
            return
        exe = info.get("CFBundleExecutable")
        bundle_id = info.get("CFBundleIdentifier")
        if not isinstance(exe, str) or not exe:
            rep.err("P-2", "Info.plist 的 CFBundleExecutable 为空")
            return
        if not isinstance(bundle_id, str) or not bundle_id:
            rep.err("P-2", "Info.plist 的 CFBundleIdentifier 为空")
            return

        # ---- P-3 主可执行文件架构 ------------------------------------------
        exe_name = f"{root}/{exe}"
        if exe_name not in names:
            rep.err("P-3", f"Info.plist 指向的可执行文件 `{exe_name}` 不在包内")
        else:
            with tempfile.TemporaryDirectory() as td:
                tmp = os.path.join(td, exe)
                with open(tmp, "wb") as fh:
                    fh.write(zf.read(exe_name))
                _check_arch(tmp, rep)
                lp = lipo_archs(tmp)
                if lp is not None:
                    try:
                        mine = macho_archs(tmp)
                    except MachOError as exc:
                        rep.err("P-3", f"自研解析失败：{exc}")
                    else:
                        if mine != lp:
                            rep.err("P-3", f"架构解析不一致：自研 {sorted(mine)} "
                                           f"vs lipo {sorted(lp)}（独立实现互证失败）")
                        else:
                            rep.note(f"架构经 lipo 交叉验证一致：{sorted(lp)}")

        # ---- P-4 metallib --------------------------------------------------
        mb = f"{root}/default.metallib"
        if mb not in names:
            rep.err("P-4", f"缺少 `{mb}` —— MetalProbe() 会诚实报「找不到 "
                           f"default.metallib」，GPU 路径永久降级（缺口 G15 的产物面）")
        elif len(zf.read(mb)) == 0:
            rep.err("P-4", f"`{mb}` 存在但为 0 字节 —— 空 metallib 会被 "
                           f"makeDefaultLibrary() 当成「库在但无函数」，比缺失更难查")

        # ---- P-6 Bundle ID 对账 --------------------------------------------
        try:
            expected_id = parse_bundle_id_from_project_yml()
        except MachOError as exc:
            rep.err("P-6", str(exc))
        else:
            if bundle_id != expected_id:
                rep.err("P-6", f"Bundle ID 漂移：包内为 `{bundle_id}`，"
                               f"project.yml 声明为 `{expected_id}`")
            else:
                rep.note(f"Bundle ID 与 project.yml 一致：{bundle_id}")

        # ---- P-8 locale 落位 -----------------------------------------------
        try:
            subdir = parse_locales_subdirectory()
            wanted = disk_locale_files()
        except MachOError as exc:
            rep.err("P-8", str(exc))
        else:
            prefix = f"{root}/{subdir}/" if subdir else f"{root}/"
            missing = [n for n in wanted if f"{prefix}{n}" not in names]
            if missing:
                where = f"`{root}/{subdir or ''}/`" if subdir else f"`{root}/`"
                rep.err("P-8",
                        f"locale 落位不符：代码按 subdirectory=\"{subdir}\" 查找，"
                        f"但 {where} 下缺 {len(missing)}/{len(wanted)} 个文件"
                        f"（例：{missing[:3]}）—— LocaleCatalog 会把它们全部判为"
                        f"缺失，UI 整体回退到键名，且**没有任何报错**。"
                        f"根因通常是 XcodeGen 的 `type: group` 把资源展平到包根。")
            else:
                rep.note(f"locale 落位正确：{root}/{subdir}/ 下 {len(wanted)} 个 json 齐全")

        # ---- P-9 PrivacyInfo -----------------------------------------------
        priv = f"{root}/PrivacyInfo.xcprivacy"
        if priv not in names:
            rep.err("P-9", f"缺少 `{priv}` —— docs/11 §1.5 / docs/12 §4.8 要求它"
                           f"位于 App 包**根目录**（上架校验项）")


def _check_arch(exe_path: str, rep: Result) -> None:
    try:
        archs = macho_archs(exe_path)
    except MachOError as exc:
        rep.err("P-3", f"主可执行文件不是可解析的 Mach-O：{exc}")
        return
    if archs != {"arm64"}:
        rep.err("P-3", f"架构应**恰好**为 {{'arm64'}}，实际为 {sorted(archs)}"
                       f"（XC-3：模拟器 slice 不得进入提包）")
    else:
        rep.note("主可执行文件架构：arm64（唯一，符合 XC-3）")


def run_stage(app_dir: str, out_ipa: str, app_name: str | None) -> int:
    if not os.path.isdir(app_dir):
        print(f"错误：--app 指向的目录不存在：{app_dir}", file=sys.stderr)
        return 1
    if app_name is None:
        app_name = os.path.basename(app_dir.rstrip("/\\"))
    if not app_name.endswith(".app"):
        print(f"错误：App 目录名 `{app_name}` 不以 .app 结尾", file=sys.stderr)
        return 1
    print(f"== 打包未签名 IPA ==")
    print(f"   App    : {app_dir}")
    print(f"   IPA    : {out_ipa}")
    zip_dir_to_ipa(app_dir, out_ipa, app_name)
    print(f"   大小   : {os.path.getsize(out_ipa)} bytes")
    print()
    return run_verify(out_ipa)


def run_verify(ipa_path: str) -> int:
    rep = Result()
    verify_ipa(ipa_path, rep)
    print("=" * 68)
    print("未签名 IPA 产物校验（tools/stage_ios_ipa.py）")
    print("=" * 68)
    for n in rep.notes:
        print(f"  说明  {n}")
    if rep.errors:
        print()
        for e in rep.errors:
            print(f"  失败  {e}")
        print()
        print(f"结论：{len(rep.errors)} 项违规 —— IPA 未通过。")
        return 3
    print()
    print("结论：P-1 容器结构 / P-2 Info.plist / P-3 仅 arm64 / P-4 metallib 在包根 / "
          "P-5 无 Contents 层 / P-6 Bundle ID 对账 / P-7 未签名 / P-8 locale 落位 / "
          "P-9 PrivacyInfo 在包根 —— 全部通过。")
    return 0


# ===========================================================================
# 自检：合成一个结构正确的 .app，逐条破坏并断言**恰好对应**的判据拒收
# ===========================================================================
def _synth_macho64(cputype: int) -> bytes:
    """合成最小但**真实布局**的 Mach-O 64 头（magic/cputype/... 共 16 字节）。"""
    return struct.pack("<IIIIIIII", MH_MAGIC_64, cputype, 0, 2, 0, 0, 0, 0)


def _synth_fat(cputypes: list[int]) -> bytes:
    n = len(cputypes)
    out = struct.pack(">II", FAT_MAGIC, n)
    off = 8 + n * 20
    for ct in cputypes:
        # cputype, cpusubtype, offset, size, align（全大端）
        out += struct.pack(">IIIII", ct, 0, off, 0x100, 14)
    return out


def _build_synthetic_app(base: str, *, bundle_id: str = "cn.qingfeng.recon",
                         exe: str = "Recon", cputype: int = CPU_ARM64,
                         metallib_bytes: bytes = b"\xbf\xba\x00metallib",
                         locales_subdir: str = "locales",
                         privacy_root: bool = True,
                         contents_layer: bool = False,
                         code_signature: bool = False) -> str:
    app = os.path.join(base, f"{exe}.app")
    os.makedirs(app, exist_ok=True)
    with open(os.path.join(app, "Info.plist"), "wb") as fh:
        plistlib.dump({"CFBundleExecutable": exe,
                       "CFBundleIdentifier": bundle_id,
                       "CFBundleName": exe,
                       "CFBundlePackageType": "APPL"}, fh,
                      fmt=plistlib.FMT_BINARY)
    with open(os.path.join(app, exe), "wb") as fh:
        fh.write(_synth_macho64(cputype))
    if metallib_bytes is not None:
        with open(os.path.join(app, "default.metallib"), "wb") as fh:
            fh.write(metallib_bytes)
    loc_dir = os.path.join(app, locales_subdir) if locales_subdir else app
    os.makedirs(loc_dir, exist_ok=True)
    for name in disk_locale_files():
        with open(os.path.join(loc_dir, name), "w", encoding="utf-8") as fh:
            fh.write('{"msg.x": "y"}')
    if privacy_root:
        with open(os.path.join(app, "PrivacyInfo.xcprivacy"), "w",
                  encoding="utf-8") as fh:
            fh.write("<plist/>")
    if contents_layer:
        d = os.path.join(app, "Contents", "Resources")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "junk"), "w", encoding="utf-8") as fh:
            fh.write("x")
    if code_signature:
        d = os.path.join(app, "_CodeSignature")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "CodeResources"), "w", encoding="utf-8") as fh:
            fh.write("x")
    return app


def _stage_synthetic(base: str, app: str, ipa: str) -> None:
    zip_dir_to_ipa(app, ipa, os.path.basename(app))


def _rewrite_ipa(src_ipa: str, dst_ipa: str, *,
                 drop_prefix: str | None = None,
                 extra: list[tuple[str, bytes]] | None = None) -> str:
    """在合法 IPA 上做**外科式**改动（删一段 / 加伪造条目），返回 dst_ipa。

    P-1 / P-2 这两条判据的注入无法用 `_build_synthetic_app` 的参数表达
    （它们判的是 ZIP 容器本身而非 .app 内容），故需要一个能直接编辑 ZIP
    往返的辅助函数。
    """
    with zipfile.ZipFile(src_ipa) as zin, \
            zipfile.ZipFile(dst_ipa, "w", zipfile.ZIP_DEFLATED) as zout:
        for it in zin.infolist():
            if drop_prefix and it.filename.startswith(drop_prefix):
                continue
            zout.writestr(it, zin.read(it.filename))
        for name, data in (extra or []):
            zout.writestr(name, data)
    return dst_ipa


def self_test() -> int:
    print("== stage_ios_ipa.py 自检（合成产物 + 逐条证伪）==")
    fails: list[str] = []
    n_cases = 0

    with tempfile.TemporaryDirectory() as td:
        # --- 基线：结构正确 → 必须全过 ---
        good = _build_synthetic_app(os.path.join(td, "good"))
        good_ipa = os.path.join(td, "good.ipa")
        _stage_synthetic(td, good, good_ipa)
        rep = Result()
        verify_ipa(good_ipa, rep)
        if rep.errors:
            fails.append(f"基线应当通过，却报错：{rep.errors}")
            print(f"  基线：失败 {rep.errors}")
        else:
            print("  基线：P-1..P-9 全部通过 ✓")

        def from_kwargs(name: str, **kw):
            app = _build_synthetic_app(os.path.join(td, name), **kw)
            ipa = os.path.join(td, f"{name}.ipa")
            _stage_synthetic(td, app, ipa)
            return ipa

        def fat_case(name: str):
            app = _build_synthetic_app(os.path.join(td, name))
            with open(os.path.join(app, "Recon"), "wb") as fh:
                fh.write(_synth_fat([CPU_ARM64, CPU_X86_64]))
            ipa = os.path.join(td, f"{name}.ipa")
            _stage_synthetic(td, app, ipa)
            return ipa

        # 每条判据恰好一项注入。name → (期望判据, 产出 ipa 的 callable)
        cases: list[tuple[str, str, object]] = [
            ("P-1-出现__MACOSX", "P-1",
             lambda: _rewrite_ipa(good_ipa, os.path.join(td, "p1macosx.ipa"),
                                  extra=[("__MACOSX/._Recon", b"x")])),
            ("P-1-顶层多出非Payload条目", "P-1",
             lambda: _rewrite_ipa(good_ipa, os.path.join(td, "p1top.ipa"),
                                  extra=[("iTunesArtwork", b"x")])),
            ("P-2-缺Info.plist", "P-2",
             lambda: _rewrite_ipa(good_ipa, os.path.join(td, "p2.ipa"),
                                  drop_prefix="Payload/Recon.app/Info.plist")),
            ("P-3-可执行文件不在包内", "P-3",
             lambda: _rewrite_ipa(good_ipa, os.path.join(td, "p3miss.ipa"),
                                  drop_prefix="Payload/Recon.app/Recon")),
            ("P-3-单架构x86_64", "P-3",
             lambda: from_kwargs("p3x86", cputype=CPU_X86_64)),
            ("P-3-FAT含arm64+x86_64", "P-3",
             lambda: fat_case("p3fat")),
            ("P-4-缺失metallib", "P-4",
             lambda: from_kwargs("p4miss", metallib_bytes=None)),
            ("P-4-空metallib", "P-4",
             lambda: from_kwargs("p4empty", metallib_bytes=b"")),
            ("P-5-出现Contents层", "P-5",
             lambda: from_kwargs("p5", contents_layer=True)),
            ("P-6-BundleID漂移", "P-6",
             lambda: from_kwargs("p6", bundle_id="com.wrong.id")),
            ("P-7-出现_CodeSignature", "P-7",
             lambda: from_kwargs("p7", code_signature=True)),
            ("P-8-locale被展平到包根", "P-8",
             lambda: from_kwargs("p8", locales_subdir="")),
            ("P-9-PrivateInfo不在包根", "P-9",
             lambda: from_kwargs("p9", privacy_root=False)),
        ]

        for name, expect, make in cases:
            n_cases += 1
            ipa = make()  # type: ignore[operator]
            rep = Result()
            verify_ipa(ipa, rep)
            got = sorted({e.split("]")[0][1:] for e in rep.errors})
            if got != [expect]:
                fails.append(f"{name}：期望恰好 {expect} 拒收，实际 "
                             f"{got or ['无错']}")
                print(f"  {name}：未按预期拒收（实际 {got or ['无错']}）✗")
            else:
                print(f"  {name}：{expect} 已拒收 ✓")

    if fails:
        print()
        for f in fails:
            print(f"  ::error:: {f}")
        print(f"\n[negative] 失败：{len(fails)}/{n_cases} 项未按预期拒收")
        return 3
    print(f"\n[negative] PASS：基线通过 + {n_cases} 项注入全部被正确拒收"
          f"（P-1 / P-2 / P-3 / P-4 / P-5 / P-6 / P-7 / P-8 / P-9 判据均有效）")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="把 Recon.app 打成未签名 IPA 并校验产物结构（docs/12 §4.8）")
    ap.add_argument("--self-test", action="store_true",
                    help="合成产物并逐条证伪（不依赖任何构建）")
    sub = ap.add_subparsers(dest="cmd")

    p_stage = sub.add_parser("stage", help="打包 IPA 并立即校验")
    p_stage.add_argument("--app", required=True, help="归档出的 .app 目录")
    p_stage.add_argument("--out", required=True, help="输出 IPA 路径")
    p_stage.add_argument("--app-name", default=None,
                         help="Payload 内的 .app 名（默认取 --app 的basename）")

    p_verify = sub.add_parser("verify", help="只校验已有 IPA")
    p_verify.add_argument("--ipa", required=True, help="待校验的 IPA")

    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if args.cmd == "stage":
        return run_stage(args.app, args.out, args.app_name)
    if args.cmd == "verify":
        return run_verify(args.ipa)
    ap.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
