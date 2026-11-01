#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""从 docs/04 §7.1 权威表生成 G6（rgbd_）参数注册代码片段。

为什么需要生成脚本（项目铁律「同一逻辑写两遍必然漂移」）：
  docs/04 §7.1 是 G6 的**展开规格**，docs/08 G6 是**权威定义**。若手工把 62 行
  抄进 params.h/params.cpp，任何一处数字抄错都会静默漂移（历史上
  `trim_confidence_quantile` 被写反、单测硬编码绕过注册表，皆属此类）。
  故采用「单一数据源 → 生成」的方式，把 §7.1 表作为唯一真源。

用法：
    python tools/gen_g6_params.py            # 生成到 build_cmake/gen/
    python tools/gen_g6_params.py --check    # 只校验 §7.1 自身一致性，不写文件

列格式（docs/04 §7.1）：
    0 键名 | 1 类型 | 2 单位 | 3 默认值 | 4 快/标/高 | 5 合法范围
    6 用户可调 | 7 生效时机 | 8 影响的精度维度 | 9 备注
"""
import argparse
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC04 = os.path.join(REPO, "docs", "04_RGBD管线.md")
OUT_DIR = os.path.join(REPO, "build_cmake", "gen")
# 契约表落在源码树，构建期无需 Python；由 CI/`--check` 保证与 §7.1 不漂移。
CONTRACT_INC = os.path.join(REPO, "tests", "unit", "g6_contract.inc")

# 「用户可调」列 → (user_adjustable, expert_only)
ADJ_MAP = {"是": (True, False), "专家": (False, True), "否": (False, False)}


def camel(key):
    """rgbd_tsdf_voxel_size → kRgbdTsdfVoxelSize"""
    return "k" + "".join(w[:1].upper() + w[1:] for w in key.split("_"))


def c_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def num(s):
    """把 §7.1 的数值字符串转成 C++ 双精度字面量。"""
    s = s.strip()
    m = re.match(r"^([-+]?[0-9.]+(?:[eE][-+]?\d+)?)$", s)
    if not m:
        raise ValueError("无法解析数值: %r" % s)
    return s


def parse_table(path):
    lines = open(path, encoding="utf-8").read().split("\n")
    rows = []
    on = False
    for ln, l in enumerate(lines, 1):
        if l.startswith("### 7.1 "):
            on = True
            continue
        if on and l.startswith("### 7.2"):
            break
        if not (on and l.startswith("| `rgbd_")):
            continue
        c = [x.strip() for x in l.strip().strip("|").split("|")]
        if len(c) < 10:
            raise ValueError("第 %d 行列数 %d < 10: %s" % (ln, len(c), l))
        raw_key = c[0]
        # 注意：§7.1 第 0 列形如 "`rgbd_xxx` **新增**" —— 「新增」标记在反引号
        # **之后**。必须先剥离标记再剥反引号，否则会残留尾部反引号并生成
        # 非法 C++ 标识符（`kFoo`` ），曾导致 62 行全部编译失败。
        is_new = "新增" in raw_key
        key = raw_key.replace("**新增**", "").replace("新增", "").strip().strip("`").strip()
        rows.append(
            {
                "key": key,
                "is_new": is_new,
                "type": c[1],
                "unit": c[2],
                "default": c[3],
                "tiers": c[4],
                "range": c[5],
                "adjustable": c[6],
                "timing": c[7],
                "affects": c[8],
                "note": c[9],
                "line": ln,
            }
        )
    return rows


def validate(rows):
    """§7.1 自身一致性校验。任何失败即退出，不产出半成品。"""
    errs = []
    seen = set()
    for r in rows:
        k = r["key"]
        if not re.match(r"^[a-z][a-z0-9_]*$", k):
            errs.append("第 %d 行键名不合法（含多余字符）: %r" % (r["line"], k))
        if k in seen:
            errs.append("重复键: %s" % k)
        seen.add(k)
        if r["type"] not in ("float", "int"):
            errs.append("%s: 未知类型 %r" % (k, r["type"]))
        if r["adjustable"] not in ADJ_MAP:
            errs.append("%s: 未知可调值 %r" % (k, r["adjustable"]))
        # 档位应为三段
        t = r["tiers"].split("/")
        if len(t) != 3:
            errs.append("%s: 档位不是三段: %r" % (k, r["tiers"]))
        # 范围应为 [lo, hi]
        m = re.match(r"^\[\s*([^,]+?)\s*,\s*([^\]]+?)\s*\]$", r["range"])
        if not m:
            errs.append("%s: 范围格式异常: %r" % (k, r["range"]))
            continue
        try:
            lo, hi = float(num(m.group(1))), float(num(m.group(2)))
            dv = float(num(r["default"]))
            if lo > hi:
                errs.append("%s: lo(%s) > hi(%s)" % (k, lo, hi))
            if not (lo <= dv <= hi):
                errs.append("%s: 默认值 %s 不在 [%s, %s]" % (k, dv, lo, hi))
            for tv in t:
                if not (lo <= float(num(tv)) <= hi):
                    errs.append("%s: 档位值 %s 不在 [%s, %s]" % (k, tv, lo, hi))
        except ValueError as e:
            errs.append("%s: 数值解析失败 (%s)" % (k, e))
    # 迁移期：int 参数（含 bool 语义）应可被 llround 处理
    return errs


def cpp_expr(r):
    """生成一行 MakeInt/MakeDouble 调用。"""
    m = re.match(r"^\[\s*([^,]+?)\s*,\s*([^\]]+?)\s*\]$", r["range"])
    lo = num(m.group(1))
    hi = num(m.group(2))
    t = [num(x.strip()) for x in r["tiers"].split("/")]
    f = "MakeInt" if r["type"] == "int" else "MakeDouble"
    adj, exp = ADJ_MAP[r["adjustable"]]
    return (
        "  p.Register(%s(keys::%s, %s, %s, %s, %s, %s, %s, %s, %s, "
        '"%s",\n                   "%s"));'
        % (
            f,
            camel(r["key"]),
            num(r["default"]),
            t[0],
            t[1],
            t[2],
            lo,
            hi,
            "true" if adj else "false",
            "true" if exp else "false",
            c_escape(r["affects"]),
            c_escape(r["note"]),
        )
    )


def contract_text(rows):
    tlines = ["    // 由 tools/gen_g6_params.py 生成。{key, default_value}"]
    for r in rows:
        tlines.append("    {%s, %s}," % ("\"" + r["key"] + "\"", num(r["default"])))
    return "\n".join(tlines) + "\n"


def check_sync(rows):
    """校验源码树中的生成物与当前 §7.1 同步（自包含，不依赖 git）。"""
    errs = []
    want = contract_text(rows)
    try:
        have = open(CONTRACT_INC, encoding="utf-8").read()
    except OSError:
        return ["缺少 %s —— 请运行 `python tools/gen_g6_params.py`" % CONTRACT_INC]
    if have != want:
        errs.append("%s 与 §7.1 不同步 —— 请重跑 `python tools/gen_g6_params.py`"
                    % os.path.relpath(CONTRACT_INC, REPO))
    return errs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="只校验（§7.1 自身一致性 + 生成物同步），不写文件")
    args = ap.parse_args()

    rows = parse_table(DOC04)
    errs = validate(rows)
    n_new = sum(1 for r in rows if r["is_new"])
    print("§7.1 解析: %d 行 (新增 %d, 既有 %d)" % (len(rows), n_new, len(rows) - n_new))
    if errs:
        print("校验失败 %d 项：" % len(errs))
        for e in errs:
            print("  - " + e)
        return 1
    if args.check:
        sync_errs = check_sync(rows)
        if sync_errs:
            print("同步校验失败 %d 项：" % len(sync_errs))
            for e in sync_errs:
                print("  - " + e)
            return 1
        print("校验通过（含生成物同步）。")
        return 0
    print("校验通过。")

    os.makedirs(OUT_DIR, exist_ok=True)
    # 1. 头文件常量
    keys = "".join(
        'inline constexpr const char* %s = "%s";\n' % (camel(r["key"]), r["key"])
        for r in rows
    )
    open(os.path.join(OUT_DIR, "g6_keys.h.inc"), "w", encoding="utf-8").write(keys)
    # 2. 注册行
    reg = "\n".join(cpp_expr(r) for r in rows) + "\n"
    open(os.path.join(OUT_DIR, "g6_register.cpp.inc"), "w", encoding="utf-8").write(reg)
    # 3. JSON（供文档回填与审计）
    json.dump(rows, open(os.path.join(OUT_DIR, "g6_params.json"), "w",
                         encoding="utf-8"), ensure_ascii=False, indent=1)
    # 4. 测试用键名表（含期望默认值），供 tests/unit/test_params.cpp include。
    #    使「注册表是否覆盖 §7.1 全部键」成为可自动验证的契约，而非靠人眼比对。
    open(CONTRACT_INC, "w", encoding="utf-8").write(contract_text(rows))
    print("已生成: g6_keys.h.inc / g6_register.cpp.inc / g6_params.json → %s" % OUT_DIR)
    print("已生成: %s" % CONTRACT_INC)
    return 0


if __name__ == "__main__":
    sys.exit(main())
