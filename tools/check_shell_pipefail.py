#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""check_shell_pipefail.py — 闸门 S：`set -o pipefail` 下的管道 SIGPIPE 陷阱。

================================================================================
为什么需要这条闸门（同一事故已发生 5 次，其中 2 次真的炸掉了 CI）
================================================================================
`set -o pipefail` 让**整条管道**的退出码等于「最后一个非零退出码」。对于
`writer | consumer` 这种形态，若 consumer **提前退出**（读完就关读端），writer 会
收到 SIGPIPE 而以 141 退出 —— pipefail 于是判定整条管道失败，**即使 consumer
完全正常**。

本仓库的两次真实事故（都发生在 macOS 上的 `bash -e {0}`，即 `-e` + pipefail）：

  ① `ios.yml` 的 `nm "$LIB" | grep -q "$sym"` —— `grep -q` 命中即退出 →
     `nm` 收 SIGPIPE 141 → `if !` 成立 → 报「缺少符号」（符号就在第一行）。
  ② `ios.yml` 的 `strings "$MB" | grep -qx "$fn"` —— 同机理，metallib 一大就必炸。
  ③④ `android.yml` 的两处 `yes | sdkmanager --licenses` —— `sdkmanager` 读完所需
     应答即正常退出并关读端 → `yes` 收 SIGPIPE 141 → 整条管道失败 →
     **后续 `sdkmanager --install` 根本不执行**。CI 只打印
     `yes: standard output: Broken pipe` 紧跟 `##[error]Process completed with
     exit code 1.`，**没有任何工具链错误** —— 于是这一条长期被误诊为
     「SDK 安装环境问题」，而真相是「Android 编译从未真正开始过」。

⑤ `android.yml` 的 `NM=$(find ... -name llvm-nm -type f | head -1)` ——
   同一机理，且形态**更危险**：命令替换里的失败在 `set -e` 下会**静默中止整个
   run 块**（不报错、不提示），后面的断言全部不执行。

前四处 ios.yml 已就地写了长篇注释，但 android.yml 的两处**没人同步排查** ——
这正是本项目反复教训的「修完一端要问另一端有没有同样的问题」。教训不该只躺在
注释里：注释不会被 CI 执行。故本闸门把这条踩了 5 次的坑变成**会失败的断言**。

================================================================================
判据（窄而准，逐条可证伪 —— 见 --self-test）
================================================================================
仅在「该 shell 片段的 pipefail 状态为 ON」时判定：

  **S-1**  写端是**已知会无限/持续写入**的命令（`yes`），而它没有吞 SIGPIPE
           （`{ yes || true; }` / `yes 2>/dev/null || true`）→ 报错。
           证伪：把 `{ yes || true; } | ...` 改回 `yes | ...` → 立即报错。

  **S-2**  管道**最后一段**是**提前退出**的消费者（`head`；或带
           `-q/-m N/--max-count` 的 `grep`），而整行未被守卫
           （无 `|| true` / 不在 `if`|`while`|`until`|`!` 条件位置）→ 报错。
           证伪：`find ... | head -1` → 报错；`find ... | head -1 || true` → 通过。

⚠️ **刻意不做的判定**（避免误伤 / 避免造出「看起来覆盖很广」的假判据）：
  · 不判定「任意 writer | grep -q」：`printf`/`echo` 的输出远小于管道缓冲
    （Linux 64 KB），writer 会在 consumer 退出前写完，**不会**产生 SIGPIPE。
    把它一并报出来只会让人「学会忽略这条闸门」——那比不设闸门更坏。
  · 不判定 python/node 等作为写端且带 `-q` 消费者的组合（同一理由）。
  · 不做 shell 执行/沙箱测试：判据必须是**能读的静态文本**，且在 ubuntu 与
    macOS 上给出**相同**结论（执行型判据做不到这一点）。

================================================================================
反空约束（本项目的硬规矩）
================================================================================
正则类判据一旦因文件改名/结构变化**失锚**，会静默退化为**恒真**（永远不会
报错，日志全绿）。故本闸门**必须先断言自己真的解析到了东西**：

  · 至少解析到 `--min-scripts`（默认 12）个 shell 片段（两个 workflow 的内联
    `run:` 块 + `tools/*.sh`）；
  · 且 pipefail 生效范围内的多段管道行数 ≥ `--min-piped`（默认 10）。
抽不到就**报错**（而非通过）。

================================================================================
用法
================================================================================
    python3 tools/check_shell_pipefail.py [--root .] [--self-test]
    python3 tools/check_shell_pipefail.py --self-test   # 只跑内嵌样本，不读仓库
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# ---------------------------------------------------------------------------
# 扫描面
# ---------------------------------------------------------------------------
WORKFLOW_DIR = os.path.join(".github", "workflows")
TOOLS_DIR = "tools"

DEFAULT_MIN_SCRIPTS = 12
DEFAULT_MIN_PIPED = 10


class Report:
    def __init__(self) -> None:
        self.errors: list[str] = []
        self.notes: list[str] = []

    def err(self, gate: str, msg: str) -> None:
        self.errors.append(f"[{gate}] {msg}")

    def note(self, msg: str) -> None:
        self.notes.append(msg)


# ---------------------------------------------------------------------------
# 词法小工具（只做静态文本处理，不执行任何 shell）
# ---------------------------------------------------------------------------
def strip_shell_comment(line: str) -> str:
    """去掉行尾注释，但**保留引号内的 `#`**。

    `#` 只有在「行首」或「紧随空白/分隔符」时才起注释作用（`${VAR#pat}` 与
    `a#b` 里的 `#` 都是字面量）。本项目 `tools/check_16kb.sh` 里就有 `${x#...}`
    形态，故不能简单地按第一个 `#` 切。
    """
    out: list[str] = []
    quote: str | None = None
    i = 0
    while i < len(line):
        c = line[i]
        if quote is None:
            if c in "'\"":
                quote = c
            elif c == "#" and (not out or out[-1] in " \t;&|("):
                break
            out.append(c)
        else:
            if c == "\\" and quote == '"' and i + 1 < len(line):
                out.append(c)
                i += 1
                out.append(line[i])
            elif c == quote:
                quote = None
            out.append(c)
        i += 1
    return "".join(out)


def split_pipes(line: str) -> list[str]:
    """按**未被引号包裹**的单竖线切分；`||` 与 `|&` 不算管道分隔符。"""
    parts: list[str] = []
    cur: list[str] = []
    quote: str | None = None
    i = 0
    while i < len(line):
        c = line[i]
        if quote is None:
            if c in "'\"":
                quote = c
                cur.append(c)
            elif c == "|":
                nxt = line[i + 1] if i + 1 < len(line) else ""
                if nxt in ("|", "&"):
                    cur.append(c + nxt)
                    i += 2
                    continue
                parts.append("".join(cur))
                cur = []
            else:
                cur.append(c)
        else:
            if c == "\\" and quote == '"' and i + 1 < len(line):
                cur.append(c)
                i += 1
                cur.append(line[i])
            elif c == quote:
                quote = None
                cur.append(c)
            else:
                cur.append(c)
        i += 1
    parts.append("".join(cur))
    return parts


# 打头的修饰词，跳过它们后再认命令名。
_PREFIX = r"^\s*(?:sudo\s+|command\s+|env\s+(?:-\S+\s+)*\S*=\S*\s+|env\s+)*"

# 写端：**已知会无限/持续写入**、因而在消费者提前退出时必然收 SIGPIPE 的命令。
# 只登记这一条 —— 每个条目都要能说清「为什么它一定会写超过管道缓冲」。
UNBOUNDED_WRITERS: list[tuple[str, str]] = [
    ("yes", "输出恒定且无限，必然写超过管道缓冲（64 KB）"),
]

# S-2 的写端名单：**输出规模随被处理的数据量增长**、因而可能超过管道缓冲
# （64 KB）的命令。刻意取保守方向（只报这一类）—— 因为 `printf`/`echo`/`ls`
# 这类写端的输出是有界的（固定目录、固定字符串），writer 会在 consumer 退出前
# 写完，**不会**产生 SIGPIPE。把这类一并报出来只会让人「学会忽略这条闸门」，
# 那比不设闸门更坏。
#
# ⚠️ 为什么**不含** `ls`：`ls -la "$APP" | head -40`（ios.yml 的产物诊断步骤）
#    是真实存在的写法，而 `ls` 单层目录列举的输出是有界的（几十行）。把它算作
#    大数据写端会让这条闸门对该行报**假红**，进而阻塞 CI —— 本条注释就是那次
#    假红的记录。
_LARGE_WRITER = re.compile(
    _PREFIX + r"(?P<cmd>"
    r"find|nm|strings|grep|egrep|objdump|readelf|otool|size"
    r"|tar|cpio|du|zipinfo|unzip|ar|ranlib|cat|yes"
    r")\b")

# 命令替换/赋值的**前导**：`NM=$(find ...` 里的写端是 `find`，不是 `NM`。
# 不去掉这一层就认不出写端 → `NM=$(find ... | head -1)` 这条**真事故**会被漏掉
# （本闸门 --self-test 的第 4 个样本就是钉这一点的）。
_LEADING_ASSIGN = re.compile(r"^\s*(?:[A-Za-z_]\w*\s*=\s*)?\$\(\s*|^\s*[A-Za-z_]\w*\s*=\s*")


def strip_leading_wrapper(seg: str) -> str:
    """剥掉 `VAR=$(...)` / `$(...)` / `VAR=` 前导，露出**真正的命令名**。"""
    prev = None
    cur = seg
    while prev != cur:
        prev = cur
        cur = _LEADING_ASSIGN.sub("", cur, count=1)
    return cur

_STATEMENT_STARTER = re.compile(r"^\s*(?:if|elif|while|until|!)\b")
# `|| true` / `|| :` 出现在**整行**任意位置即视为已守卫。
# ⚠️ 刻意取「保守放宽」方向（少报错）：裸 `\b` 之后不加 `$` 锚 —— 因为守卫可能
#    落在命令替换的收尾 `)` 之前（`NM=$(find ... | head -1 || true)`），也可能
#    落在 `>` 重定向之后。要求「紧跟行尾」会漏判这两类**已经修好**的写法，
#    从而对正确代码报假红（本闸门 --self-test 的第 4/5 个样本就是钉这一点的）。
_GUARDED = re.compile(r"\|\|\s*(?:true|:)\b")
# 提前退出的消费者。
_EARLY_CONSUMER = re.compile(
    _PREFIX + r"(?P<cmd>head|grep)\b(?P<rest>.*)$")


def classify_early_consumer(seg: str) -> str | None:
    """返回 `head` / `grep-early`，或 `None`（= 不是提前退出的消费者）。"""
    m = _EARLY_CONSUMER.match(seg)
    if not m:
        return None
    cmd = m.group("cmd")
    if cmd == "head":
        # 只认 `head`（无参时默认头 10 行；带 -n 也一样提前退出）。
        return "head"
    rest = m.group("rest")
    # ⚠️ `\b` 后面**不能**跟 `?`（`\b?` 是非法正则："nothing to repeat"）。
    if re.search(r"(?:^|\s)(?:-q|--quiet|--silent|--max-count|-m)\b(?:\s*=?\s*\d+)?",
                 rest):
        return "grep-early"
    return None


def pipefail_state(line: str, state: bool) -> bool:
    """更新 pipefail 开关状态（只认 `set -o pipefail` 系列）。"""
    if re.search(r"\bset\s+(?:-[a-zA-Z]*o\s*pipefail|-[a-zA-Z]*o\s+pipefail)", line):
        return True
    if re.search(r"\bset\s+\+o\s+pipefail", line):
        return False
    # `set -euo pipefail` / `set -eu -o pipefail` / `set -o pipefail` 长形态
    if re.search(r"\bset\s+-[a-zA-Z]*\b[^|]*\bpipefail\b", line) and "set +" not in line:
        return True
    return state


def analyze_shell(name: str, raw: str) -> tuple[list[tuple[str, str, str, int]], int, int]:
    """返回 (命中列表, 多段管道行数, 是否为惰性片段)。

    命中列表元素：(gate, 说明, 代码, 行号)。
    """
    hits: list[tuple[str, str, str, int]] = []
    pipefail = False
    n_piped = 0
    saw_pipefail = False

    for ln, raw_ln in enumerate(raw.splitlines(), 1):
        code = strip_shell_comment(raw_ln)
        if not code.strip():
            continue
        pipefail = pipefail_state(code, pipefail)
        if pipefail:
            saw_pipefail = True
        # `run: |` 的 YAML 缩进已由调用方剥掉；这里再兜一层前导空白。
        stripped = code.strip()
        parts = split_pipes(stripped)
        if len(parts) < 2:
            continue
        if not pipefail:
            continue
        n_piped += 1

        guarded = bool(_GUARDED.search(stripped))
        conditional = bool(_STATEMENT_STARTER.match(stripped))

        # ---- S-1：无限写端未吞 SIGPIPE ----------------------------------
        first = parts[0]
        for cmd, why in UNBOUNDED_WRITERS:
            if re.match(_PREFIX + re.escape(cmd) + r"\b", first) is None:
                continue
            # 三种「已处理」写法都接受：
            #   ① `{ yes || true; } | consumer` —— **推荐**：只吞 yes 的 SIGPIPE，
            #      consumer 真失败仍会传播；
            #   ② `yes 2>/dev/null | consumer || true` —— 有效但**较钝**：整条
            #      管道的失败都被吞掉（含 consumer 的真失败），故不推荐；
            #   ③ 任何形式的、落在**整行**的 `|| true`（守卫可能落在管道末段，
            #      如 `yes ... | x > /dev/null || true` —— 此时 parts[0] 里
            #      看不到它，必须按整行判）。
            if guarded:
                break
            if re.search(r"\{\s*" + re.escape(cmd) + r"\b[^}]*\|\|\s*true\s*;?\s*\}", first):
                break
            if re.search(re.escape(cmd) + r"[^|]*2>\s*/dev/null[^|]*\|\|\s*true", stripped):
                break
            hits.append((
                "S-1",
                f"写端 `{cmd}` {why}；消费者一旦提前退出它就收 SIGPIPE（141），"
                f"而 `set -o pipefail` 会令**整条管道**失败。修法："
                f"`{{ {cmd} || true; }} | consumer` —— 只吞 {cmd} 的 SIGPIPE，"
                f"消费者真失败仍会传播（若把 `|| true` 挂在整条管道尾部，"
                f"消费者真失败也会被吞掉，故不推荐）",
                stripped, ln))
            break

        # ---- S-2：提前退出的消费者未守卫 ---------------------------------
        # 只有「写端规模可能超过管道缓冲」时才判定 —— 见 `_LARGE_WRITER` 注释。
        last = parts[-1]
        early = classify_early_consumer(last)
        wm = _LARGE_WRITER.match(strip_leading_wrapper(first))
        if (early is not None and wm is not None
                and not guarded and not conditional):
            wcmd = wm.group("cmd")
            hits.append((
                "S-2",
                f"写端 `{wcmd}` 的输出规模可能超过管道缓冲（64 KB），而管道末段"
                f" `{early}` 会**提前退出并关闭读端** → `{wcmd}` 收 SIGPIPE（141）"
                f"→ pipefail 令整条管道失败。若写在 `$( )` 里，`set -e` 还会"
                f"**静默中止整个 run 块**（不报错、不提示 —— 本仓库 CI #25 的"
                f"`NM=$(find ... | head -1)` 正是此形态）。修法："
                f"`... | {early} ... || true`，或先落临时文件再对它操作，"
                f"或放进 `if`/`while` 条件位置",
                stripped, ln))

    return hits, n_piped, saw_pipefail


# ---------------------------------------------------------------------------
# 扫描器：workflow 的 run: 块 + tools/*.sh
# ---------------------------------------------------------------------------
def iter_workflow_run_blocks(root: str) -> list[tuple[str, str, int]]:
    """产出 (显示名, 脚本文本, 起始行号)。只做**缩进级**的 YAML 块解析。

    本仓库的 `run:` 只有两种形态：块标量 `run: |`（含 `>-`）与单行 `run: cmd`。
    刻意不引入 YAML 依赖 —— 一个纯文本闸门不该因为第三方库版本而变行为。
    """
    out: list[tuple[str, str, int]] = []
    wfdir = os.path.join(root, WORKFLOW_DIR)
    if not os.path.isdir(wfdir):
        return out
    for fn in sorted(os.listdir(wfdir)):
        if not fn.endswith((".yml", ".yaml")):
            continue
        path = os.path.join(wfdir, fn)
        with open(path, encoding="utf-8") as fh:
            lines = fh.read().splitlines()
        i = 0
        while i < len(lines):
            m = re.match(r"^(\s*)run:\s*[|>][-+]?\s*$", lines[i])
            if m:
                base = len(m.group(1))
                j = i + 1
                blk: list[str] = []
                while j < len(lines):
                    cur = lines[j]
                    if cur.strip() == "":
                        blk.append(cur)
                        j += 1
                        continue
                    if len(cur) - len(cur.lstrip()) <= base:
                        break
                    blk.append(cur)
                    j += 1
                # 统一剥掉块缩进（取首个非空行的缩进为准）
                ind = 0
                for b in blk:
                    if b.strip():
                        ind = len(b) - len(b.lstrip())
                        break
                body = [b[ind:] if len(b) >= ind else b for b in blk]
                out.append((f"{fn}:run@{i + 1}", "\n".join(body), i + 1))
                i = j
                continue
            m2 = re.match(r"^\s*run:\s*(\S.*)$", lines[i])
            if m2:
                out.append((f"{fn}:run@{i + 1}", m2.group(1), i + 1))
            i += 1
    return out


def iter_tool_scripts(root: str) -> list[tuple[str, str, int]]:
    out: list[tuple[str, str, int]] = []
    tdir = os.path.join(root, TOOLS_DIR)
    if not os.path.isdir(tdir):
        return out
    for fn in sorted(os.listdir(tdir)):
        if not fn.endswith(".sh"):
            continue
        path = os.path.join(tdir, fn)
        with open(path, encoding="utf-8") as fh:
            out.append((f"tools/{fn}", fh.read(), 1))
    return out


def collect_checkable(root: str, require_pipefail: bool = True):
    """返回 (片段列表, 多段管道行总数)。`require_pipefail` 只影响统计口径。"""
    frags = iter_workflow_run_blocks(root) + iter_tool_scripts(root)
    total_piped = 0
    for name, text, _ in frags:
        _, n, _ = analyze_shell(name, text)
        total_piped += n
    _ = require_pipefail
    return frags, total_piped


# ---------------------------------------------------------------------------
# 主闸门
# ---------------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--root", default=".",
                    help="仓库根（默认 .；CI 传 ${CMAKE_CURRENT_SOURCE_DIR}）")
    ap.add_argument("--self-test", action="store_true",
                    help="只跑内嵌样本自检，不读仓库")
    ap.add_argument("--min-scripts", type=int, default=DEFAULT_MIN_SCRIPTS)
    ap.add_argument("--min-piped", type=int, default=DEFAULT_MIN_PIPED)
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    root = os.path.abspath(args.root)
    rep = Report()

    frags = iter_workflow_run_blocks(root) + iter_tool_scripts(root)
    if not frags:
        print("[shell-pipefail] 未找到任何 shell 片段 —— 判据的目标对象缺失，"
              "不得静默通过")
        return 1

    # ---- 反空约束 -------------------------------------------------------
    # ⚠️ 这里**刻意不用「片段总数 ≥ N」这种做法**：任何固定阈值都会随工作区里
    #    文件增删而变，且对「某一类来源整体消失」不敏感 —— 把 `tools/*.sh` 全删掉
    #    仍可能有 36 个 run 块，总数照样大于阈值 → 断言恒真（**假判据**，本仓库
    #    已把这类东西列为最大风险类别）。故改为**按来源分类**各断言非空：
    #    两类来源（workflow 内联 run 块 / tools/*.sh）任一为空即报错。
    wf_frags = iter_workflow_run_blocks(root)
    sh_frags = iter_tool_scripts(root)
    n_pipefail_blocks = 0
    for _n, _t, _b in wf_frags:
        if re.search(r"\bset\s+[^\n]*pipefail", _t):
            n_pipefail_blocks += 1
    n_pipefail_sh = 0
    for _n, _t, _b in sh_frags:
        if re.search(r"\bset\s+[^\n]*pipefail", _t):
            n_pipefail_sh += 1

    if not wf_frags:
        rep.err("S0", f"{WORKFLOW_DIR} 下解析到 0 个 run 块 —— 判据的目标对象"
                      f"缺失（扫描面消失后本闸门会静默退化为恒真）")
    if not sh_frags:
        rep.err("S0", f"{TOOLS_DIR}/*.sh 解析到 0 个脚本 —— 同上")
    if n_pipefail_blocks == 0:
        rep.err("S0", f"{WORKFLOW_DIR} 下**没有任何** run 块声明 `set -o "
                      f"pipefail` —— 判据无从执行（本仓库应有若干）")
    if n_pipefail_sh == 0:
        rep.err("S0", f"{TOOLS_DIR}/*.sh 里没有任何脚本声明 `set -o pipefail`"
                      f" —— 判据无从执行")

    # ---- 逐片段分析 -----------------------------------------------------
    total_piped = 0
    n_with_pipefail = 0
    hits: list[tuple[str, str, str, int, str]] = []
    for name, text, _base in frags:
        h, n, saw = analyze_shell(name, text)
        total_piped += n
        if saw:
            n_with_pipefail += 1
        for gate, why, code, ln in h:
            hits.append((gate, name, code, ln, why))

    # ⚠️ 计数型阈值只作为**兜底**（防止扫描面整体塌缩），主断言是上面的分类非空。
    if len(frags) < args.min_scripts:
        rep.err("S0", f"只解析到 {len(frags)} 个 shell 片段（期望 ≥ "
                      f"{args.min_scripts}）—— 扫描面异常缩小")
    if total_piped < args.min_piped:
        rep.err("S0", f"pipefail 生效范围内的多段管道行只有 {total_piped} 行"
                      f"（期望 ≥ {args.min_piped}）—— 管道解析可能已失效"
                      f"（解析失败 → 一条也不会被检查）")

    for gate, name, code, ln, why in hits:
        rep.err(gate, f"{name}:{ln}  {why}\n        源码：{code}")

    print("=" * 68)
    print("闸门 S：set -o pipefail 下的管道 SIGPIPE 陷阱")
    print("=" * 68)
    print(f"扫描片段 {len(frags)} 个（workflow run 块 {len(wf_frags)} / "
          f"tools/*.sh {len(sh_frags)}；其中 {n_with_pipefail} 个 pipefail 生效）"
          f"，多段管道行 {total_piped} 行")
    print(f"  反空约束  workflow 内 pipefail 块 {n_pipefail_blocks} 个 / "
          f"tools 脚本内 {n_pipefail_sh} 个（两类均须 > 0）")
    for n in rep.notes:
        print(f"  说明  {n}")
    if rep.errors:
        for e in rep.errors:
            print(f"  错误  {e}")
        print(f"\n结论：FAIL（{len(rep.errors)} 项）")
        return 1
    print("  说明  S-1 无限写端（yes）未吞 SIGPIPE：0 处 ✓")
    print("  说明  S-2 提前退出消费者（head / grep -q）未守卫：0 处 ✓")
    print("\n结论：PASS（S-1 / S-2 均无命中；反空约束成立）")
    return 0


# ---------------------------------------------------------------------------
# --self-test：把判据本身当目标对象（每例都能被反向证伪）
# ---------------------------------------------------------------------------
SELF_CASES: list[tuple[str, str, str, list[str]]] = [
    # (用例名, 脚本文本, 期望判定, 期望命中的 gate 标签)
    ("S-1 真事故：yes | sdkmanager（android.yml CI #25）",
     "set -euo pipefail\n"
     "yes | sdkmanager --licenses > /dev/null\n",
     "hit", ["S-1"]),

    ("S-1 已修：{ yes || true; } | sdkmanager",
     "set -euo pipefail\n"
     "{ yes || true; } | sdkmanager --licenses > /dev/null\n",
     "clean", []),

    ("S-1 已修：yes 2>/dev/null | x || true",
     "set -euo pipefail\n"
     "yes 2>/dev/null | x > /dev/null || true\n",
     "clean", []),

    ("S-2 真事故：NM=$(find ... | head -1)",
     "set -euo pipefail\n"
     'NM=$(find "${SDK_ROOT}/ndk" -name llvm-nm -type f | head -1)\n',
     "hit", ["S-2"]),

    ("S-2 已修：find ... | head -1 || true",
     "set -euo pipefail\n"
     'NM=$(find "${SDK_ROOT}/ndk" -name llvm-nm -type f | head -1 || true)\n',
     "clean", []),

    ("S-2 真事故：grep -q（ios.yml 已修的那条）",
     "set -euo pipefail\n"
     'nm "$LIB" | grep -q "$sym"\n',
     "hit", ["S-2"]),

    ("S-2 已修：先落文件再 grep 普通文件",
     "set -euo pipefail\n"
     'NM_LIST=$(mktemp)\n'
     'nm "$LIB" > "$NM_LIST"\n'
     'grep -q "$sym" "$NM_LIST"\n',
     "clean", []),

    ("S-2 条件位置豁免：if nm | grep -q ...",
     "set -euo pipefail\n"
     'if nm "$LIB" | grep -q "$sym"; then echo ok; fi\n',
     "clean", []),

    ("S-2 已修：glslangValidator --version | head -3 || true",
     "set -euo pipefail\n"
     "glslangValidator --version | head -3 || true\n",
     "clean", []),

    ("不误伤：printf | grep -q（输出远小于管道缓冲，无 SIGPIPE）",
     "set -euo pipefail\n"
     'printf "%s" "$s" | grep -q "x"\n',
     "clean", []),

    ("不误伤：无 pipefail 时同样的坏形态（-e 单独不足以触发）",
     "set -eu\n"
     "yes | sdkmanager --licenses > /dev/null\n",
     "clean", []),

    ("不误伤：set +o pipefail 局部关闭后的坏形态",
     "set -euo pipefail\n"
     "set +o pipefail\n"
     "yes | sdkmanager --licenses > /dev/null\n",
     "clean", []),

    ("不误伤：注释里提到坏形态",
     "set -euo pipefail\n"
     "# 绝不可写 yes | sdkmanager --licenses\n"
     "{ yes || true; } | sdkmanager --licenses > /dev/null\n",
     "clean", []),

    ("不误伤：引号里的竖线不是管道",
     "set -euo pipefail\n"
     'MSG="a | b | c"\n'
     "echo \"$MSG\"\n",
     "clean", []),
]


def self_test() -> int:
    print("=" * 68)
    print("闸门 S 自检：把判据本身当目标对象（每例可反向证伪）")
    print("=" * 68)
    fail = 0
    for name, body, expect, tags in SELF_CASES:
        hits, _n, _saw = analyze_shell(name, body)
        got_tags = sorted({h[0] for h in hits})
        if expect == "hit":
            ok = bool(hits) and all(t in got_tags for t in tags)
        else:
            ok = not hits
        mark = "✓" if ok else "✗"
        print(f"  {mark} {name}")
        if not ok:
            print(f"      期望 {expect} {tags}，实得 {got_tags}")
            for h in hits:
                print(f"      命中 {h[0]}: {h[2]}")
            fail = 1
    if fail:
        print("\n::error::闸门 S 自检未通过")
        return 1
    print(f"\nOK: {len(SELF_CASES)} 个样本全部符合预期"
          f"（{sum(1 for c in SELF_CASES if c[2] == 'hit')} 例报错 / "
          f"{sum(1 for c in SELF_CASES if c[2] == 'clean')} 例不误伤）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
