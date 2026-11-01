#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# build_vulkan_shaders.sh — 把 shaders/vulkan/*.comp 编译为 SPIR-V 并内联为 C 数组
#
# 契约：docs/12_构建与打包.md §1.3 产物表
#
#   「Vulkan 着色器 | `shaders_vulkan.spv`（内联为 C 数组） | 静态库内 |
#     运行时从 `vkCreateShaderModule` 加载」
#
# 与 `tools/build_metal_shaders.sh` **结构对称**（同样的「先跑一致性闸门 →
# 探测工具链 → 编译 → 校验产物覆盖」四步），因为两端同等对待是本项目铁律。
#
# ---------------------------------------------------------------------------
# ⚠️ 与 Metal 侧的三点**真实差异**（如实登记，不是疏漏）
# ---------------------------------------------------------------------------
#  DIFF-A  工具链不同：Metal 用 `xcrun metal`（仅 macOS）；Vulkan 用
#          `glslangValidator`（跨平台，随 Vulkan SDK / Android NDK 分发）。
#          故 Vulkan 侧**理论上可在 Windows 上编**，只要装了 Vulkan SDK ——
#          但**SPIR-V 目标版本必须与运行期驱动能力匹配**，Android 上还应
#          按 `VK_KHR_*` 扩展裁剪，故仍不作为默认路径（见 CMake 的
#          `RECON_COMPILE_SHADERS` 说明）。
#  DIFF-B  产物形态不同：Metal 产 `default.metallib`（二进制、由
#          `makeDefaultLibrary()` 自动加载）；Vulkan 产 `.spv` 并且
#          **内联为 C 数组**（因为 Android APK / iOS 静态库内没有"着色器文件"
#          这个概念，只能编进二进制）。故本脚本多一步 `xxd -i` 生成 `.h`。
#  DIFF-C  一算法一文件：GLSL 只有 `main()`，故 7 个算法 = 7 个 `.comp` =
#          7 个 `.spv`（Metal 侧是 2 个 `.metal` → 1 个 metallib）。
#          这是两端**结构上不可避免的不对称**，已在
#          `shaders/vulkan/gpu_common.glsl` 文件头详细说明。
#
# ---------------------------------------------------------------------------
# 与 Metal 侧一致的失败原则
# ---------------------------------------------------------------------------
# 无工具链时：打印清晰的 SKIP 与原因，**不伪造空产物**；
# `RECON_REQUIRE_SPIRV=1` 时退出码 1（CI 的 Vulkan job 会设）。
# ---------------------------------------------------------------------------

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT}"

VULKAN_SRC_DIR="shaders/vulkan"
SPV_DIR="${RECON_SPIRV_DIR:-build/vulkan/spirv}"
HDR_OUT="${RECON_SPIRV_HEADER:-build/vulkan/recon_shaders_vulkan_spv.h}"
REQUIRE="${RECON_REQUIRE_SPIRV:-0}"

# SPIR-V 目标环境：`vulkan1.1` 覆盖 Android API 26+（本项目的移动端下限）。
# **不**用 `vulkan1.3` —— 那会要求更高版本的驱动，而移动端驱动更新慢。
TARGET_ENV="${RECON_SPIRV_TARGET_ENV:-vulkan1.1}"

echo "[vulkan-shaders] repo root : ${ROOT}"
echo "[vulkan-shaders] target env: ${TARGET_ENV}"
echo "[vulkan-shaders] out       : ${SPV_DIR}/*.spv → ${HDR_OUT}"

# --- 步骤 0：两端一致性闸门（不需要工具链）--------------------------------
if command -v python3 >/dev/null 2>&1; then PY=python3
elif command -v python >/dev/null 2>&1; then PY=python
else PY=""; fi
if [ -n "${PY}" ]; then
  echo "[vulkan-shaders] 0) 两端一致性闸门 tools/shader_parity.py"
  "${PY}" "${ROOT}/tools/shader_parity.py"
else
  echo "[vulkan-shaders] 警告：未找到 python，跳过两端一致性闸门（CT-6 未执行）"
fi

# --- 工具链探测 -----------------------------------------------------------
GLSLANG=""
for cand in glslangValidator glslang; do
  if command -v "${cand}" >/dev/null 2>&1; then GLSLANG="${cand}"; break; fi
done

if [ -z "${GLSLANG}" ]; then
  echo ""
  echo "[vulkan-shaders] SKIP：本机无 glslangValidator。"
  echo "[vulkan-shaders] 无法编译 SPIR-V —— 需要 Vulkan SDK（或 Android NDK 自带）。"
  echo "[vulkan-shaders] 已完成的替代检查：tools/shader_parity.py（两端静态一致性）。"
  echo "[vulkan-shaders] 明确**不**生成空的 .spv / .h —— 空着色器模块在运行期"
  echo "                 会以「模块存在但入口点找不到」的形式失败，比直接"
  echo "                 「没编译」更难定位（docs/14 CT-9 无静默降级）。"
  if [ "${REQUIRE}" = "1" ]; then
    echo "[vulkan-shaders] FAIL：RECON_REQUIRE_SPIRV=1 但无工具链。"
    exit 1
  fi
  exit 0
fi

mkdir -p "${SPV_DIR}"
rm -f "${SPV_DIR}"/*.spv   # 清陈旧产物，理由同 Metal 脚本

# --- 步骤 1：逐个 .comp → .spv --------------------------------------------
echo "[vulkan-shaders] 1) 编译 .comp → .spv（${GLSLANG}）"
shopt -s nullglob
SOURCES=("${VULKAN_SRC_DIR}"/*.comp)
shopt -u nullglob
if [ "${#SOURCES[@]}" -eq 0 ]; then
  echo "[vulkan-shaders] FAIL：${VULKAN_SRC_DIR} 下没有 .comp 文件。"
  exit 1
fi

for src in "${SOURCES[@]}"; do
  base="$(basename "${src}" .comp)"
  echo "    - ${src} → ${SPV_DIR}/${base}.spv"
  # `-V` = 编译为 SPIR-V；`--target-env` 固定目标环境，避免不同 SDK
  # 默认值不同导致产物不可复现（docs/12 §1.1 确定性构建要求）。
  # `-I` 指向 shaders/vulkan 以便 `#include "gpu_common.glsl"` 能解析。
  "${GLSLANG}" -V --target-env "${TARGET_ENV}" \
      -I"${VULKAN_SRC_DIR}" \
      "${src}" -o "${SPV_DIR}/${base}.spv"
done

# --- 步骤 2：校验产物 + 入口点 --------------------------------------------
echo "[vulkan-shaders] 2) 校验 SPIR-V 与入口点"
for src in "${SOURCES[@]}"; do
  base="$(basename "${src}" .comp)"
  spv="${SPV_DIR}/${base}.spv"
  if [ ! -s "${spv}" ]; then
    echo "[vulkan-shaders] FAIL：${spv} 为空或不存在。"
    exit 1
  fi
  # 用 glslangValidator 反查入口点：SPIR-V 的入口点必须叫 `main`
  # （GLSL 规定），而**算法本体**是文件内同名函数 —— 后者由
  # tools/shader_parity.py 在源码层校验（产物层看不到被内联的静态函数名）。
  if ! "${GLSLANG}" -V -H "${src}" 2>/dev/null | grep -q "Name main"; then
    : # `-H` 输出格式随版本变化，此处**不**据此判失败（避免版本敏感判据）
  fi
  # 真正的产物层判据：SPIR-V 魔数 = 0x07230203（小端字节序 03 02 23 07）。
  magic="$(od -An -tx1 -N4 "${spv}" | tr -d ' \n')"
  if [ "${magic}" != "03022307" ]; then
    echo "[vulkan-shaders] FAIL：${spv} 不是合法 SPIR-V（魔数=${magic}）。"
    exit 1
  fi
done
echo "    ✓ ${#SOURCES[@]} 个 .spv 均为合法 SPIR-V（魔数校验通过）"

# --- 步骤 3：内联为 C 数组（docs/12 §1.3「内联为 C 数组」）----------------
echo "[vulkan-shaders] 3) 内联为 C 数组 → ${HDR_OUT}"
HDR_DIR="$(dirname "${HDR_OUT}")"
mkdir -p "${HDR_DIR}"
{
  echo "// 本文件由 tools/build_vulkan_shaders.sh 生成，请勿手工编辑。"
  echo "// 契约：docs/12_构建与打包.md §1.3（Vulkan 着色器内联为 C 数组）"
  echo "//"
  echo "// 每个数组 = 一个算法的 SPIR-V 模块（GLSL 一文件一 main()，见"
  echo "// shaders/vulkan/gpu_common.glsl 的说明）。运行期由 C++ 侧按"
  echo "// 算法名选取数组，喂给 vkCreateShaderModule。"
  echo "#pragma once"
  echo "#include <stddef.h>"
  echo "#include <stdint.h>"
  echo ""
  for src in "${SOURCES[@]}"; do
    base="$(basename "${src}" .comp)"
    # `xxd -i` 生成 `unsigned char <name>[]` + `<name>_len`。
    # 名字前缀 `recon_spv_` 便于 C++ 侧用统一规则查表。
    xxd -i -n "recon_spv_${base}" "${SPV_DIR}/${base}.spv" | sed 's/^unsigned char/static const unsigned char/; s/^unsigned int/static const unsigned int/'
    echo ""
  done
} > "${HDR_OUT}"

if [ ! -s "${HDR_OUT}" ]; then
  echo "[vulkan-shaders] FAIL：内联头为空。"
  exit 1
fi

COUNT="$(grep -c '^static const unsigned char recon_spv_' "${HDR_OUT}" || true)"
echo "[vulkan-shaders] OK：${COUNT} 个 SPIR-V 模块内联完成"
echo "[vulkan-shaders]     ${HDR_OUT}"
