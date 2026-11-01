#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# ci_run_gates.sh — 按选择器运行静态契约闸门（.github/workflows/ios.yml 的
#                   `static-contract` job 调用）。
#
# ---------------------------------------------------------------------------
# 为什么需要本文件（单一实现原则，勿把映射抄回 YAML）
# ---------------------------------------------------------------------------
# `static-contract` job 需要「手工触发时只跑我关心的那几条闸门」的能力：改一个
# CMake 变量却要等 11 条闸门 + 两组 macOS CI 编译跑完，反馈周期太长。
#
# 若把「闸门 → 命令」的映射写进 YAML 的 `if:` 条件里，那份映射就会与
# 「闸门 → 名字」的映射成为**两处事实来源**：将来新增一条闸门，很容易只改一处
# ——于是新闸门在手选模式下永远不跑，而日志里看不出任何异常
# （属 MEMORY.md「静默丢弃最难查」类）。故映射只写一份放在本脚本；
# YAML 只负责把选择器传进来。
#
# 反过来，缺省（未传参）时的行为**必须与并入本脚本之前逐条一致**，否则
# push / pull_request 触发的默认 CI 覆盖面会悄悄缩小。`--self-test` 就是钉住
# 这一点的（见下）。
#
# ---------------------------------------------------------------------------
# 用法
# ---------------------------------------------------------------------------
#   bash tools/ci_run_gates.sh                    # 等价 all（默认，与历史一致）
#   bash tools/ci_run_gates.sh all
#   bash tools/ci_run_gates.sh generate,ios        # 逗号分隔；空格被忽略
#   bash tools/ci_run_gates.sh --self-test         # 只校验选择器映射，不跑闸门
#
# 选择器（= 能力点，每个对应恰好一条命令）：
#   generate  生成 iOS 工程描述（其余 iOS 类闸门的**前置**）
#   ios       iOS 静态契约 G1..G14
#   neg-ios   上条的负向注入
#   parity    双端功能对等（1..9）
#   neg-parity
#   shaders   着色器两端一致性（P1..P15）
#   neg-shaders
#   android   Android 静态契约（G-AND-1..5：JNI target 覆盖 / loadLibrary 动态库形态 /
#             Manifest 资源与类名 / Kotlin 依赖 / res 反空）
#   neg-android  上条的负向注入（15 项）
#   toolchain Android 工具链版本一致性（NDK/AGP/Gradle/Kotlin/CMake 五处声明）
#   neg-toolchain 上条的负向注入（12 项）
#   elf       ELF PT_LOAD 对齐解析器自检（16 KB 门禁的判据实现本体）
#   abi       C ABI 漂移闸门
#   params    参数契约漂移闸门
#   license   第三方许可扫描（GPL 家族硬阻断）
#   xcodegen  xcodegen --project-root 防回归检查
#
# 组合选择器（便于一次选一组，避免手抄长列表）：
#   all       = 全部能力点（默认）
#   ios       = generate,ios,neg-ios        ← 同时是能力点名，故会顺带补生成器
#   parity    = generate,parity,neg-parity
#   shaders   = shaders,neg-shaders
#   android   = android,neg-android
#   toolchain = toolchain,neg-toolchain
#   negative  = neg-ios,neg-parity,neg-shaders,neg-android,neg-toolchain
#
#   `all` 单独出现即全选。未知选择器**报错退出**（不静默忽略 —— 否则打错字会
#   表现为"以为跑了其实没跑"）。
#
# ⚠️ `generate` 的自动补齐：`ios` / `parity` / `xcodegen` 都会校验 iOS 生成物
#    （platform/ios/*.xcconfig、Package.swift、Container/project.yml）与磁盘一致。
#    漏跑生成器时这些闸门是在对**陈旧文件**判 —— "判据落在错误对象上"。
#    故只要选了这三者，本脚本自动补上 generate。
#
# ⚠️ Android 侧的**真编译**与 **16 KB APK 校验**不在这里：前者需要
#    JDK + Android SDK + NDK（属 .github/workflows/android.yml 的 build job），
#    后者需要构建出的 APK 产物（无产物时 `check_16kb.sh` 会**硬失败**，
#    不静默降级 —— 故它绝不能塞进零产物的静态 job）。
#    这里只跑"纯静态、无工具链、任何机器可跑"的那部分。
#
# ⚠️ Vulkan SPIR-V 的**实证**编译不在这里：它需要 glslangValidator，属独立 job
#    `shaders-vulkan`（与只做语法/一致性检查的闸门分开 —— 前者需要真工具链）。
#
# 退出码：0 全部通过；1 有闸门失败或用法错误。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT}"

# ---------------------------------------------------------------------------
# 能力点全集（顺序 = 执行顺序，且与并入本脚本之前的 YAML step 顺序逐条对应）
# ---------------------------------------------------------------------------
CAPS_ALL="generate ios neg-ios parity neg-parity shaders neg-shaders android neg-android toolchain neg-toolchain elf abi params license xcodegen"

# 互为「组合选择器 → 能力点」的展开表。
# 键与值都只用空格分隔的普通字符串（bash 3.2 无关联数组，macOS 上也能跑）。
selector_expand() {
  case "$1" in
    all)        echo "${CAPS_ALL}" ;;
    ios)        echo "generate ios neg-ios" ;;
    parity)     echo "generate parity neg-parity" ;;
    shaders)    echo "shaders neg-shaders" ;;
    android)    echo "android neg-android" ;;
    toolchain)  echo "toolchain neg-toolchain" ;;
    negative)   echo "neg-ios neg-parity neg-shaders neg-android neg-toolchain" ;;
    generate | neg-ios | neg-parity | neg-shaders | neg-android | neg-toolchain | \
    elf | abi | params | license | xcodegen)
                echo "$1" ;;
    *)          return 1 ;;
  esac
  return 0
}

# 选择器串 → 去重后的能力点列表（按 CAPS_ALL 顺序输出）。
# **这是唯一的解析实现**：runner 与 --self-test 都调它。
parse_selector() {
  local raw="$1"
  local sel="${raw//[[:space:]]/}"
  [ -z "${sel}" ] && sel="all"

  local picked=""
  local part
  local _oldifs="${IFS}"
  IFS=','
  for part in ${sel}; do
    IFS="${_oldifs}"
    [ -z "${part}" ] && continue
    local sub
    if ! sub="$(selector_expand "${part}")"; then
      IFS="${_oldifs}"
      echo "无法识别的选择器：'${part}'" >&2
      return 1
    fi
    picked="${picked} ${sub}"
    IFS=','
  done
  IFS="${_oldifs}"

  # 按 CAPS_ALL 的顺序过滤去重输出
  local out="" c
  for c in ${CAPS_ALL}; do
    case " ${picked} " in
      *" ${c} "*) out="${out} ${c}" ;;
    esac
  done
  echo "${out# }"
  return 0
}

# ---------------------------------------------------------------------------
# --self-test：把「选择器 → 能力点」映射本身当目标对象来判
# ---------------------------------------------------------------------------
# 三条断言（每条都能被反向证伪，故不是假闸门）：
#   ① 每个已知选择器都解析出**非空**集合；
#      证伪方式：把 selector_expand 的某个分支改成 `echo ""` → 立刻报错。
#   ② 每个窄选择器解析出的集合 ⊆ all 的集合；
#      证伪方式：让某个选择器多吐一个不存在的能力点（如 `echo "generate ios typo"`）
#      —— 注意该错值必须**不在** CAPS_ALL 里，否则被顺序过滤吃掉，反而掩盖错误。
#   ③ all 的集合大小 == CAPS_ALL 个数，且并集 == all（没有能力点被漏挂）；
#      证伪方式：往 CAPS_ALL 加一个能力点却不给它任何选择器 → ③ 报"不可达"。
if [ "${1:-}" = "--self-test" ]; then
  echo "== ci_run_gates.sh 选择器自检 =="
  fail=0

  n_all="$(parse_selector all | wc -w | tr -d ' ')"
  expected="$(echo ${CAPS_ALL} | wc -w | tr -d ' ')"
  echo "all 解析出 ${n_all} 个能力点（CAPS_ALL 声明 ${expected} 个）"
  if [ "${n_all}" != "${expected}" ]; then
    echo "::error::all 未覆盖全部能力点（${n_all} != ${expected}）"
    fail=1
  fi

  # ① 非空性 + ② 子集性
  # ⚠️ 本列表必须与 `selector_expand` 的分支**逐项一致**（含组合选择器）。
  #    漏一个组合选择器 → 它的展开结果不参与下面的 ③ 并集断言，
  #    于是"该选择器指向已被删除的能力点"这类错误不会被发现。
  for s in generate ios parity shaders android toolchain elf negative abi params license xcodegen; do
    caps="$(parse_selector "${s}")" || { echo "::error::选择器 ${s} 解析失败"; fail=1; continue; }
    n="$(echo "${caps}" | wc -w | tr -d ' ')"
    echo "  - ${s} → ${n} 个：${caps}"
    if [ "${n}" -eq 0 ]; then
      echo "::error::选择器 '${s}' 解析为空集 —— 会表现为「一条闸门都不跑但日志全绿」"
      fail=1
    fi
    for c in ${caps}; do
      case " ${CAPS_ALL} " in
        *" ${c} "*) ;;
        *) echo "::error::选择器 '${s}' 吐出了非法能力点 '${c}'"; fail=1 ;;
      esac
    done
  done

  # ③ 并集 == all，且每个能力点都可达（有选择器能选到它）
  #    ⚠️ 同上：列表必须与 selector_expand 的分支逐项一致。
  union=""
  for s in generate ios parity shaders android toolchain elf negative abi params license xcodegen; do
    union="${union} $(parse_selector "${s}")"
  done
  for c in ${CAPS_ALL}; do
    case " ${union} " in
      *" ${c} "*) ;;
      *) echo "::error::能力点 '${c}' 没有任何选择器能选到它（漏挂闸门）"; fail=1 ;;
    esac
  done

  # ④ 未知选择器必须非零退出（不静默忽略）
  if parse_selector "ions" >/dev/null 2>&1; then
    echo "::error::错拼的选择器 'ions' 被静默接受了（应与 'ios' 严格区分）"
    fail=1
  else
    echo "  - 错拼选择器 'ions' 被正确拒绝"
  fi

  if [ "${fail}" -ne 0 ]; then
    echo "::error::ci_run_gates.sh 选择器自检未通过"
    exit 1
  fi
  echo "OK: 选择器映射自检通过（非空 / 子集 / 全可达 / 错拼拒绝）"
  exit 0
fi

# ---------------------------------------------------------------------------
# 正常路径
# ---------------------------------------------------------------------------
RAW="${1:-all}"
if ! CAPS="$(parse_selector "${RAW}")"; then
  echo "::error::ci_run_gates.sh 选择器错误（值：'${RAW}'）—— 见文件头用法"
  exit 1
fi

wants() {
  case " ${CAPS} " in
    *" $1 "*) return 0 ;;
    *) return 1 ;;
  esac
}

if command -v python3 >/dev/null 2>&1; then PY=python3
elif command -v python >/dev/null 2>&1; then PY=python
else
  echo "::error::未找到 python（本 job 的全部闸门都是 Python 脚本）"
  exit 1
fi

echo "== 选择器 '${RAW}' → 将执行 $(echo ${CAPS} | wc -w | tr -d ' ') 条闸门 =="
echo "   ${CAPS}"

step() {
  echo ""
  echo "──────────────────────────────────────────────────────────────"
  echo "▶ $1"
  echo "──────────────────────────────────────────────────────────────"
  shift
  "$@"
}

# ---------------------------------------------------------------------------
# 按 CAPS 顺序执行（= 并入本脚本之前 YAML step 的顺序，便于逐轮对比日志）
# ---------------------------------------------------------------------------
if wants generate; then
  step "生成 iOS 工程描述" "${PY}" tools/gen_ios_project.py
fi

if wants ios; then
  step "iOS 静态契约闸门（G1..G14）" "${PY}" tools/check_ios_contract.py
fi
if wants neg-ios; then
  step "iOS 静态契约负向注入（证明 G 判据非假闸门）" \
    "${PY}" tools/verify_ios_contract_negative.py
fi

if wants parity; then
  step "双端功能对等闸门（1..9）" "${PY}" tools/check_platform_parity.py
fi
if wants neg-parity; then
  step "双端对等负向注入" "${PY}" tools/verify_platform_parity_negative.py
fi

if wants shaders; then
  step "着色器两端一致性闸门（P1..P15）" "${PY}" tools/shader_parity.py
fi
if wants neg-shaders; then
  step "着色器一致性负向注入" "${PY}" tools/verify_shader_parity_negative.py
fi

# --- Android 静态契约（纯静态：无 JDK/SDK/NDK/产物，任何机器可跑）---------
if wants android; then
  step "Android 静态契约闸门（G-AND-1..5）" "${PY}" tools/check_android_contract.py
fi
if wants neg-android; then
  step "Android 契约负向注入（证明 G-AND 判据非假闸门）" \
    "${PY}" tools/verify_android_contract_negative.py
fi

# --- Android 工具链版本一致性（五处声明 vs docs/12 §3.1 锁定值）-------------
if wants toolchain; then
  step "Android 工具链版本一致性（NDK/AGP/Gradle/Kotlin/CMake）" \
    "${PY}" tools/check_toolchain.py
fi
if wants neg-toolchain; then
  step "工具链版本闸门负向注入" "${PY}" tools/verify_toolchain_negative.py
fi

# --- ELF 对齐解析器自检（16 KB 门禁的**判据实现本体**）----------------------
# 为什么把它放进静态 job：`check_16kb.sh` 依赖本解析器判定 `p_align`。
# 若解析器本身在"找不到 LOAD 段"时静默返回空集，门禁就退化成**恒真**
# （MEMORY.md 明列的真事故形态）。故解析器的正确性必须**独立于 APK 产物**
# 被验证 —— 用合成 ELF + 真实 ELF 互证，不需要 NDK。
# `check_16kb.sh` 的检查 0 会在真构建 job 里再跑一次 --self-test（双保险）。
if wants elf; then
  step "ELF PT_LOAD 对齐解析器自检（合成 ELF + 反空约束）" \
    "${PY}" tools/elf_load_align.py --self-test
fi

if wants abi; then
  step "C ABI 漂移闸门" "${PY}" tools/abi_drift_check.py
fi
if wants params; then
  step "参数契约漂移闸门" "${PY}" tools/check_params.py
fi

if wants license; then
  step "第三方许可扫描（GPL 家族硬阻断）" \
    "${PY}" tools/license_scan.py --third-party third_party
fi

# ---------------------------------------------------------------------------
# xcodegen --project-root 防回归检查（原为 YAML 内联 shell，行为逐字保留）
#
# XcodeGen 的 --project-root 是"少一个参数就整个构建找不到源码、且报错信息与
# 根因距离很远"的那类陷阱（见 platform/ios/Container/project.yml 文件头）。
# 这里静态确认 workflow 与生成器产出的说明都带上了它 —— 防止将来被"简化"掉。
# ---------------------------------------------------------------------------
if wants xcodegen; then
  step "确认 xcodegen 调用带 --project-root（防回归）" bash -c '
    set -euo pipefail
    if ! grep -q -- "--project-root" .github/workflows/ios.yml; then
      echo "::error::ios.yml 的 xcodegen 调用缺少 --project-root"
      exit 1
    fi
    if ! grep -q -- "--project-root" tools/gen_ios_project.py; then
      echo "::error::gen_ios_project.py 的说明里缺少 --project-root"
      exit 1
    fi
    echo "OK: --project-root 在两处都有"
    # project.yml 必须显式声明 scheme，否则 build-app job 的
    # `xcodebuild -scheme Recon` 会依赖 XcodeGen 的自动命名（版本相关）。
    if ! grep -q "^schemes:" platform/ios/Container/project.yml; then
      echo "::error::project.yml 缺少显式 schemes 段"
      exit 1
    fi
    echo "OK: project.yml 有显式 scheme"
  '
fi

echo ""
echo "== ci_run_gates.sh 全部选定闸门通过 =="
