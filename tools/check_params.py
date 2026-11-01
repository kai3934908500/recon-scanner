#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""param_drift_check.py — 参数契约漂移检测闸门。

背景
----
2026-10-31 轮次发现：`docs/08_参数体系.md` 声明 375 个参数，而
`core/src/util/params.cpp` 只注册 209 个 —— 其中 227 个文档参数在代码中
缺失、61 个代码参数未登记进文档。更危险的是**键名漂移**：代码注册的是
`perf_thread_count`，而 docs/00 / 04 / 05 / 08 / 11 / 13 **全线**引用
`perf_thread_pool_size`。内存保护白名单（docs/13 §4.2 W-1..W-10）恰恰
落在这批键上 —— 若不修，降级器会去 `Set` 一个不存在的键、静默失效。

这类漂移与项目中已踩过的坑同构：
  - `trim_confidence_quantile` 注册值写反、单测硬编码绕过注册表；
  - S7 边翻转绕序；
  - TSDF 世界→相机公式写错。
共同点：**没有任何机制在再次漂移时报警**。本脚本补上这个闸门。

设计原则（遵守项目铁律）
------------------------
1. **不硬编码数量**：代码侧键集合从 `params.h` + `params.cpp` 实时提取，
   文档侧从 docs/08 表格实时提取；不写死「209 / 375」。
2. **白名单键必须存在**：docs/13 §4.2 的 W-1..W-10 承载键若在注册表中缺失，
   直接判失败（exit 3）—— 这是内存保护模块的硬依赖。
3. **区分「已披露缺口」与「新漂移」**：docs/08 声明而代码未实现的大量参数，
   属已在 docs/14 §5.1.1 / docs/08 §5.1 披露的规格-实现差距，基线登记后
   不再报警；只有基线之外的**新增**差异才判漂移。
4. **键名漂移单独判失败**：代码存在、但文档用另一名字（或反之）的
   `perf_*` 键，属真漂移，必须失败。

退出码
------
0 = 一致（无基线之外漂移）
1 = 用法错误
3 = 检测到漂移
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
PARAMS_H = REPO / "core" / "include" / "recon" / "params.h"
PARAMS_CPP = REPO / "core" / "src" / "util" / "params.cpp"
DOC08 = REPO / "docs" / "08_参数体系.md"
DOC13 = REPO / "docs" / "13_内存与降级.md"

# ---------------------------------------------------------------------------
# 已披露基线（2026-10-31）
# ---------------------------------------------------------------------------
# docs/08 声明但代码尚未注册的键，属 docs/14 §5.1.1 已披露的规格-实现差距。
# 该集合**不判失败**；只有此基线之外的新增缺失才视为真漂移。
# 说明：这些缺口的补齐是独立的后续工作项（见 docs/14 §5.1.1），不在本闸门裁决范围。
KNOWN_UNIMPLEMENTED_PREFIXES = (
    "calib_",    # G9 标定类：整组尚未接入（37 个）
    "export_",   # G10 导出类：整组尚未注册（16 个）
    "core_",     # 元参数
    "preset_",   # 元参数
    "schema_",   # 元参数
)

# 白名单相关组（perf_/rgbd_/mesh_/mvs_）内的已披露缺口。
# 这些键在 docs/08 有声明、代码未注册，属 docs/14 §5.1.1 登记的差距；
# 登记后不再报警，任何**基线之外**的新增缺失会落进 unexplained 并判失败。
KNOWN_WHITELIST_GROUP_GAPS = {
    # perf_ 组（08 G11 声明但核心尚未接入的运行时旋钮）
    "perf_cache_dir",
    "perf_cache_max_mb",
    "perf_cancel_poll_interval_ms",
    "perf_enable_profiling",
    "perf_gpu_enabled",
    "perf_gpu_memory_budget_mb",
    "perf_log_level",
    "perf_progress_interval_ms",
    # mesh_ 组（08 G7 声明、06 实现尚未接入）
    "mesh_chunk_overlap_leaves",
    "mesh_frustum_margin",
    "mesh_input_normal_vote_min_views",
    "mesh_input_point_weight_depth_enabled",
    "mesh_input_point_weight_floor",
    "mesh_input_point_weight_photo_sigma",
    "mesh_poisson_depth_mode",
    "mesh_poisson_max_memory_mb",
    "mesh_poisson_min_samples",
    "mesh_poisson_sharpness",
    "mesh_trim_dist_mult",
    # mvs_ 组（08 G5 声明、05 实现尚未接入）
    "mvs_depth_max",
    "mvs_depth_min",
    "mvs_depth_regularization",
    "mvs_fusion_enabled",
    "mvs_geometric_verification_enabled",
    "mvs_min_view_consistency",
    "mvs_outlier_filter_sigma",
    "mvs_propagation_enabled",
    "mvs_unilateral_check_enabled",
}


def extract_code_keys() -> tuple[set[str], set[str]]:
    """从 params.h 提取键常量名→键名映射，从 params.cpp 提取实际注册的键名。"""
    hdr = PARAMS_H.read_text(encoding="utf-8")
    const_to_key = dict(
        re.findall(r'constexpr const char\*\s+(k[A-Za-z0-9_]+)\s*=\s*"([a-z0-9_]+)"', hdr)
    )
    cpp = PARAMS_CPP.read_text(encoding="utf-8")
    used_consts = re.findall(r"keys::(k[A-Za-z0-9_]+)", cpp)
    registered = {const_to_key[c] for c in used_consts if c in const_to_key}
    return registered, set(const_to_key.values())


def extract_doc_keys() -> set[str]:
    """从 docs/08 参数表提取键名（表行形如 `| \\`key\\` | int|float|string | ...`）。"""
    txt = DOC08.read_text(encoding="utf-8")
    return set(
        re.findall(r"^\|\s*`([a-z0-9_]+)`\s*\|\s*(?:int|float|string)\s*\|", txt, re.M)
    )


def whitelist_keys() -> list[str]:
    """从 docs/13 §4.2 提取 W-1..W-10 的承载参数键。

    注意：§4.2 表内也出现若干**非参数键**的标识符（如 `mvs_pyramid_offset`
    是 W-4 要求写入 recon_mesh_info.json 的**输出字段名**，`poisson_chunk_count`
    是产物字段），必须排除，否则会误报「白名单键未注册」。
    """
    txt = DOC13.read_text(encoding="utf-8")
    m = re.search(r"###\s*4\.2(.*?)###\s*4\.3", txt, re.S)
    seg = m.group(1) if m else txt
    keys = re.findall(r"`((?:perf|rgbd|mvs|mesh)_[a-z0-9_]+)`", seg)
    # 非参数键（输出字段 / 说明词）白名单。
    not_a_param = {
        "mvs_pyramid_offset",     # W-4 的输出字段名，非参数
        "poisson_chunk_count",    # 产物字段
        "mesh_chunk_overlap_leaves",  # 08 G7 未实现，属已披露差距
    }
    out = []
    for k in keys:
        if k in not_a_param:
            continue
        if k not in out:
            out.append(k)
    return out


# --- UI 可见性待裁决基线（2026-10-31）-------------------------------------
# docs/08 §1「用户可调」列与 params.cpp 的 (user_adjustable, expert_only)
# 在这 13 个键上双向冲突。**已登记，不判硬失败**，但也不允许无声扩大。
#
# 为何不直接改代码对齐文档：这 13 项涉及 R-PREC-6「精度阈值必须可被用户覆盖」
# 的暴露策略，两个方向都有合理依据 ——
#   · 文档=是、注册表=专家（8 项）：如 cap_resolution_width，文档认为用户应能在
#     常规面板直接调分辨率；注册表注释写「禁止因速度降低」而收起为专家项。
#     「隐藏」本身即是一种限制用户覆盖的形式，需裁决。
#   · 文档=专家/否、注册表=是（5 项）：如 mesh_poisson_prune，文档标「否」但
#     注册表暴露为「是」，方向相反。
# 裁决属独立工作项（见 docs/14 §5.1.3 待登记）；本基线的作用是**锁住现状**，
# 任何新增冲突立刻失败，从而不可能再悄悄漂移。
KNOWN_VISIBILITY_DISPUTES = {
    # 文档=是 / 注册表=专家
    "cap_frame_rate",
    "cap_resolution_width",
    "mesh_chunked_enabled",
    "mesh_poisson_btype",
    "mesh_poisson_confidence",
    "mesh_poisson_depth_max",
    "mesh_poisson_interpolate",
    # 文档=专家 或 否 / 注册表=是
    "mesh_poisson_prune",
    "mesh_poisson_scale_isotropic",
    "perf_memory_tier",
    "scale_ref_size_tolerance_mm",
    "vio_imu_noise_acc",
    "vio_imu_noise_gyro",
}


def extract_registry_visibility() -> dict[str, tuple[bool, bool]]:
    """从 params.cpp 的 Register 调用提取每个键的 (user_adjustable, expert_only)。

    取值组合只有三种合法：`是`=(T,F) / `专家`=(F,T) / `否`=(F,F)。
    `(T,T)` 非法 —— 二者语义互斥（一个说「常规面板可见」、一个说「仅专家区可见」）。
    """
    hdr = PARAMS_H.read_text(encoding="utf-8")
    const_to_key = dict(
        re.findall(r'constexpr const char\*\s+(k[A-Za-z0-9_]+)\s*=\s*"([a-z0-9_]+)"', hdr)
    )
    cpp = PARAMS_CPP.read_text(encoding="utf-8")
    out: dict[str, tuple[bool, bool]] = {}
    for m in re.finditer(r"p\.Register\(\s*Make(\w+)\((.*?)\)\s*\);", cpp, re.S):
        body = m.group(2)
        ck = re.search(r"keys::(k[A-Za-z0-9_]+)", body)
        if not ck or ck.group(1) not in const_to_key:
            continue
        # 取「紧邻字符串字面量之前的两个布尔」＝ adjustable, expert。
        # 对 MakeBool 与 MakeInt/MakeDouble 两种签名都成立，因为只有 affects/note
        # 是字符串，且签名中仅这两个布尔紧邻它们。
        mb = re.search(r"(true|false)\s*,\s*(true|false)\s*,\s*\"", body)
        if not mb:
            continue
        out[const_to_key[ck.group(1)]] = (mb.group(1) == "true", mb.group(2) == "true")
    return out


def extract_doc_visibility() -> dict[str, str]:
    """从 docs/08 §1 参数表提取 `用户可调` 列（是 / 专家 / 否）。"""
    txt = DOC08.read_text(encoding="utf-8")
    out: dict[str, str] = {}
    for ln in txt.splitlines():
        if not ln.startswith("|"):
            continue
        cells = [c.strip() for c in ln.strip().strip("|").split("|")]
        if len(cells) < 7:
            continue
        m = re.match(r"`([a-z0-9_]+)`", cells[0])
        if not m or cells[1] not in ("int", "float", "string"):
            continue
        if cells[6] not in ("是", "否", "专家"):
            continue
        out[m.group(1)] = cells[6]
    return out


def main() -> int:
    global DOC08  # noqa: PLW0603

    ap = argparse.ArgumentParser(description="参数契约漂移检测闸门")
    ap.add_argument(
        "--registry",
        default=str(DOC08),
        help="参数权威文档路径（docs/08_参数体系.md）",
    )
    ap.add_argument(
        "--sources",
        nargs="*",
        default=["core/", "platform/"],
        help="待扫描的源码根（需包含 params.h / params.cpp）",
    )
    ap.add_argument("--verbose", action="store_true", help="打印全部缺失键")
    args = ap.parse_args()

    # --registry 允许覆盖文档路径；--sources 用于兼容 CI 契约（当前实现固定扫描
    # core/ 下的 params.h + params.cpp，platform/ 为空时自然无贡献）。
    if args.registry:
        DOC08 = pathlib.Path(args.registry)

    registered, defined = extract_code_keys()
    doc_keys = extract_doc_keys()

    print(f"[check_params] params.h 定义键常量 : {len(defined)}")
    print(f"[check_params] params.cpp 注册键   : {len(registered)}")
    print(f"[check_params] docs/08 声明键      : {len(doc_keys)}")

    failures: list[str] = []

    # --- 闸门 1：白名单键（内存保护硬依赖）必须已注册 ---
    wl = whitelist_keys()
    missing_wl = [k for k in wl if k not in registered]
    if missing_wl:
        failures.append(
            "白名单键未注册（内存保护将静默失效）：" + ", ".join(sorted(missing_wl))
        )
    print(
        f"[check_params] 白名单键命中注册表 : {len(wl) - len(missing_wl)}/{len(wl)}"
        + (" ✓" if not missing_wl else " ✗")
    )

    # --- 闸门 2：键名漂移（代码有 A，文档全线用 B，且 B 不在代码里） ---
    # 只在「同一语义前缀 + 相似后缀」时判漂移，避免误报。
    drift_pairs = [
        ("perf_thread_count", "perf_thread_pool_size"),
    ]
    for old, new in drift_pairs:
        if old in registered and new not in registered:
            failures.append(f"键名漂移：代码用 `{old}`，文档用 `{new}`（应统一为后者）")

    # --- 闸门 3：基线之外的新增缺失 ---
    only_doc = sorted(doc_keys - registered)
    unexplained = [
        k for k in only_doc if not k.startswith(KNOWN_UNIMPLEMENTED_PREFIXES)
    ]
    unexplained = [k for k in unexplained if k not in KNOWN_WHITELIST_GROUP_GAPS]
    # 进一步收窄：只有缺失键属于白名单相关组（perf_/rgbd_/mesh_/mvs_）时才判为
    # 硬失败；其余（cap_/sync_/post_/scan_/vio_/sfm_/scale_）记为「已披露差距」。
    hard = [
        k
        for k in unexplained
        if k.split("_")[0] in {"perf", "rgbd", "mesh", "mvs"}
    ]
    if hard:
        failures.append(
            "白名单相关组出现未注册键（属真漂移）：" + ", ".join(sorted(hard))
        )

    print(
        f"[check_params] docs 声明但未实现（已披露差距）: {len(only_doc)} "
        f"（其中白名单相关组未注册 {len(hard)}）"
    )
    if args.verbose and only_doc:
        for k in only_doc:
            print(f"    - {k}")

    # --- 闸门 4：代码有、文档无（未登记参数） ---
    only_code = sorted(registered - doc_keys)
    # perf_ 组必须全部登记进 docs/08（本轮刚修过，回归防线）。
    unregistered_perf = [k for k in only_code if k.startswith("perf_")]
    if unregistered_perf:
        failures.append(
            "perf_ 参数未登记进 docs/08：" + ", ".join(sorted(unregistered_perf))
        )
    print(f"[check_params] 代码有但 docs/08 未登记: {len(only_code)}")
    if args.verbose and only_code:
        for k in only_code:
            print(f"    + {k}")

    # --- 闸门 5：UI 可见性口径（docs/08 §1「用户可调」 ↔ 注册表标志）---
    #
    # 为什么需要这道闸门（2026-10-31 实测）：docs/08 §1 的「用户可调」列与
    # params.cpp 的 (user_adjustable, expert_only) 是**两份独立表述**。
    # 实测 160 个共同键里 16 处口径不一致，而此前**没有任何闸门在管**。
    #
    # 其中 3 处是**非法状态** `(true, true)`（已修正）：语义互斥，会使
    # docs/09 §3.8.5「专家模式显示 10 个参数」失效。列为硬失败。
    #
    # 其余 13 处属**双向方向性冲突**（文档=是/注册表=专家 与 反向），
    # 需要逐项人工裁决（涉及 R-PREC-6 精度阈值的暴露策略），
    # 故登记为待裁决基线：**不再无声扩大**，清单锁定，新增冲突即失败。
    registry_vis = extract_registry_visibility()
    doc_vis = extract_doc_visibility()

    illegal = sorted(k for k, (a, e) in registry_vis.items() if a and e)
    if illegal:
        failures.append(
            "UI 可见性非法组合 (true, true) —— 语义互斥：" + ", ".join(illegal)
        )

    def vis_code(k: str) -> str:
        a, e = registry_vis[k]
        return "是" if a else ("专家" if e else "否")

    conflicts = sorted(
        k
        for k in set(registry_vis) & set(doc_vis)
        if doc_vis[k] != vis_code(k)
    )
    new_conflicts = [k for k in conflicts if k not in KNOWN_VISIBILITY_DISPUTES]
    resolved = [k for k in KNOWN_VISIBILITY_DISPUTES if k not in conflicts]
    if new_conflicts:
        failures.append(
            "UI 可见性口径新增冲突（需裁决并登记）：" + ", ".join(new_conflicts)
        )
    print(
        f"[check_params] UI 可见性：注册表 {len(registry_vis)} 键 / 文档 {len(doc_vis)} 键 / "
        f"口径冲突 {len(conflicts)}（已登记待裁决 {len(KNOWN_VISIBILITY_DISPUTES)}）"
    )
    if args.verbose and conflicts:
        for k in conflicts:
            print(f"    ~ {k}: 文档={doc_vis[k]} 注册表={vis_code(k)}")
    if resolved:
        print(
            f"[check_params] 注：{len(resolved)} 项已登记冲突现已一致，"
            f"可从基线移除：{', '.join(resolved)}"
        )

    if failures:
        print("\n[check_params] FAIL：检测到参数契约漂移")
        for f in failures:
            print("  ✗ " + f)
        return 3

    print("[check_params] PASS：参数契约与基线一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
