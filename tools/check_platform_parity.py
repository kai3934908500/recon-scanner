#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_platform_parity.py — 双端（iOS / Android）功能对等闸门。

背景
----
项目铁律：最终交付形态是 **iOS + Android 双端 App**，两端同等对待
（记忆：用户 2026-10-30 明确「苹果和安卓平台」；docs/12 §1.2 把
`recon_platform_ios` 与 `recon_platform_android` 列为平级 target）。

但 `platform/` 长期为空，且两端采集实现（AVFoundation/CoreMotion 与
Camera2/SensorManager）天然分属两个语言生态，极易**悄悄漂移**：
一端加了权限、另一端忘了；一端的状态机事件序号变了、另一端没跟。
这类漂移在编译期不报错、在单端测试里也测不出 —— 正是项目反复踩过的
「同一逻辑写两遍必然漂移」的同构坑。

本闸门做**静态对等校验**（不需要 iOS/Android 工具链，任何机器可跑）：

  ① 权限键对等：Android 声明的每条 dangerous 权限，iOS 侧都要有对应的
     用途声明或明确的「不需要」记录；反之亦然。
  ② 零网络对等：两端都不得出现 INTERNET / 网络框架符号。
  ③ 状态机事件/状态序号对等：Android 的 CaptureEventBridge.kt 常量序号
     与 iOS 的 CaptureStateBridge.h 枚举序号必须逐一相同（它们都镜像
     capture_state.h 的 C++ 枚举）。
  ④ C ABI 调用面覆盖：NativeBridge.kt 与 ReconCore.swift 调用到的
     recon_* 函数集合应覆盖同一批；缺失即不对等。
  ⑤ recon_status 数值集合对等（Kotlin CoreTypes ↔ C ABI 头）。
  ⑥ JNI 数组布局条数对等（Kotlin COUNT ↔ native 枚举）。
  ⑦ C ABI 声明覆盖无悬空 + recon_free_buffer 豁免有效性。
  ⑧ 平台无关逻辑**只允许一份实现**（IMU 时间窗切片 / 深度单位换算必须落在
     platform/common 的 C++17，双端实现侧不得各写一份）。
  ⑨ JNI 声明↔实现闭合：Kotlin `object` 里每个 `external fun` 必须有同名
     `Java_com_recon_scan_<Obj>_<name>` 定义（反向亦然）；且每个 JNI `.cpp`
     必须已注册进根 CMakeLists 的 `recon_platform_android` target ——
     否则真机首次调用抛 `UnsatisfiedLinkError`，而**编译期与既有闸门全不报错**。

退出码
------
0 = 对等
1 = 用法错误 / 文件缺失
3 = 检测到不对等
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
ANDROID = REPO / "platform" / "android"
IOS = REPO / "platform" / "ios"

ANDROID_MANIFEST = ANDROID / "app" / "src" / "main" / "AndroidManifest.xml"
ANDROID_BRIDGE_KT = (
    ANDROID / "app" / "src" / "main" / "java" / "com" / "recon" / "scan"
    / "CaptureEventBridge.kt"
)
ANDROID_NATIVE_KT = (
    ANDROID / "app" / "src" / "main" / "java" / "com" / "recon" / "scan"
    / "NativeBridge.kt"
)
# 两端**直接调用 C ABI** 的文件 —— 真正的对等比较对象。
# （Kotlin 经 JNI 间接调用、Swift 直接调 C ABI；故用 native-lib.cpp 对 ReconCore.swift）
ANDROID_NATIVE_CPP = ANDROID / "app" / "src" / "main" / "cpp" / "native-lib.cpp"
ANDROID_CORE_TYPES_KT = (
    ANDROID / "app" / "src" / "main" / "java" / "com" / "recon" / "scan"
    / "CoreTypes.kt"
)
IOS_PLIST = IOS / "Recon" / "Info.plist"
IOS_PRIVACY = IOS / "Recon" / "Resources" / "PrivacyInfo.xcprivacy"
IOS_STATE_H = IOS / "Recon" / "CaptureStateBridge.h"
IOS_CORE_SWIFT = IOS / "Recon" / "ReconCore.swift"

# C ABI 头：导出格式枚举的权威定义处（闸门 11）。
C_API_HEADER = REPO / "core" / "include" / "recon" / "recon_c_api.h"

# 闸门 11 的对象：导出格式的三端映射。
# 权威 UI 源在 ui/ios/Recon/Sources/（platform/ios/ 只放桥接）。
UI_IOS_SOURCES = REPO / "ui" / "ios" / "Recon" / "Sources"
IOS_EXPORT_VIEW = UI_IOS_SOURCES / "Screens" / "ExportView.swift"
IOS_SESSION_CONTROLLER = UI_IOS_SOURCES / "State" / "ReconSessionController.swift"

# ---------------------------------------------------------------------------
# 导出格式三端映射的真源（闸门 11）
# ---------------------------------------------------------------------------
# 导出格式的序号是**已发布的 ABI 契约**：`recon_export_format` 的整数直接
# 从 Kotlin / Swift 传进 `recon_export()`，三端必须逐项对应。它没有编译期
# 保护 —— Swift 侧 `recon_export_format(rawValue:)` 是**可失败转换**，传一个
# C ABI 不存在的序号会得到 nil 并静默回退；Kotlin 侧直接 `static_cast`，
# 传错值只会让内核 `default:` 分支返回「格式不支持」。
#
# 因此「加了格式但只改一端」这类漂移在编译期与既有闸门里**全不报错**，
# 只有到真机上点导出才暴露。本闸门把这张表当作真源，逐项核对四份产物：
#   ① C ABI 枚举（值）       core/include/recon/recon_c_api.h
#   ② Swift 枚举（存在性+顺序）ui/ios/.../Screens/ExportView.swift
#   ③ Swift 显式序号映射     ui/ios/.../State/ReconSessionController.swift
#   ④ Kotlin 注释（值）      platform/android/.../NativeBridge.kt
#
# 改这张表 = 改契约：必须同步改四处实现 + docs/01 §2.2.7 + docs/06 §3.1。
EXPORT_FORMAT_TRUTH = [
    # (序号, C ABI 枚举名, Swift case 名, Kotlin 注释里的别名)
    (0, "RECON_EXPORT_GLB", "glb", "GLB"),
    (1, "RECON_EXPORT_OBJ_MTL", "objMtl", "OBJ_MTL"),
    (2, "RECON_EXPORT_PLY", "ply", "PLY"),
    (3, "RECON_EXPORT_STL", "stl", "STL"),
    (4, "RECON_EXPORT_3MF", "threeMf", "3MF"),
]

# ---------------------------------------------------------------------------
# 权限对等映射
# ---------------------------------------------------------------------------
# 左：Android dangerous 权限名；右：iOS 侧的对应声明（Info.plist 键）或
# 显式 None（表示 iOS 侧该能力不需要权限，须在 docs/11 有论证）。
# 该表是本闸门的**真源**：改动权限必须同步此表与 docs/11。
ANDROID_TO_IOS_PERMISSION = {
    "android.permission.CAMERA": "NSCameraUsageDescription",
    "android.permission.WRITE_EXTERNAL_STORAGE":
        "NSPhotoLibraryAddUsageDescription",  # 写入相册/公共目录
    "android.permission.POST_NOTIFICATIONS": None,  # iOS 通知为系统级，无需用途键
    # 传感器：Android 侧传感器不需运行时权限；iOS 侧 NSMotionUsageDescription
    # 用于说明用途但不弹窗（docs/11 §1.2）。
    "android.permission.WAKE_LOCK": None,
    "android.permission.FOREGROUND_SERVICE": None,   # iOS 无对应概念
    "android.permission.FOREGROUND_SERVICE_DATA_SYNC": None,
}

# 明确禁止出现的权限（零网络 / 最小权限）。
FORBIDDEN_ANDROID_PERMISSIONS = {
    "android.permission.INTERNET",
    "android.permission.ACCESS_NETWORK_STATE",
    "android.permission.RECORD_AUDIO",
    "android.permission.READ_MEDIA_IMAGES",
    "android.permission.READ_MEDIA_VIDEO",
    "android.permission.READ_MEDIA_AUDIO",
    "android.permission.READ_EXTERNAL_STORAGE",
    "android.permission.ACCESS_FINE_LOCATION",
    "android.permission.ACCESS_COARSE_LOCATION",
}

FORBIDDEN_IOS_PLIST_KEYS = {
    "NSMicrophoneUsageDescription",
    "NSPhotoLibraryUsageDescription",
    "NSLocalNetworkUsageDescription",
    "NSUserTrackingUsageDescription",
    "NSBluetoothAlwaysUsageDescription",
}


def read(path: pathlib.Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def strip_comments(text: str) -> str:
    """剥离注释，避免「明确不声明 INTERNET」这类说明文字被误判为声明。

    这对零网络闸门尤其关键：清单里**故意**用注释写明「明确不声明 INTERNET」，
    若不去注释，闸门会把注释当成真的声明 —— 是典型的假阳性。
    """
    # XML（<!-- -->）与 C 风格（/* */、//）注释。
    text = re.sub(r"<!--.*?-->", "", text, flags=re.DOTALL)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def parse_android_permissions(text: str) -> set[str]:
    return set(re.findall(
        r'<uses-permission\s+android:name="([^"]+)"', text))


def parse_ios_plist_keys(text: str) -> set[str]:
    return set(re.findall(r"<key>([^<]+)</key>", text))


def parse_android_state_consts(text: str, prefix: str) -> dict[str, int]:
    """从 CaptureEventBridge.kt 提取 `const val <PREFIX>_X = n`。"""
    out: dict[str, int] = {}
    for m in re.finditer(r"const\s+val\s+(\w+)\s*=\s*(\d+)", text):
        name, val = m.group(1), int(m.group(2))
        if name == "EXPECTED_ABI_VERSION":
            continue
        out[name] = val
    return out


def parse_ios_enum_values(text: str, enum_name: str) -> dict[str, int]:
    """从 CaptureStateBridge.h 提取 `typedef NS_ENUM(NSInteger, <name>) { A = n, ... }`。"""
    m = re.search(
        r"NS_ENUM\(NSInteger,\s*" + re.escape(enum_name) + r"\)\s*\{(.*?)\}",
        text, re.DOTALL)
    if not m:
        return {}
    body = m.group(1)
    out: dict[str, int] = {}
    for line in body.split(","):
        mm = re.match(r"\s*(\w+)\s*=\s*(\d+)\s*$", line)
        if mm:
            out[mm.group(1)] = int(mm.group(2))
    return out


def parse_recon_calls(text: str) -> set[str]:
    """提取 recon_* 标识符。用词边界保证 `recon_export` 不会匹配到
    `recon_export_format`（类型名）—— 后者不是函数调用。"""
    return set(re.findall(r"\brecon_[a-z_]+", text))


def parse_c_status_values(text: str) -> set[int]:
    """解析 `typedef enum recon_status { ... }` 的**数值集合**（只认显式赋值）。

    注意：**不能按逗号切分后 `re.match`** —— 每组的首个成员前有 `/* 通用 */`
    之类的注释，`re.match` 会因注释前缀而漏掉该组首项（实测漏 12 个值）。
    改为在枚举体内全局 `findall`。
    """
    # 先剥注释，避免注释里的 `= 100` 干扰。
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    m = re.search(r"typedef\s+enum\s+(?:recon_status\s*)?\{(.*?)\}\s*recon_status\s*;",
                  text, re.S)
    if not m:
        return set()
    return {int(v) for _n, v in
            re.findall(r"\b(RECON_\w+)\s*=\s*(\d+)", m.group(1))}


def parse_kotlin_const_ints(text: str, obj_name: str = "ReconStatus") -> set[int]:
    """解析 Kotlin `object <obj_name> { const val X = n ... }` 的数值集合。

    **必须限定在对象体内** —— 否则会把同文件里 PipelineBranch 的 0/1、
    Capability.COUNT=12、MeshInfo.COUNT=17 一并读进来（实测多出 12/17 等）。
    """
    m = re.search(r"object\s+" + re.escape(obj_name) + r"\s*\{(.*?)\n\}",
                  text, re.S)
    if not m:
        return set()
    body = re.sub(r"//[^\n]*", "", m.group(1))
    return {int(m2.group(1)) for m2 in
            re.finditer(r"const\s+val\s+\w+\s*=\s*(\d+)", body)}


def parse_kotlin_const(text: str, name: str) -> int | None:
    m = re.search(r"const\s+val\s+" + re.escape(name) + r"\s*=\s*(\d+)", text)
    return int(m.group(1)) if m else None


def parse_cpp_enum_count(text: str, marker: str) -> int | None:
    """读取 C++ 侧 `enum { kA = 0, kB, ..., kXxxCount };` 的「真实条目数」。

    约定（见 native-lib.cpp）：计数成员 `kXxxCount` 位于最后且**不参与计数**，
    其余成员才是数据项（多数为隐式自增，不可靠地用 `= n` 去数）。
    返回 = 非计数成员的名字个数。解析失败返回 None。
    """
    for m in re.finditer(r"enum\s+\w+\s*\{(.*?)\}\s*;", text, re.S):
        body = m.group(1)
        if marker not in body:
            continue
        names = re.findall(r"\b(k\w+)\b", body)
        # 去掉注释里可能出现的重复名；计数成员只有一个且等于 marker。
        names = [n for n in names]
        return len(names) - names.count(marker)
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description="双端功能对等闸门")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    failures: list[str] = []

    # 文件存在性（缺失即 3，不静默 PASS）。
    required = {
        "AndroidManifest.xml": ANDROID_MANIFEST,
        "CaptureEventBridge.kt": ANDROID_BRIDGE_KT,
        "NativeBridge.kt": ANDROID_NATIVE_KT,
        "CoreTypes.kt": ANDROID_CORE_TYPES_KT,
        "native-lib.cpp": ANDROID_NATIVE_CPP,
        "Info.plist": IOS_PLIST,
        "PrivacyInfo.xcprivacy": IOS_PRIVACY,
        "CaptureStateBridge.h": IOS_STATE_H,
        "ReconCore.swift": IOS_CORE_SWIFT,
    }
    missing = [name for name, p in required.items() if not p.exists()]
    if missing:
        print("[platform-parity] FAIL：必需文件缺失：" + ", ".join(missing))
        return 1

    android_manifest_raw = read(ANDROID_MANIFEST)
    ios_plist_raw = read(IOS_PLIST)
    ios_privacy = read(IOS_PRIVACY)

    # 剥离注释后再做「声明」判定（避免把说明文字当声明 —— 假阳性）。
    android_manifest = strip_comments(android_manifest_raw)
    ios_plist = strip_comments(ios_plist_raw)

    android_perms = parse_android_permissions(android_manifest)
    ios_keys = parse_ios_plist_keys(ios_plist)

    # --- 闸门 1a：Android 声明的权限，iOS 侧须有对等声明或明确记录 ---
    for aper in sorted(android_perms):
        if aper in FORBIDDEN_ANDROID_PERMISSIONS:
            failures.append(f"Android 声明了禁止权限：{aper}")
            continue
        if aper not in ANDROID_TO_IOS_PERMISSION:
            failures.append(
                f"Android 权限 {aper} 未在映射表中登记 —— "
                f"新增权限必须同步 ANDROID_TO_IOS_PERMISSION 与 docs/11")
            continue
        ios_key = ANDROID_TO_IOS_PERMISSION[aper]
        if ios_key is not None and ios_key not in ios_keys:
            failures.append(
                f"Android 有 {aper}，但 iOS Info.plist 缺 {ios_key}（不对等）")

    # --- 闸门 1b：iOS Info.plist 不得出现禁止键 ---
    for fk in sorted(FORBIDDEN_IOS_PLIST_KEYS):
        if fk in ios_keys:
            failures.append(f"iOS Info.plist 出现禁止键：{fk}")

    # --- 闸门 2：零网络对等 ---
    if "INTERNET" in android_manifest:
        failures.append("AndroidManifest 出现 INTERNET（违反零网络）")
    if "NSLocalNetworkUsageDescription" in ios_plist:
        failures.append("Info.plist 出现 NSLocalNetworkUsageDescription（违反零网络）")
    if re.search(r"NSPrivacyTracking</key>\s*<true", ios_privacy):
        failures.append("PrivacyInfo.xcprivacy 的 NSPrivacyTracking 为 true（违反不追踪）")
    if not re.search(r"NSPrivacyTracking</key>\s*<false", ios_privacy):
        failures.append("PrivacyInfo.xcprivacy 缺 NSPrivacyTracking=false")

    # --- 闸门 3：状态机事件/状态序号对等 ---
    kt_events = parse_android_state_consts(read(ANDROID_BRIDGE_KT), "EVENT")
    ios_events = parse_ios_enum_values(read(IOS_STATE_H), "ReconCaptureEvent")
    # Kotlin 侧命名是 EVENT 语义（如 START_SCAN），iOS 是 ReconCaptureEventStartScan。
    # 只比较**序号集合**与**条目数**（命名风格不同，序号必须一致）。
    kt_event_vals = sorted(v for k, v in kt_events.items()
                           if not k.startswith("STATE_") and not k.startswith("FAILURE_"))
    ios_event_vals = sorted(ios_events.values())
    if kt_event_vals != ios_event_vals:
        failures.append(
            f"状态机事件序号不对等：Android {kt_event_vals} vs iOS {ios_event_vals}")
    else:
        print(f"[platform-parity] 状态机事件序号对等：{len(ios_event_vals)} 项 ✓")

    kt_state_vals = sorted(v for k, v in kt_events.items() if k.startswith("STATE_"))
    ios_state_vals = sorted(parse_ios_enum_values(read(IOS_STATE_H), "ReconCaptureState").values())
    if kt_state_vals != ios_state_vals:
        failures.append(
            f"状态机状态序号不对等：Android {kt_state_vals} vs iOS {ios_state_vals}")
    else:
        print(f"[platform-parity] 状态机状态序号对等：{len(ios_state_vals)} 项 ✓")

    kt_failure_vals = sorted(v for k, v in kt_events.items() if k.startswith("FAILURE_"))
    ios_failure_vals = sorted(parse_ios_enum_values(read(IOS_STATE_H), "ReconFailureStage").values())
    if kt_failure_vals != ios_failure_vals:
        failures.append(
            f"失败阶段序号不对等：Android {kt_failure_vals} vs iOS {ios_failure_vals}")

    # --- 闸门 4：C ABI 调用面覆盖 ---
    # 比较两端**直接调用 C ABI** 的实现文件：Android 的 native-lib.cpp 与
    # iOS 的 ReconCore.swift。二者都是「平台 → 核心」的桥，理应覆盖同一批函数。
    kt_calls = parse_recon_calls(strip_comments(read(ANDROID_NATIVE_CPP)))
    sf_calls = parse_recon_calls(strip_comments(read(IOS_CORE_SWIFT)))
    # 两端都应覆盖同一批核心函数；缺失即不对等（一端实现了、另一端忘）。
    only_kt = sorted(kt_calls - sf_calls)
    only_sf = sorted(sf_calls - kt_calls)
    # 允许的合理差异：类型名（recon_config 等）不是函数调用；只比较
    # 明确的动词型函数（recon_create/destroy/…）。
    # 只比较**明确的动词型函数**（recon_create/destroy/…），并锚定结尾，
    # 避免 `recon_export` 前缀误配 `recon_export_format`（类型名）。
    func_verbs = re.compile(r"^recon_(create|destroy|abi_version|start_session|push_frame|"
                            r"finish_capture|run_reconstruction|export|cancel|is_cancelled|"
                            r"probe_capability|set_probe_data|"
                            r"set_param|get_param|apply_preset|"
                            r"get_mesh_info|calibrate_scale|set_manual_scale|"
                            r"get_scan_stats|free_string|free_buffer|export_params)$")
    only_kt_f = [c for c in only_kt if func_verbs.match(c)]
    only_sf_f = [c for c in only_sf if func_verbs.match(c)]
    if only_kt_f:
        failures.append("Android 调用了 iOS 未调用的 core 函数：" + ", ".join(only_kt_f))
    if only_sf_f:
        failures.append("iOS 调用了 Android 未调用的 core 函数：" + ", ".join(only_sf_f))
    if not only_kt_f and not only_sf_f:
        print(f"[platform-parity] C ABI 调用面覆盖对等 ✓")

    # --- 闸门 5：recon_status 数值集合对等（Kotlin ↔ C ABI） ---
    # 为什么需要：iOS 侧曾出现「Swift ReconStatus 只列 14/47 项」→ 其余错误码
    # 经 `ReconStatus(rawValue:)` 静默落回 internalError。Android 侧此前更糟 ——
    # 连一张错误码表都没有（只有裸魔数）。本闸门把三份来源钉在一起：
    #   C ABI 头文件（真源） ↔ Kotlin CoreTypes.ReconStatus.ALL
    # Kotlin 侧的 `ALL` 会随各 const 增删而漂移，故同时校验「ALL 去重后条数」
    # 与「const val 条数」相等 —— 防止 ALL 漏列新增项（那是另一种静默失效）。
    header_text = read(REPO / "core" / "include" / "recon" / "recon_c_api.h")
    kt_types_text = read(ANDROID_CORE_TYPES_KT)
    c_vals = parse_c_status_values(header_text)
    kt_vals = parse_kotlin_const_ints(kt_types_text)
    # Kotlin 对象里还有 PipelineBranch 的 RGBD=0/MONO=1，会混入；用 C ABI 的
    # 集合做参照时，只比较「C ABI 全包含于 Kotlin」+「Kotlin 不超出 C ABI」。
    if not c_vals:
        failures.append("未从 core/include/recon/recon_c_api.h 解析到 recon_status")
    if not kt_vals:
        failures.append(
            f"未从 {ANDROID_CORE_TYPES_KT.name} 解析到任何 const val 数值")
    if c_vals and kt_vals:
        missing_kt = sorted(c_vals - kt_vals)
        extra_kt = sorted(kt_vals - c_vals)
        # 允许的合理超出：PipelineBranch 的 0/1 与 0=OK 重合，不会超出；
        # 若 extra 只剩 {0,1} 这类常量则视为合法（已在 C ABI 内）。
        if missing_kt:
            failures.append(
                f"C ABI 有但 Kotlin CoreTypes 缺 {len(missing_kt)} 个状态码："
                f"{missing_kt}")
        if extra_kt:
            failures.append(
                f"Kotlin CoreTypes 有但 C ABI 无（引用了不存在的状态码）：{extra_kt}")
        # ALL 数组条数须等于 const 条数（去重）
        m_all = re.search(r"val\s+ALL\s*:\s*IntArray\s*=\s*intArrayOf\((.*?)\)",
                          kt_types_text, re.S)
        if m_all:
            all_cnt = len(re.findall(r"[A-Z_]{2,}", m_all.group(1)))
            if all_cnt != len(kt_vals):
                failures.append(
                    f"CoreTypes.ReconStatus.ALL 列了 {all_cnt} 项，但 const 有 "
                    f"{len(kt_vals)} 项 —— ALL 漏列会让 UI/日志静默漏码")
        if not missing_kt and not extra_kt:
            print(f"[platform-parity] recon_status 数值集合对等："
                  f"{len(c_vals)} 项 ✓")

    # --- 闸门 6：JNI 数组布局条数对等（Kotlin COUNT ↔ native 枚举） ---
    # 每个 data class 的 `COUNT` 必须等于 native 侧对应 `kXxxCount`。
    # 这是「下标约定」这件事的**唯一守护** —— 少了它，native 侧多加一个字段、
    # Kotlin 侧不改，`fromArray` 会静默少读一项（本项目既往的静默失效类型）。
    cpp_text = read(ANDROID_NATIVE_CPP)
    for kt_name, marker, label in (("COUNT", "kCapCount", "Capability"),
                                   ("COUNT", "kMeshCount", "MeshInfo"),
                                   ("COUNT", "kStatsCount", "ScanStats"),
                                   ("COUNT", "kScaleCount", "ScaleResult"),
                                   ("COUNT", "kParamCount", "ParamValue")):
        # 从对应 data class 内取 COUNT（多个类都有 COUNT，用 label 定位）。
        m_cls = re.search(
            r"data\s+class\s+" + label + r"\b.*?companion\s+object\s*\{(.*?)\}",
            kt_types_text, re.S)
        kt_cnt = None
        if m_cls:
            kt_cnt = parse_kotlin_const(m_cls.group(1), kt_name)
        cpp_cnt = parse_cpp_enum_count(cpp_text, marker)
        if kt_cnt is None or cpp_cnt is None:
            failures.append(f"{label} 布局条数无法解析（Kotlin={kt_cnt} "
                            f"native={cpp_cnt}）—— 无法确认对等")
            continue
        if kt_cnt != cpp_cnt:
            failures.append(
                f"{label} 数组条数不对等：Kotlin COUNT={kt_cnt} vs "
                f"native {marker}={cpp_cnt}")
        else:
            print(f"[platform-parity] {label} 数组布局对等：{kt_cnt} 项 ✓")

    # --- 闸门 7：对等判据的**覆盖面**自检（防「两端一起漏」） ---
    # 闸门 4 只比较「两端是否一致」。若**两端都漏实现**某个 C ABI 函数，
    # 它会静默通过 —— 那是典型的假闸门（恒不触发）。
    # 本闸门反过来要求：写在 `func_verbs` 白名单里的每个函数，**至少有一端**在调。
    # 白名单是「本产品声称会用的 C ABI 面」的声明，声明了却没人调 = 悬空声明。
    cpp_stripped = strip_comments(read(ANDROID_NATIVE_CPP))
    sw_stripped = strip_comments(read(IOS_CORE_SWIFT))
    all_calls = parse_recon_calls(cpp_stripped) | parse_recon_calls(sw_stripped)
    declared = {"recon_create", "recon_destroy", "recon_abi_version",
                "recon_start_session", "recon_push_frame", "recon_finish_capture",
                "recon_run_reconstruction", "recon_export", "recon_cancel",
                "recon_is_cancelled", "recon_probe_capability", "recon_set_param",
                "recon_get_param", "recon_apply_preset", "recon_get_mesh_info",
                "recon_calibrate_scale", "recon_set_manual_scale",
                "recon_get_scan_stats", "recon_free_string", "recon_free_buffer",
                "recon_export_params",
                # 缺口 AC：平台层上报能力事实的唯一入口。
                # **必须在此声明** —— 它正是「声明了却没人调 = 悬空声明」要抓的
                # 那一类。缺了它，本闸门对缺口 AC 完全无覆盖（改回「不上报」
                # 也不会有任何闸门报错，即 #109 同款检测盲区）。
                "recon_set_probe_data"}
    # 豁免：头文件里声明了、但**当前没有任何 C ABI 函数产出对应资源**，
    # 故平台层无处调用。豁免必须带理由，且由下方 `dangling_declared` 的
    # 反向校验守住 —— 一旦 core 真的新增了返回缓冲区的函数，本豁免应立刻失效
    # （届时闸门会要求实现用它），不会静默过期。
    exempt = {
        "recon_free_buffer":
            "无当前生产者：唯一返回 core 分配内存的函数是 recon_export_params"
            "（char* → recon_free_string）。头文件 §所有权段落属**预留**声明，"
            "待将来出现返回缓冲区的 ABI 函数时启用（届时删除本豁免并实现）。",
    }
    dangling_declared = sorted(declared - all_calls - set(exempt))
    if dangling_declared:
        failures.append(
            "以下 C ABI 函数被声明为「平台层会调用」，但 iOS 与 Android "
            f"**两端都没有调用**（悬空声明）：{dangling_declared}")
    else:
        print(f"[platform-parity] C ABI 声明覆盖无悬空："
              f"{len(declared) - len(exempt)} 项（豁免 {len(exempt)} 项）✓")

    # 豁免有效性反向校验：若头文件里出现了返回 core 分配缓冲区的 out 参数
    # （`void**` / `uint8_t**` 等），则 `recon_free_buffer` 的豁免**已过期**，
    # 必须实现它。用 `out_context` 是唯一合法的 `void**`（生命周期句柄，
    # 由 `recon_destroy` 回收，不经 free_buffer）。
    hdr = strip_comments(read(REPO / "core" / "include" / "recon" / "recon_c_api.h"))
    buf_outs = [m for m in re.findall(r"\b(uint8_t|void|unsigned\s+char)\s*\*\*\s*(\w+)", hdr)
                if m[1] != "out_context"]
    if buf_outs and "recon_free_buffer" in exempt:
        failures.append(
            f"recon_free_buffer 的豁免已过期：头文件出现返回缓冲区的 out 参数 "
            f"{buf_outs} —— 请实现 recon_free_buffer 并删除豁免")
    else:
        print("[platform-parity] recon_free_buffer 豁免仍有效"
              "（core 暂无缓冲区生产者）✓")

    # --- 闸门 8：平台无关逻辑**只允许一份实现**（防「同一逻辑写两遍必然漂移」）---
    #
    # 背景（第三十五轮）：iOS 曾把 IMU 环形缓冲/时间窗切片与深度单位换算**写在
    # Swift 里**。Android 若照写一份 Kotlin，两端的边界语义（`<=` 含等于）与消费
    # 语义（切窗后必须移出，否则预积分双重计数）就会各写各的，且漂移后**无编译期
    # 信号** —— 正是本项目反复踩过的坑。
    #
    # 现在规则本体在 `platform/common/`（纯 C++17，双端同源），两端只做薄封装。
    # 本闸门静态守住这条边界：双端**实现侧**源码里不得再出现这些规则。
    #
    # ── 为什么判据落在「实现特征」而非「文件名/关键词」 ───────────────────
    #
    # 判据若写成「Swift 里不得有 `IMURingBuffer` 这个词」，会被改名绕过；且
    # 注释里提到该词（如「规则本体在 platform/common」）会立刻假阳性。故此处
    # 用**实现特征**：
    #   · IMU 环缓冲：出现「按 timestamp 比较后切窗」的循环特征（`timestamp` 与
    #     `<=`/`<` 出现在同一 `while`/`for` 条件里），或 `removeFirst(` 这类
    #     就地消费调用。
    #   · 深度换算：出现「×1000 得毫米」或「>65535 夹取」的数值特征。
    # 这些特征一旦在双端出现，几乎必然是重写了一份规则。
    #
    # ⚠️ 允许在**注释**里出现（本判据前先剥注释）—— 注释说明「规则已下沉」是
    #    合规的，不得由此假阳性（本项目在 check_platform_parity 上吃过 3 次
    #    注释假阳性）。
    ui_ios_src = REPO / "ui" / "ios" / "Recon" / "Sources"
    android_kt_dir = (ANDROID / "app" / "src" / "main" / "java" / "com"
                      / "recon" / "scan")

    def _scan_impl_leak(root: pathlib.Path, suffixes: tuple[str, ...],
                        label: str) -> list[str]:
        """扫描某端实现源码，返回「疑似重写了平台无关规则」的 (文件, 原因) 列表。"""
        hits: list[str] = []
        if not root.is_dir():
            return hits
        for path in sorted(root.rglob("*")):
            if not path.is_file() or not path.name.endswith(suffixes):
                continue
            code = strip_comments(read(path))
            # 特征 1：时间戳比较出现在循环条件里（切窗逻辑）。
            for m in re.finditer(r"\b(while|for)\b[^\n{]*", code):
                seg = m.group(0)
                if "timestamp" in seg and ("<=" in seg or "<" in seg):
                    hits.append(f"{label}:{path.name} —— 循环条件含 timestamp 比较"
                                f"（疑似重写 IMU 切窗）")
                    break
            # 特征 2：深度上限常量（65535 / 65.535）。这是**深度换算特有的**
            # 数值 —— `×1000` 之类过于通用（mm³→cm³、mm→m 的标定换算也用，
            # 会大量假阳性，本项目明令禁止假闸门）。故只认这个高特异度特征。
            for pat, why in ((r"\b65_?535\b", "深度上限 65535"),
                             (r"\b65\.535\b", "深度上限 65.535")):
                if re.search(pat, code):
                    hits.append(f"{label}:{path.name} —— 含「{why}」特征"
                                f"（疑似重写深度单位换算）")
                    break
            # 特征 3（第三十七轮新增）：**YUV→RGB 色彩矩阵系数**。
            #
            # 这八个数是 BT.601 / BT.709 的矩阵增益（`2(1−kr)` 等），
            # 是 YUV 色彩数学的**指纹**：它们在别的语境里没有任何用途。
            # 规则本体在 `platform/common/src/yuv_convert.cpp`
            # （`MakeYuvToBgraParams` 派生），GPU 侧参数块由它填值 ——
            # 平台层若再次出现这些数字，就一定是重写了一份色彩数学。
            #
            # 为什么只认这八个（不用 `0.299`/`0.114` 之类）：后者是 kr/kb，
            # 会与「BT.601 灰度化」这类**合法**用途撞车（Gray 也要 0.299），
            # 用了必然假阳性。而 `1.402` / `0.714136` 这种精确增益不是。
            #
            # 为什么把 iOS 的 `.mm` 也纳入扫描：色彩数学最可能被内联的地方
            # 正是宿主/桥接层（"就几行，直接写在这里算了"）。只扫 Swift/Kotlin
            # 会漏掉这个真正的目标对象。
            #
            # ⚠️ 边界写法：**不能用 `\b1\.402\b`**。C/C++/ObjC++ 里浮点常量
            #    常带 `f` 后缀（`1.402f`），而 `2` 与 `f` 都是词字符 → 两者之间
            #    **没有词边界** → 该正则在真实代码上**恒不触发**（假闸门）。
            #    首版就是这么写的，被 P8-c 的负向注入抓出（rc=0 未拒收）。
            #    改用「前不接数字或小数点、后不接数字」的显式环视。
            for pat, why in (
                (r"(?<![\d.])1\.402(?!\d)", "BT.601 r_from_cr=1.402"),
                (r"(?<![\d.])1\.772(?!\d)", "BT.601 b_from_cb=1.772"),
                (r"(?<![\d.])0\.714136(?!\d)", "BT.601 g_from_cr=−0.714136"),
                (r"(?<![\d.])0\.344136(?!\d)", "BT.601 g_from_cb=−0.344136"),
                (r"(?<![\d.])1\.5748(?!\d)", "BT.709 r_from_cr=1.5748"),
                (r"(?<![\d.])1\.8556(?!\d)", "BT.709 b_from_cb=1.8556"),
                (r"(?<![\d.])0\.468124(?!\d)", "BT.709 g_from_cr=−0.468124"),
                (r"(?<![\d.])0\.187324(?!\d)", "BT.709 g_from_cb=−0.187324"),
            ):
                if re.search(pat, code):
                    hits.append(f"{label}:{path.name} —— 含「{why}」特征"
                                f"（疑似重写 YUV→RGB 色彩数学）")
                    break
        return hits

    leaks = (_scan_impl_leak(ui_ios_src, (".swift",), "iOS")
             + _scan_impl_leak(IOS / "Recon", (".mm", ".m"), "iOS桥接")
             + _scan_impl_leak(android_kt_dir, (".kt",), "Android"))
    if leaks:
        for leak in leaks:
            failures.append(
                f"{leak} —— 平台无关逻辑必须**只实现一份**（platform/common，"
                f"纯 C++17），两端只做薄封装。见 platform/common/include/"
                f"recon_platform/imu_window.h 与 yuv_convert.h 的文件头")
    else:
        print("[platform-parity] 平台无关逻辑单实现"
              "（IMU 切窗 / 深度换算 / YUV 色彩数学）✓")

    # --- 闸门 9：JNI 声明 ↔ 实现闭合（防 UnsatisfiedLinkError 静默漏网）---
    #
    # 背景（第三十六轮实测）：本轮新增 `imu-bridge.cpp` 时，JNI 导出名写成
    # `Java_com_recon_scan_ImuRingBuffer_append`，而 Kotlin 侧声明的是
    # `private external fun nativeAppend(...)` —— **少了一个 `native` 前缀**。
    #
    # 这是一次**真缺陷**，而且没有任何既有闸门能发现它：
    #   · 编译期：C++ 侧 `extern "C"` 函数名不参与任何 Kotlin 编译检查；
    #     Kotlin 侧 `external fun` 只声明不实现，`kotlinc` 也不校验。
    #   · 联动期：**只有真机 `System.loadLibrary` 后的首次调用**才会抛
    #     `UnsatisfiedLinkError`。本机无 Android SDK/NDK，永远跑不到。
    #   · 既有闸门 4/7 查的是 **C ABI**（`recon_*`）的调用面，而 JNI 符号名
    #     是**另一层边界**（Kotlin ↔ C++），两者无交集。
    #
    # 这与 iOS 侧缺口 N（ObjC 桥类不可见 → `cannot find ... in scope`）**同构**：
    # 都是「声明方与实现方的接线断了，而编译期不报错」。iOS 侧已用 G7-e 堵住，
    # Android 侧此前没有对等判据 —— 本闸门补上。
    #
    # ── 判据落在「目标对象」上 ─────────────────────────────────────────────
    #
    # 不做「有没有写 extern "C"」这类表面检查（那种判据恒真、是假闸门），而是
    # 直接比对**两端的符号名集合**：
    #   ① Kotlin `object X` 内的每个 `external fun name` ⟷ 必须存在
    #      `Java_<包路径>_X_name` 的 C++ 定义，否则 Kotlin 调用必崩。
    #   ② 反向：每个 JNI 定义必须有同名 Kotlin 声明，否则是死码（写了没人调，
    #      可能是改名后的残留 —— 与 ① 是同一个漂移的两个方向）。
    #
    # ── 已知边界（如实声明，不做超出能力的判定）────────────────────────────
    #
    # · 仅支持**无重载**的普通方法名。JNI 对重载方法会附加参数签名后缀
    #   （`__JI` 之类），Kotlin 侧 `object` 门面本项目不使用重载，故不处理；
    #   若将来出现重载，本闸门会误报「缺实现」—— 那是**安全的失败方向**
    #   （宁可要求人工确认，也不静默放过）。
    # · 名称含 `_` 或非 ASCII 时 JVM 会做 `_1`/`_0xxxx` 转义。本项目方法名
    #   全为 camelCase，不触发；同上，触发时会**误报**而非漏报。
    # · 不做参数类型/返回类型对账（那需要 JNI 签名描述符解析）；本闸门只保证
    #   「调用点不会因为没有实现而崩」这一最基本的不变量。类型不匹配会在
    #   真机上以 `NoSuchMethodError` 暴露，属另一条待补判据（已登记 docs/14）。
    android_cpp_dir = ANDROID / "app" / "src" / "main" / "cpp"
    kt_jni_decls: dict[str, set[str]] = {}
    for path in sorted(android_kt_dir.glob("*.kt")):
        code = strip_comments(read(path))
        for m in re.finditer(r"\bobject\s+(\w+)\s*\{(.*?)\n\}", code, re.S):
            obj, body = m.group(1), m.group(2)
            names = set(re.findall(r"\bexternal\s+fun\s+(\w+)\s*\(", body))
            if names:
                kt_jni_decls.setdefault(obj, set()).update(names)

    cpp_jni_defs: set[tuple[str, str]] = set()
    for path in sorted(android_cpp_dir.glob("*.cpp")):
        code = strip_comments(read(path))
        for m in re.finditer(r"\bJava_com_recon_scan_(\w+)_([A-Za-z0-9_]+)\s*\(", code):
            cpp_jni_defs.add((m.group(1), m.group(2)))

    jni_missing: list[str] = []
    for obj, names in sorted(kt_jni_decls.items()):
        for name in sorted(names):
            if (obj, name) not in cpp_jni_defs:
                jni_missing.append(f"{obj}.{name}")
    jni_orphan = sorted(f"{o}.{n}" for (o, n) in cpp_jni_defs
                        if n not in kt_jni_decls.get(o, set()))

    if jni_missing:
        failures.append(
            f"Kotlin 声明了 {len(jni_missing)} 个 `external fun` 但**无对应 JNI 实现**"
            f"（真机首次调用必抛 UnsatisfiedLinkError）：{jni_missing}")
    if jni_orphan:
        failures.append(
            f"存在 {len(jni_orphan)} 个 JNI 导出符号但 Kotlin 侧**未声明**"
            f"（死码 / 改名残留）：{jni_orphan}")
    if not jni_missing and not jni_orphan:
        print(f"[platform-parity] JNI 声明↔实现闭合："
              f"{sum(len(v) for v in kt_jni_decls.values())} 项 ✓")

    # --- 闸门 10：能力事实上报的**字段级**三端对等（缺口 AC）------------------
    #
    # 为什么必须有这一条（而不是只靠闸门 4 的「函数被调过」）：
    #
    # 闸门 4 只保证「两端都调了 `recon_set_probe_data`」这个**动作**存在。
    # 但缺口 AC 的真正危害是**字段漏写** —— 假设有人在 `recon_probe_data` 里
    # 加一个 `has_algo_depth` 却发现只有 Android 写了、iOS 忘了写（或反之），
    # 那么两端在同一个设备上会得出**不同的分支判定**，而且：
    #   · 闸门 4 依然全绿（函数确实被调）；
    #   · JNI 闭合闸门全绿（签名对得上）；
    #   · 真机行为差异只在「特定机型 + 特定字段」上暴露 —— 极难复现。
    #
    # 这正是本项目反复吃的「检测盲区」（#109 的教训：函数存在 ≠ 有人正确调用）。
    # 故本闸门做**字段级**三端对账：
    #   ① C ABI `recon_probe_data` 的字段集合（真源）；
    #   ② iOS `ReconProbeData.cValue` 里对该 C 结构体赋值的字段集合；
    #   ③ Android `NativeBridge.setProbeData` 的 JNI 形参 + JNI 实现里
    #      对该 C 结构体赋值的字段集合。
    # 三者必须一一对应。任一多写/少写即失败。
    #
    # ⚠️ 判据落在**目标对象**（字段赋值语句本身）上，而不是「结构体存在」或
    #    「文件里有这个词」—— 后者可被注释满足，是假判据。
    hdr_code = strip_comments(read(REPO / "core" / "include" / "recon" / "recon_c_api.h"))
    m_probe_struct = re.search(
        r"typedef\s+struct\s+recon_probe_data\s*\{(.*?)\}\s*recon_probe_data\s*;",
        hdr_code, re.S)
    if not m_probe_struct:
        failures.append(
            "无法从 core/include/recon/recon_c_api.h 解析到 `recon_probe_data` "
            "结构体 —— 缺口 AC 的字段对等判据失效（结构体被改名或删除？）")
    else:
        c_fields = set(re.findall(r"\b(?:int32_t|uint64_t|uint32_t|double|float)\s+(\w+)\s*;",
                                  m_probe_struct.group(1)))
        # ② iOS：`d.<field> = ...`（cValue 内的赋值语句）。
        ios_core = strip_comments(read(IOS_CORE_SWIFT))
        m_ios_val = re.search(r"var\s+cValue\s*:\s*recon_probe_data\s*\{(.*?)\n\s*\}",
                              ios_core, re.S)
        ios_fields = (set(re.findall(r"\bd\.(\w+)\s*=", m_ios_val.group(1)))
                      if m_ios_val else set())
        # ③ Android：JNI 实现里 `d.<field> = ...`。
        kt_native = strip_comments(read(ANDROID_NATIVE_CPP))
        m_jni_fn = re.search(
            r"Java_com_recon_scan_NativeBridge_setProbeData\s*\((.*?)\)\s*\{(.*?)\n\}",
            kt_native, re.S)
        kt_fields = (set(re.findall(r"\bd\.(\w+)\s*=", m_jni_fn.group(2)))
                     if m_jni_fn else set())

        if not ios_fields:
            failures.append(
                "iOS 侧未找到 `ReconProbeData.cValue` 的字段赋值 —— 能力事实上报"
                "在 iOS 侧缺失或已改名（缺口 AC 会以「iOS 读到全零」的形态复发）")
        if not kt_fields:
            failures.append(
                "Android 侧未找到 `setProbeData` 的字段赋值 —— 能力事实上报"
                "在 Android 侧缺失或已改名（缺口 AC 会以「Android 读到全零」的形态复发）")

        ios_missing = sorted(c_fields - ios_fields)
        kt_missing = sorted(c_fields - kt_fields)
        ios_extra = sorted(ios_fields - c_fields)
        kt_extra = sorted(kt_fields - c_fields)
        if ios_missing:
            failures.append(
                f"iOS 上报**漏字段** {ios_missing}（C ABI `recon_probe_data` 有、"
                f"`cValue` 未赋值 → 该字段恒为 0，内核读到假的「没有这个能力」）")
        if kt_missing:
            failures.append(
                f"Android 上报**漏字段** {kt_missing}（同上，Kotlin/JNI 侧未赋值）")
        if ios_extra:
            failures.append(
                f"iOS 上报**多字段** {ios_extra}（`recon_probe_data` 中不存在，"
                f"多半是 C ABI 改名后 Swift 侧没跟）")
        if kt_extra:
            failures.append(
                f"Android 上报**多字段** {kt_extra}（`recon_probe_data` 中不存在）")
        if not (ios_missing or kt_missing or ios_extra or kt_extra):
            print(f"[platform-parity] recon_probe_data 字段三端对等："
                  f"{len(c_fields)} 项 ✓（iOS {len(ios_fields)} / Android {len(kt_fields)}）")

    # --- 闸门 9-b：JNI 源文件必须注册进根 CMakeLists ---
    #
    # 只保证「声明有实现」还不够：若新桥 .cpp 未列进 `recon_platform_android`
    # 的源列表，它**根本不参与编译**，于是符号照样缺失 —— 与闸门 9 是同一个
    # 运行期故障的**上游成因**。两者必须一起查，否则会出现「闸门 9 说闭合、
    # 真机却仍崩」的假通过。
    #
    # ⚠️ 判据必须落在 **`add_library(recon_platform_android ...)` 的实参列表**上。
    #    第一版写成「文件名是否出现在整份 CMakeLists 里」→ **假闸门**：上面那段
    #    解释注释里就提到了 `imu-bridge.cpp`，于是判据恒真、负向注入不拒收。
    #    第二版改成只取 `add_library(...)` 的实参，但**仍未拒收** —— 因为那段
    #    解释注释就在 `add_library(` 与 `)` 之间（`见 imu-bridge.cpp 文件头`），
    #    于是"实参列表"里照样含文件名。
    #    根因：`strip_comments()` 只处理 `//`、`/* */`、`<!-- -->`，
    #    **不处理 CMake 的 `#` 行注释** —— 本项目在注释假阳性上已栽过多次
    #    （见本文件 §闸门 8 的同类说明）。
    #    故此处对 CMake 文本单独剥 `#` 注释后再匹配。两次假闸门均由
    #    verify_platform_parity_negative.py 的 P9-c 抓出。
    root_cmake_raw = read(REPO / "CMakeLists.txt")
    # CMake 注释：# 至行尾（本项目 CMake 中无含 `#` 的字符串字面量）。
    root_cmake = re.sub(r"#[^\n]*", "", root_cmake_raw)
    m_and_target = re.search(
        r"add_library\(\s*recon_platform_android\b(.*?)\)", root_cmake, re.S)
    android_target_sources = (m_and_target.group(1) if m_and_target
                              else "")
    unregistered = [p.name for p in sorted(android_cpp_dir.glob("*.cpp"))
                    if p.name not in android_target_sources]
    if not m_and_target:
        failures.append(
            "根 CMakeLists 里找不到 `add_library(recon_platform_android ...)` "
            "的 target 定义 —— 无法确认 JNI 源文件是否参与编译")
    elif unregistered:
        failures.append(
            f"Android JNI 源文件未注册进根 CMakeLists 的 recon_platform_android "
            f"target 源列表（不会参与编译 → 符号缺失）：{unregistered}")
    else:
        print("[platform-parity] Android JNI 源文件均已注册进根 CMakeLists ✓")

    # --- 闸门 11：导出格式三端映射对等 ---
    #
    # ⚠️ 判据必须落在**真实的枚举体与 switch 体**上，而不是「文件里出现过
    #    某个字符串」。若写成 `"stl" in text`，那么注释里提一句 stl 就能
    #    让闸门恒真 —— 本项目的 CMake 注释已经制造过两次假闸门（见闸门 9-b）。
    #    因此下面四步全部先定位结构边界（`typedef enum ... {` / `enum ... {` /
    #    `switch format {`），再在边界内取值核对。
    c_api_text = read(C_API_HEADER)
    g11_ok = True   # 门 11 的**所有**判据都过才允许打印成功行。
    # 早先版本的成功行只核对了「iOS 两侧 + Kotlin」，没把「C ABI 里有未登记
    # 格式」算进去 —— 于是出现了「同一轮里既打印『对等 ✓（C ABI 6）』又列出
    # 失败项」的自相矛盾输出（被 P11-a 负向注入抓到）。成功行必须与失败判据
    # 一一对应，否则它本身就是一条会误导人的假信息。

    # ① C ABI 枚举：`typedef enum recon_export_format { A = 0, ... }`。
    m_ef = re.search(
        r"typedef\s+enum\s+recon_export_format\s*\{(.*?)\}\s*recon_export_format\s*;",
        c_api_text, re.S)
    if not m_ef:
        failures.append(
            "闸门 11：在 recon_c_api.h 里找不到 `typedef enum recon_export_format` "
            "的枚举体 —— 导出格式契约的权威定义缺失或已改名")
        g11_ok = False
        c_ef: dict[str, int] = {}
    else:
        c_ef = {n: int(v) for n, v in
                re.findall(r"\b(RECON_EXPORT_\w+)\s*=\s*(\d+)",
                           re.sub(r"/\*.*?\*/", "", m_ef.group(1), flags=re.S))}
        # 反向：枚举体里是否有未登记的成员（新增格式忘了更新真源表）。
        truth_names = {r[1] for r in EXPORT_FORMAT_TRUTH}
        unlisted = sorted(set(c_ef) - truth_names)
        if unlisted:
            g11_ok = False
            failures.append(
                f"闸门 11：C ABI `recon_export_format` 有未登记进真源表的格式 "
                f"{unlisted} —— 新增导出格式必须同步 EXPORT_FORMAT_TRUTH、"
                f"iOS 两个文件、Kotlin 注释与 docs/01 §2.2.7")
        # 正向：真源表里的每一项都必须在 C ABI 枚举体里存在且值一致。
        for num, cname, _sw, _kt in EXPORT_FORMAT_TRUTH:
            if cname not in c_ef:
                g11_ok = False
                failures.append(
                    f"闸门 11：真源表登记的 {cname}（序号 {num}）在 C ABI "
                    f"`recon_export_format` 里不存在")
            elif c_ef[cname] != num:
                g11_ok = False
                failures.append(
                    f"闸门 11：C ABI 里 {cname} = {c_ef[cname]}，真源表为 {num}")

    # ② Swift 枚举：`public enum ExportFormat: String, CaseIterable { ... }`。
    ios_view_text = read(IOS_EXPORT_VIEW)
    m_sw = re.search(
        r"public\s+enum\s+ExportFormat\s*:\s*String\s*,\s*CaseIterable\s*\{(.*?)\n\}",
        ios_view_text, re.S)
    if not m_sw:
        failures.append(
            "闸门 11：在 ExportView.swift 里找不到 `enum ExportFormat` 的枚举体")
        g11_ok = False
        sw_cases: list[str] = []
    else:
        sw_body = strip_comments(m_sw.group(1))
        sw_cases = [c for c in re.findall(r"case\s+(\w+)", sw_body)
                    if c != "CaseIterable"]
        want_cases = [r[2] for r in EXPORT_FORMAT_TRUTH]
        if sw_cases != want_cases:
            g11_ok = False
            failures.append(
                f"闸门 11：iOS `ExportFormat` 的 case 列表与真源表**不一致**"
                f"（含顺序）：实际 {sw_cases}，期望 {want_cases}。"
                f"顺序即 C ABI 序号顺序，重排会静默改掉已发布的契约")

    # ③ Swift 显式序号映射：`switch format {` 体里的 `case .x: fmt = n`。
    ctl_text = read(IOS_SESSION_CONTROLLER)
    m_sw2 = re.search(r"let\s+fmt\s*:\s*Int32(.*?)\n\s*\}\n", ctl_text, re.S)
    if not m_sw2:
        failures.append(
            "闸门 11：在 ReconSessionController.swift 里找不到 `let fmt: Int32` "
            "的显式序号映射块 —— 若改成靠 rawValue 取序号，本闸门即失效且"
            "格式序号漂移将静默发生")
        g11_ok = False
        sw_map: dict[str, int] = {}
    else:
        sw_map = {c: int(v) for c, v in
                  re.findall(r"case\s+\.(\w+)\s*:\s*fmt\s*=\s*(\d+)",
                             strip_comments(m_sw2.group(1)))}
        want_map = {r[2]: r[0] for r in EXPORT_FORMAT_TRUTH}
        if sw_map != want_map:
            g11_ok = False
            missing = sorted(set(want_map) - set(sw_map))
            extra = sorted(set(sw_map) - set(want_map))
            wrong = sorted(f".{k}: {sw_map[k]} ≠ {want_map[k]}"
                           for k in set(sw_map) & set(want_map)
                           if sw_map[k] != want_map[k])
            failures.append(
                f"闸门 11：iOS 显式序号映射与 C ABI 不一致 —— "
                f"缺 {missing}、多 {extra}、值错 {wrong}")

    # ④ Kotlin 的**文档注释**：`0=GLB, 1=OBJ_MTL, ...`。
    #
    # Android 尚无导出 UI 层（缺口 AE），Kotlin 侧只有这份「序号↔别名」的
    # 书面约定 —— 它是 Android 侧唯一可能先于内核漂移的地方，因此必须核对。
    #
    # ⚠️ 这里**不能**用 `strip_comments()`：契约恰恰写在 `/** ... */` 里，
    #    剥掉注释就把待检对象一起删了，判据会恒报「None」。正确做法是
    #    **先定位那份文档注释块**，再在块内解析 —— 既不误伤，也不会把
    #    文件里别处的 `N=xyz` 误读成格式序号。
    kt_text = read(ANDROID_NATIVE_KT)
    kt_doc = ""
    for blk in re.findall(r"/\*\*(.*?)\*/", kt_text, re.S):
        if "recon_export_format" in blk:
            kt_doc = blk
            break
    if not kt_doc:
        failures.append(
            "闸门 11：Android NativeBridge.kt 里找不到声明 `recon_export_format` "
            "序号约定的文档注释 —— Android 侧的格式序号契约无处可查")
        g11_ok = False
        kt_pairs: dict[int, str] = {}
    else:
        kt_pairs = {int(n): alias for n, alias in
                    re.findall(r"(\d+)\s*=\s*([A-Za-z0-9_]+)", kt_doc)}
    want_kt = {r[0]: r[3] for r in EXPORT_FORMAT_TRUTH}
    kt_wrong = sorted(f"{k}: 注释为 {kt_pairs.get(k)!r}，真源为 {v!r}"
                      for k, v in want_kt.items() if kt_pairs.get(k) != v)
    if kt_wrong:
        g11_ok = False
        failures.append(
            f"闸门 11：Android NativeBridge.kt 的格式序号注释与真源不一致 —— "
            f"{kt_wrong}")

    if g11_ok:
        print(f"[platform-parity] 导出格式三端映射对等："
              f"{len(EXPORT_FORMAT_TRUTH)} 项 ✓"
              f"（C ABI {len(c_ef)} / iOS 枚举 {len(sw_cases)} / "
              f"iOS 映射 {len(sw_map)} / Android 注释 {len(kt_pairs)}）")

    # --- 汇总 ---
    print(f"[platform-parity] Android 权限 {len(android_perms)} 项 / iOS 用途键 {len(ios_keys)} 项")
    if args.verbose:
        print("    Android: " + ", ".join(sorted(android_perms)))

    if failures:
        print("\n[platform-parity] FAIL：双端功能不对等")
        for f in failures:
            print("  ✗ " + f)
        return 3

    print("[platform-parity] PASS：iOS / Android 功能对等")
    return 0


if __name__ == "__main__":
    sys.exit(main())
