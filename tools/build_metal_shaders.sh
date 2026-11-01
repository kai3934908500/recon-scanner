#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# build_metal_shaders.sh — 把 shaders/metal/*.metal 编译为 iOS 的 default.metallib
#
# 契约：docs/12_构建与打包.md §4.4「Metal 着色器编译与嵌入」
#
#   「算法同源、两端分别编译（00 §3.2）。…… 编译命令（脚本
#    `tools/build_metal_shaders.sh`）」—— 本脚本即该契约的实现。
#
# 流程（与 docs/12 §4.4 的三步逐条对应）：
#   1) 逐个 .metal → .air        （xcrun -sdk iphoneos metal -c）
#   2) 合并为 default.metallib   （xcrun -sdk iphoneos metallib）
#   3) 校验产物存在且非空
#
# 运行时加载（docs/12 MT-1 / MT-2）：
#   `MTLDevice.makeDefaultLibrary()` 自动在 `Recon.app/Contents/Resources/`
#   找 `default.metallib`；**不得**用 `makeDefaultLibrary(bundle:)` 指定自定义
#   bundle 名（会让 App Extension 等场景失效）。
#
# ---------------------------------------------------------------------------
# ⚠️ 本脚本在 Windows 上**无法运行**（无 xcrun / 无 iPhoneOS SDK）。
#    这不是缺陷，是平台的固有限制。因此：
#      · 本脚本**必须**能在「无工具链」时给出一条**清晰的失败信息**，
#        而不是静默产出空文件（docs/14 CT-9「无静默降级」的同一条原则：
#        不能"看起来成功"地跳过关键步骤）。
#      · 当 `RECON_REQUIRE_METAL=1`（CI 的 iOS job 会设）时，缺工具链
#        → 退出码 1（硬失败）；否则退出码 0 但**明确打印 SKIP 与原因**
#        （允许在非 iOS 机器上跑全量构建而不被这条拦住）。
#      · 无论哪条分支，都会先跑 `tools/shader_parity.py` —— 那是**不需要
#        macOS 的**一致性检查，在 Windows 上也必须过。
# ---------------------------------------------------------------------------

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT}"

# --- 参数 / 环境 -----------------------------------------------------------
SDK="${RECON_IOS_SDK:-iphoneos}"
# 产物目录：默认对齐 docs/12 §4.4 的 iOS bundle 结构
OUT_DIR="${RECON_METALLIB_OUT_DIR:-build/ios/Recon.app/Contents/Resources}"
AIR_DIR="${RECON_METAL_AIR_DIR:-build/ios/metal-air}"
REQUIRE="${RECON_REQUIRE_METAL:-0}"

METAL_SRC_DIR="shaders/metal"

echo "[metal-shaders] repo root : ${ROOT}"
echo "[metal-shaders] sdk       : ${SDK}"
echo "[metal-shaders] out       : ${OUT_DIR}/default.metallib"

# --- 步骤 0：先跑两端一致性闸门（**不需要 macOS**）------------------------
# 顺序很重要：若两端算法名/布局已经漂移，先编译出一份 metallib 毫无意义
# —— 那只会让"编译通过"掩盖"两端已经不同"。
if command -v python3 >/dev/null 2>&1; then
  PY=python3
elif command -v python >/dev/null 2>&1; then
  PY=python
else
  PY=""
fi
if [ -n "${PY}" ]; then
  echo "[metal-shaders] 0) 两端一致性闸门 tools/shader_parity.py"
  "${PY}" "${ROOT}/tools/shader_parity.py"
else
  echo "[metal-shaders] 警告：未找到 python，跳过两端一致性闸门（CT-6 未执行）"
fi

# --- 工具链探测 -----------------------------------------------------------
if ! command -v xcrun >/dev/null 2>&1; then
  echo ""
  echo "[metal-shaders] SKIP：本机无 xcrun（非 macOS 或未装 Xcode 命令行工具）。"
  echo "[metal-shaders] 无法编译 Metal 着色器 —— 这是平台固有限制，不是构建错误。"
  echo "[metal-shaders] 需要的环境：macOS + Xcode 15+（docs/12 §4.1）。"
  echo "[metal-shaders] 已完成的替代检查：tools/shader_parity.py（两端静态一致性）。"
  echo "[metal-shaders] 真机构建必须在 macOS 上进行；Windows 侧**不会**伪造产物。"
  # 明确**不**创建空文件：空 default.metallib 会被 makeDefaultLibrary() 当成
  # "库存在但无函数"，错误更隐蔽（本项目反复强调「静默丢弃最难查」）。
  if [ "${REQUIRE}" = "1" ]; then
    echo "[metal-shaders] FAIL：RECON_REQUIRE_METAL=1 但无工具链。"
    exit 1
  fi
  exit 0
fi

# 检查 SDK 可用
if ! xcrun --sdk "${SDK}" --show-sdk-path >/dev/null 2>&1; then
  echo "[metal-shaders] FAIL：找不到 SDK '${SDK}'。可用："
  xcrun --sdk "${SDK}" --show-sdk-path 2>&1 || true
  exit 1
fi

mkdir -p "${OUT_DIR}" "${AIR_DIR}"
# 清掉旧 .air，避免上一轮被删除的 .metal 留下的陈旧中间产物进了 metallib
# （那会造成「源码已删、库里还有旧函数」的假象，闸门查不出来 ——
#  CT-6 只看源码，不看产物）。
rm -f "${AIR_DIR}"/*.air

# --- 步骤 1：逐个 .metal → .air -------------------------------------------
echo "[metal-shaders] 1) 编译 .metal → .air"
shopt -s nullglob
SOURCES=()
while IFS= read -r f; do SOURCES+=("${f}"); done < <(find "${METAL_SRC_DIR}" -name '*.metal' | sort)
shopt -u nullglob

if [ "${#SOURCES[@]}" -eq 0 ]; then
  echo "[metal-shaders] FAIL：${METAL_SRC_DIR} 下没有 .metal 文件。"
  exit 1
fi

for src in "${SOURCES[@]}"; do
  base="$(basename "${src}" .metal)"
  echo "    - ${src} → ${AIR_DIR}/${base}.air"
  # `-c` = 只编译不链接，产出 .air（Apple Intermediate Representation）。
  # 加 `-std=metal3.0` 会在旧 Xcode 上不可用，故不指定 —— 用 SDK 默认版本。
  xcrun -sdk "${SDK}" metal -c "${src}" -o "${AIR_DIR}/${base}.air"
done

# --- 步骤 2：合并为 default.metallib --------------------------------------
echo "[metal-shaders] 2) 合并 .air → default.metallib"
xcrun -sdk "${SDK}" metallib -o "${OUT_DIR}/default.metallib" "${AIR_DIR}"/*.air

# --- 步骤 3：校验产物 ------------------------------------------------------
echo "[metal-shaders] 3) 校验产物"
if [ ! -s "${OUT_DIR}/default.metallib" ]; then
  echo "[metal-shaders] FAIL：default.metallib 为空或不存在。"
  exit 1
fi

# 额外校验：产物里的函数名必须覆盖 common 声明的算法集合。
# 这是**产物层**的 CT-6（源码层已由 shader_parity.py 覆盖）——
# 两者都要有，因为"源码写对了但没编进去"是可能的（如某个 .metal
# 被误排除出 SOURCES）。
if [ -n "${PY}" ]; then
  echo "[metal-shaders] 3b) 产物函数覆盖检查"
  # `xcrun metal-nm` 或 `strings` 都能看到 metallib 里的函数名；
  # `strings` 更通用（不依赖 Xcode 版本提供的子工具）。
  if command -v strings >/dev/null 2>&1; then
    # ⚠️ 这段 python 从 **stdin** 读入（`python - <<EOF`），此时 `__file__`
    #    **未定义** —— 若照抄常规脚本写法（`os.path.dirname(__file__)`）会抛
    #    NameError。故改为经环境变量把仓库根传进去（见下方 RECON_SHADER_ROOT=）。
    #    这是一个**只有真正跑到这一行才会暴露**的 bug，本机（无 xcrun）永远
    #    跑不到 —— 正因如此才必须在代码里写清，而不是等真机阶段踩。
    EXPECTED="$(RECON_SHADER_ROOT="${ROOT}" "${PY}" - <<'PYEOF'
import re, os, sys
root = os.environ.get("RECON_SHADER_ROOT")
if not root:
    sys.exit(0)
p = os.path.join(root, "shaders", "common", "gpu_algo.h")
try:
    t = open(p, encoding="utf-8").read()
except OSError:
    sys.exit(0)
body = re.sub(r"^\s*#\s*define[^\n]*RECON_GPU_ALGO[^\n]*$", "", t, flags=re.M)
for n in re.findall(r"RECON_GPU_ALGO\(\s*(\w+)\s*\)\s*;", body):
    print(n)
PYEOF
)"
    MISSING=""
    for fn in ${EXPECTED}; do
      if ! strings "${OUT_DIR}/default.metallib" | grep -qx "${fn}"; then
        MISSING="${MISSING} ${fn}"
      fi
    done
    if [ -n "${MISSING}" ]; then
      echo "[metal-shaders] FAIL：metallib 里找不到这些算法函数：${MISSING}"
      echo "            （源码可能写对了但未被编入；检查 SOURCES 收集逻辑）"
      exit 1
    fi
    echo "    ✓ 产物覆盖全部声明的算法函数"
  fi
fi

SIZE="$(wc -c < "${OUT_DIR}/default.metallib" | tr -d ' ')"
echo "[metal-shaders] OK：default.metallib ${SIZE} bytes"
echo "[metal-shaders]     ${OUT_DIR}/default.metallib"
