#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""第三方依赖许可扫描器。

项目合规硬性要求：所有依赖仅允许 MIT / Apache-2.0 / BSD 系，
彻底规避 GPL / AGPL / LGPL 等传染性协议。

本工具在 CI 中作为独立前置任务运行，退出码非 0 即阻断构建。

用法：
    python tools/license_scan.py --third-party third_party
    python tools/license_scan.py --third-party third_party --json report.json

检测阶段：
    1. 目录枚举：定位所有 third_party 下的依赖目录
    2. 许可文件扫描：读取 LICENSE/COPYING/COPYRIGHT 等文件，
       检测传染性协议关键词
    3. 登记一致性：核对每个依赖是否在 VENDOR.yaml 中登记，
       且登记的许可证与实际文件一致
    4. 产物侧交叉校验：扫描构建产物中是否残留被禁库的符号
    5. 生成 JSON 报告
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from typing import Dict, List, Optional, Tuple

# 传染性协议关键词。命中即阻断。
# 注意：MPL-2.0 不在此列——它是文件级 copyleft，非传染型，
# 作为白名单边界项 B-1 处理（需法务签字），见 docs/10_许可与合规.md §4。
FORBIDDEN_PATTERNS: List[Tuple[str, str]] = [
    ("GPL-3.0", r"GNU\s+General\s+Public\s+License\s+.*(?:version\s*3|v3)"),
    ("GPL-3.0", r"GNU\s+GENERAL\s+PUBLIC\s+LICENSE\s*\n?\s*Version\s*3"),
    ("GPL-2.0", r"GNU\s+General\s+Public\s+License\s+.*(?:version\s*2|v2)"),
    ("AGPL-3.0", r"GNU\s+Affero\s+General\s+Public\s+License"),
    ("AGPL", r"GNU\s+AGPL"),
    ("LGPL-3.0", r"GNU\s+Lesser\s+General\s+Public\s+License\s+.*(?:version\s*3|v3)"),
    ("LGPL-2.1", r"GNU\s+Lesser\s+General\s+Public\s+License\s+.*(?:version\s*2\.1|v2\.1)"),
    # docs/12 §2.2 强命中表还列了下列**字面写法**，此前脚本漏掉：
    ("LGPL (Library)", r"GNU\s+Library\s+General\s+Public\s+License"),
    ("GPLv2", r"\bGPLv2\b"),
    ("GPLv3", r"\bGPLv3\b"),
    ("LGPLv2", r"\bLGPLv2\b"),
    ("LGPLv3", r"\bLGPLv3\b"),
    ("AGPLv3", r"\bAGPLv3\b"),
    ("AGPL-3+", r"AGPL-3"),
    ("FIPL", r"Free\s+Image\s+Public\s+License"),
    ("Commons Clause", r"Commons\s+Clause"),
    ("SSPL", r"Server\s+Side\s+Public\s+License"),
    ("Business Source License", r"Business\s+Source\s+License"),
    ("Elastic License", r"Elastic\s+License"),
    ("CC-BY-NC", r"CC-BY-NC"),
]

# 允许的许可证标识。
ALLOWED_LICENSES = {
    "MIT",
    "Apache-2.0",
    "BSD-2-Clause",
    "BSD-3-Clause",
    "BSD-3-Clause-Clear",
    "BSL-1.0",
    "MPL-2.0",   # 边界项 B-1，需法务签字
    "Zlib",
    "Unlicense",
    "CC0-1.0",
    "0BSD",
}

# 需要法务签字的白名单边界项。
BOUNDARY_APPROVAL = {
    "MPL-2.0": "Eigen：文件级 copyleft，非传染。需法务签字，见 docs/10 §4。",
}

# 许可文件名候选。
LICENSE_FILE_NAMES = [
    "LICENSE",
    "LICENSE.txt",
    "LICENSE.md",
    "LICENCE",
    "LICENCE.txt",
    "COPYING",
    "COPYING.txt",
    "COPYRIGHT",
    "LICENSE-APACHE",
    "LICENSE-MIT",
]

# 已知依赖的许可证指纹。用于登记一致性核对。
KNOWN_LICENSES: Dict[str, str] = {
    "ceres": "BSD-3-Clause",
    "eigen": "MPL-2.0",
    "opencv": "Apache-2.0",
    "colmap": "BSD-3-Clause",
    "poselib": "BSD-3-Clause",
    "poisson_recon": "MIT",
    # 上游原始下载包（PoissonRecon V7.0，MIT，mkazhdan 2015）。
    # 集成到 third_party/poisson_recon/ 的是本项目的子集 + 4 条 patch
    # （见 third_party/poisson_recon/PATCHES.md），原始包保留以备复现。
    "poissonrecon-master": "MIT",
    "flann": "BSD-3-Clause",
    "nanoflann": "BSD-3-Clause",
    "basalt": "BSD-3-Clause",
    "dbow2": "BSD-3-Clause",
    "xatlas": "MIT",
    "jsoncpp": "MIT",
}

# 必须在分发前物理删除的传染性协议文件 / 子树（COLMAP 源码树内）。
#
# 依据：
#   · docs/10_许可与合规.md §5.2 的 X-1 / X-2 / X-3 三条硬性排除要求；
#   · docs/12_构建与打包.md §2.4 / §2.4.1 的实现。
# 这些路径若存在于 vendor 后的 colmap 树中，即构成许可违规。
MUST_DELETE_FILES = [
    "scripts/python/plyfile.py",          # GPL-3+
    "debian",                             # GPL-2+
    "src/thirdparty",                     # X-2/X-3：内含 LSD(AGPL-3+) 与 SiftGPU(UNC 非商业)
    "src/colmap/sensor/bitmap.h",         # X-1：唯一引入 FreeImage(FIPL/GPL 双许可)
    "src/colmap/sensor/bitmap.cc",        # X-1
]

# X-2/X-3 的额外显式路径（覆盖 dep 前缀不确定的情形，如 colmap/ 或 colmap-3.10.0/）。
MUST_DELETE_SUFFIXES = [
    "/src/thirdparty",                    # LSD + SiftGPU 所在
    "/src/colmap/sensor/bitmap.h",
    "/src/colmap/sensor/bitmap.cc",
]

# 产物侧不得出现的符号/字符串（对应 docs/12 §4.4–4.6）。
FORBIDDEN_ARTIFACT_SYMBOLS = [
    ("freeimage", "X-1: FreeImage 为 FIPL/GPL 双许可，不得进入产物"),
    ("siftgpu", "X-3: SiftGPU 为 UNC 非商业许可，不得进入产物"),
    ("lsd_", "X-2: LSD 为 AGPL-3+，不得进入产物"),
    ("gpl", "GPL 传染性符号残留"),
    ("cgal", "CGAL 为 GPL/商业双许可，CGAL_ENABLED 应为 OFF"),
]

# ---------------------------------------------------------------------------
# 「四表一致」校验（docs/12 §2.1 D-2；docs/10 §6 检查项 2）
#
#   表 A = docs/10_许可与合规.md §2.1
#   表 B = docs/12_构建与打包.md   §2.1
#   表 C = cmake/ThirdParty.cmake  RECON_THIRD_PARTY
#   表 D = cmake/Licenses.cmake    （顺序派生自 C，本工具校验「无独立列表」）
#
# 表 C 是唯一真源；A/B 是展示表。三者不一致即失败。
# ---------------------------------------------------------------------------

# 文档展示名 → 登记目录名。归一化后仍不相等时查此表。
#
# 为什么需要：docs 用人类可读名（"Ceres Solver"），ThirdParty.cmake 用
# 目录名（ceres）。归一化（小写 + 去非字母数字）能解决大部分
# （"PoissonRecon"→poissonrecon 对 "poisson_recon"→poissonrecon），
# 但 "Ceres Solver"→ceressolver 无法自动对上，必须显式登记别名。
REGISTRY_NAME_ALIASES = {
    "ceressolver": "ceres",
    "poissonrecon": "poisson_recon",
}

# 登记表的字段数（cmake/ThirdParty.cmake 的表头注释即契约）。
REGISTRY_TABLE_FIELDS = {
    "RECON_THIRD_PARTY": 8,
    "RECON_THIRD_PARTY_BOUNDARY_ITEMS": 9,
    "RECON_THIRD_PARTY_BUILD_ONLY": 4,
    "RECON_THIRD_PARTY_SUPPORT_DIRS": 3,
}

# 黑名单名称 / 许可标识：**不得出现在任何登记段**（含 boundary_items、
# build_only、support_dirs）。对应 docs/10 §3 黑名单表与 docs/00 §8.4 的
# 「CI 强制校验」声明、docs/10 §6 检查项 3。
# 一旦有人在登记表里加了 GPL/AGPL/LGPL/FIPL 系依赖，这里立刻拦下。
FORBIDDEN_REGISTRY_TOKENS = [
    "gpl", "agpl", "lgpl", "fipl",       # 传染性许可族
    "cgal",                              # GPL/商业双许可，CGAL_ENABLED 必须 OFF
    "freeimage", "siftgpu",              # X-1 / X-3 非白名单
    "lsd",                               # X-2 AGPL-3+
    "sspl", "commons clause", "elastic license",
    "business source license", "cc-by-nc",
    "qt5", "qt6", "onnxruntime", "faiss",  # docs/12 §4.1 禁入产物
]

# 交付期离线运行时不联网：本工具**不**跑 cmake 也能完成登记一致性校验。
# 只有 --verify-licenses-cmake 需要 cmake 可执行（缺省则记 info，不阻断）。


class Finding:
    """单条扫描结论。"""

    def __init__(self, severity: str, check: str, path: str, message: str):
        self.severity = severity  # "error" | "warning" | "info"
        self.check = check
        self.path = path
        self.message = message

    def to_dict(self) -> dict:
        return {
            "severity": self.severity,
            "check": self.check,
            "path": self.path,
            "message": self.message,
        }


def find_license_files(root: str) -> List[str]:
    """在目录树中查找许可文件。"""
    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        # 跳过构建产物与版本库目录。
        dirnames[:] = [d for d in dirnames if d not in (".git", "build", ".cache")]
        for fn in filenames:
            if fn in LICENSE_FILE_NAMES:
                found.append(os.path.join(dirpath, fn))
    return found


def scan_forbidden(third_party: str) -> List[Finding]:
    """阶段 2：扫描许可文件中的传染性协议关键词。"""
    findings = []
    for path in find_license_files(third_party):
        try:
            with open(path, "r", encoding="utf-8", errors="ignore") as fh:
                content = fh.read()
        except OSError:
            continue

        upper = content.upper()
        for license_name, pattern in FORBIDDEN_PATTERNS:
            if re.search(pattern, content, re.IGNORECASE | re.MULTILINE):
                # 区分「本文件就是该协议」与「仅提及」。
                is_the_license = upper.startswith("GNU") or "AFFERO" in upper
                findings.append(
                    Finding(
                        "error" if is_the_license else "warning",
                        "forbidden_license",
                        path,
                        f"检测到 {license_name} 相关文本。"
                        + ("该文件本身即为传染性协议。" if is_the_license
                           else "仅为提及，需人工确认。"),
                    )
                )
    return findings


def detect_license_id(path: str) -> Optional[str]:
    """从许可文件内容推断许可证标识。"""
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as fh:
            content = fh.read()
    except OSError:
        return None

    head = content[:4000]
    if "MIT License" in head or "Permission is hereby granted, free of charge" in head:
        return "MIT"
    if "Apache License" in head and "Version 2.0" in head:
        return "Apache-2.0"
    if "Mozilla Public License Version 2.0" in head:
        return "MPL-2.0"
    if "Boost Software License" in head:
        return "BSL-1.0"
    if "Redistribution and use in source and binary forms" in head:
        return "BSD-3-Clause"
    return None


def registry_expected_licenses(registry_path: Optional[str]) -> Dict[str, str]:
    """从登记真源取「目录名 → license_spdx」。

    否则回退到 KNOWN_LICENSES（内置兜底表）。
    **设计原则**：登记表是唯一真源；KNOWN_LICENSES 与之重复，仅在未传
    `--registry` 的旧调用方式下使用，避免「同一逻辑写两遍必然漂移」。
    """
    if registry_path:
        reg = parse_registry(registry_path)
        if reg:
            rows = reg.get("RECON_THIRD_PARTY") or []
            out: Dict[str, str] = {}
            for fields, err in rows:
                if err:
                    continue
                out[fields[0].lower()] = fields[2]
            if out:
                return out
    return {k: v for k, v in KNOWN_LICENSES.items()}


def check_registration(third_party: str,
                       registry_path: Optional[str] = None) -> List[Finding]:
    """阶段 3：核对依赖与登记的许可证一致性。

    期望值优先来自 `--registry`（唯一真源）；缺省回退 KNOWN_LICENSES。
    """
    findings = []
    if not os.path.isdir(third_party):
        return findings

    expected_table = registry_expected_licenses(registry_path)

    # 支持目录（非依赖）：仅在登记真源中登记了用途，不参与依赖许可证核对，
    # 但仍要过阶段 2（黑名单内容扫描）与 check_must_delete。
    support_names: set = set()
    if registry_path:
        reg = parse_registry(registry_path) or {}
        for fields, err in (reg.get("RECON_THIRD_PARTY_SUPPORT_DIRS") or []):
            if not err:
                support_names.add(_canonical_registry_name(fields[0]))

    for dep in sorted(os.listdir(third_party)):
        dep_path = os.path.join(third_party, dep)
        if not os.path.isdir(dep_path):
            continue

        if _canonical_registry_name(dep) in support_names:
            continue

        expected = expected_table.get(dep.lower())
        if expected is None:
            findings.append(
                Finding("warning", "unknown_dependency", dep_path,
                        "依赖未在登记真源中登记，请补充 cmake/ThirdParty.cmake 条目。")
            )
            continue

        # 源码内联依赖必须有 VENDOR.yaml（docs/12 §2.2）。
        # 当前处于「尚未 vendor」阶段，缺 VENDOR.yaml 记 warning 而非 error；
        # 一旦登记表的 commit 字段被填满（vendor 完成），本项应升为 error。
        if not os.path.isfile(os.path.join(dep_path, "VENDOR.yaml")):
            findings.append(
                Finding("warning", "missing_vendor_yaml", dep_path,
                        "依赖缺少 VENDOR.yaml（docs/12 §2.2）。"
                        "vendor 阶段完成后必须补齐，届时本项升为 error。")
            )

        license_files = [
            os.path.join(dep_path, fn) for fn in LICENSE_FILE_NAMES
            if os.path.isfile(os.path.join(dep_path, fn))
        ]
        if not license_files:
            findings.append(
                Finding("error", "missing_license", dep_path,
                        "依赖缺少 LICENSE 文件，无法确认许可证。")
            )
            continue

        detected = None
        for lf in license_files:
            detected = detect_license_id(lf)
            if detected:
                break

        if detected is None:
            findings.append(
                Finding("warning", "undetected_license", license_files[0],
                        f"无法自动识别许可证，期望 {expected}，需人工确认。")
            )
        elif detected != expected:
            findings.append(
                Finding("error", "license_mismatch", license_files[0],
                        f"登记许可证 {expected} 与实际检测 {detected} 不一致。")
            )
    return findings


def check_must_delete(third_party: str) -> List[Finding]:
    """检查必须物理删除的传染性协议文件 / 子树是否仍存在。

    两条判据同时生效：
      · MUST_DELETE_FILES：相对路径精确匹配（含目录）。
      · MUST_DELETE_SUFFIXES：路径后缀匹配，覆盖 vendor 目录名不确定的情形。
    """
    findings = []
    for rel in MUST_DELETE_FILES:
        path = os.path.join(third_party, rel)
        if os.path.exists(path):
            findings.append(
                Finding("error", "must_delete", path,
                        "该文件/子树含传染性协议（GPL/AGPL）或非商业许可，"
                        "必须在分发前物理删除（见 docs/12 §2.4.1）。")
            )

    if not os.path.isdir(third_party):
        return findings
    for dirpath, _dirnames, filenames in os.walk(third_party):
        norm = dirpath.replace(os.sep, "/")
        for suffix in MUST_DELETE_SUFFIXES:
            if norm.endswith(suffix):
                findings.append(
                    Finding("error", "must_delete", dirpath,
                            f"该子树匹配禁止路径 {suffix}（见 docs/12 §2.4.1）。")
                )
        for fn in filenames:
            rel = dirpath.replace(os.sep, "/") + "/" + fn
            for suffix in MUST_DELETE_SUFFIXES:
                if rel.endswith(suffix):
                    findings.append(
                        Finding("error", "must_delete", rel,
                                f"该文件匹配禁止路径 {suffix}（见 docs/12 §2.4.1）。")
                    )
    return findings


def check_boundary_approval(third_party: str,
                            registry_path: Optional[str] = None) -> List[Finding]:
    """检查白名单边界项（MPL-2.0）是否登记了法务签字。

    ⚠️ 曾经的实现只找 `third_party/VENDOR.yaml`（根级单文件），而 docs/12 §2.2
    规定 VENDOR.yaml 是**每个依赖目录一份** —— 该检查因此**永不触发**（空门）。
    现改为：以登记真源的 `RECON_THIRD_PARTY_BOUNDARY_ITEMS` 为权威清单，
    逐个校验对应依赖目录的 VENDOR.yaml 是否含 `legal_approval`。
    """
    findings: List[Finding] = []
    if not os.path.isdir(third_party) or not registry_path:
        return findings

    reg = parse_registry(registry_path)
    if reg is None:
        return findings
    rows = reg.get("RECON_THIRD_PARTY_BOUNDARY_ITEMS") or []
    if not rows:
        return findings

    for fields, err in rows:
        if err:
            findings.append(Finding("error", "boundary_field_count", registry_path,
                                    f"边界项登记字段数错误：{err}"))
            continue
        name, license_id, boundary_item, requires_signoff = fields[0], fields[1], fields[2], fields[3]
        if str(requires_signoff).strip().lower() != "true":
            continue
        vfile = os.path.join(third_party, name, "VENDOR.yaml")
        if not os.path.isfile(vfile):
            findings.append(Finding(
                "warning", "boundary_vendor_missing", vfile,
                f"边界项 {boundary_item}（{name}，{license_id}）需法务签字，"
                "但依赖目录尚无 VENDOR.yaml，签字记录无处落档（docs/12 §2.5 B1-4）。"))
            continue
        try:
            with open(vfile, "r", encoding="utf-8") as fh:
                content = fh.read()
        except OSError:
            continue
        if "legal_approval" not in content:
            findings.append(Finding(
                "error", "missing_legal_approval", vfile,
                f"边界项 {boundary_item}（{name}，{license_id}）的 VENDOR.yaml "
                f"缺少 legal_approval 记录。{BOUNDARY_APPROVAL.get(license_id, '')}"))
    return findings


def _parse_set_block(text: str, var: str) -> Optional[List[str]]:
    """从 CMake 源码中取出 `set(<var> ... )` 里的字符串列表。

    只做**保守**解析：`set(` 后到与之配对的 `)` 之间，逐行取引号内容。
    找不到该变量时返回 None（便于区分「未登记」与「登记为空」）。
    """
    m = re.search(r"\bset\(\s*" + re.escape(var) + r"\b", text)
    if not m:
        return None
    # 从 set( 之后开始，按括号配平找到结束位置。
    i = text.index("(", m.start())
    depth = 0
    end = None
    for j in range(i, len(text)):
        c = text[j]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                end = j
                break
    if end is None:
        return None
    body = text[i + 1:end]
    items: List[str] = []
    for line in body.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        stripped = line.strip('"')
        if stripped and stripped != line:
            items.append(stripped)
    return items


def _norm_name(name: str) -> str:
    """把依赖名归一化到可比较的形式（小写 + 仅保留字母数字）。"""
    return re.sub(r"[^a-z0-9]", "", name.lower())


def _canonical_registry_name(name: str) -> str:
    n = _norm_name(name)
    return REGISTRY_NAME_ALIASES.get(n, n)


def parse_registry(registry_path: str) -> Optional[dict]:
    """解析 cmake/ThirdParty.cmake。

    返回 {"REChTTP_THIRD_PARTY": [ [field0..field7], ... ],
          "RECON_THIRD_PARTY_BOUNDARY_ITEMS": [...], ...}
    文件不存在或顶层列表缺失时返回 None。
    """
    if not os.path.isfile(registry_path):
        return None
    try:
        with open(registry_path, "r", encoding="utf-8") as fh:
            text = fh.read()
    except OSError:
        return None

    out: dict = {}
    for var, nfields in REGISTRY_TABLE_FIELDS.items():
        items = _parse_set_block(text, var)
        if items is None:
            out[var] = None
            continue
        rows = []
        for it in items:
            fields = it.split("|")
            if len(fields) != nfields:
                rows.append((fields, f"字段数应为 {nfields}，实际 {len(fields)}"))
            else:
                rows.append((fields, None))
        out[var] = rows
    return out


def check_registry_consistency(registry_path: str, docs: Optional[dict]) -> List[Finding]:
    """四表一致性校验（docs/12 §2.1 D-2）。

    docs 形如 {"docs/10 §2.1": [name, ...], "third_party_dirs": [dir, ...]}；
    为 None 时跳过对应比对（文档缺失不算违规，记 info）。
    """
    findings: List[Finding] = []
    reg = parse_registry(registry_path)
    if reg is None:
        findings.append(Finding("error", "registry_missing", registry_path,
                                "登记真源不存在或无法解析，四表一致性无从校验。"))
        return findings

    main_rows = reg.get("RECON_THIRD_PARTY")
    if not main_rows:
        findings.append(Finding("error", "registry_empty", registry_path,
                                "RECON_THIRD_PARTY 未登记任何依赖。"))
        return findings

    # 字段数自检。
    names: List[str] = []
    for idx, (fields, err) in enumerate(main_rows):
        if err:
            findings.append(Finding("error", "registry_field_count", registry_path,
                                    f"RECON_THIRD_PARTY 第 {idx + 1} 条：{err}"))
            continue
        names.append(fields[0])

    if len(set(names)) != len(names):
        dupes = sorted({n for n in names if names.count(n) > 1})
        findings.append(Finding("error", "registry_duplicate", registry_path,
                                f"RECON_THIRD_PARTY 存在重复依赖名：{dupes}"))

    # 支持目录（非依赖）：third_party/ 一级目录集合许可的额外成员。
    support_rows = reg.get("RECON_THIRD_PARTY_SUPPORT_DIRS") or []
    support_names = [f[0] for f, err in support_rows if not err]

    canon_reg = {_canonical_registry_name(n) for n in names}
    canon_support = {_canonical_registry_name(d) for d in support_names}
    if len(support_names) > 3:
        findings.append(Finding(
            "error", "support_dir_count", registry_path,
            f"支持目录登记数 {len(support_names)} 超过上限 3"
            "（防止用「支持目录」淹没目录集合校验，见 docs/12 §2.6.3 L-6）。"))

    # 与 third_party/ 实际目录集合比对（阶段 1.1 的另一半：目录 → 登记）。
    dirs = (docs or {}).get("third_party_dirs")
    if dirs is not None:
        canon_dirs = {_canonical_registry_name(d) for d in dirs}
        allowed = canon_reg | canon_support
        unregistered = sorted(canon_dirs - allowed)
        if unregistered:
            findings.append(Finding(
                "warning", "unregistered_dir",
                os.path.join(os.path.dirname(registry_path), "..", "third_party"),
                f"third_party/ 下存在未登记的目录：{unregistered}。"
                "转入库前需登记为依赖，或在 RECON_THIRD_PARTY_SUPPORT_DIRS 登记为支持目录。"))
        # 已 vendor 的依赖：登记了但目录不存在 → 如实记 warning（不阻断本地开发）。
        # 发布构建应为 0，故以 registry_missing_dir 单列，便于 CI 区分。
        for d in sorted(canon_reg - canon_dirs):
            findings.append(Finding(
                "warning", "registry_missing_dir", registry_path,
                f"登记依赖 '{d}' 在 third_party/ 下尚无目录（尚未 vendor，"
                "见 docs/12 §2.1 D-3）。发布前必须补齐。"))
        # 支持目录登记了但目录不存在 → 登记陈旧。
        for d in sorted(canon_support - canon_dirs):
            findings.append(Finding(
                "warning", "support_dir_stale", registry_path,
                f"支持目录 '{d}' 已登记但 third_party/ 下不存在，登记陈旧。"))

    # 与两份文档展示表比对（third_party_dirs 已经单独处理，跳过）。
    for label, doc_names in (docs or {}).items():
        if label == "third_party_dirs":
            continue
        if not isinstance(doc_names, list):
            continue
        canon_doc = {_canonical_registry_name(n) for n in doc_names}
        missing = sorted(canon_reg - canon_doc)
        extra = sorted(canon_doc - canon_reg)
        if missing or extra:
            findings.append(Finding(
                "error", "doc_registry_mismatch", label,
                f"{label} 的依赖表与 cmake/ThirdParty.cmake 不一致："
                f"登记有而文档缺 {missing}；文档有而登记缺 {extra}。"))

    # ---- 黑名单名称不得出现在**任何**登记段（docs/10 §6 检查项 3）----
    try:
        with open(registry_path, "r", encoding="utf-8") as fh:
            reg_text = fh.read()
    except OSError:
        reg_text = ""
    if reg_text:
        lowered = reg_text.lower()
        for tok in FORBIDDEN_REGISTRY_TOKENS:
            if tok in lowered:
                findings.append(Finding(
                    "error", "forbidden_in_registry", registry_path,
                    f"登记真源中出现黑名单标识 '{tok}'。"
                    "对应 docs/10 §3 黑名单与 docs/00 §8.4 的三项硬性排除；"
                    "任何登记段（含边界项/构建期/支持目录）都不得含该标识。"))

    # licenses_cmake 必须由真源派生，不得自带列表（防「同一逻辑写两遍」）。
    lic = os.path.join(os.path.dirname(registry_path), "Licenses.cmake")
    if os.path.isfile(lic):
        try:
            with open(lic, "r", encoding="utf-8") as fh:
                lic_text = fh.read()
        except OSError:
            lic_text = ""
        # 只禁止**重新登记**一条形如 "name|a|b|..." 的记录（≥2 个 `|`）；
        # 注释里提到 RECON_THIRD_PARTY_NAMES 是允许的。
        # 注意**不**锚定行首：真实写法是 `set(RECON_THIRD_PARTY "name|...")`，
        # 锚定行首会漏检（本实现中已踩到，已用注入测试证伪）。
        if re.search(r'"[a-z][a-z0-9_]*\|[^\n"]*\|[^\n"]*"', lic_text):
            findings.append(Finding(
                "error", "licenses_cmake_duplicates_registry", lic,
                "cmake/Licenses.cmake 内出现了 | 分隔的登记记录，"
                "违反「许可清单必须从 ThirdParty.cmake 派生」的约束。"))
    return findings


def parse_doc_dependency_names(md_path: str, section_anchor: str) -> Optional[List[str]]:
    """从 docs 的依赖总表（markdown）里抽取依赖名。

    启发式：定位到含 section_anchor 的标题行，向下读取紧邻的 markdown 表格，
    取每行第 2 个单元格（列 1 为序号，列 2 为依赖名），剥掉粗体与反引号。
    抽不到表格时返回 None（调用方记 info，不误报）。
    """
    if not os.path.isfile(md_path):
        return None
    try:
        with open(md_path, "r", encoding="utf-8") as fh:
            lines = fh.read().splitlines()
    except OSError:
        return None

    start = None
    for i, ln in enumerate(lines):
        if ln.lstrip("#").strip().startswith(section_anchor):
            start = i
            break
    if start is None:
        return None

    names: List[str] = []
    seen_table = False
    for ln in lines[start + 1:]:
        s = ln.strip()
        if s.startswith("#") and seen_table:
            break
        if not s.startswith("|"):
            if seen_table:
                break
            continue
        cells = [c.strip() for c in s.strip("|").split("|")]
        if len(cells) < 2:
            continue
        if set(cells[0]) <= set("-: "):  # 分隔行 |---|---|
            seen_table = True
            continue
        raw = cells[1].replace("**", "").strip().strip("`").strip()
        if raw and raw != "依赖名":
            names.append(raw)
            seen_table = True
    return names or None


def run_cmake(cmake_exe: str, script: str, out_file: str) -> Tuple[bool, str]:
    """跑 `cmake -P <script> -- <out_file>`，返回 (成功, 输出)。"""
    try:
        proc = subprocess.run(
            [cmake_exe, "-P", script, "--", out_file],
            capture_output=True, text=True, errors="ignore", timeout=180)
    except (OSError, subprocess.SubprocessError) as exc:
        return False, str(exc)
    return proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")


def verify_licenses_cmake(registry_path: str, licenses_text: str,
                          third_party: str) -> List[Finding]:
    """校验**已生成**的 THIRD_PARTY_LICENSES.txt 与 third_party/ 逐字节一致。

    ⚠️ 关键设计（勿改）：本函数**不重新生成清单**，只校验磁盘上**既有的**
    那份。曾经的实现在每次校验时当场跑 CMake 重生成再比对 —— 那是
    **自证循环**：新清单必然等于新源文件，篡改源文件也永远「通过」
    （已用「改 Jonathan Young→Younq」实测证伪，假阴性 100%）。
    正确语义来自 docs/12 §2.7「防止清单与实际脱节」：清单先由 configure
    阶段生成（或 `licenses_manifest` target），此后源文件一旦变动而未
    重生成，本校验即报红。

    结构按生成端格式解析：`===== <dep> =====` / 元信息 / `--- <file> ---`。
    """
    findings: List[Finding] = []

    if not os.path.isfile(licenses_text):
        findings.append(Finding(
            "error", "licenses_text_missing", licenses_text,
            "待校验的许可清单不存在。请先运行 CMake configure（生成到 "
            "CMAKE_BINARY_DIR）或 `cmake --build <dir> --target licenses_manifest`。"))
        return findings

    try:
        with open(licenses_text, "r", encoding="utf-8", newline="") as fh:
            manifest = fh.read()
    except OSError as exc:
        findings.append(Finding("error", "licenses_read_failed", licenses_text,
                                f"无法读取许可清单：{exc}"))
        return findings

    # CMake 的 file(WRITE) 在 Windows 上会把 \n 扩成 \r\n；归一化到 LF 后
    # 再做结构解析与正文比对（正文比对本来也按归一化口径）。
    manifest = manifest.replace("\r\n", "\n").replace("\r", "\n")

    # 结构：===== <dep> ===== / 元信息 4 行 / (--- <file> --- 全文)*
    dep_re = re.compile(r"^===== (.+?) =====$", re.MULTILINE)
    marks = list(dep_re.finditer(manifest))
    if not marks:
        findings.append(Finding("error", "licenses_manifest_empty", licenses_text,
                                "清单为空或结构不可识别，无法校验。"))
        return findings

    checked = 0
    for i, m in enumerate(marks):
        dep = m.group(1)
        if dep.startswith("尚未 vendor"):
            continue
        # 清单里的依赖名必须能在登记真源中找到（防清单含未登记依赖）。
        seg_end = marks[i + 1].start() if i + 1 < len(marks) else len(manifest)
        segment = manifest[m.end():seg_end]

        file_re = re.compile(r"^--- (.+?) ---$", re.MULTILINE)
        fmarks = list(file_re.finditer(segment))
        for k, fm in enumerate(fmarks):
            fname = fm.group(1)
            body_end = fmarks[k + 1].start() if k + 1 < len(fmarks) else len(segment)
            body = segment[fm.end():body_end]
            # `$`（MULTILINE）匹配在行尾换行**之前**，故正文区以该行换行开头。
            if body.startswith("\n"):
                body = body[1:]

            src = os.path.join(third_party, dep, fname)
            if not os.path.isfile(src):
                findings.append(Finding("error", "licenses_file_missing", src,
                                        f"清单引用了不存在的许可文件 {dep}/{fname}。"))
                continue
            try:
                with open(src, "r", encoding="utf-8", newline="") as fh:
                    raw = fh.read()
            except OSError as exc:
                findings.append(Finding("error", "licenses_file_unreadable", src,
                                        f"无法读取 {src}：{exc}"))
                continue
            raw_norm = raw.replace("\r\n", "\n").replace("\r", "\n")

            # 生成端对每个文件写入「正文 + 一个分隔用换行」。用**前缀精确
            # 匹配**而非「剥尾部换行」—— 后者依赖生成端空白数量的隐含约定，
            # 一旦格式微调即静默失配（本次实现中已踩到）。
            expected = raw_norm + "\n"
            if not body.startswith(expected):
                findings.append(Finding(
                    "error", "licenses_byte_mismatch", src,
                    f"清单中 {dep}/{fname} 的正文与源文件不一致"
                    f"（期望前 {len(expected)} 字节逐字节相同，实际 {len(body)} 字节）。"
                    "若源文件确已更新，请重新生成清单。"))
            else:
                checked += 1

    findings.append(Finding("info", "licenses_verified", licenses_text,
                            f"许可清单逐字节校验通过：{checked} 个许可文件。"))
    return findings


def check_artifact_symbols(artifact: str) -> List[Finding]:
    """阶段 4：扫描构建产物中的禁止符号 / 字符串（docs/12 §4.1–4.6）。

    用 `nm -C --defined-only` 导出符号后逐条匹配 FORBIDDEN_ARTIFACT_SYMBOLS。
    找不到 nm 或产物不存在时记 info，不阻断（允许在无工具链的环境跑）。
    """
    findings: List[Finding] = []
    if not os.path.isfile(artifact):
        findings.append(Finding("info", "artifact_missing", artifact,
                                "产物不存在，跳过符号扫描。"))
        return findings

    nm = shutil.which("nm")
    if nm is None:
        findings.append(Finding("info", "nm_unavailable", artifact,
                                "未找到 nm，跳过产物符号扫描。"))
        return findings

    try:
        proc = subprocess.run(
            [nm, "-C", "--defined-only", artifact],
            capture_output=True, text=True, errors="ignore", timeout=120)
        blob = (proc.stdout + "\n" + proc.stderr).lower()
    except (OSError, subprocess.SubprocessError) as exc:
        findings.append(Finding("info", "nm_failed", artifact,
                                f"nm 执行失败：{exc}"))
        return findings

    for token, why in FORBIDDEN_ARTIFACT_SYMBOLS:
        if token in blob:
            findings.append(Finding("error", "artifact_symbol", artifact,
                                    f"产物中出现禁止符号 '{token}'。{why}"))
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description="第三方依赖许可扫描")
    parser.add_argument("--third-party", default=None,
                        help="第三方依赖目录")
    parser.add_argument("--root", default=None,
                        help="--third-party 的别名（docs/12 §2.8 步骤 4 使用）")
    parser.add_argument("--json", "--output", dest="json",
                        help="JSON 报告输出路径（--output 为 docs/12 §2.6.1 的写法）")
    parser.add_argument("--allow-boundary", action="store_true",
                        help="允许边界项（需配合法务签字记录）")
    parser.add_argument("--scan-artifact", action="append", default=[],
                        help="额外扫描构建产物（.a/.so/可执行文件）中的禁止符号，"
                             "可重复指定")
    parser.add_argument("--registry", default=None,
                        help="cmake/ThirdParty.cmake 路径（登记一致性校验）")
    parser.add_argument("--licenses-cmake", default=None,
                        help="cmake/Licenses.cmake 路径")
    parser.add_argument("--licenses-text", default=None,
                        help="待校验的 THIRD_PARTY_LICENSES.txt 路径"
                             "（--verify-licenses-cmake；缺省自动探测构建目录）")
    parser.add_argument("--fail-on-violation", action="store_true",
                        help="检测到违规即非零退出（本工具默认行为）")
    parser.add_argument("--check-registry", action="store_true",
                        help="只做四表登记一致性校验（docs/12 §2.1 D-2）")
    parser.add_argument("--verify-licenses-cmake", action="store_true",
                        help="校验许可清单与 third_party 逐字节一致（docs/12 §2.7）")
    parser.add_argument("--skip-doc-cross-check", action="store_true",
                        help="登记校验时不比对 docs 展示表（仅本地临时使用）")
    args = parser.parse_args()

    third_party = args.third_party or args.root or "third_party"
    findings: List[Finding] = []

    # -----------------------------------------------------------------
    # 模式 A：--check-registry —— 只做四表一致性（docs/12 §2.1 D-2）
    # -----------------------------------------------------------------
    if args.check_registry:
        if not args.registry:
            print("[license_scan] --check-registry 需要 --registry cmake/ThirdParty.cmake")
            return 1
        docs: Dict[str, object] = {}
        if not args.skip_doc_cross_check:
            d10 = parse_doc_dependency_names(
                "docs/10_许可与合规.md", "2.1 白名单总表")
            d12 = parse_doc_dependency_names(
                "docs/12_构建与打包.md", "2.1 依赖总表")
            if d10 is None:
                findings.append(Finding("info", "doc_table_unparsed",
                                        "docs/10_许可与合规.md",
                                        "未能解析 §2.1 白名单总表，跳过该表比对。"))
            else:
                docs["docs/10 §2.1"] = d10
            if d12 is None:
                findings.append(Finding("info", "doc_table_unparsed",
                                        "docs/12_构建与打包.md",
                                        "未能解析 §2.1 依赖总表，跳过该表比对。"))
            else:
                docs["docs/12 §2.1"] = d12
        if os.path.isdir(third_party):
            docs["third_party_dirs"] = [
                d for d in sorted(os.listdir(third_party))
                if os.path.isdir(os.path.join(third_party, d))]
        findings.extend(check_registry_consistency(args.registry, docs))

        errors = [f for f in findings if f.severity == "error"]
        print("[license_scan] 登记一致性校验（--check-registry）")
        print(f"  真源：{args.registry}")
        print(f"  错误：{len(errors)}  警告：{len([f for f in findings if f.severity == 'warning'])}")
        for f in findings:
            mark = "ERROR" if f.severity == "error" else (
                "WARN" if f.severity == "warning" else "INFO")
            print(f"  [{mark}] {f.check}: {f.path}")
            print(f"         {f.message}")
        if args.json:
            with open(args.json, "w", encoding="utf-8") as fh:
                json.dump({"check": "registry", "findings": [f.to_dict() for f in findings]},
                          fh, ensure_ascii=False, indent=2)
        if errors:
            print("[license_scan] 失败：四表不一致（退出码 3）。")
            return 3
        print("[license_scan] 通过：四表一致。")
        return 0

    # -----------------------------------------------------------------
    # 模式 B：--verify-licenses-cmake —— 校验**已生成**的清单（docs/12 §2.7）
    # -----------------------------------------------------------------
    if args.verify_licenses_cmake:
        # 清单路径解析顺序：
        #   1) 显式 --licenses-text
        #   2) <build>/THIRD_PARTY_LICENSES.txt（--licenses-cmake 的同级/上级 build 目录）
        #   3) 常见构建目录探测
        manifest_path = args.licenses_text
        if not manifest_path:
            cands: List[str] = []
            if args.licenses_cmake:
                base = os.path.dirname(os.path.abspath(args.licenses_cmake))
                cands.append(os.path.join(base, "THIRD_PARTY_LICENSES.txt"))
            root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
            for bd in ("build_cmake", "build", "build_desktop"):
                cands.append(os.path.join(root_dir, bd, "THIRD_PARTY_LICENSES.txt"))
            for c in cands:
                if os.path.isfile(c):
                    manifest_path = c
                    break
            if not manifest_path:
                manifest_path = cands[0] if cands else "THIRD_PARTY_LICENSES.txt"

        findings.extend(verify_licenses_cmake(args.licenses_cmake or "", manifest_path,
                                              third_party))
        errors = [f for f in findings if f.severity == "error"]
        print("[license_scan] 许可清单校验（--verify-licenses-cmake）")
        print(f"  清单：{manifest_path}")
        print(f"  错误：{len(errors)}")
        for f in findings:
            mark = "ERROR" if f.severity == "error" else (
                "WARN" if f.severity == "warning" else "INFO")
            print(f"  [{mark}] {f.check}: {f.path}")
            print(f"         {f.message}")
        if args.json:
            with open(args.json, "w", encoding="utf-8") as fh:
                json.dump({"check": "licenses_cmake", "manifest": manifest_path,
                           "findings": [f.to_dict() for f in findings]},
                          fh, ensure_ascii=False, indent=2)
        if errors:
            print("[license_scan] 失败：许可清单与 third_party 不一致。")
            return 2
        print("[license_scan] 通过：许可清单逐字节一致。")
        return 0

    # -----------------------------------------------------------------
    # 模式 C：全量扫描（默认；docs/12 §2.6.2 五阶段）
    # -----------------------------------------------------------------
    if not os.path.isdir(third_party):
        print(f"[license_scan] 目录不存在：{third_party}（跳过）")
        findings.append(
            Finding("info", "missing_dir", third_party, "第三方目录不存在。")
        )
    else:
        findings.extend(scan_forbidden(third_party))
        findings.extend(check_registration(third_party, args.registry))
        findings.extend(check_must_delete(third_party))
        findings.extend(check_boundary_approval(third_party, args.registry))

    for opt_name, opt_val in (("--registry", args.registry),
                              ("--licenses-cmake", args.licenses_cmake)):
        if opt_val is not None and not os.path.isfile(opt_val):
            findings.append(
                Finding("warning", "missing_file", opt_val,
                        f"{opt_name} 指定的文件不存在。")
            )

    # 提供了 --registry 时，顺带做四表一致性（不额外要求 --check-registry）。
    if args.registry:
        docs2: Dict[str, object] = {}
        if not args.skip_doc_cross_check:
            d10 = parse_doc_dependency_names("docs/10_许可与合规.md", "2.1 白名单总表")
            d12 = parse_doc_dependency_names("docs/12_构建与打包.md", "2.1 依赖总表")
            if d10:
                docs2["docs/10 §2.1"] = d10
            if d12:
                docs2["docs/12 §2.1"] = d12
        if os.path.isdir(third_party):
            docs2["third_party_dirs"] = [
                d for d in sorted(os.listdir(third_party))
                if os.path.isdir(os.path.join(third_party, d))]
        findings.extend(check_registry_consistency(args.registry, docs2))

    for artifact in args.scan_artifact:
        findings.extend(check_artifact_symbols(artifact))

    errors = [f for f in findings if f.severity == "error"]
    warnings = [f for f in findings if f.severity == "warning"]
    registry_errors = [f for f in errors
                       if f.check in ("registry_missing", "registry_empty",
                                      "registry_field_count", "registry_duplicate",
                                      "doc_registry_mismatch",
                                      "licenses_cmake_duplicates_registry")]

    print("[license_scan] 扫描结果")
    print(f"  目录：{third_party}")
    print(f"  错误：{len(errors)}  警告：{len(warnings)}")

    for f in findings:
        mark = "ERROR" if f.severity == "error" else (
            "WARN" if f.severity == "warning" else "INFO")
        print(f"  [{mark}] {f.check}: {f.path}")
        print(f"         {f.message}")

    if args.json:
        report = {
            "schema_version": 1,
            "third_party": third_party,
            "error_count": len(errors),
            "warning_count": len(warnings),
            "findings": [f.to_dict() for f in findings],
        }
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(report, fh, ensure_ascii=False, indent=2)
        print(f"  报告：{args.json}")

    if registry_errors:
        print("[license_scan] 失败：登记不一致（退出码 3）。")
        return 3
    if errors:
        print("[license_scan] 失败：检测到传染性协议依赖，构建被阻断。")
        return 2

    print("[license_scan] 通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
