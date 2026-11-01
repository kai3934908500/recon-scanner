#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""abi_drift_check.py — C ABI 契约漂移检测闸门。

背景
----
`docs/01_接口契约.md` §2.1 草案声明了 37 个 C ABI 函数，而真实头文件
`core/include/recon/recon_c_api.h` 只有 21 个（交集 14，草案独占 23，实现独占 7）。
2026-10-31 已裁定「以真实头文件为准」，并把 §2.1 / §2.3 标注为「未采纳草案」。
见 docs/01 的「契约漂移裁定」锚点。

问题：该裁定没有任何机制在**再次漂移**时报警。本脚本补上这个闸门。

设计原则（遵守项目铁律）
------------------------
1. **不硬编码数字**：函数集合直接从两个文件实时提取；锚点里声明的
   「21 / 37 / 交集 14 / only-doc 23 / only-real 7」全部**现算**，再与锚点核对。
2. **不以 docs/01 草案当真源**：真相源是真实头文件。草案只用于「对照」。
3. **无替代机制检测**：扫描 docs/ 中除 01 外的文档，若引用到**草案独占**的函数名，
   说明下游已假设了裁定明确禁止存在的能力 → 报警（exit 3）。
4. **POD 布局一致性**：裁定声称「POD 两边一致，差异仅在函数层」。脚本抽取两侧
   的 `typedef struct recon_*` 名单对比，若出现只在一侧的结构体 → 报警。

退出码
------
0 = 一致（无漂移）
1 = 用法错误
3 = 检测到漂移（函数集合变化 / 下游引用了草案独占函数 / POD 结构体不一致 /
    锚点声明数字与实测不符）
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

# ---------------------------------------------------------------------------
# 已裁定基线（2026-10-31）
# ---------------------------------------------------------------------------
# 这些是**已知且已被裁定接受**的差异，脚本不再对它们报警；只有「基线之外」的
# 新增/消失才视为真漂移。基线必须与 docs/01 锚点逐条对应。
#
# 草案独占结构体（真实头文件中不存在）—— 锚点 §2 裁定第 3 条：
KNOWN_DRAFT_ONLY_STRUCTS = {
    "recon_context",              # 真实侧用裸 void*
    "recon_progress",             # 真实侧只有 recon_progress_cb（回调指针）
    "recon_callbacks",
    "recon_reconstruction_options",
    "recon_export_report",
    "recon_frame_quality",
    "recon_param_value",
    "recon_param_issue",
    "recon_calibration_request",
}
# 实现独占结构体（草案中不存在）
# `recon_probe_data`：2026-11-01 缺口 AC 新增（见 docs/01 §2 追加裁定）。
#   它是**平台层上报事实**的 POD（平台 → core），与 `recon_capability`
#   （core 判定结果，core → 平台）方向相反、字段不同，两者不得混用。
#   不递增 RECON_ABI_VERSION：属「新增函数/结构体（追加）」，未改动任何
#   已存在 POD 布局与函数签名（docs/01 §1.3）。
KNOWN_REAL_ONLY_STRUCTS: set[str] = {
    "recon_probe_data",
}

# 实现独占函数（真实头文件有、草案无）—— 锚点 §2 裁定表格：
KNOWN_ONLY_REAL_FUNCS = {
    "recon_abi_version",
    "recon_apply_preset",
    "recon_calibrate_scale",
    "recon_export_params",
    "recon_get_param",
    "recon_probe_capability",
    "recon_set_param",
    # 2026-11-01 缺口 AC：平台层上报能力事实（全局 / 幂等）。
    #   **它必须保持在 only-real 一侧** —— 因为草案里没有对应函数，这个
    #   「不对称」正是缺口 AC 的存在证据。若哪天它进了交集，说明有人
    #   往草案里补了同名函数，那时应改为递增 ABI 版本并走完整流程。
    "recon_set_probe_data",
}
# 草案独占函数（真实头文件无）—— 锚点 §2 裁定 A/B/C 三类：
KNOWN_ONLY_DOC_FUNCS = {
    # A 类：纯命名/形态差异（12）
    "recon_get_abi_version",
    "recon_get_capability",
    "recon_run_scale_calibration",
    "recon_get_scale_result",
    "recon_params_set",
    "recon_params_get",
    "recon_params_get_effective",
    "recon_params_get_group",
    "recon_params_list_keys",
    "recon_params_apply_preset",
    "recon_params_reset",
    "recon_params_validate",
    # B 类：由回调入参覆盖（4）
    "recon_get_progress",
    "recon_set_callbacks",
    "recon_get_last_error",
    "recon_set_log_level",
    # C 类：真实缺口（7）
    "recon_get_export_report",
    "recon_export_validate",
    "recon_resume_session",
    "recon_get_frame_quality",
    "recon_get_mesh_attribute",
    "recon_get_pipeline_version",
    "recon_abort_capture",
}

# ---------------------------------------------------------------------------
# 错误码基线（2026-10-31 错误码表裁定，见 docs/01 §4 锚点）
# ---------------------------------------------------------------------------
# 真实头文件 `enum recon_status` 当前实现 47 个码（无 RECON_W_* 告警枚举）。
# 脚本对「基线外的新增/消失」报警；对已裁定状态不重复喊狼。
# 下游文档引用的、头文件未定义的码 = 待实现清单（docs/01 §4.20），
# 脚本会统计并提示，但**不作为硬失败**（因为这是已披露的规格-实现差距）。
KNOWN_ERROR_CODES = {
    # 通用
    "RECON_OK", "RECON_ERR_INVALID_ARGUMENT", "RECON_ERR_OUT_OF_MEMORY",
    "RECON_ERR_NOT_IMPLEMENTED", "RECON_ERR_INTERNAL", "RECON_ERR_CANCELLED",
    "RECON_ERR_IO", "RECON_ERR_UNSUPPORTED_PLATFORM", "RECON_ERR_VERSION_MISMATCH",
    "RECON_ERR_NO_CAPABILITY",
    # 采集
    "RECON_ERR_CAPTURE_CAMERA_UNAVAILABLE", "RECON_ERR_CAPTURE_PERMISSION_DENIED",
    "RECON_ERR_CAPTURE_FRAME_DROPPED", "RECON_ERR_CAPTURE_STALLED",
    "RECON_ERR_CAPTURE_INTERRUPTED",
    # 同步
    "RECON_ERR_SYNC_TIME_NONMONOTONIC", "RECON_ERR_SYNC_CLOCK_DRIFT",
    "RECON_ERR_SYNC_IMU_GAP", "RECON_ERR_SYNC_CALIBRATION_MISSING",
    # VIO
    "RECON_ERR_VIO_TRACKING_LOST", "RECON_ERR_VIO_NOT_INITIALIZED",
    "RECON_ERR_VIO_SCALE_UNOBSERVABLE",
    # SfM
    "RECON_ERR_SFM_INSUFFICIENT_MATCHES", "RECON_ERR_SFM_INITIALIZATION_FAILED",
    "RECON_ERR_SFM_BUNDLE_ADJUSTMENT_FAILED", "RECON_ERR_SFM_INSUFFICIENT_PARALLAX",
    # MVS
    "RECON_ERR_MVS_NO_DEPTH", "RECON_ERR_MVS_FILTER_EXHAUSTED",
    "RECON_ERR_MVS_GPU_FAILURE",
    # 深度 / RGBD
    "RECON_ERR_RGBD_NO_DEPTH", "RECON_ERR_RGBD_DEPTH_MISALIGNED",
    "RECON_ERR_RGBD_LOW_CONFIDENCE",
    # 网格
    "RECON_ERR_MESH_POISSON_FAILED", "RECON_ERR_MESH_OUT_OF_MEMORY",
    "RECON_ERR_MESH_NON_MANIFOLD_LIMIT",
    # 后处理
    "RECON_ERR_POST_HOLE_FILL_FAILED", "RECON_ERR_POST_UV_FAILED",
    "RECON_ERR_POST_TEXTURE_FAILED",
    # 标定
    "RECON_ERR_CALIB_REFERENCE_NOT_FOUND", "RECON_ERR_CALIB_DEGENERATE",
    "RECON_ERR_CALIB_NO_REFERENCE", "RECON_ERR_CALIB_INCONSISTENT",
    # 导出
    "RECON_ERR_EXPORT_WRITE_FAILED", "RECON_ERR_EXPORT_VALIDATION_FAILED",
    "RECON_ERR_EXPORT_UNSUPPORTED_FORMAT",
    # 内存
    "RECON_ERR_MEMORY_LIMIT_REACHED", "RECON_ERR_MEMORY_BLOCK_TOO_LARGE",
}


# ---------------------------------------------------------------------------
# 提取
# ---------------------------------------------------------------------------

# 真实头文件里的函数声明：RECON_API <ret> <name>(
_REAL_FN = re.compile(r"RECON_API\s+[A-Za-z_][\w\*]*\s+(recon_\w+)\s*\(")

# docs/01 草案里的函数声明：<ret> <name>(，ret ∈ {recon_status,uint32_t,void,
# int32_t,const char*,recon_context*}
_DOC_FN = re.compile(
    r"\b(recon_status|uint32_t|void|int32_t|const char\*|recon_context\*)\s+(recon_\w+)\s*\("
)

# 结构体定义（两侧都要能提取）：两种形态都要覆盖
#   1) typedef struct <name> { ... } <name>;   （有成员）
#   2) typedef struct <name> <name>;           （不透明 / opaque，如 recon_context）
_STRUCT_OPEN = re.compile(r"typedef\s+struct\s+(recon_\w+)\s*\{")
_STRUCT_OPAQUE = re.compile(r"typedef\s+struct\s+(recon_\w+)\s+recon_\w+\s*;")

# 任意 recon_* 标识符（用于扫下游引用）
_ANY_TOK = re.compile(r"\brecon_[a-z0-9_]+")

# 任意错误码 / 告警码标识符（用于扫下游引用）
_ANY_CODE = re.compile(r"\bRECON_(?:ERR|W)_[A-Z0-9_]+")


def _read(p: pathlib.Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def extract_real_functions(header: pathlib.Path) -> set[str]:
    return set(_REAL_FN.findall(_read(header)))


def extract_doc_functions(doc01: pathlib.Path) -> set[str]:
    # 只在 §2.1 的 ```c 代码块内取，避免把正文散落提及的函数名当成声明。
    text = _read(doc01)
    blocks = re.findall(r"```c\b(.*?)```", text, flags=re.DOTALL)
    fns: set[str] = set()
    for b in blocks:
        for m in _DOC_FN.finditer(b):
            fns.add(m.group(2))
    # 兜底：若代码块解析不到（格式变化），退回全文扫描并去噪。
    if not fns:
        for m in _DOC_FN.finditer(text):
            fns.add(m.group(2))
    return fns


def extract_structs(text: str) -> set[str]:
    return set(_STRUCT_OPEN.findall(text)) | set(_STRUCT_OPAQUE.findall(text))


# enum recon_status { ... } —— 提取带显式赋值 "= <num>" 的成员
_STATUS_ENUM = re.compile(r"typedef\s+enum\s+recon_status\s*\{(.*?)\}\s*recon_status\s*;",
                          re.DOTALL)
_STATUS_MEMBER = re.compile(r"\b(RECON_[A-Z0-9_]+)\s*=\s*\d+")


def extract_error_codes(header: pathlib.Path) -> set[str]:
    """从真实头文件的 enum recon_status 提取已定义错误码（仅带 = num 的）。"""
    text = _read(header)
    m = _STATUS_ENUM.search(text)
    if not m:
        return set()
    return set(_STATUS_MEMBER.findall(m.group(1)))


# docs/01 §4 表格行： | <num> | `RECON_ERR_/W_...` |
_DOC_CODE = re.compile(r"\|\s*\d+\s*\|\s*`(RECON_(?:ERR|W)_[A-Z0-9_]+)`")


def extract_doc_error_codes(doc01: pathlib.Path) -> set[str]:
    return set(_DOC_CODE.findall(_read(doc01)))


def collect_downstream_code_refs(docs_dir: pathlib.Path) -> dict[str, set[str]]:
    """收集除 docs/01 外全部文档引用的 RECON_ERR_* / RECON_W_* 及其来源文件。"""
    refs: dict[str, set[str]] = {}
    for p in sorted(docs_dir.glob("*.md")):
        if p.name.startswith("01_"):
            continue
        for c in set(_ANY_CODE.findall(_read(p))):
            refs.setdefault(c, set()).add(p.name)
    return refs


def extract_anchor_numbers(doc01: pathlib.Path) -> dict[str, int]:
    """从锚点里解析出它**声明**的数字，用于交叉核对。"""
    text = _read(doc01)
    got: dict[str, int] = {}

    m = re.search(r"函数总数\s*\|\s*\*\*(\d+)\*\*", text)
    if m:
        got["doc_total"] = int(m.group(1))
    # 真实头文件那一列
    m = re.search(r"函数总数\s*\|.*?\|\s*\*\*(\d+)\*\*", text)
    if m:
        got["real_total"] = int(m.group(1))
    m = re.search(r"交集大小\s*=\s*(\d+)", text)
    if m:
        got["intersect"] = int(m.group(1))
    m = re.search(r"草案独占的\s*\*\*(\d+)\s*个函数\*\*", text)
    if m:
        got["only_doc"] = int(m.group(1))
    # 结构体侧（缺口 AC 追加：让「新增跨语言边界」除集合级对照外，还有一条
    # 独立的锚点数字交叉核对 —— 集合级基线两侧同源，单靠它无法证明「人确实
    # 在 docs/01 里登记过」；这两条数字来自锚点正文，改代码不改锚点即失败）。
    m = re.search(r"真实头文件定义\s*\*\*(\d+)\s*个\*\*", text)
    if m:
        got["real_struct_total"] = int(m.group(1))
    m = re.search(r"草案\*\*另独占\s*(\d+)\s*个\*\*结构体", text)
    if m:
        got["draft_only_struct"] = int(m.group(1))
    return got


# ---------------------------------------------------------------------------
# 检查
# ---------------------------------------------------------------------------


def run(args: argparse.Namespace) -> int:
    root = pathlib.Path(args.root).resolve()
    header = root / "core/include/recon/recon_c_api.h"
    doc01 = root / "docs/01_接口契约.md"
    docs_dir = root / "docs"

    for p, label in ((header, "头文件"), (doc01, "docs/01")):
        if not p.is_file():
            print(f"[abi-drift] 找不到{label}: {p}", file=sys.stderr)
            return 1

    errors: list[str] = []
    warnings: list[str] = []

    real = extract_real_functions(header)
    doc = extract_doc_functions(doc01)
    inter = real & doc
    only_real = sorted(real - doc)
    only_doc = sorted(doc - real)

    print(f"[abi-drift] 真实头文件函数 : {len(real)}")
    print(f"[abi-drift] docs/01 草案函数 : {len(doc)}")
    print(f"[abi-drift] 交集             : {len(inter)}")
    print(f"[abi-drift] 实现独占(only-real) : {len(only_real)}  {only_real}")
    print(f"[abi-drift] 草案独占(only-doc)  : {len(only_doc)}")

    # --- 检查 1：函数集合对照「已裁定基线」（集合级，不比总数） ----------
    # 总数是代理指标：同时加一个 only-real、删一个 only-doc 时总数不变但集合变了。
    # 故此处做**集合级**对照。
    cur_only_real = set(only_real)
    cur_only_doc = set(only_doc)
    new_only_real_fn = sorted(cur_only_real - KNOWN_ONLY_REAL_FUNCS)
    gone_only_real_fn = sorted(KNOWN_ONLY_REAL_FUNCS - cur_only_real)
    new_only_doc_fn = sorted(cur_only_doc - KNOWN_ONLY_DOC_FUNCS)
    gone_only_doc_fn = sorted(KNOWN_ONLY_DOC_FUNCS - cur_only_doc)

    if new_only_real_fn:
        errors.append(
            f"真实头文件出现基线之外的新函数 {new_only_real_fn} —— 要么补进 docs/01"
            f" 契约、要么从实现移除；不得静默新增跨语言边界"
        )
    if gone_only_real_fn:
        errors.append(
            f"基线的实现独占函数消失 {gone_only_real_fn} —— 属破坏性变更，"
            f"必须递增 RECON_ABI_VERSION 并更新基线"
        )
    if new_only_doc_fn:
        warnings.append(
            f"草案出现基线之外的新独占函数 {new_only_doc_fn} —— 若非有意，文档已漂移"
        )
    if gone_only_doc_fn:
        warnings.append(
            f"基线中的草案独占函数消失 {gone_only_doc_fn} —— 若实现侧已补上，"
            f"请更新基线常量为佳"
        )

    # --- 检查 2：专用数量与锚点声明一致（防止改了一侧忘了改锚点） -----------
    declared = extract_anchor_numbers(doc01)
    if declared:
        checks = [
            ("real_total", len(real), "锚点声明的真实头文件函数总数"),
            ("doc_total", len(doc), "锚点声明的草案函数总数"),
            ("intersect", len(inter), "锚点声明的交集大小"),
            ("only_doc", len(only_doc), "锚点声明的草案独占数"),
        ]
        for key, actual, label in checks:
            if key in declared and declared[key] != actual:
                errors.append(
                    f"{label} = {declared[key]}，实测 = {actual}（锚点与代码不一致，"
                    f"要么代码漂移、要么锚点待更新）"
                )
    else:
        warnings.append("未能在锚点中解析出声明数字，跳过数量交叉核对")

    # --- 检查 2：下游文档不得引用「草案独占」函数 --------------------------
    # （裁定明确：UI 侧不得假设这些函数存在）
    offenders: dict[str, set[str]] = {}
    for p in sorted(docs_dir.glob("*.md")):
        if p.name.startswith("01_"):
            continue
        if not args.scan_docs:
            break
        text = _read(p)
        tokens = set(_ANY_TOK.findall(text))
        hit = tokens & set(only_doc)
        if hit:
            offenders[p.name] = hit
    if offenders:
        for fname, fns in sorted(offenders.items()):
            errors.append(
                f"{fname} 引用了草案独占函数 {sorted(fns)} —— 裁定要求「UI 侧不得"
                f"假设其存在」，若确需，必须先扩展头文件并递增 ABI 版本"
            )
    elif args.scan_docs:
        print("[abi-drift] 下游文档零引用草案独占函数 ✓")

    # --- 检查 3：POD 结构体对照「已裁定基线」 -----------------------------
    # 裁定已接受：草案独占 9 个结构体（见 docs/01 锚点）。基线下，脚本只对
    # 「基线之外的新增/消失」报警 —— 那才是真漂移；对已知状态不重复喊狼。
    real_structs = extract_structs(_read(header))
    doc_structs = extract_structs(_read(doc01))
    s_only_real = real_structs - doc_structs
    s_only_doc = doc_structs - real_structs

    new_only_doc = sorted(s_only_doc - KNOWN_DRAFT_ONLY_STRUCTS)
    gone_only_doc = sorted(KNOWN_DRAFT_ONLY_STRUCTS - s_only_doc)
    new_only_real = sorted(s_only_real - KNOWN_REAL_ONLY_STRUCTS)
    gone_only_real = sorted(KNOWN_REAL_ONLY_STRUCTS - s_only_real)

    if new_only_real:
        errors.append(
            f"真实头文件出现草案未定义的**新**结构体 {new_only_real} —— 两侧 POD 已"
            f"分叉，需同步 docs/01 并评估 ABI 影响"
        )
    if new_only_doc:
        warnings.append(
            f"草案出现基线之外的**新**独占结构体 {new_only_doc} —— 若非有意，"
            f"说明文档再次漂移"
        )
    if gone_only_doc:
        warnings.append(
            f"基线中的草案独占结构体已消失 {gone_only_doc} —— 若已删除请更新基线常量"
        )
    if gone_only_real:
        warnings.append(
            f"基线中的实现独占结构体已消失 {gone_only_real} —— 若已删除请更新基线常量"
        )

    # --- 检查 3b：结构体数量与锚点声明一致（集合级基线两侧同源，需独立交叉核对）--
    # 为什么必须有这一条：`KNOWN_REAL_ONLY_STRUCTS` 是**脚本内**常量，若允许它
    # 无声跟随实现变化，则「在头文件里塞一个新结构体 + 往白名单里抄一遍」
    # 就能让闸门全绿 —— 那就等于闸门不存在。锚点数字在 docs/01 正文里，
    # 改代码不改锚点必然对不上。
    if declared:
        if "real_struct_total" in declared and declared["real_struct_total"] != len(real_structs):
            errors.append(
                f"锚点声明的真实头文件结构体总数 = {declared['real_struct_total']}，"
                f"实测 = {len(real_structs)}（锚点与代码不一致，要么代码漂移、"
                f"要么锚点待更新）"
            )
        if "draft_only_struct" in declared and declared["draft_only_struct"] != len(s_only_doc):
            errors.append(
                f"锚点声明的草案独占结构体数 = {declared['draft_only_struct']}，"
                f"实测 = {len(s_only_doc)}（锚点与代码不一致）"
            )
    if not (new_only_real or new_only_doc):
        print(
            f"[abi-drift] POD 结构体对照基线通过（共有 {len(real_structs & doc_structs)} 个；"
            f"草案独占 {len(s_only_doc)} 个，与基线一致={set(s_only_doc)==set(KNOWN_DRAFT_ONLY_STRUCTS)}）✓"
        )

    # --- 检查 4：错误码集合对照「已裁定基线」 -----------------------------
    real_codes = extract_error_codes(header)
    new_codes = sorted(real_codes - KNOWN_ERROR_CODES)
    gone_codes = sorted(KNOWN_ERROR_CODES - real_codes)
    if new_codes:
        errors.append(
            f"头文件出现基线之外的**新**错误码 {new_codes} —— 新增跨语言错误码必须在 "
            f"docs/01 §4 登记并更新本脚本基线"
        )
    if gone_codes:
        errors.append(
            f"基线的错误码消失 {gone_codes} —— 属破坏性变更，ABI 已声明「永不重排/复用」，"
            f"如确需删除请标记废弃并更新基线"
        )
    if not (new_codes or gone_codes):
        print(f"[abi-drift] 错误码对照基线通过（头文件实现 {len(real_codes)} 个）✓")

    # --- 检查 5：下游文档引用「未定义错误码」的统计（提示，非硬失败） -------
    # 这是已披露的「规格-实现差距」（docs/01 §4.20），故仅统计提示。
    if args.scan_docs:
        code_refs = collect_downstream_code_refs(docs_dir)
        undef = {c: f for c, f in code_refs.items() if c not in real_codes}
        print(
            f"[abi-drift] 下游文档引用错误码 {len(code_refs)} 个，其中头文件未定义 "
            f"{len(undef)} 个（属已披露的规格-实现差距，见 docs/01 §4.20，不判失败）"
        )
        if args.strict_codes and undef:
            for c, fs in sorted(undef.items()):
                errors.append(f"下游文档引用未定义错误码 {c}（来源 {sorted(fs)}）")

    # --- 结论 -------------------------------------------------------------
    for w in warnings:
        print(f"[abi-drift] WARN: {w}")
    if errors:
        for e in errors:
            print(f"[abi-drift] ERROR: {e}", file=sys.stderr)
        print(f"[abi-drift] 检测到 {len(errors)} 项漂移", file=sys.stderr)
        return 3
    print("[abi-drift] PASS：C ABI 契约与裁定锚点一致")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="C ABI 契约漂移检测闸门")
    ap.add_argument("--root", default=".", help="仓库根目录（含 core/ docs/）")
    ap.add_argument(
        "--scan-docs",
        action="store_true",
        default=True,
        help="扫描 docs/ 下游文档是否引用草案独占函数（默认开）",
    )
    ap.add_argument(
        "--no-scan-docs",
        dest="scan_docs",
        action="store_false",
        help="关闭下游文档扫描",
    )
    ap.add_argument(
        "--strict-codes",
        action="store_true",
        help="把「下游文档引用未定义错误码」也视为错误（默认仅统计提示）",
    )
    return run(ap.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
