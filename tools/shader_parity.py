#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""shader_parity.py — 着色器两端一致性闸门（docs/14 CT-6 / docs/12 MT-3、MT-4）。

背景
----
docs/12 §4.4 的契约是「**算法同源、两端分别编译**」：

    `shaders/common/` 描述算法，`shaders/metal/*.metal` 与
    `shaders/vulkan/*.comp` 是各自实现；二者的**函数签名与数学式必须逐条对齐**，
    对齐检查由 `tools/shader_parity.py` 在 CI 中执行（docs/12 §6.2）。

但这个契约有一个**结构性困难**：无 macOS / 无 Xcode / 无 glslang 的机器
（如本项目当前的 Windows 开发机）**无法编译任何一端**，因此"编译过了就等于
对齐了"这条退路不存在。于是对齐检查只能落到**静态结构比对**上。

本闸门就做这件事，并且刻意覆盖三类**互不相同**的漂移：

  ① **名称漂移**（第 1–3 类判据）：一端加了/改了/删了算法函数名。
     闸门的期望集合来自 `shaders/common/gpu_algo.h` 的 `RECON_GPU_ALGO(...)`
     —— 那是唯一事实来源，声明即注册（见该头文件说明）。

  ② **布局漂移**（第 4–7 类判据）：参数块字段集合/顺序、workgroup 尺寸、
     固定长度数组上界、跨端共享常量值 在两端不一致。
     这类漂移**两端都能各自编译通过**，只在运行期表现为数值错位 ——
     是本项目历史上最贵的一类坑（见 MEMORY.md「静默丢弃最难查」）。

  ③ **进度漂移**（第 8–10 类判据）：一端把某个缺口补上了、另一端没有。
     函数名一致、布局一致、**一切静态检查都通过**，但语义已经不同。
     这是最隐蔽的一类，也是本闸门最独特的判据：
     期望缺口集合来自 common 头的 `RECON_GPU_GAP(模块, 编号)`，
     **两端都必须标注**；任一端补齐后必须同时删登记，否则另一端立刻报错。

为什么用「解析源码」而不是「维护一份 JSON 清单」
------------------------------------------------
一份 JSON 清单 = 第二处事实来源，必然漂移（MEMORY.md：「第二条事实来源
必然漂移 —— 手抄『汇总表』也不例外」）。故本闸门**只解析源码**：
期望值全部从 `gpu_algo.h` 现算，从不手抄。

判据一览
--------
  P1  算法名集合：common 声明 ↔ Metal kernel ↔ Vulkan 入口函数。三方相等。
  P2  Metal 侧不得出现 common 未声明的 `kernel void recon_*(`。
  P3  Vulkan 侧不得出现 common 未声明的 `void recon_*(` 算法入口，
      且**文件名 = 算法名**（一文件一主，因 GLSL 只有 main()）。
  P4  参数块字段：`struct Recon*GpuParams` 字段名序列三方逐条相等。
  P5  参数块字段类型：仅允许 `int`/`float`（拒绝 vec2/3/4/mat 混入 ——
      std140 会引入隐式填充而 MSL 紧凑布局不会，见 gpu_common.glsl 说明）。
  P6  workgroup 尺寸：common 常量 / Metal `threads_per_threadgroup` /
      Vulkan `local_size_*` 三者一致。
  P7  跨端共享常量：两端公共头的 `RECON_GPU_*` 数值逐条相等（按数值而非
      字面量比对，容许 GLSL 无 `f` 后缀的语法差异）。
  P8  缺口标注：common 的每个 `RECON_GPU_GAP(M,i)` 在**两端**都出现
      `SOURCE-TODO-M-i` 至少一次。
  P9  缺口反向：两端出现的 `SOURCE-TODO-*` 编号都必须在 common 登记
      （防止"补完了但忘了删标注"或"凭感觉编号"）。
  P10 平台差异标注：common 的每个 `RECON_GPU_DIFF(n)` 在**两端**都可见。
  P11 两端 helper 集合对称（公共头层面）。
  P12 CMakeLists.txt 的 metal 清单 ↔ 磁盘 .metal 一致。
  P13 定点累加协议两端同构：量化 / 饱和 / 反量化三段数学式逐字对齐，
      且两端都不得出现浮点原子（`atomic_float` 是 Metal 3.0 / iOS 16 起
      才有，本项目部署下限 iOS 15.0；GLSL 侧无该扩展）。
      ⚠️ 本判据每面都带**反空**约束：匹配数为 0 即报「假判据」失败。
  P14 Vulkan `.comp` 的**预处理指令顺序**自洽（本判据诞生于一次真实事故，
      见下方长注释）：`#include "gpu_common.glsl"` 必须早于任何引用它定义
      的宏的 `layout(local_size_*)`，且 `#version` 必须是第一个非注释指令。
      纯静态可查，**不需要 glslang** —— 故本机（Windows）也能守住它。
  P15 Vulkan **共享头**的编译期扩展自洽：对裸 `texture2D` 调用
      `texelFetch` / `textureSize` / `texelFetchOffset` 等 samplerless 函数时，
      必须声明 `#extension GL_EXT_samplerless_texture_functions`。
      （诞生于第四十一轮**首次真编译**：P14 修好 include 可见性后，
      `gpu_common.glsl` 第一次进编译器就因缺该扩展而失败 —— 8 个 .comp
      同源报错。P14 只能证明"共享头被读到了"，证明不了"它能编过"。）

退出码
------
  0 = 一致
  1 = 用法错误 / 关键文件缺失
  3 = 检测到不一致

用法
----
    python tools/shader_parity.py            # 从仓库根或任意目录跑均可（用 ROOT 绝对路径）
    python tools/shader_parity.py --verbose  # 打印解析到的全部集合
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# ⚠️ 必须用绝对路径：ctest 的工作目录是 `build_cmake/`，不是仓库根。
# 本项目已在 G10（check_ios_contract.py）踩过这个坑 —— 相对路径在本地跑绿、
# 在 ctest 里「找不到头文件」。此处的两个常量即该教训的复用。
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMMON_H = os.path.join(ROOT, "shaders", "common", "gpu_algo.h")
METAL_DIR = os.path.join(ROOT, "shaders", "metal")
VULKAN_DIR = os.path.join(ROOT, "shaders", "vulkan")
METAL_COMMON = os.path.join(METAL_DIR, "gpu_common.h")
VULKAN_COMMON = os.path.join(VULKAN_DIR, "gpu_common.glsl")

# 参数块结构体名（三方必须同名，否则字段比对无从谈起）。
PARAM_STRUCTS = ("ReconMvsGpuParams", "ReconTsdfGpuParams", "ReconYuvToBgraGpuParams")


# ---------------------------------------------------------------------------
# 报告器（与 check_ios_contract.py 的 Report 风格保持一致，便于统一阅读）
# ---------------------------------------------------------------------------
class Report:
    def __init__(self) -> None:
        self.failures: list[str] = []
        self.notes: list[str] = []

    def fail(self, msg: str) -> None:
        self.failures.append(msg)

    def note(self, msg: str) -> None:
        self.notes.append(msg)

    def dump(self) -> None:
        for n in self.notes:
            print("说明  " + n)
        for f in self.failures:
            print("失败  " + f)


def read(path: str) -> str:
    """读文件；缺失时返回空串（由调用方判空并报「文件缺失」失败）。

    不在此处抛异常：闸门要能一次报出**所有**缺失文件，而不是遇到第一个就退出
    （否则修一个跑一次，体验很差）。
    """
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return fh.read()
    except OSError:
        return ""


def strip_comments(text: str) -> str:
    """去掉 // 与 /* */ 注释，避免注释里的示例代码被误当代码解析。

    ⚠️ 顺序很重要：若先删 /* */ 再删 //，则 `// /*` 这类会残留；
    但此处两个正则都用非贪婪 + DOTALL，先块后行即可覆盖本项目实际写法。
    本函数**只用于「找代码」**；凡需要读注释的判据（缺口标注、差异标注）
    必须直接用原文 —— 否则正是本项目踩过的「判据落在错误对象上」的坑。
    """
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


# ---------------------------------------------------------------------------
# 解析：算法名（单一事实来源 = common 头的 RECON_GPU_ALGO 宏调用）
# ---------------------------------------------------------------------------
def parse_common_algos(text: str) -> list[str]:
    """从 `RECON_GPU_ALGO(name);` 提取期望算法名（**保持出现顺序**）。

    注意排除宏**定义**本身（`#define RECON_GPU_ALGO(function_name)`）——
    该行是 `RECON_GPU_ALGO(` 后跟 `function_name)`，不含分号结尾的实参形态。
    用「后跟分号」锚定调用形态，与定义形态区分。
    """
    # 去掉 #define 行，避免把宏定义里的形参误认为一次注册。
    body = re.sub(r"^\s*#\s*define[^\n]*RECON_GPU_ALGO[^\n]*$", "", text,
                  flags=re.M)
    return re.findall(r"RECON_GPU_ALGO\(\s*(\w+)\s*\)\s*;", body)


def parse_common_gaps(text: str) -> list[tuple[str, str]]:
    """从 `RECON_GPU_GAP(module, index);` 提取期望缺口（保持顺序）。

    返回 [(module, index)]，如 [("MVS","1"), ...]。
    """
    body = re.sub(r"^\s*#\s*define[^\n]*RECON_GPU_GAP[^\n]*$", "", text,
                  flags=re.M)
    return re.findall(r"RECON_GPU_GAP\(\s*(\w+)\s*,\s*(\d+)\s*\)\s*;", body)


def parse_common_diffs(text: str) -> list[str]:
    """从 `RECON_GPU_DIFF(n);` 提取期望差异编号（保持顺序）。"""
    body = re.sub(r"^\s*#\s*define[^\n]*RECON_GPU_DIFF[^\n]*$", "", text,
                  flags=re.M)
    return re.findall(r"RECON_GPU_DIFF\(\s*(\d+)\s*\)\s*;", body)



# ---------------------------------------------------------------------------
# 缺口标注的行首提取（**这条规则由负向注入确立**，见 verify_*_negative.py）
# ---------------------------------------------------------------------------
# 最初的判据是「标签在文本里出现过至少一次」。负向注入立刻证明它是**弱判据**：
# 本仓库的 TSDF-1 在文件头有**两处**出现 ——
#     ① `// ⚠️ 实现状态（与 MSL 侧 SOURCE-TODO-TSDF-1..4 **一一对应**）`   ← 汇总行
#     ② `//   SOURCE-TODO-TSDF-1  块分配原子（…）`                     ← 真正的条目
# 只要求"出现过"，则删掉条目②、留下汇总行①，闸门**仍然通过** ——
# 于是"缺口已在另一端补齐"这件事会被悄悄漏掉。
#
# 收紧后的规则：标签必须出现在**某一行的行首**（只允许前置空白与注释符）。
# 这与本仓库实际书写约定一致（`//   SOURCE-TODO-MVS-1  …`），
# 也使判据落在「是否存在真实的缺口条目」而非「字符串是否出现」上 ——
# 正是 MEMORY.md 那条「判据必须落在目标对象上」的直接应用。
GAP_LINE_RE = re.compile(
    r"^[ \t]*(?://+|#)?[ \t]*(SOURCE-TODO-[A-Z]+-\d+)\b",
    re.M)


def find_gap_tags(text: str) -> set[str]:
    """返回**以行首条目形式**出现的缺口标签集合。"""
    return set(GAP_LINE_RE.findall(text))


# ---------------------------------------------------------------------------
# 解析：Metal 侧
# ---------------------------------------------------------------------------
def parse_metal_kernels(text: str) -> list[str]:
    """提取 `kernel void <name>(...)` 的 name（去注释后解析）。"""
    code = strip_comments(text)
    return re.findall(r"\bkernel\s+void\s+(\w+)\s*\(", code)


def parse_metal_helpers(text: str) -> list[str]:
    """提取 MSL 公共头里 `recon_*` 自由函数名（非 kernel）。

    用途：比对两端 helper 集合，防止「一边加了 helper、另一边没有」
    导致数值漂移（如端点复制 vs 填 0）。
    """
    code = strip_comments(text)
    names = re.findall(r"\b(?:static\s+inline\s+)?[\w:]+\s+(recon_\w+)\s*\(", code)
    return sorted(set(names))


# ---------------------------------------------------------------------------
# 解析：Vulkan 侧
# ---------------------------------------------------------------------------
def parse_vulkan_algos(text: str) -> list[str]:
    """提取 `.comp` 里的**算法入口**函数名。

    ⚠️ 本函数的设计也经历了一次**自我证伪**（教训类型：「判据在真实数据上
    无法区分目标对象」）：

      第一版用「缩进 0 列」区分算法入口与文件内 helper。实测失败 ——
      因为 GLSL 的约定是**顶层函数一律顶格**，文件内私有 helper
      （`recon_mvs_consistency_invalid`、`recon_voxel_accumulate`）
      同样顶格，缩进**根本没有区分力**。若继续用它，就会把 helper 误报为
      "common 未声明的算法入口"（实测正是如此，3 项误报）。

      第二版改为**显式命名约定**，这是唯一零歧义的方案：

          算法入口      →  `recon_<模块>_<动作>`，与 common 的
                            `RECON_GPU_ALGO(...)` **逐字相同**（MT-3）
          文件内私有 helper → 必须以 `recon_vk_local_` 开头

      该前缀同时是本仓库**唯一一处"允许不跨端对齐"的函数命名空间**：
      helper 是文件内实现细节，两端本就不必同名（例如 Vulkan 侧的
      `recon_vk_local_voxel_accumulate` 封装了「浮点原子 / 定点原子」的
      双路径分支，Metal 侧没有这个分支，自然没有对应函数）。
      跨端**真正需要**对齐的是公共头的 helper —— 那由 P11 单独管。

      判据的**负向注入**验证（见 §「闸门自身验证」）：
      把一个算法入口改名成不带 `recon_vk_local_` 前缀的别的名字 →
      本判据立刻报"common 声明的算法在 Vulkan 侧缺失"，确认判据有效。
    """
    code = strip_comments(text)
    names = re.findall(r"^void\s+(recon_\w+)\s*\(", code, flags=re.M)
    return [n for n in names if not n.startswith("recon_vk_local_")]


def parse_vulkan_helpers(text: str) -> list[str]:
    """提取 GLSL 公共头里 recon_* 自由函数名。"""
    code = strip_comments(text)
    names = re.findall(r"^\s*(?:[\w]+\s+)+(recon_\w+)\s*\(", code, flags=re.M)
    return sorted(set(names))


def parse_vulkan_local_size(text: str) -> tuple[str, str, str] | None:
    """解析 `layout(local_size_x = ..., ...) in;`。

    值可能是字面量 `8` 或宏名 `RECON_VK_WORKGROUP_X` —— 两者都接受，
    字面量路径下会与 common 的数值比对，宏路径下由 P7 的常量比对兜住。
    """
    code = strip_comments(text)
    m = re.search(
        r"local_size_x\s*=\s*([\w]+)\s*,\s*"
        r"local_size_y\s*=\s*([\w]+)\s*,\s*"
        r"local_size_z\s*=\s*([\w]+)", code)
    return m.groups() if m else None


def parse_metal_threadgroup(text: str) -> tuple[str, str, str] | None:
    """解析 `[[threads_per_threadgroup(N, M, K)]]` 或 `threads_per_threadgroup` 属性。

    本仓库当前**未显式声明**该属性（Metal 可由 host 侧 dispatch 时决定，
    或由 `max_total_threads_per_threadgroup` 属性约束）。因此解析允许失败 ——
    失败时由 P6 走「Metal 侧未声明 → 以 common 常量为准并在报告中说明」分支。
    """
    code = strip_comments(text)
    m = re.search(r"threads_per_threadgroup\s*\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)", code)
    return m.groups() if m else None


# ---------------------------------------------------------------------------
# 解析：参数块字段
# ---------------------------------------------------------------------------
def parse_struct_fields(text: str, struct_name: str) -> list[tuple[str, str]]:
    """解析 `struct <name> { ... };` 的字段，返回 [(type, field), ...]（保持顺序）。

    只取「以分号结尾的简单成员声明」，跳过注释与嵌套结构（本项目的参数块
    只有 4 字节标量，无嵌套）。

    实现要点（**这是本函数最容易写错的地方**）：
      · 必须先 `strip_comments`，否则字段名后的 `// 注释` 会混进字段名；
      · 必须用 `[^}]*?` 非贪婪到**第一个** `}` —— 否则会跨到下一个 struct。
    """
    code = strip_comments(text)
    m = re.search(
        r"\bstruct\s+" + re.escape(struct_name) + r"\s*\{(.*?)\}",
        code, flags=re.S)
    if not m:
        return []
    body = m.group(1)
    out: list[tuple[str, str]] = []
    for decl in body.split(";"):
        decl = decl.strip()
        if not decl:
            continue
        # 形如 `int32_t  ref_width` / `int ref_width` / `float rand_search_radius`
        fm = re.match(r"^([\w:]+)\s+(\w+)$", decl)
        if fm:
            out.append((fm.group(1), fm.group(2)))
    return out


# 类型归一化：三方各写各的语言类型名，比对的是**归一化后的语义类型**。
#   C: int32_t / uint32_t / float
#   MSL: int / uint / float
#   GLSL: int / uint / float
_TYPE_ALIAS = {
    "int32_t": "int", "int": "int",
    "uint32_t": "uint", "uint": "uint",
    "float": "float",
}


def norm_type(t: str) -> str:
    return _TYPE_ALIAS.get(t, t)


# ---------------------------------------------------------------------------
# 解析：跨端共享常量
# ---------------------------------------------------------------------------
def parse_defines(text: str, prefix: str) -> dict[str, str]:
    """提取 `#define <prefix>... <value>` 的 {名字: 原始值文本}。

    值取该行 `#define NAME` 之后的**全部剩余 token（去注释、去续行）**。
    本项目的共享常量都是单 token（数值），故足够；若将来出现表达式，
    会以文本比对（那反而是更严格的判据，可接受）。
    """
    out: dict[str, str] = {}
    for line in text.splitlines():
        m = re.match(r"\s*#\s*define\s+(" + re.escape(prefix) + r"\w*)\s+(.+?)\s*$", line)
        if m:
            name, val = m.group(1), m.group(2)
            val = re.sub(r"//.*$", "", val).strip()
            # 去掉 MSL 的 f/F 后缀（GLSL 无此前缀），使两端可数值比对
            val = re.sub(r"([0-9a-fA-F])([fF])\s*$", r"\1", val)
            out[name] = val
    return out


def parse_number(text: str):
    """把常量文本解析为可比较的数值。

    支持十进制整数、十六进制（`0x...u`）、浮点（含 `e` 指数）。
    返回 float 或 int 或 None（无法解析）。
    十六进制统一为 int 以便 0x7feb352du 与 0x7feb352d 相等。
    """
    t = text.strip().rstrip("uUlL")
    try:
        if t.lower().startswith("0x"):
            return int(t, 16)
        if re.fullmatch(r"-?\d+", t):
            return int(t)
        return float(t)
    except ValueError:
        return None


# ===========================================================================
# 判据 P1..P15
# ===========================================================================
def gate_naming(rep: Report, common_algos: list[str],
                metal_kernels: list[str],
                vulkan_by_file: dict[str, list[str]]) -> None:
    """P1/P2/P3 —— 算法名三方一致性。"""
    expected = set(common_algos)
    if not expected:
        rep.fail("未能从 shaders/common/gpu_algo.h 解析到任何 RECON_GPU_ALGO(...)"
                 "（解析器失配或声明被删）")
        return

    # P1-a common 内部不得重复注册同名算法
    dup = [a for a in set(common_algos) if common_algos.count(a) > 1]
    if dup:
        rep.fail("common 重复注册算法：" + ", ".join(sorted(dup)))

    # P1-b Metal kernel 集合
    metal_set = set(metal_kernels)
    miss_m = sorted(expected - metal_set)
    extra_m = sorted(metal_set - expected)
    if miss_m:
        rep.fail("Metal 侧缺失算法实现（common 已声明）：" + ", ".join(miss_m))
    if extra_m:
        rep.fail("Metal 侧存在 common 未声明的 kernel：" + ", ".join(extra_m))

    # P1-c Vulkan 入口集合（所有 .comp 合并）
    vk_all: dict[str, list[str]] = {}
    for fname, names in vulkan_by_file.items():
        for n in names:
            vk_all.setdefault(n, []).append(fname)
    vk_set = set(vk_all)
    miss_v = sorted(expected - vk_set)
    extra_v = sorted(vk_set - expected)
    if miss_v:
        rep.fail("Vulkan 侧缺失算法实现（common 已声明）：" + ", ".join(miss_v))
    if extra_v:
        rep.fail("Vulkan 侧存在 common 未声明的算法入口："
                 + ", ".join(f"{n}({vk_all[n][0]})" for n in extra_v))

    # P3 一文件一算法：文件名必须等于算法名（GLSL 只有 main()，见 gpu_common.glsl）
    for fname, names in vulkan_by_file.items():
        base = os.path.splitext(fname)[0]
        # 从该文件里筛出「属于期望集合」的入口 —— 那些才是算法本体
        algos_here = [n for n in names if n in expected]
        if not algos_here:
            continue          # 无算法入口的文件（不预期存在，但不在此判据管辖）
        if len(algos_here) > 1:
            rep.fail(f"Vulkan 文件 {fname} 导出了多个算法入口 {algos_here}；"
                     f"GLSL 一文件只能有一个 main()，应一算法一文件")
        if base != algos_here[0]:
            rep.fail(f"Vulkan 文件 {fname} 的算法入口为 {algos_here[0]}，"
                     f"与文件名不同（契约：文件名 = 算法名）")

    if not rep.failures:
        rep.note(f"P1 算法名三方一致：{len(expected)} 个"
                 f"（Metal kernel {len(metal_set)} / Vulkan 入口 {len(vk_set)}）")


def gate_param_layout(rep: Report,
                      common_fields: dict[str, list[tuple[str, str]]],
                      metal_fields: dict[str, list[tuple[str, str]]],
                      vulkan_fields: dict[str, list[tuple[str, str]]]) -> None:
    """P4/P5 —— 参数块字段名序列与类型。"""
    for st in PARAM_STRUCTS:
        c = common_fields.get(st, [])
        m = metal_fields.get(st, [])
        v = vulkan_fields.get(st, [])
        if not c or not m or not v:
            rep.fail(f"参数块 {st} 在某一端未解析到字段"
                     f"（common {len(c)} / Metal {len(m)} / Vulkan {len(v)}）")
            continue

        cn = [f for _, f in c]
        mn = [f for _, f in m]
        vn = [f for _, f in v]

        if cn != mn:
            rep.fail(f"{st} 字段序列 common vs Metal 不一致："
                     f"common={cn}\n            Metal ={mn}")
        if cn != vn:
            rep.fail(f"{st} 字段序列 common vs Vulkan 不一致："
                     f"common={cn}\n            Vulkan={vn}")

        # P5 类型白名单：仅 int/uint/float
        #   理由见 gpu_common.glsl 的 std140 段：vec3/数组会引入隐式填充，
        #   而 MSL 紧凑布局不会 → 两端各自编译通过但运行期错位。
        for lang, fields in (("common", c), ("Metal", m), ("Vulkan", v)):
            for ty, name in fields:
                nt = norm_type(ty)
                if nt not in ("int", "uint", "float"):
                    rep.fail(f"{st}.{name} 在 {lang} 侧类型为 {ty}"
                             f"（归一化 {nt}）；参数块**只允许** int/uint/float "
                             f"（std140 填充风险）")

    if not rep.failures:
        shapes = {st: len(common_fields.get(st, [])) for st in PARAM_STRUCTS}
        rep.note("P4/P5 参数块字段三方一致：" +
                 ", ".join(f"{k}({v} 字段)" for k, v in shapes.items()))


def gate_workgroup(rep: Report, common_text: str, metal_text: str,
                   vulkan_text: str) -> None:
    """P6 —— workgroup 尺寸三方一致。"""
    def common_val(name: str):
        m = re.search(r"#\s*define\s+" + name + r"\s+(\d+)", common_text)
        return int(m.group(1)) if m else None

    cwx, cwy, cwz = (common_val("RECON_GPU_WORKGROUP_X"),
                     common_val("RECON_GPU_WORKGROUP_Y"),
                     common_val("RECON_GPU_WORKGROUP_Z"))
    if None in (cwx, cwy, cwz):
        rep.fail("common 头未定义 RECON_GPU_WORKGROUP_X/Y/Z")
        return

    # Vulkan：`local_size_*` 声明在**每个 .comp** 里（GLSL 的 layout 是逐可执行
    # 单元的，公共头里写它只是宏定义，不构成声明）。故逐文件解析并要求一致。
    vk_vals = None
    vk_files_checked = 0
    for fname in sorted(f for f in os.listdir(VULKAN_DIR) if f.endswith(".comp")):
        ftext = read(os.path.join(VULKAN_DIR, fname))
        vk = parse_vulkan_local_size(ftext)
        if vk is None:
            rep.fail(f"Vulkan 文件 {fname} 未声明 local_size_x/y/z")
            continue
        vals: list[int | None] = []
        for tok in vk:
            if re.fullmatch(r"\d+", tok):
                vals.append(int(tok))
            else:
                # 宏名：先在**本文件**找，再退到公共头找（布局限定符要求编译期常量，
                # 宏会被预处理器展开，故两种写法都合法）。
                m = (re.search(r"#\s*define\s+" + re.escape(tok) + r"\s+(\d+)", ftext)
                     or re.search(r"#\s*define\s+" + re.escape(tok) + r"\s+(\d+)", vulkan_text))
                vals.append(int(m.group(1)) if m else None)
        if None in vals:
            rep.fail(f"Vulkan 文件 {fname} 的 local_size 无法解析为数值：{vk}")
            continue
        got = (vals[0], vals[1], vals[2])
        vk_files_checked += 1
        if got != (cwx, cwy, cwz):
            rep.fail(f"Vulkan 文件 {fname} 的 workgroup 尺寸 ({got[0]},{got[1]},{got[2]}) "
                     f"与 common ({cwx},{cwy},{cwz}) 不一致")
        if vk_vals is None:
            vk_vals = got
        elif vk_vals != got:
            rep.fail(f"Vulkan 各 .comp 的 workgroup 尺寸不一致："
                     f"{vk_vals} vs {fname} 的 {got}")
    if vk_vals is None:
        return

    # Metal：本仓库未显式声明 threads_per_threadgroup（由 host dispatch 决定）。
    # 若声明了则必须一致；未声明则记录说明（不判失败 —— 那会变成"要求写冗余声明"）。
    mt = parse_metal_threadgroup(metal_text)
    if mt is not None:
        mt_vals = []
        for tok in mt:
            if re.fullmatch(r"\d+", tok):
                mt_vals.append(int(tok))
            else:
                m = re.search(r"#\s*define\s+" + re.escape(tok) + r"\s+(\d+)", metal_text)
                mt_vals.append(int(m.group(1)) if m else None)
        if tuple(mt_vals) != (cwx, cwy, cwz):
            rep.fail(f"workgroup 尺寸不一致：common=({cwx},{cwy},{cwz}) "
                     f"Metal=({mt_vals[0]},{mt_vals[1]},{mt_vals[2]})")

    if not rep.failures:
        rep.note(f"P6 workgroup 尺寸一致：common=({cwx},{cwy},{cwz}) "
                 f"Vulkan=({vk_vals[0]},{vk_vals[1]},{vk_vals[2]})"
                 f"（{vk_files_checked} 个 .comp 全部一致）"
                 + ("；Metal 侧未显式声明（由 host dispatch 决定，允许）"
                    if mt is None else "；Metal 侧已声明且一致"))


def gate_shared_constants(rep: Report, common_text: str, metal_text: str,
                          vulkan_text: str, metal_all: str, vulkan_all: str) -> None:
    """P7 —— 跨端共享常量 `RECON_GPU_*` 的值一致 + 「引用即定义」。

    ⚠️ 本判据的设计经历了一次**自我证伪**，值得记录（教训类型：
    「判据落在错误对象上」）：

      第一版要求「common 定义的所有 `RECON_GPU_*` 在两端都必须有定义」。
      实测立即报 5 项失败 —— 但那是**判据本身错了**，不是代码错了：
        `RECON_GPU_WORKGROUP_X` 是 common 给 C++ 侧的默认值，Metal 侧用
        `threads_per_threadgroup(8,8)` 字面量、**根本不需要**这个宏；
        `RECON_GPU_ALLOW_FP16_DEFAULT` 亦然（它是 C++ 侧探测结果的默认值）。
      强行要求两端都定义 → 逼人写无用宏 → 反而鼓励"抄一遍凑数"。

      改后的判据是**用法驱动**的，不预设"两端必须有哪些宏"：
        ① 「引用即定义」：某端着色器代码里**用到了** `RECON_GPU_x`，
           该端就必须在**自己的**头里定义它（否则编译必失败，而本机无法编译
           → 这条判据是唯一能替代编译的检查）。
        ② 「值一致」：两端各自定义的共享常量，其值必须与 common 相等
           （common 是登记表）；两端之间自然也相等。
        ③ 「不得私设」：端点定义的 `RECON_GPU_*` 必须在 common 登记过
           （单侧专用量请用 `RECON_VK_` / `RECON_MSL_` 前缀）。

    这样既拦住了真正的漂移（值不同、用而不定义），又不逼人写无用代码。
    """
    c = parse_defines(common_text, "RECON_GPU_")
    m = parse_defines(metal_text, "RECON_GPU_")
    v = parse_defines(vulkan_text, "RECON_GPU_")

    macros = ("RECON_GPU_ALGO", "RECON_GPU_GAP", "RECON_GPU_DIFF")

    def numeric(d: dict[str, str]) -> dict[str, str]:
        return {k: val for k, val in d.items()
                if k not in macros and parse_number(val) is not None}

    cn = numeric(c)
    mn = numeric(m)
    vn = numeric(v)

    if not cn:
        rep.fail("common 头未解析到任何数值型跨端共享常量（RECON_GPU_*）")
        return

    # ---- ① 引用即定义 ----------------------------------------------------
    # 在「代码」（去注释、去 #define 行）里找用到的共享常量标识符。
    def used(text_all: str, own_header: str) -> set[str]:
        code = strip_comments(text_all)
        code = re.sub(r"^\s*#\s*define[^\n]*$", "", code, flags=re.M)
        names = set(re.findall(r"\b(RECON_GPU_[A-Z0-9_]+)\b", code))
        return {n for n in names if n not in macros}

    for lang, all_text, own_defs in (("Metal", metal_all, mn), ("Vulkan", vulkan_all, vn)):
        missing = sorted(used(all_text, "") - set(own_defs))
        if missing:
            rep.fail(f"{lang} 侧引用了未在本端定义的跨端常量："
                     + ", ".join(missing)
                     + "（本机无法编译着色器，此判据是编译失败的唯一替代检查）")

    # ---- ② 值一致（下方按 common 登记项逐条比对）-------------------------

    # ---- ③ 不得私设 ------------------------------------------------------
    for lang, d in (("Metal", mn), ("Vulkan", vn)):
        extra = sorted(set(d) - set(cn))
        if extra:
            rep.fail(f"{lang} 侧定义了 common 未登记的跨端常量："
                     + ", ".join(f"{k}={d[k]}" for k in extra)
                     + "（共享量必须在 common 登记；单侧专用量请用 RECON_VK_/RECON_MSL_ 前缀）")

    # ---- 值比对：common 与任一端都定义了 → 值必须相等 -------------------
    compared = 0
    for name, cval in sorted(cn.items()):
        cnum = parse_number(cval)
        for lang, d in (("Metal", mn), ("Vulkan", vn)):
            if name not in d:
                continue          # 该端不需要这个常量（见 ① 的说明），允许
            compared += 1
            if parse_number(d[name]) != cnum:
                rep.fail(f"跨端共享常量 {name} 数值不一致："
                         f"common={cval} vs {lang}={d[name]}")

    if not rep.failures:
        both = sorted(set(cn) & set(mn) & set(vn))
        rep.note(f"P7 跨端共享常量：common 登记 {len(cn)} 个，"
                 f"两端同有 {len(both)} 个且值一致"
                 f"（{', '.join(both)}）；"
                 f"common 独有 {len(set(cn) - set(both))} 个为 C++ 侧默认值"
                 f"（{', '.join(sorted(set(cn) - set(both))) or '无'}），"
                 f"两端引用即定义检查通过")


def gate_gaps(rep: Report, gaps: list[tuple[str, str]],
              metal_text: str, vulkan_text: str,
              metal_label: str = "Metal", vulkan_label: str = "Vulkan") -> None:
    """P8/P9 —— 缺口标注两端同步。"""
    if not gaps:
        rep.fail("common 头未解析到任何 RECON_GPU_GAP(模块,编号) 登记"
                 "（若确已全部补齐，应删掉本段判据而非留空 —— 故此处判失败）")
        return

    metal_tags = find_gap_tags(metal_text)
    vulkan_tags = find_gap_tags(vulkan_text)

    for module, index in gaps:
        tag = f"SOURCE-TODO-{module}-{index}"
        if tag not in metal_tags:
            rep.fail(f"{metal_label} 侧未以条目形式标注缺口 {tag}"
                     f"（common 已登记；两端必须一致地「未完成」）")
        if tag not in vulkan_tags:
            rep.fail(f"{vulkan_label} 侧未以条目形式标注缺口 {tag}"
                     f"（common 已登记；两端必须一致地「未完成」）")

    # P9 反向：两端出现的 SOURCE-TODO-* 编号都必须在 common 登记
    registered = {f"SOURCE-TODO-{m}-{i}" for m, i in gaps}
    for lang, tags in ((metal_label, metal_tags), (vulkan_label, vulkan_tags)):
        orphan = sorted(tags - registered)
        if orphan:
            rep.fail(f"{lang} 侧出现 common 未登记的缺口编号："
                     + ", ".join(orphan)
                     + "（补齐缺口后必须同步删除 common 的登记与两端的标注）")

    if not rep.failures:
        rep.note(f"P8/P9 缺口标注两端同步：{len(gaps)} 项"
                 f"（{', '.join(f'SOURCE-TODO-{m}-{i}' for m, i in gaps)}）")


def gate_diffs(rep: Report, diffs: list[str],
               metal_text: str, vulkan_text: str) -> None:
    """P10 —— 平台差异登记两端可见。"""
    if not diffs:
        rep.fail("common 头未解析到任何 RECON_GPU_DIFF(n) 登记")
        return
    for n in diffs:
        # 允许两端用各自前缀书写：VK-DIFF-n / MSL-DIFF-n / 裸 "DIFF-n"
        pat = re.compile(r"(?:VK|MSL)?-?DIFF-" + re.escape(n) + r"\b")
        if not pat.search(metal_text):
            rep.fail(f"平台差异 DIFF-{n} 未在 Metal 侧可见"
                     f"（common 已登记；两端都须说明「我是差异的哪一端」）")
        if not pat.search(vulkan_text):
            rep.fail(f"平台差异 DIFF-{n} 未在 Vulkan 侧可见")
    if not rep.failures:
        rep.note(f"P10 平台差异两端可见：{len(diffs)} 项"
                 f"（{', '.join('DIFF-' + d for d in diffs)}）")


def gate_helper_symmetry(rep: Report, metal_helpers: list[str],
                         vulkan_helpers: list[str]) -> None:
    """P11 —— 两端 helper 集合对称。

    这一条不进 P1..P10 的编号（避免与 docs 契约编号混淆），但它拦的是
    本项目最贵的一类坑：**同一逻辑在一端有 helper、另一端内联写死**，
    于是改了一处忘了另一处（MEMORY.md：「同一逻辑写两遍必然漂移」、
    裁剪处漏排除自身导致主体被裁掉 79%）。

    处置：两端公共头的 `recon_*` helper 名集合必须相等。
    —— 单侧必要的 helper 应显式登记在 common 头的差异段，而非悄悄多出来。
    """
    only_m = sorted(set(metal_helpers) - set(vulkan_helpers))
    only_v = sorted(set(vulkan_helpers) - set(metal_helpers))
    if only_m:
        rep.fail("Metal 侧独有 helper（Vulkan 侧缺失，或应为内联）："
                 + ", ".join(only_m))
    if only_v:
        rep.fail("Vulkan 侧独有 helper（Metal 侧缺失，或应为内联）："
                 + ", ".join(only_v))
    if not rep.failures:
        rep.note(f"P11 两端 helper 对称：{len(metal_helpers)} 个"
                 f"（{', '.join(metal_helpers)}）")


# ===========================================================================
# P12 CMake 列举 ↔ 实际 .metal 文件一致
# ===========================================================================
# 为什么需要这条判据（来自一次真实事故，第三十八轮）：
#
#   `CMakeLists.txt` 的 `target_sources(recon_shaders_metal INTERFACE ...)`
#   手抄了一份 .metal 清单。第三十七轮新增 `shaders/metal/yuv.metal`
#   （第 8 个算法 `recon_yuv_to_bgra`）时，**清单没跟着加**。
#
#   这类漂移的危害与「静默丢弃最难查」同构：CMake 不会因为少列一个源文件
#   而报错 —— 它只是让那个 .metal 不进 build phase。而 P1 判据查的是
#   「.metal 文件里的 kernel 名 ↔ common 声明」，它读的是**磁盘上的文件**，
#   与 CMake 清单无关 → P1 全绿，漏列照旧。
#
#   ⇒ 必须有一条判据落在「CMake 清单」这个**具体对象**上。
#     这正是 MEMORY.md「判据必须落在目标对象上」的直接应用。
def gate_cmake_metal_list(rep: Report, metal_files: list[str]) -> None:
    """P12 —— CMakeLists.txt 的 metal target_sources 清单必须与磁盘一致。"""
    cmake_path = os.path.join(ROOT, "CMakeLists.txt")
    if not os.path.isfile(cmake_path):
        rep.fail("找不到 CMakeLists.txt，无法校验 metal 源文件清单")
        return
    text = read(cmake_path)

    # 抓取 `target_sources(recon_shaders_metal INTERFACE` 之后的那个括号块。
    m = re.search(r"target_sources\(\s*recon_shaders_metal\s+INTERFACE(.*?)\)\s*",
                  text, re.S)
    if not m:
        rep.fail("CMakeLists.txt 中找不到 `target_sources(recon_shaders_metal INTERFACE ...)`；"
                 "若已改名/删除，请同步更新本判据（不要让它变成恒不触发的假判据）")
        return
    block = m.group(1)

    # 只提取 **.metal** 引用，且只比文件名。
    #   ⚠️ 不能用「所有 .h/.metal」——两端各有一份同名 `gpu_common.h`
    #      （shaders/metal/ 与 shaders/vulkan/），按 basename 比会把 Metal
    #      那份误判成「不存在的文件」。这是判据落在**错误对象**上的又一例：
    #      要校验的是「.metal 源文件集合」，不是「这个 block 里的所有引用」。
    listed: set[str] = set()
    for ref in re.findall(r"([A-Za-z0-9_./-]+\.metal)\b", block):
        listed.add(os.path.basename(ref))

    # 磁盘上的 .metal 是事实来源（P1 也读同一集合，但 P1 不校验 CMake）。
    on_disk = {f for f in metal_files if f.endswith(".metal")}

    miss = sorted(on_disk - listed)
    if miss:
        rep.fail("CMakeLists.txt 的 recon_shaders_metal 清单**漏列**了这些 .metal："
                 + ", ".join(miss)
                 + "（后果：它们不会进 build phase，且 P1 查不出）")

    if not rep.failures:
        rep.note(f"P12 CMake metal 清单 ↔ 磁盘一致：{len(on_disk)} 个 .metal"
                 f"（{', '.join(sorted(on_disk))}）")


# ===========================================================================
# P14 Vulkan .comp 预处理指令顺序自洽
# ===========================================================================
# 为什么需要这条判据（诞生于第四十一轮 CI 的一次真实事故）：
#
#   本轮新增了 `shaders-vulkan` job（用 glslangValidator 真编译 8 个 .comp），
#   它**第一次**把这些着色器交给编译器 —— 立刻全炸：
#
#     ERROR: shaders/vulkan/recon_mvs_consistency.comp:56:
#            'RECON_VK_WORKGROUP_X' : undeclared identifier
#     ERROR: :56: 'local_size' : needs a literal integer
#     ERROR: 6 compilation errors.  No code generated.
#
#   根因是**预处理指令的顺序**（8 个 .comp 全部同病）：
#
#     #version 450
#     layout(local_size_x = RECON_VK_WORKGROUP_X, …) in;   ← 56 行，引用了宏
#     #include "gpu_common.glsl"                           ← 60 行，宏在这里才定义
#
#   `RECON_VK_WORKGROUP_X` 定义在 `gpu_common.glsl` 里，而 include 写在它**之后**
#   → 展开时是未声明标识符。附带还有第二个问题：全仓库**没有**任何
#   `#extension GL_GOOGLE_include_directive : require`，`#include` 本身就不是
#   GLSL 的合规指令（glslang 只是宽容地当注释/忽略处理，于是宏永远不生效）。
#
#   为什么既有 13 条判据一条都没拦住：它们比的是**名字集合、字段序列、常量值**
#   —— 全是"文件里有没有出现某串文本"。而这是**文本的顺序与可见性**问题：
#   所有被比对的文本都在，只是出现的位置没用。P1..P13 全绿，编译全炸。
#   （与 MEMORY.md「判据必须落在目标对象上」同源：这里的目标对象是
#    "预处理后的可见性"，不是"符号集合"。）
#
#   本判据刻意做成**纯静态**（不调 glslang）：
#   ① 本机（Windows）没有 glslang，静态版让我在推送前就能自查，反馈以秒计；
#   ② CI 的 shaders-vulkan job 是**权威**（真编译），两者互补 ——
#      静态版负责"快速反馈 + 解释为什么"，CI 版负责"最终裁决"。
#
# 判据（对每个 .comp，可逐条负向注入证伪）：
#   P14-a 必须存在且**恰好一条** `#version <数字>`，且它是第一个非注释、
#         非空行的指令（GLSL 规范：`#version` 必须是首个语句）。
#         证伪：在 `#version` 前插一行 `int x;` → 报错。
#   P14-b 若引用了 `#include`，则**必须**同时声明
#         `#extension GL_GOOGLE_include_directive : require|enable`
#         （GLSL 不认识 `#include`；缺声明时 glslang 只当无关指令跳过，
#          于是 `gpu_common.glsl` **从未被包含**，宏永远不可见）。
#         证伪：删掉那行 `#extension` → 报错。
#   P14-c 任何 `#include "X"` 必须**早于**第一个引用 X 所定义宏的 `layout(...)；
#         且被 include 的文件必须**真实存在**（防引用不存在的公共头）。
#         证伪：把 include 行移到 `layout(local_size_x…)` 之后 → 报错。
#   P14-d `#include` 必须早于**任何**使用 `RECON_VK_*` / `RECON_GPU_*` 宏的位置
#         （不止 layout —— 常量也可能是编译期数组上界）。
#         证伪：在 include 前写 `#define N RECON_GPU_FIXED_MAX` 并用于数组 → 报错。
#
#   ⚠️ 反空约束：至少要有 1 个 .comp 被检查到，否则说明扫描式已失效 → 失败。
def code_lines_keep_lineno(text: str) -> list[str]:
    """逐行剥掉 `//` 与 `/* */` 注释，**保持行数不变**（行号 = 列表下标 + 1）。

    为什么不用现成的 `strip_comments()`：它用 `DOTALL` 匹配 `/* */`，会把
    跨行块注释里的换行一起删掉 → 后续所有行号左移 → 报错信息指向**无关的行**。
    本判据的核心价值就是"报出准确的行号"（CI 报的是 `:56`，本地必须能对上），
    所以必须有保行号的剥离。

    剥离粒度是**字符级**（不是"整行是注释就丢"）：`x = 1; // RECON_VK_X` 这种
    行尾注释里的宏名不应被当成使用，而同一行前面的真实代码仍要保留。
    """
    out: list[str] = []
    in_block = False
    for line in text.split("\n"):
        buf: list[str] = []
        i = 0
        while i < len(line):
            two = line[i:i + 2]
            if in_block:
                if two == "*/":
                    in_block = False
                    i += 2
                else:
                    i += 1
                continue
            if two == "/*":
                in_block = True
                i += 2
                continue
            if two == "//":
                break                      # 行尾注释：本行剩余部分丢弃
            buf.append(line[i])
            i += 1
        out.append("".join(buf))
    return out


def gate_vulkan_preprocess_order(rep: Report, vulkan_files: list[str]) -> None:
    """P14 —— Vulkan .comp 的 `#version` / `#extension` / `#include` 顺序自洽。

    `vulkan_files` 是 `shaders/vulkan/` 下的 `.comp` 文件名列表（不含路径）。
    """
    checked = 0

    for fname in vulkan_files:
        if not fname.endswith(".comp"):
            continue
        path = os.path.join(VULKAN_DIR, fname)
        if not os.path.isfile(path):
            continue
        checked += 1
        raw = read(path)
        # 一切判据都基于 `lines`（**已剥注释、行数不变**）：若用原文，文件头
        # 注释里出现的宏名（如「量化方案：value → … × RECON_GPU_FIXED_SCALE」）
        # 会被当成"使用了该宏"→ 红绿由注释措辞决定 → 判据本身不可信。
        lines = code_lines_keep_lineno(raw)

        # --- P14-a：`#version` 必须是第一个非注释行，且恰好一条 -------
        ver_lines = [i for i, s in enumerate(lines)
                     if re.match(r"^\s*#\s*version\b", s)]
        if not ver_lines:
            rep.fail(f"P14-a {fname}：没有 `#version` 指令（GLSL 计算着色器必须声明）")
            continue
        if len(ver_lines) > 1:
            rep.fail(f"P14-a {fname}：出现 {len(ver_lines)} 条 `#version`"
                     f"（行 {[i + 1 for i in ver_lines]}）—— 只允许一条")
        vline = ver_lines[0]
        if not re.search(r"#\s*version\s+\d+", lines[vline]):
            rep.fail(f"P14-a {fname}:{vline + 1}：`#version` 后缺版本号"
                     f"（形如 `#version 450`）")
        first_code = next((i for i, s in enumerate(lines)
                           if s.strip()), None)
        if first_code is not None and first_code != vline:
            rep.fail(f"P14-a {fname}：`#version` 位于第 {vline + 1} 行，"
                     f"但它之前第 {first_code + 1} 行已有非注释内容 —— "
                     f"GLSL 规定 `#version` 必须是第一个语句")

        # --- P14-b：有 #include 就必须有 GL_GOOGLE_include_directive -----
        has_include = any(re.match(r'^\s*#\s*include\s+"', s) for s in lines)
        if has_include:
            ext_ok = any(re.search(r"^\s*#\s*extension\s+GL_GOOGLE_include_directive\s*:"
                                   r"\s*(require|enable)", s) for s in lines)
            if not ext_ok:
                rep.fail(f"P14-b {fname}：用了 `#include` 但没有声明 "
                         f"`#extension GL_GOOGLE_include_directive : require`"
                         f"（GLSL 不认识 `#include`；缺此声明时编译器会把它当无关"
                         f"指令跳过，被包含的文件**从未生效**，其宏永远不可见 —— "
                         f"这正是第四十一轮 8 个 .comp 全部编译失败的原因）")

        # --- P14-c / P14-d：include 必须早于任何宏使用 -------------------
        inc_lines = [i for i, s in enumerate(lines)
                     if re.match(r'^\s*#\s*include\s+"', s)]
        for il in inc_lines:
            m = re.match(r'^\s*#\s*include\s+"([^"]+)"', lines[il])
            assert m is not None
            target = m.group(1)
            # 被包含文件必须真实存在（相对 shaders/vulkan/）
            if not os.path.isfile(os.path.join(VULKAN_DIR, target)):
                rep.fail(f"P14-c {fname}:{il + 1}：`#include \"{target}\"` 指向的"
                         f"文件不存在（应位于 shaders/vulkan/）")
                continue
            # 收集被包含文件里定义的全部宏名
            inc_text = read(os.path.join(VULKAN_DIR, target))
            macros = set(re.findall(r"^\s*#\s*define\s+([A-Za-z_]\w*)",
                                    inc_text, re.M))
            if not macros:
                continue
            # P14-c：`#include` 必须早于**第一个**引用这些宏的 `layout(...)`。
            #   （这里只针对 layout 限定符 —— 它是"宏必须编译期可见"的最典型
            #   位置。其余任意位置的使用由下面的 P14-d 统一覆盖。）
            lay_lines = [j for j, s in enumerate(lines)
                         if re.search(r"\blayout\s*\(", s)
                         and any(re.search(r"\b" + re.escape(mc) + r"\b", s)
                                 for mc in macros)]
            if lay_lines and lay_lines[0] < il:
                rep.fail(f"P14-c {fname}：第 {lay_lines[0] + 1} 行的 "
                         f"`layout(...)` 引用了 `{target}` 里的宏，"
                         f"但 `#include \"{target}\"` 在第 {il + 1} 行（更晚）—— "
                         f"布局限定符要求编译期常量，此时宏尚未定义")
            # P14-d：include **之前**是否已使用被包含文件里的宏（任意位置，
            #   含数组上界等非 layout 用法）。
            for j in range(0, il):
                used = sorted(mc for mc in macros
                              if re.search(r"\b" + re.escape(mc) + r"\b", lines[j]))
                if used:
                    rep.fail(f"P14-d {fname}:{j + 1}：在第 {il + 1} 行的 "
                             f"`#include \"{target}\"` **之前**就使用了它定义的宏 "
                             + ", ".join(used)
                             + "（预处理按文本顺序展开，此处宏尚未定义 → "
                               "编译期报 `undeclared identifier`）")

        # --- P14-d 补充：凡使用了 RECON_VK_* / RECON_GPU_* 宏，就必须有 include
        #   否则宏不可能有来源（防"某文件根本没 include 却用了公共宏"）。
        macro_use_re = re.compile(r"\b(RECON_VK_\w+|RECON_GPU_\w+)\b")
        if not inc_lines:
            bad = []
            for j, s in enumerate(lines):
                if re.match(r"^\s*#\s*(define|include)\b", s):
                    continue           # 定义处与（不存在的）include 行不算使用
                if macro_use_re.search(s):
                    bad.append((j + 1, macro_use_re.search(s).group(1)))
            if bad:
                rep.fail(f"P14-d {fname}：使用了公共宏却没有 `#include \"gpu_common.glsl\"`"
                         f"（首个：第 {bad[0][0]} 行的 {bad[0][1]}）")

    if checked == 0:
        rep.fail("P14：没有扫描到任何 .comp 文件 —— 判据已失效（反空约束）")
    elif not rep.failures:
        rep.note(f"P14 Vulkan .comp 预处理顺序自洽：{checked} 个文件"
                 f"（#version 首位 / include 早于宏使用 / GL_GOOGLE_include_directive 已声明）")


# ===========================================================================
# P15 Vulkan 共享头的「编译期扩展」自洽
# ===========================================================================
# 为什么需要这条判据（诞生于第四十一轮**首次真编译**）：
#
#   在 P14 修好 `#include` 的可见性之后，`gpu_common.glsl` **第一次**真正进
#   编译器，立刻暴露它自身的一个缺陷（8 个 .comp 全部同源、均指向该文件）：
#
#     ERROR: gpu_common.glsl:338: 'texelFetch' : required extension not
#            requested: GL_EXT_samplerless_texture_functions
#     ERROR: gpu_common.glsl:338: '=' : cannot convert from
#            'global highp 4-component vector of float' to 'temp highp float'
#
#   根因：`texelFetch(<texture2D>, coord, lod)` 的第一参数**不带 sampler**，
#   而 GLSL 核心只为 `sampler2D` 提供 `texelFetch` 重载；对裸 `texture2D`
#   做 texel fetch 必须请求 `GL_EXT_samplerless_texture_functions`。
#
#   这条判据的存在价值 = **把「共享头需要哪个扩展」变成可被静态检查的命题**。
#   P14 只能证明"共享头被读到了"，证明不了"共享头自己能编过" —— 二者是
#   不同的目标对象，不能互相替代（本项目铁律：判据要落在目标对象上）。
#
# 判据（对每个共享头文件解析其**代码体**，不含注释）：
#   P15-a 若代码里出现「对裸 texture2D 的 texelFetch / textureSize / texelFetchOffset」
#         这类 samplerless 调用，则必须声明
#         `#extension GL_EXT_samplerless_texture_functions : require|enable`。
#   P15-b 反向：若声明了该扩展，就必须真的有 samplerless 调用点
#         （防"声明了却没用到"→ 会让别人误以为该扩展是必需的）。
#   ⚠️ 反空约束：必须至少解析到 1 个共享头文件，否则判据已失效。
#
# ⚠️ 本判据是**近似**（正则不做类型推断）：它按"标识符名"判定调用对象是不是
#   裸 texture2D —— 做法是看该标识符在**本文件**里的声明是否形如
#    `texture2D <name>`（无 sampler）。对 `recon_sample_bilinear` 那种
#    **形参**为 `texture2D` 的情况，靠"函数签名里出现过 texture2D 形参"覆盖。
#    真正的最终裁决仍是 CI 的 glslang（本机现在也有 glslang 可自测）。
def gate_vulkan_samplerless_ext(rep: Report) -> None:
    """P15 —— 使用裸 texture2D 的 texelFetch 时必须请求 samplerless 扩展。"""
    shared_headers = ["gpu_common.glsl"]
    checked = 0
    ext_re = re.compile(
        r"^\s*#\s*extension\s+GL_EXT_samplerless_texture_functions\s*:"
        r"\s*(require|enable)", re.M)
    # samplerless 族：这些函数在 GLSL 核心只对 sampler* 有重载，对裸 texture*
    # 需要该扩展（`textureSize`/`texelFetch`/`texelFetchOffset` 是常用三个）。
    samplerless_fn = r"(texelFetch|texelFetchOffset|textureSize|textureQueryLod)"

    for name in shared_headers:
        path = os.path.join(VULKAN_DIR, name)
        if not os.path.isfile(path):
            continue
        checked += 1
        code = strip_comments(read(path))

        has_ext = bool(ext_re.search(code))

        # 找本文件里"裸 texture2D"的标识符：声明（uniform texture2D x / 形参
        # texture2D x）+ 形参中出现的 texture2D。
        bare_ids = set(re.findall(r"texture2D\s+([A-Za-z_]\w*)", code))
        # 也考虑 `texture2D` 出现在函数签名里的情况（helper 接收裸纹理）。
        sig_bare = bool(re.search(r"\(\s*texture2D\s+\w+", code))

        uses = []
        if bare_ids or sig_bare:
            # 只要有 samplerless 调用出现在本文件，就算需要扩展
            # （本文件的裸纹理标识符是有限的、且都在本文件内声明）。
            for m in re.finditer(samplerless_fn + r"\s*\(", code):
                uses.append(m.group(1))

        if uses and not has_ext:
            rep.fail(
                f"P15-a {name}：使用了 samplerless 纹理函数 "
                f"{', '.join(sorted(set(uses)))}（对裸 `texture2D` 调用），"
                f"但没有声明 `#extension GL_EXT_samplerless_texture_functions : require`"
                f" —— GLSL 核心只为 `sampler2D` 提供这些重载，缺此声明时编译报"
                f"`required extension not requested` + 连带的返回类型不匹配")
        elif has_ext and not uses:
            rep.fail(
                f"P15-b {name}：声明了 `GL_EXT_samplerless_texture_functions`"
                f"但代码里没有任何 samplerless 纹理函数调用 —— "
                f"该声明是多余的（会误导读者以为本文件必须依赖它）")

    if checked == 0:
        rep.fail("P15：没有扫描到任何 Vulkan 共享头 —— 判据已失效（反空约束）")
    elif not rep.failures:
        rep.note(f"P15 Vulkan 共享头扩展自洽：{checked} 个文件"
                 f"（samplerless 调用 ↔ GL_EXT_samplerless_texture_functions 声明成对）")



# 为什么需要这条判据（本判据的诞生背景 = 一次**假通过**被 CI 揭穿）：
#
#   第 39 轮之前，Metal 侧的 `recon_mvs_fuse_voxel` 用 `atomic_float` 做
#   S/W 累加，Vulkan 侧用定点 `int` 原子。CI 首次真编译（run 9）报了 8 个
#   错误：`atomic_float` 是 **Metal 3.0 / iOS 16 起**才有的类型别名，
#   而本项目部署下限是 **iOS 15.0** → 在契约允许的最低平台上该类型不存在。
#
#   修复方式是把 Metal 也改为定点整数原子（与 Vulkan 统一）。
#   但这带来一个**新的跨端不变量**：
#
#       两端的「量化 / 饱和 / 反量化」三段数学式必须逐字同构。
#
#   为什么这是硬要求而非风格问题：定点方案下两端的结果要能**逐位对拍**
#   （这是 docs/14 的跨端一致性验收基础）。任何一端少做一次饱和修正、
#   或把 `round` 换成截断、或把 scale 写成别的数，都会产生**无法归因**的
#   数值偏差 —— 而这类偏差在两端各自的单元测试里**都是绿的**。
#
#   这与 P7 的关系（**不重复**）：P7 比的是**常量的值**
#   （`#define RECON_GPU_FIXED_SCALE` 两端相等），它管不到**表达式**。
#   常量相等而表达式不同（例如一端 `round` 一端 `trunc`、
#   一端做饱和回写一端不做）恰恰是本判据要拦的形态。
#
# ── 判据分解（每一面都落在**具体可解析对象**上）─────────────────────────
#   P13a 量化式：`clamp(round(<v> * SCALE), ±MAX)` 的**操作数序列**两端相等。
#   P13b 饱和式：条件 `old_<x> > MAX - d<x>` 与回写 `MAX - (old_<x> + d<x>)`
#        两端**集合相等**（要求 x 在条件与回写中同名，即 s↔s、w↔w）。
#   P13c 反量化式：`float(<buf>[idx]) * SCALE_INV` 的**被读缓冲名集合**相等。
#   P13d 禁浮点原子：MVS 两端**都不得**出现 `atomic_float` /
#        `atomic<float>`（防未来改动把它悄悄加回来 —— 那要等一次 macOS CI
#        才能发现，代价是一整轮 CI）。
#
# ── 反空判据（**关键**）────────────────────────────────────────────────
#   每一面都强制「匹配数 >= 1」并且「两端计数相等」。
#   若某次重构把表达式换个写法导致正则匹配为 0，本判据**必须报错**而非静默
#   通过 —— 一条匹配不到任何东西的判据是**假判据**，"绿"毫无信息量。
#   这正是 MEMORY.md「判据必须落在目标对象上」与「每条闸门判据必须负向注入
#   证伪」的直接应用：负向注入时若连目标串都匹配不到，注入自然"失败"，
#   说明判据已失效。
#
# ── 语法差异的归一化（受控且最小）──────────────────────────────────────
#   两端语法确有差异，本判据只归一化**已确认为纯语法**的三处，不做语义揣测：
#     ① MSL 的 `float(RECON_GPU_FIXED_MAX)` ↔ GLSL 的 `RECON_GPU_FIXED_MAX`
#        —— MSL 的 `clamp` 要求三实参同型，故必须显式转 float（见 mvs.metal）。
#     ② MSL 的 `float(x[idx])` ↔ GLSL 的 `float(x[idx])` —— 两侧写法本就相同，
#        归一化后便于统一比对。
#     ③ 原子调用拼写（`atomic_fetch_add_explicit(&b[i], v, memory_order_relaxed)`
#        ↔ `atomicAdd(b[i], v)`）—— **本判据不比对调用拼写**，只比对**被加
#        的算术表达式**。故归一化时直接绕开调用外壳。
_CAST_STRIP_RE = re.compile(
    r"\bfloat\s*\(\s*(RECON_GPU_FIXED_MAX|RECON_GPU_FIXED_SCALE"
    r"|[A-Za-z_]\w*\s*\[\s*[^\]]+\s*\])\s*\)")


def _norm_expr(s: str) -> str:
    """把表达式归一化为「无空白 + 去纯语法 cast」的紧凑形式。

    只做空白与 cast 归一化 —— **不做**变量改名、不做等价变换。
    任何超出"纯语法"的改写都会让判据失去区分力（把真差异一起抹平）。
    """
    s = _CAST_STRIP_RE.sub(r"\1", s)
    s = re.sub(r"\s+", "", s)
    return s


# 量化式：`clamp(round(<v> * RECON_GPU_FIXED_SCALE), -MAX, MAX)`
#   捕获组 = 被量化的操作数名（`sum_contrib` / `w_contrib`）。
#   `-?` 与 `MAX` 的 cast 由 `_norm_expr` 归一化后比对。
_QUANT_RE = re.compile(
    r"clamp\s*\(\s*round\s*\(\s*(\w+)\s*\*\s*RECON_GPU_FIXED_SCALE\s*\)\s*,"
    r"\s*-(?:float\s*\(\s*)?RECON_GPU_FIXED_MAX(?:\s*\))?\s*,"
    r"\s*(?:float\s*\(\s*)?RECON_GPU_FIXED_MAX(?:\s*\))?\s*\)", re.S)

# 饱和条件：`old_<x> > RECON_GPU_FIXED_MAX - d<x>`
_SAT_COND_RE = re.compile(
    r"old_(\w+)\s*>\s*RECON_GPU_FIXED_MAX\s*-\s*d(\w+)")

# 饱和回写：`RECON_GPU_FIXED_MAX - (old_<x> + d<x>)`
_SAT_WRITE_RE = re.compile(
    r"RECON_GPU_FIXED_MAX\s*-\s*\(\s*old_(\w+)\s*\+\s*d(\w+)\s*\)")

# 反量化：`float(<buf>[idx]) * RECON_GPU_FIXED_SCALE_INV`
_DEQUANT_RE = re.compile(
    r"float\s*\(\s*(\w+)\s*\[[^\]]*\]\s*\)\s*\*\s*RECON_GPU_FIXED_SCALE_INV")

# 禁浮点原子：MSL 的 `atomic_float` 与 MSL/GLSL 共同的 `atomic<float>`。
_FLOAT_ATOMIC_RE = re.compile(r"atomic_float\b|atomic\s*<\s*float\s*>|"
                              r"atomic_float32_t\b")


def gate_fixed_point_atomics(rep: Report, metal_all: str,
                             vulkan_all: str) -> None:
    """P13 —— 定点累加协议两端同构（量化 / 饱和 / 反量化 / 禁浮点原子）。"""
    m_code = strip_comments(metal_all)
    v_code = strip_comments(vulkan_all)

    # ---- P13d 禁浮点原子（先查：若一端用了浮点原子，后三面的比对已无意义）----
    m_fa = sorted(set(_FLOAT_ATOMIC_RE.findall(m_code)))
    v_fa = sorted(set(_FLOAT_ATOMIC_RE.findall(v_code)))
    if m_fa:
        rep.fail("P13d Metal 侧出现浮点原子（" + ", ".join(m_fa) + "）—— "
                 "`atomic_float` 是 Metal 3.0 / iOS 16 起才有，本项目部署下限 "
                 "iOS 15.0（docs/12 §4.6）。必须用 `atomic_int` + 定点量化。")
    if v_fa:
        rep.fail("P13d Vulkan 侧出现浮点原子（" + ", ".join(v_fa) + "）—— "
                 "GLSL 无浮点原子支持（除非启用 "
                 "GL_EXT_shader_atomic_float_add，本项目已放弃该扩展）。")

    # ---- 三个「同构」面：逐一(两端计数>=1 且 两端集合相等) ----
    def pair(name: str, pat: re.Pattern, capture: bool = True) -> None:
        m_hits = pat.findall(m_code)
        v_hits = pat.findall(v_code)
        if not m_hits or not v_hits:
            rep.fail(f"P13 「{name}」在一端匹配为 0（Metal {len(m_hits)} / "
                     f"Vulkan {len(v_hits)}）—— 判据已失效（假判据），"
                     f"请检查正则是否还匹配当前写法")
            return
        if len(m_hits) != len(v_hits):
            rep.fail(f"P13 「{name}」两端出现次数不等"
                     f"（Metal {len(m_hits)} / Vulkan {len(v_hits)}）")
        ms = sorted(_norm_expr("".join(h) if isinstance(h, tuple) else h)
                    for h in m_hits)
        vs = sorted(_norm_expr("".join(h) if isinstance(h, tuple) else h)
                    for h in v_hits)
        if ms != vs:
            rep.fail(f"P13 「{name}」两端表达式不一致："
                     f"Metal={ms} Vulkan={vs}")

    pair("量化式 clamp(round(v * SCALE), ±MAX)", _QUANT_RE)
    pair("饱和条件 old_x > MAX - d_x", _SAT_COND_RE)
    pair("饱和回写 MAX - (old_x + d_x)", _SAT_WRITE_RE)
    pair("反量化 float(buf[idx]) * SCALE_INV", _DEQUANT_RE)

    # ---- 饱和式额外约束：条件与回写中的 `old_/d_` 后缀必须同名同集合 ----
    #   否则会出现「条件判 s、回写改 w」这类**跨缓冲写错**，
    #   而上面 pair() 只比对两端是否同构、不管内部自洽。
    for tag, code in (("Metal", m_code), ("Vulkan", v_code)):
        c = set(_SAT_COND_RE.findall(code))
        w = set(_SAT_WRITE_RE.findall(code))
        bad = {p for p in (c | w) if p[0] != p[1]}
        if bad:
            rep.fail(f"P13 {tag} 侧饱和式后缀不自洽（条件与回写不是同一累加器）："
                     + ", ".join(f"old_{a}/d_{b}" for a, b in sorted(bad)))
        if c != w:
            rep.fail(f"P13 {tag} 侧饱和条件集合与回写集合不等："
                     f"cond={sorted(c)} write={sorted(w)}")

    if not rep.failures:
        n_q = len(_QUANT_RE.findall(m_code))
        n_d = len(_DEQUANT_RE.findall(m_code))
        rep.note(f"P13 定点累加协议两端同构：量化 {n_q} 式 / "
                 f"饱和 {len(_SAT_COND_RE.findall(m_code))} 式 / "
                 f"反量化 {n_d} 式；两端均无浮点原子")


# ===========================================================================
def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--verbose", action="store_true",
                    help="打印解析到的全部集合，便于人工复核")
    args = ap.parse_args()

    # ---- 关键文件存在性（缺一个都无从比对）----
    missing = [p for p in (COMMON_H, METAL_COMMON, VULKAN_COMMON)
               if not os.path.isfile(p)]
    for d in (METAL_DIR, VULKAN_DIR):
        if not os.path.isdir(d):
            missing.append(d)
    # Vulkan 侧至少要有 7 个 .comp（MVS 六 pass + TSDF）
    if os.path.isdir(VULKAN_DIR):
        comps = sorted(f for f in os.listdir(VULKAN_DIR) if f.endswith(".comp"))
        if not comps:
            missing.append(os.path.join(VULKAN_DIR, "*.comp"))
    if missing:
        print("[shader-parity] 关键文件缺失：")
        for p in missing:
            print("  ✗ " + p)
        return 1

    common_text = read(COMMON_H)
    metal_common_text = read(METAL_COMMON)
    vulkan_common_text = read(VULKAN_COMMON)

    # Metal 侧文本 = 公共头 + 全部 .metal（缺口标注可能写在任一文件）
    metal_files = sorted(f for f in os.listdir(METAL_DIR) if f.endswith(".metal"))
    metal_all = metal_common_text + "\n" + "\n".join(
        read(os.path.join(METAL_DIR, f)) for f in metal_files)

    # Vulkan 侧文本 = 公共头 + 全部 .comp
    vulkan_files = sorted(f for f in os.listdir(VULKAN_DIR) if f.endswith(".comp"))
    vulkan_all = vulkan_common_text + "\n" + "\n".join(
        read(os.path.join(VULKAN_DIR, f)) for f in vulkan_files)

    rep = Report()

    # ---- 解析 ----
    common_algos = parse_common_algos(common_text)
    gaps = parse_common_gaps(common_text)
    diffs = parse_common_diffs(common_text)

    metal_kernels: list[str] = []
    for f in metal_files:
        metal_kernels.extend(parse_metal_kernels(read(os.path.join(METAL_DIR, f))))

    vulkan_by_file: dict[str, list[str]] = {}
    for f in vulkan_files:
        vulkan_by_file[f] = parse_vulkan_algos(read(os.path.join(VULKAN_DIR, f)))

    common_fields = {st: parse_struct_fields(common_text, st) for st in PARAM_STRUCTS}
    metal_fields = {st: parse_struct_fields(metal_common_text, st) for st in PARAM_STRUCTS}
    vulkan_fields = {st: parse_struct_fields(vulkan_common_text, st) for st in PARAM_STRUCTS}

    if args.verbose:
        print("== 解析结果 ==")
        print("common 算法 :", common_algos)
        print("common 缺口 :", [f"{m}-{i}" for m, i in gaps])
        print("common 差异 :", diffs)
        print("Metal kernel:", sorted(set(metal_kernels)))
        print("Vulkan 入口 :", {k: v for k, v in vulkan_by_file.items()})
        for st in PARAM_STRUCTS:
            print(f"{st} common:", [f for _, f in common_fields[st]])
            print(f"{st} Metal :", [f for _, f in metal_fields[st]])
            print(f"{st} Vulkan:", [f for _, f in vulkan_fields[st]])

    # ---- 判据 ----
    gate_naming(rep, common_algos, metal_kernels, vulkan_by_file)
    gate_param_layout(rep, common_fields, metal_fields, vulkan_fields)
    gate_workgroup(rep, common_text, metal_common_text, vulkan_common_text)
    gate_shared_constants(rep, common_text, metal_common_text, vulkan_common_text,
                          metal_all, vulkan_all)
    gate_gaps(rep, gaps, metal_all, vulkan_all)
    gate_diffs(rep, diffs, metal_all, vulkan_all)
    gate_helper_symmetry(rep,
                         parse_metal_helpers(metal_common_text),
                         parse_vulkan_helpers(vulkan_common_text))
    gate_cmake_metal_list(rep, metal_files)
    gate_fixed_point_atomics(rep, metal_all, vulkan_all)
    gate_vulkan_preprocess_order(rep, vulkan_files)
    gate_vulkan_samplerless_ext(rep)

    print(f"[shader-parity] common 算法 {len(common_algos)} / "
          f"Metal kernel {len(set(metal_kernels))} / "
          f"Vulkan 入口 {sum(len(v) for v in vulkan_by_file.values())}；"
          f"缺口 {len(gaps)} / 差异 {len(diffs)}")

    rep.dump()

    if rep.failures:
        print("")
        print(f"[shader-parity] FAIL：{len(rep.failures)} 项不一致")
        return 3

    print("[shader-parity] PASS：着色器两端一致（P1..P15 全部通过）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
