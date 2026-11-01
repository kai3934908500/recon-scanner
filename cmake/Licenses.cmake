# SPDX-License-Identifier: BSD-3-Clause
#
# 第三方许可清单生成器（docs/12_构建与打包.md §2.7）。
#
# 产物：THIRD_PARTY_LICENSES.txt
#   · 内容顺序 = cmake/ThirdParty.cmake 的登记顺序（本文件**不重复登记**，
#     直接 include 唯一真源，从根本上杜绝「同一逻辑写两遍必然漂移」）。
#   · 依赖内部按许可文件名的字典序。
#   · 每个依赖的条目格式见 §2.7。
#
# 两种运行方式：
#
#   (1) configure 模式：被顶层 CMakeLists.txt `include()`。
#       生成到 ${CMAKE_BINARY_DIR}/THIRD_PARTY_LICENSES.txt，
#       并登记平台化的嵌入路径（iOS / Android / 桌面，见 §2.7「嵌入位置」）。
#
#   (2) 脚本模式（CI 用，不依赖构建树）：
#         cmake -P cmake/Licenses.cmake -- <输出文件路径>
#       供 `tools/license_scan.py --verify-licenses-cmake` 复现清单并与
#       third_party/ 中的许可文件逐条比对。
#
# 合规锚点：Eigen（B-1）必须收录 COPYING.MPL2 + COPYING.README 全文；
#          OpenCV（Apache-2.0）必须收录 LICENSE 全文（含 3rdparty 声明）。

cmake_minimum_required(VERSION 3.22)

get_filename_component(RECON_LICENSES_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(RECON_LICENSES_REPO_ROOT "${RECON_LICENSES_DIR}/.." ABSOLUTE)

# 依赖登记的唯一真源。所有 12 项的名称 / 顺序 / 许可证 / upstream / ref
# 都从它派生——本文件**不得**再写死一份列表。
include("${RECON_LICENSES_DIR}/ThirdParty.cmake")

# ---------------------------------------------------------------------------
# 许可文件识别：与 tools/license_scan.py 的 LICENSE_FILE_NAMES 保持同口径
# （docs/12 §2.6.2 阶段 1.3：LICENSE / LICENCE / COPYING / NOTICE / COPYRIGHT
#   及其带后缀形式；大小写不敏感）。
# ---------------------------------------------------------------------------
set(RECON_LICENSE_GLOBS
    "LICENSE" "LICENSE.*" "LICENCE" "LICENCE.*"
    "COPYING" "COPYING.*" "COPYRIGHT" "COPYRIGHT.*"
    "NOTICE" "NOTICE.*"
)

# recon_license_files(<dep_dir> <out_var>)
#   列出 <dep_dir> 顶层（非递归）的许可文件，按文件名字典序。
#   目录不存在时返回空列表（调用方据此判定「尚未 vendor」）。
function(recon_license_files dep_dir out_var)
  set(_found "")
  if(IS_DIRECTORY "${dep_dir}")
    foreach(_pat IN LISTS RECON_LICENSE_GLOBS)
      file(GLOB _hits "${dep_dir}/${_pat}")
      foreach(_h IN LISTS _hits)
        if(IS_DIRECTORY "${_h}")
          continue()
        endif()
        get_filename_component(_base "${_h}" NAME)
        list(APPEND _found "${_base}")
      endforeach()
    endforeach()
  endif()
  if(_found)
    list(REMOVE_DUPLICATES _found)
    list(SORT _found)
  endif()
  set(${out_var} "${_found}" PARENT_SCOPE)
endfunction()

# recon_generate_licenses_manifest(<out_file> <out_missing_var>)
#   生成清单；<out_missing_var> 回传「尚未 vendor 的依赖名列表」。
function(recon_generate_licenses_manifest out_file out_missing_var)
  set(_tp_root "${RECON_LICENSES_REPO_ROOT}/third_party")
  set(_doc "")
  set(_missing "")
  set(_first TRUE)

  foreach(_rec IN LISTS RECON_THIRD_PARTY)
    recon_parse_third_party("${_rec}" _name _upstream _license _ref_type
                            _ref _commit _subtree _integration)
    if(NOT _first)
      string(APPEND _doc "\n")
    endif()
    set(_first FALSE)

    if(_ref)
      set(_ref_line "Ref: ${_ref}")
      if(_commit)
        string(APPEND _ref_line " (${_commit})")
      endif()
    elseif(_commit)
      set(_ref_line "Ref: ${_commit}")
    else()
      set(_ref_line "Ref: (待 vendor 阶段填入)")
    endif()

    string(APPEND _doc "===== ${_name} =====\n")
    string(APPEND _doc "${_license}\n")
    string(APPEND _doc "Upstream: ${_upstream}\n")
    string(APPEND _doc "${_ref_line}\n")
    string(APPEND _doc "Subtree: ${_subtree}\n")

    recon_license_files("${_tp_root}/${_name}" _files)
    if(NOT _files)
      # 依赖尚未 vendor —— **不伪造**任何许可文本，只在末尾汇总区列出。
      list(APPEND _missing "${_name}")
      continue()
    endif()

    foreach(_f IN LISTS _files)
      file(READ "${_tp_root}/${_name}/${_f}" _text)
      string(APPEND _doc "--- ${_f} ---\n")
      string(APPEND _doc "${_text}")
      string(APPEND _doc "\n")
    endforeach()
  endforeach()

  if(_missing)
    string(APPEND _doc "\n===== 尚未 vendor 的依赖（清单暂缺，见 docs/12 §2.1 D-3）=====\n")
    string(APPEND _doc "以下依赖的源码与许可文本尚未落入 third_party/，"
                       "本清单不包含其全文。发布前必须补齐。\n")
    foreach(_m IN LISTS _missing)
      string(APPEND _doc "  - ${_m}\n")
    endforeach()
  endif()

  # 换行归一化为 LF，保证跨平台产物一致（Linux CI 与 Windows 本地同字节）。
  string(REPLACE "\r\n" "\n" _doc "${_doc}")
  string(REPLACE "\r" "\n" _doc "${_doc}")

  file(WRITE "${out_file}" "${_doc}")
  set(${out_missing_var} "${_missing}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# 模式 (2)：脚本模式（CI）：cmake -P cmake/Licenses.cmake -- <out_file>
# ---------------------------------------------------------------------------
if(DEFINED CMAKE_SCRIPT_MODE_FILE)
  # `cmake -P <script> -- <out_file>`：CMAKE_ARGV0=cmake，之后依次是
  # -P、脚本、`--`、<out_file>。取 `--` 之后的第一个参数；兼容直接
  # `cmake -P <script> -o <out>` 形式时取最后一个非选项参数。
  set(_out "")
  set(_seen_sep FALSE)
  set(_last "")
  foreach(_i RANGE 1 32)
    if(NOT DEFINED CMAKE_ARGV${_i})
      break()
    endif()
    set(_a "${CMAKE_ARGV${_i}}")
    if(_seen_sep AND NOT _out)
      set(_out "${_a}")
    endif()
    if(_a STREQUAL "--")
      set(_seen_sep TRUE)
    elseif(NOT _a STREQUAL "-P" AND NOT _a STREQUAL "${CMAKE_SCRIPT_MODE_FILE}")
      set(_last "${_a}")
    endif()
  endforeach()
  if(NOT _out)
    set(_out "${_last}")
  endif()
  if(NOT _out)
    set(_out "${RECON_LICENSES_REPO_ROOT}/build_cmake/THIRD_PARTY_LICENSES.txt")
  endif()
  get_filename_component(_out "${_out}" ABSOLUTE)
  recon_generate_licenses_manifest("${_out}" _missing)
  message(STATUS "recon: 许可清单已生成 → ${_out}")
  if(_missing)
    message(STATUS "recon: 尚未 vendor 的依赖 ${_missing}")
  endif()
  return()
endif()

# ---------------------------------------------------------------------------
# 模式 (1)：configure 模式
# ---------------------------------------------------------------------------
if(NOT DEFINED CMAKE_BINARY_DIR)
  message(FATAL_ERROR "cmake/Licenses.cmake 需要 CMake 上下文（configure 或 -P 脚本模式）")
endif()

set(RECON_THIRD_PARTY_LICENSES_FILE
    "${CMAKE_BINARY_DIR}/THIRD_PARTY_LICENSES.txt"
    CACHE FILEPATH "第三方许可清单产物路径" FORCE)

recon_generate_licenses_manifest("${RECON_THIRD_PARTY_LICENSES_FILE}" _recon_missing_deps)
if(_recon_missing_deps)
  list(LENGTH _recon_missing_deps _recon_missing_deps_count)
  # 如实告警，不掩盖：发布构建必须为 0。
  # 分号列表会被 CMake 按列宽折行，先转成箭头分隔的单串再输出。
  string(REPLACE ";" " → " _recon_missing_str "${_recon_missing_deps}")
  message(WARNING
    "recon: 以下 ${_recon_missing_deps_count} 项依赖尚未 vendor，"
    "许可清单暂缺（发布前必须补齐，见 docs/12 §2.1 D-3）：\n"
    "  ${_recon_missing_str}")
endif()
message(STATUS "recon: 许可清单 → ${RECON_THIRD_PARTY_LICENSES_FILE}")

# ---- 平台化嵌入位置（docs/12 §2.7「嵌入位置」）-----------------------------
if(RECON_PLATFORM_IOS)
  set(RECON_LICENSES_EMBED_DEST "Recon.app/Contents/Resources")
elseif(RECON_PLATFORM_ANDROID)
  set(RECON_LICENSES_EMBED_DEST "assets")
else()
  set(RECON_LICENSES_EMBED_DEST ".")
endif()

install(FILES "${RECON_THIRD_PARTY_LICENSES_FILE}"
        DESTINATION "${RECON_LICENSES_EMBED_DEST}"
        COMPONENT licenses)

# 便于 CI / 开发者一键重生成（不依赖重新 configure）。
add_custom_target(licenses_manifest
  COMMAND "${CMAKE_COMMAND}"
          -P "${RECON_LICENSES_DIR}/Licenses.cmake"
          -- "${RECON_THIRD_PARTY_LICENSES_FILE}"
  COMMENT "重新生成 THIRD_PARTY_LICENSES.txt"
  VERBATIM)
