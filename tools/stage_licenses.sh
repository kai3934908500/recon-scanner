#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# stage_licenses.sh — 把 THIRD_PARTY_LICENSES.txt 拷入 iOS App 包的**包根**
#
# 契约：docs/09_UI规格.md §3.13 U-L-3（「许可全文随 App 打包，不强依赖网络」）、
#       docs/12_构建与打包.md §2.7（许可清单生成与嵌入位置）、
#       §2.1 D-3（未 vendor 的依赖发布前必须补齐）。
#
# ── 为什么需要本脚本（2026-11-18 发现并修复的**真实阻塞缺口**）─────────────
#
#   U-13 许可清单页读包内 `Recon.app/THIRD_PARTY_LICENSES.txt`，但**该文件
#   此前从不进包**，三处证据：
#     ① `cmake/Licenses.cmake` 的 `install(FILES ... DESTINATION "Recon.app")`
#        只在 **CMake install 阶段**生效，而 iOS 交付链（CI `build-app`）走的是
#        `xcodegen` + `xcodebuild`，**从不跑 `cmake --install`**；
#     ② `platform/ios/Container/project.yml` 的 `sources` 里**没有**该文件条目；
#     ③ `tools/stage_ios_ipa.py`（打未签名 IPA）也不注入它。
#   于是 U-13 在真机上会**永远读到空**（`Bundle.main.url(...)` 返回 nil），
#   且编译期与既有闸门全绿 —— 正是本项目反复记录的「静默失效」形态
#   （与 G15 的 metallib 落位、缺口 J 的 RGB 丢弃同型）。
#
#   修法与 metallib 完全同构：在 target 的构建脚本里生成/拷入包根
#   （`${BUILT_PRODUCTS_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}`）。
#
# ── 为什么用「优先本地生成、失败即取仓内既有产物」───────────────────────
#   a) `cmake -P cmake/Licenses.cmake -- <out>` 可在**无构建树**下复现清单
#      （`Licenses.cmake` 自带脚本模式，CI 的 `license_manifest_verify` 即用它）；
#   b) 但它需要 Python？—— 不需要，纯 CMake；`cmake` 在 macOS runner 上必有。
#   故首选 (a) 现生成（保证与 `third_party/` 当前状态一致，是最新真值）；
#   若 `cmake` 不可用或生成失败，**回退**到仓内既有产物
#   `build_cmake/THIRD_PARTY_LICENSES.txt`（若存在），并**打印警告说明来源**。
#   ⚠️ 回退**不得静默**：必须打印用的是哪一份、时间戳（U-D-7 不隐藏降级）。
#   两份都不存在时 → **硬失败**（`RECON_REQUIRE_LICENSES=1`），不产空文件。
#
# ── 为什么默认 OUT_DIR 里**没有** `Contents/` ──────────────────────────────
#   iOS bundle 是**扁平**结构，非本地化资源在包根（`Recon.app/`），**不存在**
#   `Contents/` 一层。写成 `Recon.app/Contents/Resources/` 会在编译期与 CI 全绿
#   的情况下把文件放到设备上不存在的位置（G15-c 正是守这一事实）。
#   调用方应传 `RECON_LICENSES_OUT_DIR="${BUILT_PRODUCTS_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}"`
#   （iOS = 包根，macOS = Contents/Resources，由 Xcode 按平台展开）。
#
# ---------------------------------------------------------------------------
# ⚠️ 本脚本在 Windows 上**可运行但无意义**（无 xcodebuild）；它只被 Xcode
#    Build Phase 调用。非 iOS 环境下若被直接执行，会按 OUT_DIR 写文件（默认
#    落在仓库外的临时目录），不污染源码树。
# ---------------------------------------------------------------------------

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# --- 参数 / 环境 -----------------------------------------------------------

# 产物落位目录。**必须**由调用方传 `.../UNLOCALIZED_RESOURCES_FOLDER_PATH`
# （iOS = 包根）。缺省给一个**不会污染仓库**的临时目录，仅用于本机手测。
OUT_DIR="${RECON_LICENSES_OUT_DIR:-${TMPDIR:-/tmp}/recon-licenses}"
REQUIRE="${RECON_REQUIRE_LICENSES:-0}"

# 仓内既有产物（回退用）。与 cmake/Licenses.cmake 的默认路径一致。
REPO_SNAPSHOT="${ROOT}/build_cmake/THIRD_PARTY_LICENSES.txt"

DEST="${OUT_DIR}/THIRD_PARTY_LICENSES.txt"

echo "[stage_licenses] 仓库根 = ${ROOT}"
echo "[stage_licenses] 落位目录 = ${OUT_DIR}"

if [ -z "${OUT_DIR}" ]; then
  echo "FAIL: RECON_LICENSES_OUT_DIR 为空 —— 无法确定许可清单落位" >&2
  exit 1
fi

mkdir -p "${OUT_DIR}"

# --- 路径 (a)：用 Licenses.cmake 的脚本模式现场生成 -------------------------

generated=0
if command -v cmake >/dev/null 2>&1; then
  echo "[stage_licenses] 尝试 cmake -P cmake/Licenses.cmake 现场生成 …"
  if ( cd "${ROOT}" && cmake -P cmake/Licenses.cmake -- "${DEST}" >/dev/null 2>&1 ); then
    if [ -s "${DEST}" ]; then
      generated=1
      echo "[stage_licenses] 已由 Licenses.cmake 生成（真值来自 cmake/ThirdParty.cmake）"
    fi
  fi
  if [ "${generated}" -ne 1 ]; then
    echo "[stage_licenses] cmake 脚本模式未产出有效文件，尝试回退既有产物"
  fi
else
  echo "[stage_licenses] 未找到 cmake，跳过现场生成"
fi

# --- 路径 (b)：回退到仓内既有产物（**打印警告，不静默**）--------------------

if [ "${generated}" -ne 1 ]; then
  if [ -s "${REPO_SNAPSHOT}" ]; then
    cp -f "${REPO_SNAPSHOT}" "${DEST}"
    echo "[stage_licenses] 警告：回退使用仓内既有产物 ${REPO_SNAPSHOT}" >&2
    if command -v stat >/dev/null 2>&1; then
      # BSD/GNU stat 参数不同；失败不影响流程，仅少一行信息。
      stat -f "  mtime=%Sm" "${REPO_SNAPSHOT}" 2>/dev/null \
        || stat -c "  mtime=%y" "${REPO_SNAPSHOT}" 2>/dev/null \
        || true
    fi
    echo "[stage_licenses] 该产物可能落后于 third_party/ 当前状态；" >&2
    echo "[stage_licenses] 请在本机跑 \`cmake -P cmake/Licenses.cmake\` 刷新后重新构建。" >&2
  else
    echo "FAIL: 既无法现场生成、也无仓内既有产物 ${REPO_SNAPSHOT}" >&2
    if [ "${REQUIRE}" = "1" ]; then
      exit 1
    fi
    echo "（未设 RECON_REQUIRE_LICENSES=1，按非硬失败放过 —— 但 App 内许可页会读到空）" >&2
    exit 0
  fi
fi

# --- 校验：产物存在且非空 ---------------------------------------------------

if [ ! -s "${DEST}" ]; then
  echo "FAIL: 落位后 ${DEST} 为空" >&2
  if [ "${REQUIRE}" = "1" ]; then
    exit 1
  fi
  exit 0
fi

# --- 校验：清单头必须是 CMake 的 `===== <name> =====` 形态 -------------------
#  故意不用「含中文字面量」之类的判据（脆）；锚**结构性**特征：首行以
#  `===== ` 开头且以 ` =====` 结尾。生成器恒写出该形态（Licenses.cmake:96）。
head_line="$(head -n 1 "${DEST}")"
case "${head_line}" in
  "===== "*" ====="*)
    ;;
  *)
    echo "FAIL: ${DEST} 首行不是依赖条目头，文件形态不符（首行='${head_line}'）" >&2
    if [ "${REQUIRE}" = "1" ]; then
      exit 1
    fi
    ;;
esac

echo "[stage_licenses] OK: $(wc -c < "${DEST}") bytes → ${DEST}"
