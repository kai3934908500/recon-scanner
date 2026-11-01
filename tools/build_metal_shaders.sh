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
#   1) 逐个 .metal → .air        （xcrun -sdk <sdk> metal -target <triple> -c）
#   2) 合并为 default.metallib   （xcrun -sdk <sdk> metallib）
#   3) 校验产物存在且非空
#
# ⚠️ `-target` 不是可选项（2026-11-01 CI 实证）：
#   `metal` 从 target 三元组的 OS 版本**推导** `__METAL_VERSION__`。不传则取
#   SDK 默认版本（高于本项目的最低部署目标 iOS 15.0），产物在部署下限机型上
#   运行期抛 `Function is using language version 3.0 which is incompatible
#   with this OS`。三元组由 `RECON_IOS_DEPLOYMENT_TARGET`（默认 15.0）与
#   `RECON_IOS_SDK`（iphoneos / iphonesimulator）共同决定。
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
# 最低部署目标（docs/12 §4.6：iOS 15.0）。**它不是可有可无的元数据** ——
# 由它推出 `-target` 三元组，进而决定 `__METAL_VERSION__`（见下）。
DEPLOY="${RECON_IOS_DEPLOYMENT_TARGET:-15.0}"

METAL_SRC_DIR="shaders/metal"

echo "[metal-shaders] repo root : ${ROOT}"
echo "[metal-shaders] sdk       : ${SDK}"
echo "[metal-shaders] deploy    : ${DEPLOY}"
echo "[metal-shaders] out       : ${OUT_DIR}/default.metallib"

# ---------------------------------------------------------------------------
# 部署目标校验 + `-target` 三元组构造（**不需要工具链**，故前置于 SKIP 分支）
# ---------------------------------------------------------------------------
# 放在工具链探测**之前**是有意的：本机（Windows，无 xcrun）也能验证这条判据
# 真的会拒收 —— 否则它在 CI 之前无法被负向注入，就成了一条"只在 macOS 上
# 才可能触发、且没人验证过"的判据（本项目称之为假闸门）。
#
# ── 为什么必须显式构造并传 `-target`（2026-11-01 CI 实证的根因）──────────
# `metal` 编译器**从 target 三元组里的 OS 版本推出可用语言版本**，而不是
# "SDK 有多新就用多新"。Apple 工具链工程师的原话（论坛 thread/726165）：
#
#     "just `-target air64-apple-ios16.2` … will automatically enable the
#      corresponding available Metal language version. … iOS 15 gives up to
#      Metal 2.4 while setting iOS 16 target will also give Metal 3.0."
#
# 不传 `-target` 时编译器取 **SDK 默认版本**（Xcode 16.4 的 iOS SDK 默认
# Metal 3.1/3.2），于是会**静默产出一份语言版本高于最低部署目标的产物**：
#
#     · 编译期一切正常（设备 SDK 下 CI 曾 16/16 全绿）；
#     · 真机运行期在 iOS 15 设备上抛
#       `Function … is using language version 3.0 which is incompatible
#        with this OS` —— 而本项目的部署下限正是 iOS 15.0（docs/12 §4.6）。
#
# 这是「静默失效」的典型形态：构建成功、静态闸门全绿，问题只在目标机型上
# 出现。**必须在编译期就按部署目标锁定语言版本**。
#
# ── 为什么不用 `-std=` ────────────────────────────────────────────────────
# 上一版注释写「加 `-std=metal3.0` 会在旧 Xcode 上不可用，故不指定」。
# 那个顾虑本身没错（硬编 `-std` 会把版本写死、与 Xcode 版本耦合），但结论
# **错了**：正确做法不是"不指定"，而是指定 **`-target`** —— 由工具链自己
# 从 OS 版本推出匹配的语言版本。两者语义完全不同：
#     `-std=metal3.0`    → 强制语言版本（可能高于部署目标 → 真机不可用）
#     `-target …ios15.0` → 语言版本由部署目标**推导**（必然与目标一致）
# Apple 论坛同一贴明确指出："When building Metal shaders from Xcode 14.2
# there is no `-std` or `-mios-version-min`. Instead there is just
# `-target air64-apple-ios16.2`."
#
# ── 部署目标下限硬断言 ────────────────────────────────────────────────────
# 与 cmake/toolchains/ios.cmake 同一条判据，此处是**第二道关口** —— 因为本
# 脚本也会被 Xcode Build Phase 直接调用（那时不经过 CMake 工具链）。
# 低于 15.0 会破坏 sceneDepth 语义与时间同步精度（docs/12 §4.6），硬失败。
DEPLOY_MAJOR="${DEPLOY%%.*}"
if ! [ "${DEPLOY_MAJOR}" -ge 15 ] 2>/dev/null; then
  echo "[metal-shaders] FAIL：RECON_IOS_DEPLOYMENT_TARGET='${DEPLOY}' 低于下限 15.0。"
  echo "[metal-shaders] 见 docs/12_构建与打包.md §4.6（iOS 15.0 为最低部署版本）。"
  exit 1
fi

# ── 三元组形式（Apple 论坛 thread/707695 的官方示例）──────────────────────
#     -target air64-apple-ios16.0                （真机）
#     -target air64-apple-ios16.0-simulator      （模拟器，后缀 `-simulator`）
# 架构恒为 `air64`。SDK 只认 iphoneos / iphonesimulator 两种 —— 写错即硬失败，
# 不静默回退（否则会把模拟器构建悄悄编成真机产物）。
case "${SDK}" in
  iphoneos)        METAL_TARGET="air64-apple-ios${DEPLOY}" ;;
  iphonesimulator) METAL_TARGET="air64-apple-ios${DEPLOY}-simulator" ;;
  *)
    echo "[metal-shaders] FAIL：不支持的 SDK '${SDK}'（只认 iphoneos / iphonesimulator）。"
    exit 1
    ;;
esac
echo "[metal-shaders] target    : ${METAL_TARGET}"

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
  # `-target` **必须传**（见上方「部署目标校验 + 三元组构造」段的长注释）：
  # 它决定 `__METAL_VERSION__`，不传则取 SDK 默认（高于部署目标）→ 产物在
  # 部署下限机型上运行期不可用。这里**不传** `-std`：语言版本由 `-target`
  # 的 OS 版本推导，硬编 `-std` 反而会与部署目标脱钩。
  xcrun -sdk "${SDK}" metal -target "${METAL_TARGET}" -c "${src}" \
      -o "${AIR_DIR}/${base}.air"
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
#
# 判定逻辑收敛到 tools/check_metallib_symbols.py（单一实现）：
# CI 的「校验 default.metallib」步骤调同一个工具，避免两处实现漂移。
if [ -n "${PY}" ]; then
  echo "[metal-shaders] 3b) 产物函数覆盖检查"
  "${PY}" "${ROOT}/tools/check_metallib_symbols.py" \
      --metallib "${OUT_DIR}/default.metallib" --repo-root "${ROOT}"
fi

SIZE="$(wc -c < "${OUT_DIR}/default.metallib" | tr -d ' ')"
echo "[metal-shaders] OK：default.metallib ${SIZE} bytes"
echo "[metal-shaders]     ${OUT_DIR}/default.metallib"
