// SPDX-License-Identifier: BSD-3-Clause
//
// NativeBridge.kt — JNI 桥接（Android ↔ core C ABI）。
//
// 契约：
//   · core/include/recon/recon_c_api.h —— 唯一跨语言边界（00 §7.1）。
//   · docs/11_平台权限与集成.md §3.7 —— 平台 → 核心层的字段对接契约。
//
// 设计约束：
//   1. **只经 C ABI**：本类与方法签名一一对应 recon_c_api.h 的导出函数，
//      不直接引用任何 C++ 类或容器。
//   2. **不阻塞 UI**：所有重计算（recon_run_reconstruction 等）在 native 层
//      同步阻塞，必须在后台线程/协程调用（铁律：相机帧回调不得直接调用重计算）。
//   3. **句柄不可并发**：除 recon_cancel 外，同一 context 不得跨线程并发调用。
package com.recon.scan

/**
 * core C ABI 的 Kotlin 门面。
 *
 * 所有方法均为 `external`，由 native 侧 [native-lib.cpp] 实现。
 * 返回值方括号中的数字对应 `recon_status` 枚举（docs/01 §4）。
 */
object NativeBridge {

    /** 与 core 的 [RECON_ABI_VERSION] 对应。 */
    const val EXPECTED_ABI_VERSION: Int = 1

    /** recon_status 的常用值（避免 Kotlin 侧散落魔数）。 */
    const val RECON_OK: Int = 0
    // --------------------------------------------------------------------- //
    // 版本与生命周期                                                          //
    // --------------------------------------------------------------------- //

    /** 对应 recon_abi_version()。 */
    external fun abiVersion(): Int

    /** 对应 recon_create()。返回不透明句柄；失败返回 0。 */
    external fun create(sessionDir: String, preset: Int, maxThreads: Int): Long

    /** 对应 recon_destroy()。 */
    external fun destroy(context: Long)

    // --------------------------------------------------------------------- //
    // 能力探测与参数                                                          //
    // --------------------------------------------------------------------- //

    /**
     * 对应 recon_probe_capability()。
     *
     * @return IntArray，布局见 [Capability]（下标常量见其 `fromArray`）。
     *         失败返回 null。调用方应经 [Capability.fromArray] 解包，
     *         **不要**在下标上自行数位（会与 native 侧漂移）。
     */
    external fun probeCapability(context: Long): IntArray?

    /** 对应 recon_set_param()。 */
    external fun setParam(context: Long, key: String, valueInt: Int, valueDouble: Double): Int

    /**
     * 对应 recon_get_param()。
     *
     * @return DoubleArray `[int 值, double 值]`（下标见 [ParamValue]）；
     *         句柄缺失或键不存在时返回 null（**不返回 0 冒充**）。
     */
    external fun getParam(context: Long, key: String): DoubleArray?

    /** 对应 recon_apply_preset()。 */
    external fun applyPreset(context: Long, preset: Int): Int

    /**
     * 对应 recon_export_params()。
     *
     * 返回的 JSON 已在 native 侧经 `recon_free_string` 释放，
     * Kotlin 侧拿到的是独立 String，**无需也不得**再释放。
     */
    external fun exportParams(context: Long): String?

    // --------------------------------------------------------------------- //
    // 采集                                                                    //
    // --------------------------------------------------------------------- //

    /** 对应 recon_start_session()。 */
    external fun startSession(context: Long, sessionId: String): Int

    /**
     * 对应 recon_push_frame()。
     *
     * @param rgb 可空（BGRA8）；@param depth 可空（uint16 毫米）。
     * @param imuInterleaved 布局 [ts_ns, ax, ay, az, gx, gy, gz] × imuCount。
     * @return recon_status；RECON_ERR_CAPTURE_STALLED(103) 表示队列满，由平台层决定丢帧。
     */
    external fun pushFrame(
        context: Long,
        frameId: Long,
        timestampNs: Long,
        rgb: ByteArray?, rgbWidth: Int, rgbHeight: Int, rgbStride: Int,
        depth: ShortArray?, depthWidth: Int, depthHeight: Int, depthConfidence: ByteArray?,
        depthSource: Int,
        fx: Double, fy: Double, cx: Double, cy: Double, k1: Double, k2: Double, k3: Double, p1: Double, p2: Double,
        poseR: DoubleArray?, poseT: DoubleArray?, poseTsNs: Long,
        imuInterleaved: DoubleArray?, imuCount: Int,
        exposureUs: Double, iso: Double
    ): Int

    /** 对应 recon_finish_capture()。 */
    external fun finishCapture(context: Long): Int

    /**
     * 对应 recon_get_scan_stats()。
     *
     * @return DoubleArray（下标见 [ScanStats]）；失败返回 null。
     *         「覆盖方位」的真实来源即 `azimuthCoverage` —— UI 不得自行估算。
     */
    external fun getScanStats(context: Long): DoubleArray?

    /**
     * 对应 recon_calibrate_scale()。
     *
     * @param detectedCorners 8 个角点 `(x0,y0,...,x3,y3)`，顺序为
     *        左上、右上、右下、左下（`recon_c_api.h` 明示）。
     * @return DoubleArray（下标见 [ScaleResult]）；失败返回 null。
     */
    external fun calibrateScale(
        context: Long,
        rgb: ByteArray?, rgbWidth: Int, rgbHeight: Int, rgbStride: Int,
        depth: ShortArray?, depthWidth: Int, depthHeight: Int,
        fx: Double, fy: Double, cx: Double, cy: Double,
        poseR: DoubleArray?, poseT: DoubleArray?, poseTsNs: Long,
        detectedCorners: DoubleArray, referenceType: Int,
        refWidthMm: Double, refHeightMm: Double
    ): DoubleArray?

    /**
     * 对应 recon_set_manual_scale()。
     *
     * @return DoubleArray（下标见 [ScaleResult]）；失败返回 null。
     */
    external fun setManualScale(context: Long, scale: Double): DoubleArray?

    /** 对应 recon_run_reconstruction()。阻塞直至完成；必须在后台线程调用。 */
    external fun runReconstruction(context: Long, progressCallback: ProgressCallback): Int

    /** 对应 recon_get_mesh_info()。返回 DoubleArray 或 null（布局见 [MeshInfo]）。 */
    external fun getMeshInfo(context: Long): DoubleArray?

    // --------------------------------------------------------------------- //
    // 导出与控制                                                              //
    // --------------------------------------------------------------------- //

    /** 对应 recon_export()。format: 0=GLB, 1=OBJ_MTL, 2=PLY。 */
    external fun export(context: Long, format: Int, outPath: String, applyScale: Boolean, embedTexture: Boolean): Int

    /** 对应 recon_cancel()。线程安全，可在任意线程调用。 */
    external fun cancel(context: Long): Int

    /** 对应 recon_is_cancelled()。 */
    external fun isCancelled(context: Long): Int

    /**
     * 进度回调接口。由 native 侧在重建线程回调，**注意**：实现方需自行切回主线程
     * 再更新 UI。
     */
    interface ProgressCallback {
        /** @param progress [0,1]。 */
        fun onProgress(stage: Int, progress: Double, message: String)
    }
}
