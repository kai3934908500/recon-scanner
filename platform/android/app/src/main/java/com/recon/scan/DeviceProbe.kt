// SPDX-License-Identifier: BSD-3-Clause
//
// DeviceProbe.kt — Android 侧设备能力**事实**探测与上报（缺口 AC）。
//
// 契约：
//   · core/include/recon/recon_c_api.h 的 `recon_probe_data` / `recon_set_probe_data`
//   · docs/11_平台权限与集成.md §3.4（Camera2）、§3.5（ARCore）、§3.6（SensorManager）、
//     §3.7（平台 → 核心层对接表）
//   · docs/00_架构总纲.md §5.2（分支决策表）、§8.2（深度层级与可信度）
//
// 本文件是 iOS 侧 `ARCapability.probeData()`（`ui/ios/Recon/Sources/Platform/
// CameraCapture.swift`）的**对等物**。两端必须上报同一组字段，由
// `tools/check_platform_parity.py` 的字段对等判据静态校验。
//
// ── 为什么必须有这一步（缺口 AC）──────────────────────────────────────────
//
// 在此之前，平台层从未把设备事实送进内核：能注册提供者的
// `SetPlatformProbeProvider` 声明在 core 内部头 `recon/context.h`，而平台层的
// 架构铁律是「只允许 include `recon_c_api.h`」（本文件所在目录的
// `native-lib.cpp:6` 明文，G7-b 亦守）。两条约束叠加 → **双端都注册不了** →
// `ProbePlatform()` 恒返回全零 → `has_camera=0` + `depth_tier=NONE` +
// `branch=MONO` → 真机一进扫描就被判成 T-03「相机不可用」→ `Failed`。
//
// ── 探测期**必须不申请权限、不打开相机**（docs/11 §1.3）──────────────────
//
// 这与「尽量多报事实」有张力，处置如下（**逐字段诚实，不填补占位值**）：
//
// | 字段 | 来源 | 探测期可取？ |
// |---|---|---|
// | `hasCamera` | `PackageManager.FEATURE_CAMERA_ANY` | ✓ 不需要权限 |
// | `hasImu` | `SensorManager` 的 GYROSCOPE / ACCELEROMETER 非空 | ✓ |
// | `hasTof` | `PackageManager.FEATURE_CAMERA_DEPTH` | ✓ 硬件特性声明 |
// | `hasLidar` | `false`（常量） | ✓ Android 无 LiDAR 类硬件 |
// | `hasAlgoDepth` | — | ✗ **见下方「已知缺口」** |
// | `supportsGpu` | `ActivityManager.deviceConfigurationInfo.reqGlEsVersion` | ✓ |
// | `maxImageWidth/Height/maxFps` | `CameraManager.getCameraCharacteristics` 的 `SCALER_STREAM_CONFIGURATION_MAP` | ✓ **不需要打开相机**（Camera2 允许只读特性） |
// | `imuMaxRateHz` | `Sensor.getMaxDelay()` 取倒数 | ✓ |
// | `total/availableMemoryBytes` | `ActivityManager.MemoryInfo` | ✓ |
//
// ── 已知缺口（**登记，不掩盖**）：`hasAlgoDepth` 在探测期不可判定 ──────────
//
// ARCore 运动推断深度（`ALGO_DEPTH`）的能力判定 API 是
// `Session.isDepthModeSupported(Config.DepthMode.AUTOMATIC)` —— 它**需要一个
// 已构造的 `Session` 实例**。而：
//   · 构造 `Session` 会**占用相机**（docs/11 §3.5 的 `updateMode` 行）；
//   · docs/11 §3.5 的 **AND-ARC-1 明文「ARCore 与 Camera2 互斥，主路径不启动
//     ARCore」**；
//   · 能力探测又必须发生在任何采集之前（§1.3）。
//
// 三者叠加的结果：**探测期拿不到 ARCore 深度事实**。故此处报 `false`
// 并在 [`CoreTypes.ProbeData`] 的 `hasAlgoDepth` 上留痕。
//
// 影响与后续（**不在此处偷填 true 凑出 RGBD 分支**）：
//   · 具备 ARCore 运动深度但无硬件 ToF 的机型，本次会得出 `depth_tier=NONE`
//     → `branch=MONO`。这是**诚实的降级**，不是错误结果 —— 内核据实判定。
//   · 真正的修复点是 `docs/11` 需要新增一节「ARCore 深度能力在探测期的判定
//     时机」（或在 ARCore 会话建立后**再次上报** —— `recon_set_probe_data`
//     幂等，但 `Context::ProbeCapability()` 有一次性缓存，
//     故二次上报必须配 `recon_create` 之后的全新 context 才生效）。
//     该规格缺口**登记在案**，不擅自裁决。
//
// ── 本机无法验证（如实声明）──────────────────────────────────────────────
//
// 开发机为 Windows，**无 JDK / Android SDK / NDK**，故本文件的正确性由静态
// 闸门（`tools/check_android_contract.py` 的字段对等判据）保证，**不是编译
// 验证**。真机运行须在具备 Android 工具链的机器上做。

package com.recon.scan

import android.app.ActivityManager
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.hardware.Sensor
import android.hardware.SensorManager
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.util.Size

/**
 * 读取设备能力**事实**并上报内核（缺口 AC）。
 *
 * 全流程**只读**：不申请权限、不打开相机、不构造任何会话。任何需要打开设备才能
 * 取到的量，宁可不报（保持 0）也不填占位值 —— 详见文件头「已知缺口」。
 */
object DeviceProbe {

    /**
     * 探测并上报。
     *
     * ⚠️ **必须在首次 `NativeBridge.probeCapability()` 之前调用**（顺序不可交换）：
     * core 的 `Context::ProbeCapability()` 有一次性缓存（`capability_probed_`，
     * `core/src/util/context.cpp:108`），第一次调用时读到的平台事实会被固化。
     *
     * @return 内核的 `recon_status`；非 [ReconStatus.OK] 表示上报失败。
     *         **不吞错**：调用方应把它记入日志。
     */
    fun report(context: Context): Int = probe(context).report()

    /** 只探测、不上报（便于单测与「打印将上报什么」）。 */
    fun probe(context: Context): ProbeData {
        val pm = context.packageManager
        val sensors = context.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
        val am = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager
        val cm = context.getSystemService(Context.CAMERA_SERVICE) as? CameraManager

        // ── 相机存在性 ────────────────────────────────────────────────────
        // FEATURE_CAMERA_ANY = 「本机有任一相机」（FEATURE_CAMERA 特指后置）。
        // 用 ANY 是因为本产品只用后置，但「后置」应由 Camera2 的
        // LENS_FACING 决定，而不是靠清单特性缺失来推断。
        val hasCamera = pm.hasSystemFeature(PackageManager.FEATURE_CAMERA_ANY)

        // ── IMU 能力 ──────────────────────────────────────────────────────
        // docs/11 §3.6：预积分输入是「陀螺 + 加计」组合（AND-IMU-1 明确不用
        // 厂商融合的 ROTATION_VECTOR）。任一可用即具备 IMU 能力。
        val hasGyro = sensors?.getDefaultSensor(Sensor.TYPE_GYROSCOPE) != null
        val hasAccel = sensors?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) != null
        val hasImu = hasGyro || hasAccel

        // ── 硬件深度（ToF / 结构光）──────────────────────────────────────
        // FEATURE_CAMERA_DEPTH 表示设备提供深度输出（docs/11 §3.4 提到的
        // DEPTH_OUTPUT 能力）。这是**硬件**事实，与 ARCore 的软件深度无关。
        val hasTof = pm.hasSystemFeature(PackageManager.FEATURE_CAMERA_DEPTH)

        // ── GPU ──────────────────────────────────────────────────────────
        // 清单已 `uses-feature glEsVersion=0x30000 required=true`，故此项
        // 实际上恒真；仍**实测**而非直接写 true —— 清单声明是「安装门槛」，
        // 而这里要报的是「运行期事实」。0x30000 = OpenGL ES 3.0。
        val gles = am?.deviceConfigurationInfo?.reqGlEsVersion ?: 0
        val supportsGpu = gles >= 0x30000

        // ── 相机格式上限（**不需要打开相机**）──────────────────────────────
        // Camera2 允许在未打开设备时读 `CameraCharacteristics`（只需设备 id，
        // 不需 CAMERA 权限）。取后置相机的最大 JPEG/PRIVATE 输出尺寸与
        // 该尺寸下的最小帧间隔 → 上限帧率。
        val (maxW, maxH, maxFps) = cameraFormatCap(cm)

        // ── IMU 硬件上限 ──────────────────────────────────────────────────
        // `Sensor.getMaxDelay()` 是**最大采样间隔（微秒）**，其倒数为最大速率。
        // 取陀螺的上限（docs/11 §3.6：陀螺才是预积分输入，与 iOS 侧
        // IOS-IMU-2「以 CMGyro 上限为准」口径一致）。
        var imuMaxRate = 0
        val gyro = sensors?.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
        if (gyro != null) {
            val maxDelayUs = gyro.maxDelay
            if (maxDelayUs > 0) {
                // 微秒 → Hz；钳到 Int 域防御异常驱动返回值。
                val rate = 1_000_000L / maxDelayUs
                if (rate in 1..Int.MAX_VALUE.toLong()) imuMaxRate = rate.toInt()
            }
        }

        // ── 内存 ──────────────────────────────────────────────────────────
        // `MemoryInfo.totalMem` = 设备物理内存；`availMem` = 系统当前可回收
        // 之外还可用给新分配的量（含可回收缓存）。
        var totalMem = 0L
        var availMem = 0L
        if (am != null) {
            val mi = ActivityManager.MemoryInfo()
            am.getMemoryInfo(mi)
            totalMem = mi.totalMem
            availMem = mi.availMem
        }

        return ProbeData(
            hasCamera = hasCamera,
            hasImu = hasImu,
            // Android 无 LiDAR 类硬件（LiDAR 是 Apple 的命名）；写 false 是事实。
            hasLidar = false,
            hasTof = hasTof,
            // ⚠️ 探测期不可判定，见文件头「已知缺口」。**不填 true 凑分支。**
            hasAlgoDepth = false,
            supportsGpu = supportsGpu,
            maxImageWidth = maxW,
            maxImageHeight = maxH,
            maxFps = maxFps,
            imuMaxRateHz = imuMaxRate,
            totalMemoryBytes = totalMem,
            availableMemoryBytes = availMem,
        )
    }

    /**
     * 从 Camera2 特性读取「最大分辨率 + 该尺寸上限帧率」。
     *
     * 返回 `(0, 0, 0)` 表示读不到（无相机 / 无特性 / 驱动异常）——
     * 调用方据实上报 0，**不回落默认值**（编造分辨率比留 0 更坏：
     * 它会让 U-02 显示用户设备上并不存在的规格）。
     */
    private fun cameraFormatCap(cm: CameraManager?): Triple<Int, Int, Int> {
        if (cm == null) return Triple(0, 0, 0)
        val ids = try {
            cm.cameraIdList
        } catch (e: Exception) {
            // CameraAccessException 等：探测期失败即如实留 0，不中断整个探测。
            return Triple(0, 0, 0)
        }

        var bestW = 0
        var bestH = 0
        var bestFps = 0
        for (id in ids) {
            val chars = try {
                cm.getCameraCharacteristics(id)
            } catch (e: Exception) {
                continue
            }
            // 只看后置相机（本产品采集后置视角）。
            val facing = chars.get(CameraCharacteristics.LENS_FACING)
            if (facing != null && facing != CameraCharacteristics.LENS_FACING_BACK) continue

            val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: continue
            // 用 JPEG 尺寸集合代表「相机能出的最大分辨率」；不取 PRIVATE，
            // 因为 PRIVATE 集合可能包含仅供 GPU 的非常规尺寸。
            val sizes: Array<Size> = try {
                map.getOutputSizes(ImageFormat.JPEG) ?: emptyArray()
            } catch (e: Exception) {
                emptyArray()
            }
            // 用 YUV_420_888 代表采集路径的真实格式（docs/11 §3.4.1：
            // 采集走 ImageReader 的 YUV_420_888）。帧率上限按它查更准。
            val yuvSizes: Array<Size> = try {
                map.getOutputSizes(ImageFormat.YUV_420_888) ?: emptyArray()
            } catch (e: Exception) {
                emptyArray()
            }

            for (s in sizes) {
                val px = s.width.toLong() * s.height.toLong()
                if (px <= bestW.toLong() * bestH.toLong()) continue
                bestW = s.width
                bestH = s.height
                // 在 YUV 路径上查该尺寸的最小帧间隔 → 上限帧率。
                // `getOutputMinFrameDuration` 需要 Surface 标记为 PRIVATE 时也
                // 成立；用 ImageFormat 重载（API 23+，minSdk 24 满足）。
                bestFps = fpsFor(map, ImageFormat.YUV_420_888, s, yuvSizes) ?: bestFps
            }
        }
        return Triple(bestW, bestH, bestFps)
    }

    /**
     * 查某尺寸下的上限帧率（帧）。
     *
     * `getOutputMinFrameDuration` 返回**最小帧间隔（纳秒）**，0 表示不受限
     * （此时无法据此推出上限，返回 null 让调用方保留已知值）。
     */
    private fun fpsFor(
        map: android.hardware.camera2.params.StreamConfigurationMap,
        format: Int,
        size: Size,
        candidates: Array<Size>,
    ): Int? {
        // 仅当该格式确实支持这个尺寸时才查（否则 getOutputMinFrameDuration
        // 的行为未定义）。
        if (candidates.none { it.width == size.width && it.height == size.height }) {
            return null
        }
        val dur = try {
            map.getOutputMinFrameDuration(format, size)
        } catch (e: Exception) {
            return null
        }
        if (dur <= 0L) return null
        // 纳秒 → Hz。上限通常 ≤ 240，钳位只为防御异常驱动返回值。
        val fps = 1_000_000_000L / dur
        if (fps in 1..Int.MAX_VALUE.toLong()) return fps.toInt()
        return null
    }

    /** 供将来 ARCore 路径使用：判断本机是否声明支持 ARCore（本地查询，不下载）。 */
    @Suppress("unused")
    private fun hasArCore(pm: PackageManager): Boolean =
        pm.hasSystemFeature("com.google.ar.core")
}
