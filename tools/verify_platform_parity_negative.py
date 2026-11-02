#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_platform_parity_negative.py — check_platform_parity.py 的负向注入自检。

为什么需要这个文件
------------------
见 .workbuddy/memory/MEMORY.md：「恒不触发」的判据等于没有判据（假闸门）。
每写一条闸门判据，必须做一次负向注入证明它真的会拒收。

本脚本针对 `tools/check_platform_parity.py` 中**新增的**两类判据：

  闸门 5 recon_status 数值集合对等（Kotlin ↔ C ABI）
      · 从 Kotlin 删一个 const   → 闸门须报「C ABI 有但 Kotlin 缺」
      · 给 Kotlin 加一个 C ABI 没有的值 → 闸门须报「引用了不存在的状态码」
      · 从 ALL 数组删一项（但 const 仍在）→ 闸门须报「ALL 漏列」
  闸门 6 JNI 数组布局条数对等（Kotlin COUNT ↔ native 枚举）
      · 改 Capability.COUNT     → 闸门须报「Capability 数组条数不对等」
      · 改 native kMeshCount 条数 → 闸门须报「MeshInfo 数组条数不对等」
      · 改 ScanStats / ScaleResult / ParamValue 条数 → 须报对应不对等
  闸门 7 C ABI 声明覆盖无悬空（防「两端一起漏」）
      · 两端同时删掉 recon_get_param 的调用 → 闸门须报「悬空声明」
        （若只有闸门 4，两端一致地漏实现会**静默通过** —— 这正是本判据要堵的洞）
      · 两端同时删掉 recon_free_buffer 的调用 → 须报「悬空声明」
        （2026-11-02 起 free_buffer 有真实生产者 recon_get_preview_mesh，
         豁免已删除；该注入证明「两端一起漏释放」会被拦下）
  闸门 7b recon_free_buffer 豁免失效条件（2026-11-02 起豁免为空）
      · 给头文件加一个返回缓冲区的 out 参数 → 闸门须报「豁免已过期」
        ⚠️ 该注入在**豁免非空时**才可能触发；豁免已删后，此形态改由
        闸门 7 的悬空声明覆盖（见上一条）。本注入保留并**改为断言**：
        头文件出现缓冲区出口且平台层未调用时，闸门必须失败 —— 否则报假判据。
  闸门 8 平台无关逻辑单实现（第三十五轮新增）
      · 在 iOS 的 IMUWindow.swift 重写一份切窗循环 → 闸门须报「疑似重写 IMU 切窗」
      · 在 Android 的 CaptureActivity.kt 重写一份深度换算 → 须报「疑似重写深度
        单位换算」
  闸门 9 JNI 声明↔实现闭合（第三十六轮新增）
      · 把 JNI 导出名改成与 Kotlin `external fun` 不一致 → 须报「无对应 JNI 实现」
      · 把 JNI 导出名改名而 Kotlin 不同步 → 须报「未声明」（反向漏网）
      · 把 imu-bridge.cpp 从根 CMakeLists 的 target 源列表移除 → 须报「未注册进
        根 CMakeLists」（闸门 9 的上游成因，不查它会「闸门 9 说闭合但真机仍崩」）
  闸门 10 能力事实上报的**字段级**三端对等（第四十轮新增，缺口 AC）
      · iOS `cValue` 漏一个字段 → 须报「iOS 上报**漏字段**」
      · Android JNI 实现漏一个字段 → 须报「Android 上报**漏字段**」
      · 两端**同时**漏同一字段 → 仍须报（闸门 4 只查「函数被调过」，对
        「两端一起漏」完全无覆盖 —— 这正是本闸门存在的理由）
      · iOS 上报一个 C ABI 不存在的字段 → 须报「iOS 上报**多字段**」

⚠️ 本脚本会**临时修改** `platform/android/.../CoreTypes.kt`、
   `platform/android/.../cpp/native-lib.cpp`、`platform/ios/Recon/ReconCore.swift`、
   `ui/ios/Recon/Sources/Platform/IMUWindow.swift`、
   `platform/android/.../CaptureActivity.kt`、
   `platform/android/.../cpp/imu-bridge.cpp`、`CMakeLists.txt`
   与 `core/include/recon/recon_c_api.h` 再还原。安全措施同
   verify_ios_contract_negative.py。

用法
----
    python tools/verify_platform_parity_negative.py

退出码
------
  0 = 全部注入都被正确拒收（闸门有效）
  1 = 环境/用法错误
  3 = 有注入**未被拒收** → 存在假判据
"""

from __future__ import annotations

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GATE = os.path.join(ROOT, "tools", "check_platform_parity.py")
CORE_TYPES = os.path.join(ROOT, "platform", "android", "app", "src", "main",
                          "java", "com", "recon", "scan", "CoreTypes.kt")
NATIVE_CPP = os.path.join(ROOT, "platform", "android", "app", "src", "main",
                          "cpp", "native-lib.cpp")
IOS_CORE_SWIFT = os.path.join(ROOT, "platform", "ios", "Recon", "ReconCore.swift")
# 闸门 8 注入目标：iOS 的 IMU 门面 与 Android 的采集 Activity（实现侧源码）。
IOS_IMU_WINDOW = os.path.join(ROOT, "ui", "ios", "Recon", "Sources", "Platform",
                              "IMUWindow.swift")
ANDROID_CAPTURE_ACTIVITY = os.path.join(
    ROOT, "platform", "android", "app", "src", "main", "java", "com", "recon",
    "scan", "CaptureActivity.kt")
# 闸门 9 注入目标：Android 的 IMU JNI 桥 与 仓库根 CMakeLists。
ANDROID_IMU_BRIDGE = os.path.join(ROOT, "platform", "android", "app", "src",
                                  "main", "cpp", "imu-bridge.cpp")
ROOT_CMAKE = os.path.join(ROOT, "CMakeLists.txt")
C_API_HEADER = os.path.join(ROOT, "core", "include", "recon", "recon_c_api.h")
# 闸门 8（第三十七轮扩展）注入目标：iOS 桥接层的 YUV 转换实现。
# 色彩数学最可能被内联的地方正是宿主/桥接层（"就几行，直接写这里算了"），
# 故判据必须覆盖 .mm —— 只扫 Swift/Kotlin 会漏掉真正的目标对象。
IOS_METAL_BACKEND = os.path.join(ROOT, "platform", "ios", "Recon",
                                 "MetalComputeBackend.mm")


# 闸门 11 注入目标：导出格式三端映射（C ABI / iOS 枚举 / iOS 映射 / Kotlin 注释）。
IOS_EXPORT_VIEW = os.path.join(ROOT, "ui", "ios", "Recon", "Sources", "Screens",
                               "ExportView.swift")
IOS_SESSION_CONTROLLER = os.path.join(ROOT, "ui", "ios", "Recon", "Sources",
                                      "State", "ReconSessionController.swift")
ANDROID_NATIVE_KT = os.path.join(ROOT, "platform", "android", "app", "src",
                                 "main", "java", "com", "recon", "scan",
                                 "NativeBridge.kt")


def slurp(path: str) -> str:
    with open(path, "r", encoding="utf-8") as fh:
        return fh.read()


def spit(path: str, text: str) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def run_gate() -> tuple[int, str]:
    proc = subprocess.run([sys.executable, GATE], cwd=ROOT,
                          capture_output=True, text=True)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


INJECTIONS: list[tuple[str, str, str, list[tuple[str, str, str]]]] = [
    (
        "P8-a-ios-imu-reimpl",
        "在 iOS Swift 里重写一份 IMU 切窗（循环条件含 timestamp 比较）",
        "疑似重写 IMU 切窗",
        [(IOS_IMU_WINDOW,
          "        impl.window(beforeNs: Int64(timestamp * 1_000_000_000))\n"
          "    }\n",
          "        // 注入：重写一份切窗规则\n"
          "        var split = 0\n"
          "        while split < 0 && sampleTs[split] <= timestamp { split += 1 }\n"
          "        impl.window(beforeNs: Int64(timestamp * 1_000_000_000))\n"
          "    }\n")],
    ),
    (
        "P8-b-android-depth-reimpl",
        "在 Android Kotlin 里重写一份深度换算（×1000 / 65535 夹取）",
        "疑似重写深度单位换算",
        [(ANDROID_CAPTURE_ACTIVITY,
          "package com.recon.scan\n",
          "package com.recon.scan\n\n"
          "// 注入：重写一份深度单位换算规则（应被闸门 8 拒收）\n"
          "private const val DEPTH_MAX_MM = 65535\n"
          "private fun toMillimeters(m: Float): Int = (m * 1000.0f + 0.5f).toInt()\n")],
    ),
    # --- 闸门 8（第三十七轮扩展）：YUV 色彩数学也必须在 platform/common 单实现 --
    #     新增色度/亮度归一化与 BT.601/709 矩阵增益的数值指纹判据，并把扫描范围
    #     扩到 iOS 桥接层的 `.mm`（色彩数学最可能被内联进宿主代码）。
    #     若判据只落在「文件名/关键词」，改名即可绕过 —— 故用高特异度数值。
    (
        "P8-c-yuv-color-math-inlined",
        "在 iOS 桥接层 .mm 里内联 BT.601 色彩增益（1.402 / 1.772）",
        "疑似重写 YUV→RGB 色彩数学",
        [(IOS_METAL_BACKEND,
          "#import \"MetalComputeBackend.h\"\n",
          "#import \"MetalComputeBackend.h\"\n\n"
          "// 注入：把规则本体的系数内联进宿主层（应被闸门 8 拒收）\n"
          "static const float kNegInjectRFromCr = 1.402f;\n"
          "static const float kNegInjectBFromCb = 1.772f;\n")],
    ),
    (
        "P8-d-yuv-color-math-swift",
        "在 iOS Swift 侧重写一份 BT.709 色彩增益（1.5748 / 1.8556）",
        "疑似重写 YUV→RGB 色彩数学",
        [(IOS_IMU_WINDOW,
          "import Foundation\n",
          "import Foundation\n\n"
          "// 注入：重写一份 BT.709 色彩增益（应被闸门 8 拒收）\n"
          "let negInjectRFromCr: Double = 1.5748\n"
          "let negInjectBFromCb: Double = 1.8556\n")],
    ),
    (
        "P5-a-kotlin-missing",
        "Kotlin 侧删一个错误码 const（C ABI 有而 Kotlin 缺）",
        "但 Kotlin CoreTypes 缺",
        [(CORE_TYPES,
          "    const val ERR_MESH_POISSON_FAILED = 700\n",
          "")],
    ),
    (
        "P5-b-kotlin-extra",
        "Kotlin 侧加一个 C ABI 没有的状态码",
        "引用了不存在的状态码",
        [(CORE_TYPES,
          "    const val ERR_EXPORT_UNSUPPORTED_FORMAT = 1002\n",
          "    const val ERR_EXPORT_UNSUPPORTED_FORMAT = 1002\n"
          "    const val ERR_BOGUS = 9999\n")],
    ),
    (
        "P5-c-all-missing",
        "从 ALL 数组删一项（const 仍在）→ ALL 漏列",
        "ALL 漏列",
        [(CORE_TYPES,
          "        ERR_MEMORY_LIMIT_REACHED, ERR_MEMORY_BLOCK_TOO_LARGE,\n",
          "        ERR_MEMORY_LIMIT_REACHED,\n")],
    ),
    (
        "P6-a-cap-count",
        "改 Kotlin Capability.COUNT（与 native kCapCount 不符）",
        "Capability 数组条数不对等",
        [(CORE_TYPES,
          "        /** 与 native `kCapCount` 相同。 */\n        const val COUNT = 12",
          "        /** 与 native `kCapCount` 相同。 */\n        const val COUNT = 11")],
    ),
    (
        "P6-b-mesh-native",
        "改 native 侧 MeshInfo 枚举条数（删 kMeshDegraded）",
        "MeshInfo 数组条数不对等",
        [(NATIVE_CPP,
          "  kMeshPoissonDepth, kMeshDegraded, kMeshCount\n",
          "  kMeshPoissonDepth, kMeshCount\n")],
    ),
    (
        "P6-c-stats-count",
        "改 Kotlin ScanStats.COUNT（与 native kStatsCount 不符）",
        "ScanStats 数组条数不对等",
        [(CORE_TYPES,
          "        /** 与 native `kStatsCount` 相同。 */\n        const val COUNT = 13",
          "        /** 与 native `kStatsCount` 相同。 */\n        const val COUNT = 12")],
    ),
    (
        "P6-d-scale-native",
        "改 native 侧 ScaleResult 枚举条数（删 kScaleMeasuredHeightMm）",
        "ScaleResult 数组条数不对等",
        [(NATIVE_CPP,
          "  kScaleRefWidthMm, kScaleRefHeightMm, kScaleMeasuredWidthMm, "
          "kScaleMeasuredHeightMm,\n  kScaleCount\n",
          "  kScaleRefWidthMm, kScaleRefHeightMm, kScaleMeasuredWidthMm,\n"
          "  kScaleCount\n")],
    ),
    (
        "P6-e-param-count",
        "改 Kotlin ParamValue.COUNT（与 native kParamCount 不符）",
        "ParamValue 数组条数不对等",
        [(CORE_TYPES,
          "        /** 与 native `kParamCount` 相同。 */\n        const val COUNT = 2",
          "        /** 与 native `kParamCount` 相同。 */\n        const val COUNT = 3")],
    ),
    (
        "P7-a-dangling",
        "两端同时删掉一个已声明函数的调用 → 悬空声明（原闸门 4 会静默放过）",
        "悬空声明",
        # 同时从 native-lib.cpp 与 ReconCore.swift 移除 recon_get_param。
        [(NATIVE_CPP,
          "      recon_get_param(reinterpret_cast<void*>(context), k, &v_int, &v_double);",
          "      RECON_OK;  // 注入：删掉调用"),
         (IOS_CORE_SWIFT,
          "        let st = key.withCString { recon_get_param(h, $0, &vInt, &vDouble) }",
          "        let st = ReconStatus.ok.rawValue  // 注入：删掉调用")],
    ),
    (
        "P9-a-jni-symbol-mismatch",
        "JNI 导出名与 Kotlin `external fun` 不一致（少 `native` 前缀）→ 真机崩",
        "无对应 JNI 实现",
        [(ANDROID_IMU_BRIDGE,
          "Java_com_recon_scan_ImuRingBuffer_nativeAppend(",
          "Java_com_recon_scan_ImuRingBuffer_append(")],
    ),
    (
        "P9-b-jni-orphan-symbol",
        "JNI 导出名改名后 Kotlin 侧未同步 → 遗留死码符号",
        "未声明",
        [(ANDROID_IMU_BRIDGE,
          "Java_com_recon_scan_ImuRingBuffer_nativeCount(",
          "Java_com_recon_scan_ImuRingBuffer_nativeCountRenamed(")],
    ),
    (
        "P9-c-jni-source-unregistered",
        "把 imu-bridge.cpp 从根 CMakeLists 的 target 源列表移除 → 不参与编译",
        "未注册进根 CMakeLists",
        [(ROOT_CMAKE,
          "      platform/android/app/src/main/cpp/imu-bridge.cpp)",
          "      )")],
    ),
    (
        "P8-a-free-buffer-asymmetry",
        "两端释放调用不对称：删掉 iOS 侧两处 recon_free_buffer → 闸门须报"
        "「Android 调用了 iOS 未调用的 core 函数」"
        "（2026-11-02 起 free_buffer 的唯一生产者是 recon_get_preview_mesh，"
        "豁免已删除。闸门 4 会先抓到不对称 —— 证明两端**必须都释放**；"
        "原 P8-a-exempt-stale 押「豁免已过期」，在豁免为空后恒不触发，属假判据，已替换）",
        "Android 调用了 iOS 未调用的 core 函数",
        [(IOS_CORE_SWIFT,
          "                recon_free_buffer(UnsafeMutableRawPointer(p), raw.vertex_count > 0\n"
          "                    ? Int(raw.vertex_count) * 3 * MemoryLayout<Float>.size : 0)\n",
          ""),
         (IOS_CORE_SWIFT,
          "                recon_free_buffer(UnsafeMutableRawPointer(i), raw.triangle_count > 0\n"
          "                    ? Int(raw.triangle_count) * 3 * MemoryLayout<UInt32>.size : 0)\n",
          "")],
    ),
    (
        "P8-b-free-buffer-both-missing",
        "两端**同时**不释放：iOS 删两处 + Android 删一处 → 闸门须报「悬空声明」"
        "（这是闸门 7 的**独立**价值：闸门 4 只查两端是否一致，"
        "两端一致地漏掉会静默通过 —— 本注入正是为堵这个洞）",
        "悬空声明",
        [(IOS_CORE_SWIFT,
          "                recon_free_buffer(UnsafeMutableRawPointer(p), raw.vertex_count > 0\n"
          "                    ? Int(raw.vertex_count) * 3 * MemoryLayout<Float>.size : 0)\n",
          ""),
         (IOS_CORE_SWIFT,
          "                recon_free_buffer(UnsafeMutableRawPointer(i), raw.triangle_count > 0\n"
          "                    ? Int(raw.triangle_count) * 3 * MemoryLayout<UInt32>.size : 0)\n",
          ""),
         (NATIVE_CPP,
          "    recon_free_buffer(mesh.positions, static_cast<size_t>(mesh.vertex_count) * 3 * sizeof(float));\n",
          ""),
         (NATIVE_CPP,
          "    recon_free_buffer(mesh.indices, static_cast<size_t>(mesh.triangle_count) * 3 * sizeof(uint32_t));\n",
          "")],
    ),
    # --- 闸门 10：能力事实上报的**字段级**三端对等（缺口 AC，第四十轮新增）----
    #
    # 为什么必须为它写注入：闸门 4 只保证「两端都调了 recon_set_probe_data」这个
    # **动作**存在；而缺口 AC 的真实危险形态是**字段漏写** —— 一端写了、
    # 另一端忘了，两端在**同一台设备**上会得出不同的分支判定，且闸门 4 全绿。
    # 这正是 #109 式的检测盲区，必须用注入证明闸门 10 真的会拒收。
    (
        "P10-a-ios-field-missing",
        "iOS `ReconProbeData.cValue` 漏写一个字段（has_algo_depth）",
        "iOS 上报**漏字段**",
        [(IOS_CORE_SWIFT,
          "        d.has_algo_depth = hasAlgoDepth ? 1 : 0\n",
          "")],
    ),
    (
        "P10-b-android-field-missing",
        "Android `setProbeData` 的 JNI 实现漏写一个字段（imu_max_rate_hz）",
        "Android 上报**漏字段**",
        [(NATIVE_CPP,
          "  d.imu_max_rate_hz = (imu_max_rate_hz > 0) ? imu_max_rate_hz : 0;\n",
          "")],
    ),
    (
        "P10-c-both-field-missing",
        "两端**同时**漏同一字段（total_memory_bytes）—— 闸门 4 抓不到的那一类",
        "漏字段",
        [(IOS_CORE_SWIFT,
          "        d.total_memory_bytes = totalMemoryBytes\n",
          ""),
         (NATIVE_CPP,
          "  d.total_memory_bytes =\n"
          "      (total_memory_bytes > 0) ? static_cast<uint64_t>(total_memory_bytes) : 0u;\n",
          "")],
    ),
    (
        "P10-d-ios-extra-field",
        "iOS 上报一个 `recon_probe_data` 不存在的字段（C ABI 改名后 Swift 没跟）",
        "iOS 上报**多字段**",
        [(IOS_CORE_SWIFT,
          "        d.has_algo_depth = hasAlgoDepth ? 1 : 0\n",
          "        d.has_algo_depth = hasAlgoDepth ? 1 : 0\n"
          "        d.has_neg_inject_bogus = 1\n")],
    ),
    # --- 闸门 11（第四十五轮新增）：导出格式三端映射对等 ---
    #
    # 每一条都对应一种**真实会发生的漂移**，而不是为了凑数：
    #   P11-a 加格式只改 C ABI     → 两端都漏（典型「先写内核，忘了 UI」）
    #   P11-b 两端 case 都在但顺序反 → 序号互换，导出格式错位但不报错
    #   P11-c 只改一处序号映射     → Swift 枚举有 5 项但映射少一项
    #   P11-d 只改 Kotlin 注释     → Android 侧文档先于内核漂移
    (
        "P11-a-cabi-only",
        "只在 C ABI 加一个导出格式（iOS 枚举与映射都不动）",
        "未登记进真源表的格式",
        [(C_API_HEADER,
          "    RECON_EXPORT_3MF      = 4   /**< 3MF。OPC/ZIP 容器，`unit=\"millimeter\"` */\n"
          "} recon_export_format;\n",
          "    RECON_EXPORT_3MF      = 4,  /**< 3MF。OPC/ZIP 容器，`unit=\"millimeter\"` */\n"
          "    RECON_EXPORT_USDZ     = 5   /**< 注入：未经三端同步的新格式 */\n"
          "} recon_export_format;\n")],
    ),
    (
        "P11-b-ios-case-order-swapped",
        "iOS 枚举交换 stl / threeMf 的顺序（SWIFT 侧序号与 C ABI 错位）",
        "iOS `ExportFormat` 的 case 列表与真源表**不一致**",
        [(IOS_EXPORT_VIEW,
          "    case stl       // STL（打印）\n"
          "    case threeMf   // 3MF（打印，带单位声明）\n",
          "    case threeMf   // 3MF（打印，带单位声明）\n"
          "    case stl       // STL（打印）\n")],
    ),
    (
        "P11-c-ios-map-drift",
        "iOS 枚举保留 5 个 case，但显式序号映射里少写一项（漏了 3MF）",
        "iOS 显式序号映射与 C ABI 不一致",
        [(IOS_SESSION_CONTROLLER,
          "        case .threeMf: fmt = 4\n",
          "")],
    ),
    (
        "P11-d-kotlin-doc-drift",
        "只改 Kotlin 文档注释里的某个格式序号（Android 侧约定先漂移）",
        "Android NativeBridge.kt 的格式序号注释与真源不一致",
        [(ANDROID_NATIVE_KT,
          "     * `0=GLB, 1=OBJ_MTL, 2=PLY, 3=STL, 4=3MF`。\n",
          "     * `0=GLB, 1=OBJ_MTL, 2=PLY, 3=3MF, 4=STL`。\n")],
    ),
]


def main() -> int:
    for p in (GATE, CORE_TYPES, NATIVE_CPP, IOS_CORE_SWIFT, C_API_HEADER,
              IOS_IMU_WINDOW, ANDROID_CAPTURE_ACTIVITY, ANDROID_IMU_BRIDGE,
              ROOT_CMAKE, IOS_METAL_BACKEND, IOS_EXPORT_VIEW,
              IOS_SESSION_CONTROLLER, ANDROID_NATIVE_KT):
        if not os.path.isfile(p):
            print(f"[negative] 环境错误：找不到 {p}", file=sys.stderr)
            return 1

    print("=" * 68)
    print("平台对等闸门负向注入自检（tools/verify_platform_parity_negative.py）")
    print("=" * 68)

    base_rc, base_out = run_gate()
    if base_rc != 0:
        print("[negative] 基线失败：未注入任何缺陷时闸门就报错。")
        print(base_out)
        return 1
    print("[negative] 基线：闸门 PASS（未注入缺陷）✓")
    print("")

    failures: list[str] = []
    for idx, (code, desc, expect, edits) in enumerate(INJECTIONS, 1):
        originals: dict[str, str] = {}
        for path, _, _ in edits:
            if path not in originals:
                originals[path] = slurp(path)

        applied = False
        try:
            for path, old, new in edits:
                text = slurp(path)
                if old not in text:
                    failures.append(f"{code}: 注入片段在 {os.path.basename(path)} "
                                    f"中找不到，注入无效（需修正本脚本）")
                    break
                spit(path, text.replace(old, new, 1))
                applied = True

            if not applied and edits:
                continue

            rc, out = run_gate()
            if rc == 0:
                failures.append(f"{code}: **闸门未拒收**（退出码 0）→ 假闸门。"
                                f" 意图：{desc}")
                print(f"  [{idx:02d}] ✗ {code}  —— 未被拒收！（假闸门）")
            elif expect not in out:
                failures.append(f"{code}: 闸门拒收了（rc={rc}），但失败信息里"
                                f"找不到预期片段「{expect}」。实际输出见下：\n{out}")
                print(f"  [{idx:02d}] ✗ {code}  —— 拒收了但信息不符预期")
            else:
                print(f"  [{idx:02d}] ✓ {code}  —— 已拒收（rc={rc}）")
        finally:
            for path, text in originals.items():
                spit(path, text)

    rc_after, out_after = run_gate()
    print("")
    if rc_after != 0:
        failures.append("还原后闸门仍失败 —— 还原不完整！输出：\n" + out_after)
        print("[negative] ✗ 还原后闸门仍失败（还原不完整）")
    else:
        print("[negative] 还原后闸门重新 PASS ✓")

    print("")
    if failures:
        print(f"[negative] FAIL：{len(failures)} 项")
        for f in failures:
            print("  ✗ " + f)
        return 3

    print(f"[negative] PASS：{len(INJECTIONS)} 项注入全部被正确拒收"
          f"（闸门 5 / 6 / 7 / 8 / 9 / 10 / 11 均有效）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
