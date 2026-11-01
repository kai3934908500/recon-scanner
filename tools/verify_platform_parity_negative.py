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
  闸门 8 recon_free_buffer 豁免有效性
      · 给头文件加一个返回缓冲区的 out 参数 → 闸门须报「豁免已过期」
  闸门 8 平台无关逻辑单实现（第三十五轮新增）
      · 在 iOS 的 IMUWindow.swift 重写一份切窗循环 → 闸门须报「疑似重写 IMU 切窗」
      · 在 Android 的 CaptureActivity.kt 重写一份深度换算 → 须报「疑似重写深度
        单位换算」
  闸门 9 JNI 声明↔实现闭合（第三十六轮新增）
      · 把 JNI 导出名改成与 Kotlin `external fun` 不一致 → 须报「无对应 JNI 实现」
      · 把 JNI 导出名改名而 Kotlin 不同步 → 须报「未声明」（反向漏网）
      · 把 imu-bridge.cpp 从根 CMakeLists 的 target 源列表移除 → 须报「未注册进
        根 CMakeLists」（闸门 9 的上游成因，不查它会「闸门 9 说闭合但真机仍崩」）

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
        "P8-a-exempt-stale",
        "给头文件加一个返回缓冲区的 out 参数 → free_buffer 豁免过期须报错",
        "豁免已过期",
        [(C_API_HEADER,
          "RECON_API recon_status recon_get_mesh_info(void* context,\n"
          "                                           recon_mesh_info* out_info);",
          "RECON_API recon_status recon_get_mesh_info(void* context,\n"
          "                                           recon_mesh_info* out_info);\n"
          "RECON_API recon_status recon_get_preview_blob(void* context,\n"
          "                                               uint8_t** out_buffer);")],
    ),
]


def main() -> int:
    for p in (GATE, CORE_TYPES, NATIVE_CPP, IOS_CORE_SWIFT, C_API_HEADER,
              IOS_IMU_WINDOW, ANDROID_CAPTURE_ACTIVITY, ANDROID_IMU_BRIDGE,
              ROOT_CMAKE, IOS_METAL_BACKEND):
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
          f"（闸门 5 / 6 / 7 / 8 / 9 均有效）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
