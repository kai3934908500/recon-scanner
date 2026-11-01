# SPDX-License-Identifier: BSD-3-Clause
#
# 第三方依赖登记真源（docs/12_构建与打包.md §2.1 的 D-1）。
#
# 本文件是「四表一致」链条的**唯一真源**（docs/10 §2.1 / docs/12 §2.1 /
# 本文件 / cmake/Licenses.cmake）。任何一行的新增、删除、版本变更都必须四处同步，
# 否则 `tools/license_scan.py --check-registry` 失败（docs/12 D-2）。
#
# 登记格式：`RECON_THIRD_PARTY` 的每一项为一条记录，字段以 `|` 分隔：
#
#   name | upstream | license_spdx | ref_type | ref | commit | subtree | integration
#
#   · name         依赖目录名（third_party/<name>/），必须与 VENDOR.yaml 的 name 一致
#   · upstream     上游仓库（owner/repo，或 org/repo 的子树）
#   · license_spdx SPDX 标识；必须落在 docs/10 §1.2 白名单类型集合内
#   · ref_type     tag | commit
#   · ref          tag 名；ref_type=commit 时为本字段留空
#   · commit       40 位十六进制 SHA；**由 vendor 阶段离线解析后填入**
#   · subtree      vendor 的子树范围；`-` 表示整棵仓库
#   · integration  集成方式：static | interface | build_only
#
# ⚠️ 关于空的 `commit`：本项目当前尚未把全部 12 项依赖 vendor 到
#    third_party/（见 docs/12 §2.1 D-3）。`commit` 由 vendor 阶段离线解析、
#    人工写入（构建容器不联网，无法在 configure 阶段解析 tag）。
#    `tools/license_scan.py` 对**已存在目录**的依赖要求 commit 为 40 hex；
#    对**尚未 vendor** 的依赖，空 commit 记 warning（不阻断本地开发）。

set(RECON_THIRD_PARTY_NAMES
    ceres
    eigen
    opencv
    colmap
    poselib
    poisson_recon
    flann
    nanoflann
    basalt
    dbow2
    xatlas
    jsoncpp
)

# name|upstream|license_spdx|ref_type|ref|commit|subtree|integration
set(RECON_THIRD_PARTY
    "ceres|ceres-solver/ceres|BSD-3-Clause|tag|2.2.0||-|static"
    "eigen|libeigen/eigen|MPL-2.0|tag|3.4.0||Eigen/|interface"
    "opencv|opencv/opencv|Apache-2.0|tag|4.10.0||modules/(受 §2.3 裁剪)|static"
    "colmap|colmap/colmap|BSD-3-Clause|tag|3.10.0||src/colmap/{feature,estimators,sfm,util,geometry,sensor(仅 camera_models),math,scene}|static"
    "poselib|PoseLib/PoseLib|BSD-3-Clause|tag|v2.0.1||-|static"
    "poisson_recon|mkazhdan/PoissonRecon|MIT|tag|v2.1.0||-|static"
    "flann|flann2/flann|BSD-3-Clause|tag|1.6.0||-|static"
    "nanoflann|jlblancoc/nanoflann|BSD-3-Clause|tag|v1.6.2||include/|interface"
    "basalt|vkutuzov/basalt|BSD-3-Clause|commit|||-|static"
    "dbow2|UZ-SLAMLab/ORB_SLAM2|BSD-3-Clause|commit|||Thirdparty/BoW2/|static"
    "xatlas|jkoolpix/xatlas|MIT|tag|v2023-12-15||-|static"
    "jsoncpp|open-source-parsers/jsoncpp|MIT|tag|1.9.6||-|static"
)

# ---------------------------------------------------------------------------
# 白名单边界项（docs/10 §4）。需法务签字；扫描器校验其必须带签字引用。
# ---------------------------------------------------------------------------
# name|license_spdx|boundary_item|requires_legal_signoff|approver|date|reference|allowed_integration|source_modification_allowed
set(RECON_THIRD_PARTY_BOUNDARY_ITEMS
    "eigen|MPL-2.0|B-1|true|见 docs/legal/signoff/B-1_eigen_2026-10-30.md|2026-10-30|docs/10_许可与合规.md#B-1|static|false"
)

# ---------------------------------------------------------------------------
# 构建期依赖（不进入发布包）。由 `--no-build-only-in-release` 校验其未进入 Release。
# ---------------------------------------------------------------------------
# name|license_spdx|version|purpose
set(RECON_THIRD_PARTY_BUILD_ONLY
    "googletest|BSD-3-Clause|v1.15.2|单元测试框架，不链接进 App 二进制"
)

# ---------------------------------------------------------------------------
# 支持目录（**不是依赖**）：third_party/ 下一级子目录中，既非发布依赖、
# 也非构建期依赖，而是 vendor 流程的原料 / 中间产物。
#
# 存在的意义：docs/12 §2.6.2 阶段 1.1 要求「third_party/ 一级子目录与登记
# 的 name 一一对应」。支持目录若不显式登记，扫描器只能记 warning，长期会
# 淹没真信号（违反 §2.6.3 L-6 的精神）。因此在此**显式登记并说明用途**，
# 与 docs/12 §2.1 D-1 一致（本文件仍是唯一真源）。
#
# ⚠️ 支持目录不得被任何构建目标引用；一旦被编译进产物即为违规。
# ---------------------------------------------------------------------------
# name|kind|purpose
set(RECON_THIRD_PARTY_SUPPORT_DIRS
    "PoissonRecon-master|raw_archive|上游原始下载包（mkazhdan/PoissonRecon, MIT），保留以备复现；实际集成的是 poisson_recon/ 子集，见 third_party/poisson_recon/PATCHES.md"
)

# ---------------------------------------------------------------------------
# 辅助函数：把一条登记记录拆成具名变量，供 CMake 逻辑使用。
#
# 用法：
#   recon_parse_third_party("${rec}" name upstream license ref_type ref commit subtree integration)
# ---------------------------------------------------------------------------
function(recon_parse_third_party record out_name out_upstream out_license out_ref_type
         out_ref out_commit out_subtree out_integration)
  string(REPLACE "|" ";" _fields "${record}")
  list(LENGTH _fields _n)
  if(NOT _n EQUAL 8)
    message(FATAL_ERROR
      "cmake/ThirdParty.cmake: 登记记录字段数应为 8，实际 ${_n}：\n  ${record}")
  endif()
  list(GET _fields 0 _name)
  list(GET _fields 1 _upstream)
  list(GET _fields 2 _license)
  list(GET _fields 3 _ref_type)
  list(GET _fields 4 _ref)
  list(GET _fields 5 _commit)
  list(GET _fields 6 _subtree)
  list(GET _fields 7 _integration)
  set(${out_name} "${_name}" PARENT_SCOPE)
  set(${out_upstream} "${_upstream}" PARENT_SCOPE)
  set(${out_license} "${_license}" PARENT_SCOPE)
  set(${out_ref_type} "${_ref_type}" PARENT_SCOPE)
  set(${out_ref} "${_ref}" PARENT_SCOPE)
  set(${out_commit} "${_commit}" PARENT_SCOPE)
  set(${out_subtree} "${_subtree}" PARENT_SCOPE)
  set(${out_integration} "${_integration}" PARENT_SCOPE)
endfunction()

# 返回登记表中的全部依赖名（等价于 RECON_THIRD_PARTY_NAMES，但由记录派生，
# 防止两个列表不同步）。
function(recon_third_party_names out_var)
  set(_names "")
  foreach(_rec IN LISTS RECON_THIRD_PARTY)
    recon_parse_third_party("${_rec}" _name _u _l _rt _r _c _s _i)
    list(APPEND _names "${_name}")
  endforeach()
  set(${out_var} "${_names}" PARENT_SCOPE)
endfunction()
