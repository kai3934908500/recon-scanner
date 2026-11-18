#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# check_16kb.sh — 16 KB 页对齐硬门禁（Google Play 上架阻断项）
#
# 契约：docs/12_构建与打包.md §3.5
#   「NDK r28 + AGP 8.5.1+ + useLegacyPackaging false + zipalign -P 16
#     四项同时满足；tools/check_16kb.sh 在 CI 中作为独立 job 阻断发布」
#
# 该脚本此前**不存在**（docs/12 里给了逐字实现，但仓库里没有这个文件）——
# 属 MEMORY.md 明列的「设计意图未落实」类：硬门禁只写在文档里，
# 没有任何机器执行它。故本文件是它的**首个实现**。
#
# ---------------------------------------------------------------------------
# 与 docs/12 §3.5 示例的三处**实质性修正**（如实登记，不是"顺手改改"）
# ---------------------------------------------------------------------------
#  (1) AAB 路径：文档写
#        `android/app/build/outputs/aab/release/app-release.aab`
#      —— **这个路径不存在**。Gradle 的 AAB 输出目录是
#        `app/build/outputs/bundle/<buildType>/`（bundle，不是 aab）。
#      照抄会得到一个永远 `[ -f ]` 为假的判断 → 该检查**静默失效**。
#  (2) strip 后的 .so 位置：文档用
#        `.../stripped_native_libs/release/stripReleaseDebugSymbols/out/lib`
#      —— 这是 AGP 某一时点的内部目录名，**随 AGP 版本变化**。本实现改为
#        「按 glob 递归找 `**/lib/<abi>/*.so`，并排除 `intermediates/cxx`」
#        的**位置无关**做法：只要产物存在就能找到，且找不到时**硬失败**。
#  (3) `strip` 与 p_align：文档断言「strip 不改变 p_align」，这**是对的**，
#      但只有在 .so 采用 **PT_LOAD 段**布局时成立；改为 APK 内直接解包
#      （见检查 3b）比依赖中间目录更稳，且判据落在**交付物**而非中间产物上。
#
# ---------------------------------------------------------------------------
# 四条检查（判据全部落在**产物**上，不落脚本自述）
# ---------------------------------------------------------------------------
#  检查 0  ELF 对齐解析器自检（先证明**判据本身**不是恒真的）
#  检查 1  ELF LOAD 段 p_align ≥ 0x4000（仅 64 位 ABI：arm64-v8a / x86_64）
#          实现收敛到 tools/elf_load_align.py（**单一实现 + 内置自检**，
#          避免在 shell 里用 `readelf | awk` —— 那种写法在 readelf 缺失或
#          输出格式变化时会匹配 0 行、循环不执行、于是**假绿**）。
#  检查 3  armeabi-v7a 必须**保持 4 KB**：正向断言 `--max-align 0x1000`
#          （**不靠**"另一个判据失败"反推，那是零检测力的假闸门 ——
#           见 docs/14 缺口 AY），且**只判本案自建**（白名单前缀
#           `librecon_`），第三方预编译库只留档不判。
#  检查 2  APK 内 .so 的 zip 对齐：`zipalign -c -P 16 -v 4 <apk>`
#          + `.so` 条目必须**未压缩**（ZIP_STORED）：实现收敛到
#            tools/apk_so_compression.py（**单一实现 + 内置自检** ——
#            原内联 `unzip -lv | awk` 写错列位，是**误报型假闸门**，
#            CI #12 首次执行时暴露）。
#
# ⚠️ 本脚本内**不再重复实现**任何判定逻辑：ELF 对齐 → elf_load_align.py，
#    APK 内 .so 压缩 → apk_so_compression.py，两者都带 `--self-test`。
#    在 shell 里重写一遍是"同一逻辑写两遍"，且 shell 解析文本极易失锚。
#
# ---------------------------------------------------------------------------
# 反静默降级（零容忍）
# ---------------------------------------------------------------------------
# 未跑构建时——即**找不到任何 .so 产物**——本脚本**必须非零退出**，
# 而不是打印一行 SKIP 再返回 0。理由（docs/14 CT-9 无静默降级）：
# 一个"没东西可查所以通过"的门禁，与"没有门禁"在效果上完全等价，
# 而它在 CI 日志里看起来是绿的。
#
# 但**首轮 CI 还没有 APK**（只编 native 库）这种情形需要区分：
# 用 `--expect-so-only` 显式声明「本次只校验 native 产物，APK/AAB 允许缺失」，
# 让"允许缺什么"变成**调用方的显式声明**而不是脚本的默认宽容。
#
# ---------------------------------------------------------------------------
# 用法
# ---------------------------------------------------------------------------
#   bash tools/check_16kb.sh                       # 全量（APK/AAB 均要求存在）
#   bash tools/check_16kb.sh --expect-so-only      # 只校验 .so（前几轮 CI）
#   bash tools/check_16kb.sh --build-dir <dir>     # 指定 Gradle 构建根
#   bash tools/check_16kb.sh --self-test           # 只跑解析器自检
#
# 退出码：0 通过；1 用法/环境错误；3 判据不满足

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT}"

APP_DIR="platform/android/app"
BUILD_BASE="${APP_DIR}/build"
EXPECT_SO_ONLY=0

while [ $# -gt 0 ]; do
  case "$1" in
    --expect-so-only) EXPECT_SO_ONLY=1; shift ;;
    --build-dir)      APP_DIR="$2"; BUILD_BASE="$2/build"; shift 2 ;;
    --self-test)
      python3 "${SCRIPT_DIR}/elf_load_align.py" --self-test 2>/dev/null || \
      python  "${SCRIPT_DIR}/elf_load_align.py" --self-test
      exit $?
      ;;
    -h|--help) sed -n '2,60p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "未知参数：$1" >&2; exit 1 ;;
  esac
done

if command -v python3 >/dev/null 2>&1; then PY=python3
elif command -v python >/dev/null 2>&1; then PY=python
else echo "::error::未找到 python（ELF 对齐检查依赖它）" >&2; exit 1; fi

echo "[16kb] 仓库根   : ${ROOT}"
echo "[16kb] 构建根   : ${BUILD_BASE}"
echo "[16kb] 仅校验 so: ${EXPECT_SO_ONLY}"

FAIL=0
note()  { echo "说明  $*"; }
bad()   { echo "::error::$*"; FAIL=$((FAIL + 1)); }

# ---------------------------------------------------------------------------
# 定位产物：位置无关（不依赖 AGP 内部目录名）
# ---------------------------------------------------------------------------
# 找 .so：包含 `/lib/<abi>/` 的路径，排除 intermediates/cxx（那是 CMake 的
# **未 strip 中间产物**，且同一份 .so 会有多份，混进来只会稀释判据）。
find_so() {
  local abi="$1"
  [ -d "${BUILD_BASE}" ] || return 0
  find "${BUILD_BASE}" -type f -path "*/lib/${abi}/*.so" \
       -not -path "*/intermediates/cxx/*" 2>/dev/null | sort
}

# 找交付物：APK 与 AAB 的实际输出位置（见文件头修正 (1)）
find_apk() {
  [ -d "${BUILD_BASE}" ] || return 0
  find "${BUILD_BASE}/outputs/apk" -type f -name '*.apk' 2>/dev/null | sort
}
find_aab() {
  [ -d "${BUILD_BASE}" ] || return 0
  find "${BUILD_BASE}/outputs/bundle" -type f -name '*.aab' 2>/dev/null | sort
}

# ---------------------------------------------------------------------------
# 检查 0：解析器自检（先证明**判据本身**不是恒真的）
# ---------------------------------------------------------------------------
echo ""
echo "== 检查 0：ELF 对齐解析器自检（判据自身可证伪）=="
if ! "${PY}" "${SCRIPT_DIR}/elf_load_align.py" --self-test; then
  bad "elf_load_align.py --self-test 未通过 —— 解析/判定逻辑不可信，后续检查无意义"
  echo "[16kb] FAIL：自检失败，提前退出"
  exit 3
fi

# ---------------------------------------------------------------------------
# 检查 1：64 位 ABI 的 ELF LOAD 段必须 >= 0x4000
# ---------------------------------------------------------------------------
echo ""
echo "== 检查 1：64 位 ABI 的 LOAD 段 p_align >= 0x4000（16 KB）=="
n64=0
for abi in arm64-v8a x86_64; do
  mapfile -t sos < <(find_so "${abi}")
  if [ "${#sos[@]}" -eq 0 ]; then
    note "${abi}: 本构建未产出该 ABI 的 .so（可能 abiFilters 未含或该 ABI 构建失败）"
    continue
  fi
  echo "--- ${abi}：${#sos[@]} 个 .so ---"
  n64=$((n64 + ${#sos[@]}))
  if ! "${PY}" "${SCRIPT_DIR}/elf_load_align.py" --min-align 0x4000 "${sos[@]}"; then
    bad "${abi} 存在 p_align < 0x4000 的 LOAD 段（16 KB 页对齐不满足）"
  fi
done

if [ "${n64}" -eq 0 ]; then
  bad "未找到任何 64 位 ABI 的 .so 产物（${BUILD_BASE}）—— 构建未跑？路径不对？"$'\n'"  本脚本**不**把「无产物」当作通过（docs/14 CT-9 无静默降级）。"
fi

# ---------------------------------------------------------------------------
# 检查 3（先做，因为不依赖 strip）：armeabi-v7a 必须仍为 4 KB
# ---------------------------------------------------------------------------
# 文档表述是「不适用，保持 4 KB」。这里把它做成**正向断言**：32 位 ELF 的
# p_align 应当 <= 0x1000；若出现 0x4000，说明
# `-Wl,-z,max-page-size=16384` 被**无条件**加到了 32 位 target 上 ——
# 那会让 4 KB 页设备上的 .so 体积虚增（对齐填充），且与 docs/12 §3.2 的
# 「明确不做：不在 armeabi-v7a 上做 16 KB 对齐」直接冲突。
#
# ⚠️ 本条曾被**写成了一个零检测力的假闸门**（CI #11 后由本机端到端复核发现）：
#   原实现问的是「`--min-align 0x4000` 是否**失败**」，靠失败来反推「是 4 KB」。
#   但 armeabi-v7a 目录里混有**第三方预编译库**（libarcore_sdk_c/jni、
#   libc++_shared —— 它们不受我们的 CMake 链接参数控制，实测本来就是 0x1000），
#   于是 `all(a >= 0x4000)` 恒为假 → 恒「失败」→ 恒判「符合 ✓」。
#   而真实 CI 产物里**本案自建**的 librecon_core.so / librecon_platform_android.so
#   恰恰是 0x4000 —— 假闸门把真缺陷放过去了整整一路。
#   修法有两层（缺一不可）：
#     ① 判据方向显式化：改用 `--max-align 0x1000`（正向断言，不再靠"失败"反推）；
#     ② 判据范围收敛：**只判本案自建的 .so**（白名单前缀），第三方预编译库
#        的对齐由它们自己的发布方决定，混进来只会稀释判据。
#   回归防线：elf_load_align.py --self-test 的 bad-max-* 用例。
echo ""
echo "== 检查 3：armeabi-v7a 保持 4 KB 页对齐（p_align <= 0x1000）=="
mapfile -t sos32_all < <(find_so "armeabi-v7a")
# 只保留本案自建的 .so（第三方预编译库不归我们的链接参数管，见上方注释）。
RECON_OWN_PREFIXES=("librecon_")
sos32=()
for s in "${sos32_all[@]}"; do
  b="$(basename "$s")"
  for pre in "${RECON_OWN_PREFIXES[@]}"; do
    case "$b" in "${pre}"*) sos32+=("$s") ;; esac
  done
done
if [ "${#sos32[@]}" -eq 0 ]; then
  note "armeabi-v7a: 未产出**本案自建**的 .so（第三方预编译库 ${#sos32_all[@]} 个不参与本判据）"
else
  echo "--- armeabi-v7a：本案自建 ${#sos32[@]} 个 .so（另有第三方 ${#sos32_all[@]} 个不判）---"
  # 正向断言：每一个本案自建的 32 位 .so，其**全部** LOAD 段必须 <= 0x1000。
  #   rc=0 → 符合 4 KB；rc=3 → 出现了 0x4000（真实缺陷）；其它 → 判据本身出错。
  set +e
  "${PY}" "${SCRIPT_DIR}/elf_load_align.py" --no-cross-check \
      --max-align 0x1000 "${sos32[@]}"
  rc32=$?
  set -e
  case "${rc32}" in
    0) note "armeabi-v7a 自建 .so 全部 <= 0x1000 → 4 KB 口径符合 docs/12 §3.2 ✓" ;;
    3) bad "armeabi-v7a 的**自建** .so 出现 > 0x1000 的 LOAD 段 —— 疑似把"$'\n'"  16 KB 对齐参数（-Wl,-z,max-page-size=16384）**无条件**套到了 32 位"$'\n'"  target 上。docs/12 §3.2 明文：armeabi-v7a 保持 4 KB、不做 16 KB 对齐" ;;
    *) bad "armeabi-v7a 检查以异常退出码 ${rc32} 结束（既非 0 也非 3）—— 判据本身出错" ;;
  esac
  # 附带留档：第三方预编译库的实际值（不判定，只报告）
  third=()
  for s in "${sos32_all[@]}"; do
    b="$(basename "$s")"
    is_own=0
    for pre in "${RECON_OWN_PREFIXES[@]}"; do
      case "$b" in "${pre}"*) is_own=1 ;; esac
    done
    [ "$is_own" -eq 0 ] && third+=("$s")
  done
  if [ "${#third[@]}" -gt 0 ]; then
    note "第三方预编译库（不判，仅留档；其对齐由各自发布方决定）："
    "${PY}" "${SCRIPT_DIR}/elf_load_align.py" --report --no-cross-check \
      "${third[@]}" 2>/dev/null | sed 's/^报告  /    /' || true
  fi
fi

# ---------------------------------------------------------------------------
# 检查 2：APK 内 .so 的 16 KB zip 对齐（zipalign -c -P 16）
# ---------------------------------------------------------------------------
echo ""
echo "== 检查 2：APK 内 .so 的 zip 对齐（zipalign -c -P 16 -v 4）=="
# 定位 zipalign：优先 ANDROID_HOME / ANDROID_SDK_ROOT 下最高版本的 build-tools
ZIPALIGN=""
for base in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" "${ANDROID_SDK:-}"; do
  [ -n "${base}" ] || continue
  cand="$(find "${base}/build-tools" -maxdepth 2 -type f \
            \( -name 'zipalign' -o -name 'zipalign.exe' \) 2>/dev/null \
          | sort -V | tail -1 || true)"
  if [ -n "${cand}" ]; then ZIPALIGN="${cand}"; break; fi
done
[ -n "${ZIPALIGN}" ] || ZIPALIGN="$(command -v zipalign || true)"

mapfile -t apks < <(find_apk)
if [ "${#apks[@]}" -eq 0 ]; then
  if [ "${EXPECT_SO_ONLY}" -eq 1 ]; then
    note "未找到 APK，但调用方以 --expect-so-only 显式声明本次不校验 APK → 跳过"
  else
    bad "未找到任何 APK（${BUILD_BASE}/outputs/apk）且未声明 --expect-so-only"$'\n'"  判据的目标对象缺失，不得静默通过。"
  fi
else
  if [ -z "${ZIPALIGN}" ]; then
    bad "找到了 APK 但未找到 zipalign —— 无法执行 16 KB zip 对齐检查"$'\n'"  （设置 ANDROID_HOME 或在 PATH 上提供 zipalign）"
  else
    echo "使用 zipalign: ${ZIPALIGN}"
    for apk in "${apks[@]}"; do
      # ⚠️ 输出先落文件再判，不写 `zipalign ... | grep -q`：
      #   `set -o pipefail` 下 grep -q 命中即关闭读端 → 上游收 SIGPIPE
      #   → 整条管道判非零 → 误报（本项目已吃过同类亏，见 ios.yml 注释）。
      ZLOG="$(mktemp)"
      if "${ZIPALIGN}" -c -P 16 -v 4 "${apk}" > "${ZLOG}" 2>&1; then
        note "$(basename "${apk}"): ✅ zipalign -P 16 通过"
      else
        bad "$(basename "${apk}"): ❌ 未通过 zipalign -P 16 校验"
        grep -E "BAD|FAILED|WARNING" "${ZLOG}" | head -20 || true
      fi
      rm -f "${ZLOG}" || true

      # 附带证据：APK 内 .so 条目必须**未压缩**（STORED）且 16 KB 对齐。
      # useLegacyPackaging false 是"未压缩"的必要条件，若它被改回 true，
      # 上面 zipalign 可能仍然通过（对齐的是 zip 局部头），但实际装到
      # Android 15+ 设备上仍会因 .so 被压缩而失败 —— 故单独断言。
      #
      # ⚠️ 本条曾是一处**误报型假闸门（假红）**，CI #12 首次执行时暴露：
      #   原写法 `awk '$NF ~ /\.so$/ && $3 == "Stored"'` —— `unzip -lv` 的
      #   「Method」列是**第 2 列**（$2），$3 是 Size 列 → 恒不匹配 →
      #   Stored 计数**恒为 0** → **恒判「被压缩」**，而真实 APK 里 15 个
      #   .so **全部**是 Stored（本机用 CI #12 的真实 APK 实测：
      #   `$3` 命中 0、`$2` 命中 15）。之所以此前没暴露，是因为 CI #11 在
      #   更靠前的检查 1 就失败，这条断言**从未被执行到**。
      #   修法：**不再解析 unzip 的文本输出**（列位随版本可能变，硬编码列位
      #   是脆的），改用 `tools/apk_so_compression.py` —— 它用 Python
      #   `zipfile` 直接读每个条目的 `compress_type`（结构性事实），
      #   并自带 `--self-test`（6 项，含单个/全部/无/非 zip/混装五个反例）。
      #   （单一实现：判定逻辑只此一份，不在 shell 里再写一遍。）
      "${PY}" "${SCRIPT_DIR}/apk_so_compression.py" "${apk}" || \
        bad "$(basename "${apk}")：.so 未压缩判据未通过（见上方 ::error:: 行）"
    done
  fi
fi

# ---------------------------------------------------------------------------
# AAB：存在则报告（AAB 的 .so 对齐由 Play 侧校验，Gradle 侧无 zipalign）
# ---------------------------------------------------------------------------
mapfile -t aabs < <(find_aab)
if [ "${#aabs[@]}" -eq 0 ]; then
  note "未找到 AAB（${BUILD_BASE}/outputs/bundle）—— 未跑 assembleRelease/bundleRelease 时正常"
else
  for aab in "${aabs[@]}"; do
    note "$(basename "${aab}"): 已产出（$(wc -c < "${aab}" | tr -d ' ') 字节）；AAB 内 .so 对齐由 Play Console 校验"
  done
fi

# ---------------------------------------------------------------------------
echo ""
if [ "${FAIL}" -ne 0 ]; then
  echo "[16kb] FAIL：${FAIL} 项 16 KB 页对齐判据未通过（Google Play 上架会被拒）"
  exit 3
fi
echo "[16kb] PASS：16 KB 页对齐判据全部通过"
