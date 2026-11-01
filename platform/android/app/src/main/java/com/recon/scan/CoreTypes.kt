// SPDX-License-Identifier: BSD-3-Clause
//
// CoreTypes.kt — Android 侧 core C ABI 的数据类型与取值表。
//
// 契约：
//   · core/include/recon/recon_c_api.h —— 唯一跨语言边界（00 §7.1）。
//   · 本文件是 iOS 侧 platform/ios/Recon/ReconCore.swift 的**对等物**
//     （其中 ReconStatus / ReconCapability / ReconMeshInfo）。
//     两端**必须**保持功能对等（铁律：双端同等对待，docs/14 §5.1）。
//     对等性由 tools/check_platform_parity.py 静态校验。
//
// 为什么把数组布局与错误码表放在这里（而不是散在 native-lib.cpp）：
//   native-lib.cpp 里的 `enum CapabilityIndex` / `MeshInfoIndex` 是**下标约定**，
//   Kotlin 侧此前只有裸 `IntArray` / `DoubleArray`，**没有任何命名索引** ——
//   调用方只能自己数第几个元素，极易与 native 侧漂移（本项目铁律：
//   「同一逻辑写两遍必然漂移」）。此处把下标提为具名常量，作为**唯一来源**；
//   native 侧的 `enum` 必须与该表逐项对应。
//
//   （对应 iOS 侧用 struct 承载、由 C 头文件天然保证布局；JNI 只能传数组，
//     故 Android 侧额外需要这层「具名下标」。这是**平台差异**，不是缺陷。）
package com.recon.scan

/**
 * core 的 `recon_status` 取值。
 *
 * 事实来源：`core/include/recon/recon_c_api.h` 的 `typedef enum recon_status`。
 * 此处与头文件**逐值一一对应**（共 47 项），由
 * `tools/check_platform_parity.py` 的 C ABI 状态码判据静态校验。
 *
 * **改本表必须同步头文件（或反之），CI 会拒收单边改动。**
 *
 * 与 iOS `ReconStatus` 的命名风格不同（Kotlin SCREAMING_SNAKE vs Swift camelCase），
 * 但**数值集合必须完全相同**；对等闸门只比较数值，不比较名字。
 */
object ReconStatus {
    const val OK = 0

    // 通用
    const val ERR_INVALID_ARGUMENT = 1
    const val ERR_OUT_OF_MEMORY = 2
    const val ERR_NOT_IMPLEMENTED = 3
    const val ERR_INTERNAL = 4
    const val ERR_CANCELLED = 5
    const val ERR_IO = 6
    const val ERR_UNSUPPORTED_PLATFORM = 7
    const val ERR_VERSION_MISMATCH = 8
    const val ERR_NO_CAPABILITY = 9

    // 采集
    const val ERR_CAPTURE_CAMERA_UNAVAILABLE = 100
    const val ERR_CAPTURE_PERMISSION_DENIED = 101
    const val ERR_CAPTURE_FRAME_DROPPED = 102
    const val ERR_CAPTURE_STALLED = 103
    const val ERR_CAPTURE_INTERRUPTED = 104

    // 同步
    const val ERR_SYNC_TIME_NONMONOTONIC = 200
    const val ERR_SYNC_CLOCK_DRIFT = 201
    const val ERR_SYNC_IMU_GAP = 202
    const val ERR_SYNC_CALIBRATION_MISSING = 203

    // VIO
    const val ERR_VIO_TRACKING_LOST = 300
    const val ERR_VIO_NOT_INITIALIZED = 301
    const val ERR_VIO_SCALE_UNOBSERVABLE = 302

    // SfM
    const val ERR_SFM_INSUFFICIENT_MATCHES = 400
    const val ERR_SFM_INITIALIZATION_FAILED = 401
    const val ERR_SFM_BUNDLE_ADJUSTMENT_FAILED = 402
    const val ERR_SFM_INSUFFICIENT_PARALLAX = 403

    // MVS
    const val ERR_MVS_NO_DEPTH = 500
    const val ERR_MVS_FILTER_EXHAUSTED = 501
    const val ERR_MVS_GPU_FAILURE = 502

    // 深度 / RGBD
    const val ERR_RGBD_NO_DEPTH = 600
    const val ERR_RGBD_DEPTH_MISALIGNED = 601
    const val ERR_RGBD_LOW_CONFIDENCE = 602

    // 网格
    const val ERR_MESH_POISSON_FAILED = 700
    const val ERR_MESH_OUT_OF_MEMORY = 701
    const val ERR_MESH_NON_MANIFOLD_LIMIT = 702

    // 后处理
    const val ERR_POST_HOLE_FILL_FAILED = 800
    const val ERR_POST_UV_FAILED = 801
    const val ERR_POST_TEXTURE_FAILED = 802

    // 标定
    const val ERR_CALIB_REFERENCE_NOT_FOUND = 900
    const val ERR_CALIB_DEGENERATE = 901
    const val ERR_CALIB_NO_REFERENCE = 902
    const val ERR_CALIB_INCONSISTENT = 903

    // 导出
    const val ERR_EXPORT_WRITE_FAILED = 1000
    const val ERR_EXPORT_VALIDATION_FAILED = 1001
    const val ERR_EXPORT_UNSUPPORTED_FORMAT = 1002

    // 内存
    const val ERR_MEMORY_LIMIT_REACHED = 1100
    const val ERR_MEMORY_BLOCK_TOO_LARGE = 1101

    /** 全部取值（供闸门与 UI 遍历；与上方常量**必须**逐项一致）。 */
    val ALL: IntArray = intArrayOf(
        OK,
        ERR_INVALID_ARGUMENT, ERR_OUT_OF_MEMORY, ERR_NOT_IMPLEMENTED, ERR_INTERNAL,
        ERR_CANCELLED, ERR_IO, ERR_UNSUPPORTED_PLATFORM, ERR_VERSION_MISMATCH,
        ERR_NO_CAPABILITY,
        ERR_CAPTURE_CAMERA_UNAVAILABLE, ERR_CAPTURE_PERMISSION_DENIED,
        ERR_CAPTURE_FRAME_DROPPED, ERR_CAPTURE_STALLED, ERR_CAPTURE_INTERRUPTED,
        ERR_SYNC_TIME_NONMONOTONIC, ERR_SYNC_CLOCK_DRIFT, ERR_SYNC_IMU_GAP,
        ERR_SYNC_CALIBRATION_MISSING,
        ERR_VIO_TRACKING_LOST, ERR_VIO_NOT_INITIALIZED, ERR_VIO_SCALE_UNOBSERVABLE,
        ERR_SFM_INSUFFICIENT_MATCHES, ERR_SFM_INITIALIZATION_FAILED,
        ERR_SFM_BUNDLE_ADJUSTMENT_FAILED, ERR_SFM_INSUFFICIENT_PARALLAX,
        ERR_MVS_NO_DEPTH, ERR_MVS_FILTER_EXHAUSTED, ERR_MVS_GPU_FAILURE,
        ERR_RGBD_NO_DEPTH, ERR_RGBD_DEPTH_MISALIGNED, ERR_RGBD_LOW_CONFIDENCE,
        ERR_MESH_POISSON_FAILED, ERR_MESH_OUT_OF_MEMORY, ERR_MESH_NON_MANIFOLD_LIMIT,
        ERR_POST_HOLE_FILL_FAILED, ERR_POST_UV_FAILED, ERR_POST_TEXTURE_FAILED,
        ERR_CALIB_REFERENCE_NOT_FOUND, ERR_CALIB_DEGENERATE, ERR_CALIB_NO_REFERENCE,
        ERR_CALIB_INCONSISTENT,
        ERR_EXPORT_WRITE_FAILED, ERR_EXPORT_VALIDATION_FAILED,
        ERR_EXPORT_UNSUPPORTED_FORMAT,
        ERR_MEMORY_LIMIT_REACHED, ERR_MEMORY_BLOCK_TOO_LARGE,
    )
}

/**
 * 管线分支（`recon_pipeline_branch`）。
 */
object PipelineBranch {
    const val RGBD = 0
    const val MONO = 1
}

/**
 * 设备能力（对应 `recon_capability`）。
 *
 * 由 [NativeBridge.probeCapability] 返回的 `IntArray` 经 [fromArray] 解包。
 * 下标与 `native-lib.cpp` 的 `enum CapabilityIndex` **必须逐项对应**。
 */
data class Capability(
    val branch: Int,
    val depthTier: Int,
    val depthSource: Int,
    val hasCamera: Boolean,
    val hasImu: Boolean,
    val hasLidar: Boolean,
    val hasTof: Boolean,
    val supportsGpu: Boolean,
    val maxImageWidth: Int,
    val maxImageHeight: Int,
    val maxFps: Int,
    val imuMaxRateHz: Int,
) {
    companion object {
        /** 与 native `kCapCount` 相同。 */
        const val COUNT = 12

        fun fromArray(a: IntArray): Capability? {
            if (a.size < COUNT) return null
            return Capability(
                branch = a[0],
                depthTier = a[1],
                depthSource = a[2],
                hasCamera = a[3] != 0,
                hasImu = a[4] != 0,
                hasLidar = a[5] != 0,
                hasTof = a[6] != 0,
                supportsGpu = a[7] != 0,
                maxImageWidth = a[8],
                maxImageHeight = a[9],
                maxFps = a[10],
                imuMaxRateHz = a[11],
            )
        }
    }
}

/**
 * 网格信息（对应 `recon_mesh_info`）。
 *
 * 由 [NativeBridge.getMeshInfo] 返回的 `DoubleArray` 经 [fromArray] 解包。
 * 下标与 `native-lib.cpp` 的 `enum MeshInfoIndex` **必须逐项对应**。
 */
data class MeshInfo(
    val vertexCount: Long,
    val triangleCount: Long,
    val textureCount: Long,
    val nonManifoldEdges: Int,
    val boundaryLoops: Int,
    val holeCount: Int,
    val holeAreaRatio: Double,
    val bboxMin: DoubleArray,
    val bboxMax: DoubleArray,
    val scaleFactor: Double,
    val meanEdgeLengthM: Double,
    val poissonDepth: Int,
    val degraded: Boolean,
) {
    companion object {
        /** 与 native `kMeshCount` 相同。 */
        const val COUNT = 17

        fun fromArray(a: DoubleArray): MeshInfo? {
            if (a.size < COUNT) return null
            return MeshInfo(
                // 顶点/三角面/纹理数在 core 内是 uint64；double 的 53 位尾数
                // 在 2^53 以内可精确表示，远超任何真实网格规模（亿级以内）。
                // 这是**JNI 复用 double 数组**的既有权衡，如实记录。
                vertexCount = a[0].toLong(),
                triangleCount = a[1].toLong(),
                textureCount = a[2].toLong(),
                nonManifoldEdges = a[3].toInt(),
                boundaryLoops = a[4].toInt(),
                holeCount = a[5].toInt(),
                holeAreaRatio = a[6],
                bboxMin = doubleArrayOf(a[7], a[8], a[9]),
                bboxMax = doubleArrayOf(a[10], a[11], a[12]),
                scaleFactor = a[13],
                meanEdgeLengthM = a[14],
                poissonDepth = a[15].toInt(),
                degraded = a[16] != 0.0,
            )
        }
    }
}

/**
 * 采集统计（对应 `recon_scan_stats`）。
 *
 * 由 [NativeBridge.getScanStats] 返回的 `DoubleArray` 经 [fromArray] 解包。
 * 下标与 `native-lib.cpp` 的 `enum StatsIndex` **必须逐项对应**。
 *
 * 「覆盖方位」的真实来源即 [azimuthCoverage] —— UI **不得**自行估算
 * （对应 iOS `ReconScanStats`）。
 */
data class ScanStats(
    val framesCaptured: Long,
    val framesAccepted: Long,
    val keyframes: Long,
    val imuSamples: Long,
    val durationS: Double,
    val azimuthCoverage: Double,
    val elevationCoverage: Double,
    val anomalyBlur: Int,
    val anomalyFastMotion: Int,
    val anomalyLowOverlap: Int,
    val anomalySpecular: Int,
    val anomalyLowTexture: Int,
    val anomalyBlindSpot: Int,
) {
    companion object {
        /** 与 native `kStatsCount` 相同。 */
        const val COUNT = 13

        fun fromArray(a: DoubleArray): ScanStats? {
            if (a.size < COUNT) return null
            return ScanStats(
                framesCaptured = a[0].toLong(),
                framesAccepted = a[1].toLong(),
                keyframes = a[2].toLong(),
                imuSamples = a[3].toLong(),
                durationS = a[4],
                azimuthCoverage = a[5],
                elevationCoverage = a[6],
                anomalyBlur = a[7].toInt(),
                anomalyFastMotion = a[8].toInt(),
                anomalyLowOverlap = a[9].toInt(),
                anomalySpecular = a[10].toInt(),
                anomalyLowTexture = a[11].toInt(),
                anomalyBlindSpot = a[12].toInt(),
            )
        }
    }
}

/**
 * 尺度标定结果（对应 `recon_scale_result`）。
 *
 * 由 [NativeBridge.calibrateScale] / [NativeBridge.setManualScale] 返回的
 * `DoubleArray` 经 [fromArray] 解包。下标与 `native-lib.cpp` 的
 * `enum ScaleResultIndex` **必须逐项对应**（对应 iOS `ReconScaleResult`）。
 */
data class ScaleResult(
    val ok: Boolean,
    val scale: Double,
    /** 相对不确定度 σ_s/s。§3.9 置信度：≤0.02 高 / ≤0.05 中 / 其余低。 */
    val relativeSigma: Double,
    val referenceType: Int,
    val referenceWidthMm: Double,
    val referenceHeightMm: Double,
    val measuredWidthMm: Double,
    val measuredHeightMm: Double,
) {
    companion object {
        /** 与 native `kScaleCount` 相同。 */
        const val COUNT = 8

        fun fromArray(a: DoubleArray): ScaleResult? {
            if (a.size < COUNT) return null
            return ScaleResult(
                ok = a[0] != 0.0,
                scale = a[1],
                relativeSigma = a[2],
                referenceType = a[3].toInt(),
                referenceWidthMm = a[4],
                referenceHeightMm = a[5],
                measuredWidthMm = a[6],
                measuredHeightMm = a[7],
            )
        }
    }
}

/**
 * 参数读取结果（对应 `recon_get_param` 的两个出参）。
 *
 * 由 [NativeBridge.getParam] 返回的 `DoubleArray` 经 [fromArray] 解包。
 * 下标与 `native-lib.cpp` 的 `enum ParamValueIndex` **必须逐项对应**。
 */
data class ParamValue(
    val intValue: Int,
    val doubleValue: Double,
) {
    companion object {
        /** 与 native `kParamCount` 相同。 */
        const val COUNT = 2

        fun fromArray(a: DoubleArray): ParamValue? {
            if (a.size < COUNT) return null
            return ParamValue(intValue = a[0].toInt(), doubleValue = a[1])
        }
    }
}
