# SPDX-License-Identifier: BSD-3-Clause
#
# android/app/src/main/cpp/CMakeLists.txt —— 仅作转发。
# 仓库根 CMakeLists.txt 是唯一真源（B-1），本文件不得定义任何 target。
# 契约：docs/12_构建与打包.md §3.4.3。
#
# 转发路径必须用相对路径推导，禁止写死绝对路径（可复现构建）。

cmake_minimum_required(VERSION 3.24.0)

# CMAKE_CURRENT_LIST_DIR = <repo>/platform/android/app/src/main/cpp
# 向上 5 级到仓库根：cpp → main → src → app → android → platform → <repo>
get_filename_component(RECON_REPO_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../" ABSOLUTE)

if(NOT EXISTS "${RECON_REPO_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "未在 ${RECON_REPO_ROOT} 找到仓库根 CMakeLists.txt。"
        "薄壳转发路径失效，见 docs/12_构建与打包.md §3.4.3。")
endif()

include("${RECON_REPO_ROOT}/CMakeLists.txt")
