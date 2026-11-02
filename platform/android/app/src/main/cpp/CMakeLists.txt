# SPDX-License-Identifier: BSD-3-Clause
#
# android/app/src/main/cpp/CMakeLists.txt —— 仅作转发。
# 仓库根 CMakeLists.txt 是唯一真源（B-1），本文件不得定义任何 target。
# 契约：docs/12_构建与打包.md §3.4.3。
#
# 转发路径必须用相对路径推导，禁止写死绝对路径（可复现构建）。

cmake_minimum_required(VERSION 3.24.0)

# CMAKE_CURRENT_LIST_DIR = <repo>/platform/android/app/src/main/cpp
# 向上 6 级到仓库根：cpp → main → src → app → android → platform → <repo>
#
# ⚠️ 层级数必须精确数箭头，**逐个数目录名**，不要凭"看起来差不多"写：
#   1×../ = main、2×../ = src、3×../ = app、4×../ = android、
#   5×../ = platform、**6×../ = <repo>**。
#   本文件曾误写 **5 个** `../`（注释文字还自相矛盾地写着"5 级"、却把 6 个
#   目录名一路列了出来）→ `RECON_REPO_ROOT` 落到 `<repo>/platform`，
#   那里**没有** CMakeLists.txt → 触发下方 FATAL_ERROR →
#   Android CI `:app:configureCMakeRelease[arm64-v8a]` **从 #3 起从未成功**
#   （本机有 `CMakeLists.txt` 的地方在上一级，故桌面构建完全不受影响，
#   是"只有 CI 能发现"的一类错误）。
#   回归防线：`tools/check_android_contract.py` **G-AND-6**（层级数实测校验）。
get_filename_component(RECON_REPO_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../../" ABSOLUTE)

if(NOT EXISTS "${RECON_REPO_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "未在 ${RECON_REPO_ROOT} 找到仓库根 CMakeLists.txt。"
        "薄壳转发路径失效，见 docs/12_构建与打包.md §3.4.3。")
endif()

include("${RECON_REPO_ROOT}/CMakeLists.txt")
