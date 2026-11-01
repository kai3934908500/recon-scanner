#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_shader_parity_negative.py — shader_parity.py 的**负向注入**自检。

为什么需要这个文件
------------------
本项目反复吃过的坑（见 .workbuddy/memory/MEMORY.md）：

    「恒不触发」的判据等于没有判据（假闸门）。
    每写一条闸门判据，必须做一次**负向注入**证明它真的会拒收；
    否则无法区分「通过」与「根本没检查」。

本脚本就是 `tools/shader_parity.py` 的那份证明：它对**每一类判据**
注入一个故意的缺陷，运行闸门，要求它 **以非 0 退出** 并且**报出预期的那条
失败信息**，然后还原文件。

⚠️ 本脚本会**临时修改** `shaders/` 下的文件再还原。为安全起见：
  · 全部改动在内存中做，落盘后立即在 `finally` 中还原原文；
  · 每次注入前先记录原文，还原用原文写回（不依赖任何 git）；
  · 任一注入失败（闸门没拒收）→ 本脚本退出码 3，CI 视为失败。
    这正是关键：**若注入不被拒收，说明闸门有假判据**。

用法
----
    python tools/verify_shader_parity_negative.py
    python tools/verify_shader_parity_negative.py --keep-going   # 全部跑完再汇总

退出码
------
  0 = 全部注入都被正确拒收（闸门有效）
  1 = 环境/用法错误
  3 = 有注入**未被拒收** → 存在假判据
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GATE = os.path.join(ROOT, "tools", "shader_parity.py")

COMMON_H = os.path.join(ROOT, "shaders", "common", "gpu_algo.h")
METAL_COMMON = os.path.join(ROOT, "shaders", "metal", "gpu_common.h")
METAL_MVS = os.path.join(ROOT, "shaders", "metal", "mvs.metal")
METAL_TSDF = os.path.join(ROOT, "shaders", "metal", "tsdf.metal")
VULKAN_COMMON = os.path.join(ROOT, "shaders", "vulkan", "gpu_common.glsl")
VULKAN_SEED = os.path.join(ROOT, "shaders", "vulkan", "recon_mvs_seed.comp")
VULKAN_TSDF = os.path.join(ROOT, "shaders", "vulkan", "recon_tsdf_fuse.comp")
VULKAN_PM = os.path.join(ROOT, "shaders", "vulkan", "recon_mvs_patchmatch.comp")
CMAKE_LISTS = os.path.join(ROOT, "CMakeLists.txt")


def slurp(path: str) -> str:
    with open(path, "r", encoding="utf-8") as fh:
        return fh.read()


def spit(path: str, text: str) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def run_gate() -> tuple[int, str]:
    proc = subprocess.run([sys.executable, GATE], cwd=ROOT,
                          capture_output=True, text=True)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


# ---------------------------------------------------------------------------
# 注入定义：每项 = (编号, 说明, 期望失败信息片段, [[(路径, 原文片段, 替换片段)]])
# ---------------------------------------------------------------------------
# 约束：替换片段必须**唯一可定位**（`str.replace(..., 1)` 语义按精确子串）。
# 每项必须命中至少一处，否则注入本身失效（也算失败）。
INJECTIONS: list[tuple[str, str, str, list[tuple[str, str, str]]]] = [
    (
        "P1-a-common-dup",
        "common 里重复注册同一个算法（应被报为重复注册）",
        "common 重复注册算法",
        [(COMMON_H,
          "RECON_GPU_ALGO(recon_mvs_seed);",
          "RECON_GPU_ALGO(recon_mvs_seed);\nRECON_GPU_ALGO(recon_mvs_seed);")],
    ),
    (
        "P1-b-metal-missing",
        "Metal 侧删掉一个算法实现（common 仍声明）",
        "Metal 侧缺失算法实现",
        [(METAL_MVS,
          "kernel void recon_mvs_subpixel(texture2d<float, access::read>  in_cost",
          "kernel void recon_zzz_subpixel(texture2d<float, access::read>  in_cost")],
    ),
    (
        "P1-b-metal-extra",
        "Metal 侧多出一个 common 未声明的 kernel",
        "Metal 侧存在 common 未声明的 kernel",
        [(METAL_TSDF,
          "kernel void recon_tsdf_fuse(",
          "kernel void recon_tsdf_fuse_extra(\nkernel void recon_tsdf_fuse(")],
    ),
    (
        "P1-c-vulkan-missing",
        "Vulkan 侧删掉一个算法入口（common 仍声明）",
        "Vulkan 侧缺失算法实现",
        [(VULKAN_SEED, "void recon_mvs_seed(ivec2 gid)", "void recon_zzz_seed(ivec2 gid)")],
    ),
    (
        "P3-file-name-mismatch",
        "Vulkan 文件名与算法名不一致（契约：文件名 = 算法名）",
        "Vulkan 侧存在 common 未声明的算法入口",
        [(VULKAN_TSDF,
          "void recon_tsdf_fuse(uint gid)",
          "void recon_tsdf_blend(uint gid)")],
    ),
    (
        "P4-field-order",
        "Metal 参数块把两个字段调换顺序（序列不同）",
        "字段序列 common vs Metal 不一致",
        [(METAL_COMMON,
          "    int   ref_width;\n    int   ref_height;",
          "    int   ref_height;\n    int   ref_width;")],
    ),
    (
        "P4-field-rename",
        "Vulkan 参数块字段改名（序列不同）",
        "字段序列 common vs Vulkan 不一致",
        [(VULKAN_COMMON,
          "    float geo_consistency_pct;",
          "    float geo_consistency_ratio;")],
    ),
    (
        "P5-vec-type",
        "参数块混入 vec4（std140 会引入隐式填充）",
        "参数块**只允许** int/uint/float",
        [(VULKAN_COMMON,
          "    int   reserved0;\n};\n\n// ---------------------------------------------------------------------------\n// 参数块：TSDF",
          "    vec4  reserved0;\n};\n\n// ---------------------------------------------------------------------------\n// 参数块：TSDF")],
    ),
    (
        "P6-workgroup",
        "Vulkan 某个 .comp 把 local_size 改成 16×16",
        "workgroup 尺寸",
        [(VULKAN_PM,
          "layout(local_size_x = RECON_VK_WORKGROUP_X,\n"
          "       local_size_y = RECON_VK_WORKGROUP_Y,\n"
          "       local_size_z = RECON_VK_WORKGROUP_Z) in;",
          "layout(local_size_x = 16,\n"
          "       local_size_y = 16,\n"
          "       local_size_z = 1) in;")],
    ),
    (
        "P7-const-value",
        "两端共享常量值不同（Vulkan 侧 EPS_DEPTH 改了）",
        "跨端共享常量",
        [(VULKAN_COMMON,
          "#define RECON_GPU_EPS_DEPTH     1.0e-6",
          "#define RECON_GPU_EPS_DEPTH     1.0e-5")],
    ),
    (
        "P7-use-without-def",
        "Vulkan 侧引用了本端未定义的共享常量",
        "引用了未在本端定义的跨端常量",
        [(VULKAN_SEED,
          "vec4(RECON_GPU_COST_INF, 0.0, 0.0, 1.0)",
          "vec4(RECON_GPU_NO_SUCH_CONST, 0.0, 0.0, 1.0)")],
    ),
    (
        "P7-private-const",
        "Vulkan 侧私设一个 common 未登记的共享常量",
        "定义了 common 未登记的跨端常量",
        [(VULKAN_COMMON,
          "#define RECON_GPU_PIXEL_CENTER  0.5",
          "#define RECON_GPU_PIXEL_CENTER  0.5\n#define RECON_GPU_PRIVATE_ONE   7.0")],
    ),
    (
        "P8-gap-metal-missing",
        "Metal 侧删掉一条缺口条目（common 仍登记；汇总行不算）",
        "Metal 侧未以条目形式标注缺口 SOURCE-TODO-MVS-1",
        [(METAL_MVS, "SOURCE-TODO-MVS-1", "SOURCE-TODO-MVS-XX")],
    ),
    (
        "P8-gap-vulkan-missing",
        "Vulkan 侧删掉一条缺口条目（common 仍登记；汇总行不算）",
        "Vulkan 侧未以条目形式标注缺口 SOURCE-TODO-TSDF-1",
        [(VULKAN_TSDF,
          "//   SOURCE-TODO-TSDF-1  块分配原子",
          "//   SOURCE-TODO-TSDF-XX  块分配原子")],
    ),
    (
        "P9-orphan-gap",
        "Metal 侧出现 common 未登记的缺口编号",
        "common 未登记的缺口编号",
        [(METAL_MVS, "SOURCE-TODO-MVS-5", "SOURCE-TODO-MVS-5\n// SOURCE-TODO-MVS-99")],
    ),
    (
        "P10-diff-missing",
        "Metal 侧删掉平台差异编号说明",
        "平台差异 DIFF-1 未在 Metal 侧可见",
        [(METAL_COMMON, "VK-DIFF-1 / MSL-DIFF-1", "这个差异不再说明")],
    ),
    (
        "P11-helper-asymmetry",
        "Vulkan 侧多出一个公共 helper（两端 helper 不对称）",
        "Vulkan 侧独有 helper",
        [],
    ),
    (
        # P12 的现实原型：第三十七轮新增 shaders/metal/yuv.metal 时，
        # CMakeLists.txt 的 recon_shaders_metal 清单没跟着加 —— 构建不报错，
        # 只是那个 .metal 静默不进 build phase，P1 也查不出（P1 读磁盘文件）。
        "P12-cmake-metal-missing",
        "从 CMakeLists.txt 的 recon_shaders_metal 清单里删掉一个 .metal",
        "清单**漏列**了这些 .metal",
        [(CMAKE_LISTS,
          '      "${CMAKE_CURRENT_SOURCE_DIR}/shaders/metal/yuv.metal"\n',
          "")],
    ),
]


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--keep-going", action="store_true",
                    help="某注入未被拒收时继续跑其余注入（默认也继续，只影响输出风格）")
    args = ap.parse_args()

    if not os.path.isfile(GATE):
        print("找不到 " + GATE)
        return 1

    # 先确认「无注入时闸门是 PASS」—— 否则后面的"拒收"可能只是基线本来就红。
    base_rc, base_out = run_gate()
    if base_rc != 0:
        print("[negative] 基线失败：未注入任何缺陷时闸门就报错，无法进行负向验证。")
        print(base_out)
        return 1
    print("[negative] 基线：闸门 PASS（未注入缺陷）✓")
    print("")

    # P11 的注入需要"给 Vulkan 加一个 helper"（令 Metal 缺失）——
    # 具体做法：在 Vulkan 公共头加一个只此一端的 helper。
    p11 = list(INJECTIONS[[i for i, x in enumerate(INJECTIONS) if x[0] == "P11-helper-asymmetry"][0]])
    p11[3] = [(VULKAN_COMMON,
               "bool recon_in_image(vec2 uv, vec2 size) {",
               "bool recon_vk_only_helper(vec2 uv) { return uv.x > 0.0; }\n\n"
               "bool recon_in_image(vec2 uv, vec2 size) {")]
    INJECTIONS[[i for i, x in enumerate(INJECTIONS) if x[0] == "P11-helper-asymmetry"][0]] = tuple(p11)

    failures: list[str] = []
    for idx, (code, desc, expect, edits) in enumerate(INJECTIONS, 1):
        # --- 备份原文 ---
        originals: dict[str, str] = {}
        for path, _, _ in edits:
            if path not in originals:
                originals[path] = slurp(path)

        applied = False
        try:
            for path, old, new in edits:
                text = slurp(path)
                if old not in text:
                    failures.append(f"{code}: 注入片段在 {os.path.basename(path)} "
                                    f"中找不到，注入无效（需修正本脚本）")
                    break
                spit(path, text.replace(old, new, 1))
                applied = True

            if not applied and edits:
                continue      # 已记失败

            rc, out = run_gate()
            if rc == 0:
                failures.append(f"{code}: **闸门未拒收**（退出码 0）→ 该判据是假闸门。"
                                f" 意图：{desc}")
                print(f"  [{idx:02d}] ✗ {code}  —— 未被拒收！（假闸门）")
            elif expect not in out:
                failures.append(f"{code}: 闸门拒收了（rc={rc}），但失败信息里"
                                f"找不到预期片段「{expect}」。实际输出见下：\n{out}")
                print(f"  [{idx:02d}] ✗ {code}  —— 拒收了但信息不符预期")
            else:
                print(f"  [{idx:02d}] ✓ {code}  —— 已拒收（rc={rc}）")
        finally:
            for path, text in originals.items():
                spit(path, text)

    # --- 收尾：确认还原后闸门重新 PASS ---
    rc_after, out_after = run_gate()
    print("")
    if rc_after != 0:
        failures.append("还原后闸门仍失败 —— 还原不完整！输出：\n" + out_after)
        print("[negative] ✗ 还原后闸门仍失败（还原不完整）")
    else:
        print("[negative] 还原后闸门重新 PASS ✓")

    print("")
    if failures:
        print(f"[negative] FAIL：{len(failures)} 项")
        for f in failures:
            print("  ✗ " + f)
        return 3

    print(f"[negative] PASS：{len(INJECTIONS)} 项注入全部被正确拒收"
          f"（闸门 P1..P12 均有效）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
