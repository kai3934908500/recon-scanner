// SPDX-License-Identifier: BSD-3-Clause
//
// CaptureActivity.kt — 采集主界面 + 状态机驱动（Android）
//
// 契约：docs/09_UI规格.md §4.1/§4.2（采集状态机）、
//       docs/11_平台权限与集成.md §2.3（运行时申请时序）、§4.2（前后台切换）。
//
// ── 本轮补链（缺口 AE / AV / 重建回推）──────────────────────────────────────
// 此前本类只有状态机骨架、没有 setContentView → 装上黑屏；且重建走独立
// ReconstructionService，完成后**不回推** RECONSTRUCTION_SUCCEEDED/FAILED，
// Processing 是死态。本版：
//   ① setContentView(MainUI.root) —— 真正显示界面；
//   ② 重建在 Activity 内用协程闭环，完成后显式发状态机事件（不再依赖 Service 回推）；
//   ③ 采集期轮询 recon_get_scan_stats，统计行显示真实内核计数（不再是全 0）。
//
// 线程模型（docs/11 §3.7）：native recon_* 一律 Dispatchers.IO；主界面更新回主线程。
// 状态机 CaptureEventBridge 是单实例、仅主线程调用。

package com.recon.scan

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class CaptureActivity : ComponentActivity() {

    private var captureContext: Long = 0L
    private lateinit var ui: MainUI

    /** 相机链路诊断计数：回调到达 / 成功投递内核 / 被拒（缺口 AV 同口径）。 */
    @Volatile private var arrivedFrames = 0
    @Volatile private var ingestedFrames = 0
    @Volatile private var rejectedFrames = 0

    /** 采集统计轮询与重建任务，onDestroy 统一取消。 */
    private var statsJob: Job? = null
    private var reconJob: Job? = null
    private val mainHandler = Handler(Looper.getMainLooper())

    private val cameraPermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) {
                onEvent(CaptureEventBridge.PERMISSIONS_GRANTED)
            } else {
                onEvent(CaptureEventBridge.CAMERA_PERMISSION_DENIED)
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        ui = MainUI(this) { action -> onAction(action) }
        setContentView(ui.root)

        // T-01：Idle → CapabilityProbe（经状态机），随后探测 → T-02 申请权限。
        onEvent(CaptureEventBridge.START_SCAN)
    }

    // ------------------------------------------------------------------- //
    // 按钮动作 → 状态机事件
    // ------------------------------------------------------------------- //
    private fun onAction(action: MainUI.Action) {
        when (action) {
            MainUI.Action.START -> onEvent(CaptureEventBridge.USER_START)
            MainUI.Action.FINISH -> onEvent(CaptureEventBridge.FINISH_SCAN)
            MainUI.Action.RECONSTRUCT -> onEvent(CaptureEventBridge.USER_CONFIRM_RECONSTRUCT)
            MainUI.Action.CONTINUE -> onEvent(CaptureEventBridge.USER_CONTINUE_SCAN)
            MainUI.Action.RECAPTURE -> onEvent(CaptureEventBridge.RECAPTURE)
            MainUI.Action.RETRY -> onEvent(CaptureEventBridge.RETRY)
            MainUI.Action.HOME -> onEvent(CaptureEventBridge.BACK_TO_HOME)
            MainUI.Action.EXPORT_GLB -> export(0)
            MainUI.Action.EXPORT_STL -> export(3)
        }
    }

    private fun export(format: Int) {
        lifecycleScope.launch {
            val path = filesDir.resolve("model.${if (format == 0) "glb" else "stl"}").absolutePath
            val status = withContext(Dispatchers.IO) {
                if (captureContext == 0L) ReconStatus.ERR_INVALID_ARGUMENT
                else NativeBridge.export(captureContext, format, path, true, true)
            }
            toast(if (status == ReconStatus.OK) "已导出：$path" else "导出失败 status=$status")
        }
    }

    // ------------------------------------------------------------------- //
    // 状态机事件分发
    // ------------------------------------------------------------------- //
    private fun onEvent(event: Int) {
        val next = CaptureEventBridge.apply(event)
        when (next) {
            CaptureEventBridge.STATE_CAPABILITY_PROBE -> probeCapability()
            CaptureEventBridge.STATE_REQUESTING_PERMISSION -> requestPermissions()
            CaptureEventBridge.STATE_PREVIEWING -> onPreviewing()
            CaptureEventBridge.STATE_CAPTURING -> startCapture()
            CaptureEventBridge.STATE_REVIEWING -> onReviewing()
            CaptureEventBridge.STATE_PROCESSING -> runReconstruction()
            CaptureEventBridge.STATE_COMPLETED -> onCompleted()
            CaptureEventBridge.STATE_FAILED -> onFailed()
            CaptureEventBridge.STATE_CANCELLED -> onCancelled()
            CaptureEventBridge.STATE_IDLE -> renderNow()
            else -> renderNow()
        }
        // 事件被接受后（状态可能不变，如 T-08）也要刷新界面。
        renderNow()
    }

    private fun probeCapability() {
        lifecycleScope.launch {
            val cap = withContext(Dispatchers.IO) {
                // 平台事实必须在首次 probeCapability 之前上报（缺口 AC）。
                val probeStatus = DeviceProbe.report(applicationContext)
                if (probeStatus != ReconStatus.OK) {
                    Log.w(TAG, "recon_set_probe_data 失败 status=$probeStatus")
                }
                if (captureContext == 0L) {
                    captureContext = NativeBridge.create(
                        sessionDir = filesDir.resolve("sessions/current").absolutePath,
                        preset = 1, maxThreads = 0)
                }
                if (captureContext == 0L) null else NativeBridge.probeCapability(captureContext)
            }
            lastCapability = cap?.let { Capability.fromArray(it) }
            onEvent(if (cap == null) CaptureEventBridge.PROBE_CAMERA_UNAVAILABLE
                    else CaptureEventBridge.PROBE_COMPLETE)
        }
    }

    private fun requestPermissions() {
        val granted = ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) ==
            PackageManager.PERMISSION_GRANTED
        if (granted) onEvent(CaptureEventBridge.PERMISSIONS_GRANTED)
        else cameraPermissionLauncher.launch(Manifest.permission.CAMERA)
    }

    private fun onPreviewing() {
        // 进入预览即重置 IMU 环形缓冲（规则本体在 platform/common）。
        ImuRingBuffer.reset()
        arrivedFrames = 0; ingestedFrames = 0; rejectedFrames = 0
        stopStatsPolling()
        renderNow()
    }

    private fun startCapture() {
        lifecycleScope.launch {
            val st = withContext(Dispatchers.IO) {
                if (captureContext == 0L) ReconStatus.ERR_INVALID_ARGUMENT
                else NativeBridge.startSession(
                    captureContext, System.currentTimeMillis().toString())
            }
            if (st != ReconStatus.OK) {
                Log.e(TAG, "startSession 失败 status=$st")
                CaptureEventBridge.setFailureStage(CaptureEventBridge.FAILURE_CAMERA)
                onEvent(CaptureEventBridge.CAMERA_BUSY)
                return@launch
            }
            startStatsPolling()
            renderNow()
        }
    }

    private fun onReviewing() {
        stopStatsPolling()
        // 采集统计作为自检依据的如实呈现；具体判据由 native 侧后续版本接入。
        lastStats = if (captureContext != 0L) NativeBridge.getScanStats(captureContext)
                        ?.let { ScanStats.fromArray(it) } else null
        renderNow()
    }

    /** 重建在 Activity 内闭环：完成/失败显式回推状态机（修 Processing 死态）。 */
    private fun runReconstruction() {
        reconJob?.cancel()
        reconJob = lifecycleScope.launch {
            val status = withContext(Dispatchers.IO) {
                if (captureContext == 0L) ReconStatus.ERR_INVALID_ARGUMENT
                else NativeBridge.runReconstruction(captureContext,
                    object : NativeBridge.ProgressCallback {
                        override fun onProgress(stage: Int, progress: Double, message: String) {
                            mainHandler.post {
                                lastProgress = "阶段 $stage · ${(progress * 100).toInt()}% · $message"
                                renderNow()
                            }
                        }
                    })
            }
            if (status == ReconStatus.OK) {
                lastMesh = withContext(Dispatchers.IO) {
                    NativeBridge.getMeshInfo(captureContext)?.let { MeshInfo.fromArray(it) }
                }
                onEvent(CaptureEventBridge.RECONSTRUCTION_SUCCEEDED)
            } else {
                Log.e(TAG, "重建失败 status=$status")
                CaptureEventBridge.setFailureStage(CaptureEventBridge.FAILURE_RECONSTRUCTION)
                onEvent(CaptureEventBridge.RECONSTRUCTION_FAILED)
            }
        }
    }

    private fun onCompleted() {
        stopStatsPolling()
        renderNow()
    }

    private fun onFailed() {
        stopStatsPolling()
        renderNow()
    }

    private fun onCancelled() {
        stopStatsPolling()
        renderNow()
    }

    // ------------------------------------------------------------------- //
    // 统计轮询（真实内核计数，不再全 0）
    // ------------------------------------------------------------------- //
    private fun startStatsPolling() {
        stopStatsPolling()
        statsJob = lifecycleScope.launch {
            while (isActive) {
                val s = withContext(Dispatchers.IO) {
                    if (captureContext == 0L) null
                    else NativeBridge.getScanStats(captureContext)?.let { ScanStats.fromArray(it) }
                }
                if (s != null) {
                    lastStats = s
                    renderNow()
                }
                delay(500)
            }
        }
    }

    private fun stopStatsPolling() {
        statsJob?.cancel()
        statsJob = null
    }

    // ------------------------------------------------------------------- //
    // 渲染
    // ------------------------------------------------------------------- //
    private var lastCapability: Capability? = null
    private var lastStats: ScanStats? = null
    private var lastMesh: MeshInfo? = null
    private var lastProgress: String = ""
    private var lastExportNote: String = ""

    private fun renderNow() {
        val state = when (CaptureEventBridge.currentState()) {
            CaptureEventBridge.STATE_IDLE -> getString(R.string.state_idle)
            CaptureEventBridge.STATE_CAPABILITY_PROBE -> getString(R.string.state_capability_probe)
            CaptureEventBridge.STATE_REQUESTING_PERMISSION -> getString(R.string.state_requesting_permission)
            CaptureEventBridge.STATE_PREVIEWING -> getString(R.string.state_previewing)
            CaptureEventBridge.STATE_CAPTURING -> getString(R.string.state_capturing)
            CaptureEventBridge.STATE_REVIEWING -> getString(R.string.state_reviewing)
            CaptureEventBridge.STATE_PROCESSING -> getString(R.string.state_processing)
            CaptureEventBridge.STATE_COMPLETED -> getString(R.string.state_completed)
            CaptureEventBridge.STATE_FAILED -> getString(R.string.state_failed)
            CaptureEventBridge.STATE_CANCELLED -> getString(R.string.state_cancelled)
            else -> getString(R.string.state_unknown)
        }
        val hint = when (CaptureEventBridge.currentState()) {
            CaptureEventBridge.STATE_PREVIEWING -> getString(R.string.ui_hint_preview)
            CaptureEventBridge.STATE_CAPTURING -> getString(R.string.ui_hint_capturing)
            CaptureEventBridge.STATE_REVIEWING -> getString(R.string.ui_hint_reviewing)
            CaptureEventBridge.STATE_PROCESSING -> getString(R.string.ui_hint_processing)
            CaptureEventBridge.STATE_COMPLETED -> getString(R.string.ui_hint_completed)
            CaptureEventBridge.STATE_FAILED -> getString(R.string.ui_hint_failed)
            else -> ""
        }
        val actions = when (CaptureEventBridge.currentState()) {
            CaptureEventBridge.STATE_PREVIEWING -> setOf(MainUI.Action.START)
            CaptureEventBridge.STATE_CAPTURING -> setOf(MainUI.Action.FINISH)
            CaptureEventBridge.STATE_REVIEWING -> setOf(
                MainUI.Action.CONTINUE, MainUI.Action.RECONSTRUCT)
            CaptureEventBridge.STATE_COMPLETED -> setOf(
                MainUI.Action.EXPORT_GLB, MainUI.Action.EXPORT_STL, MainUI.Action.RECAPTURE)
            CaptureEventBridge.STATE_FAILED -> setOf(MainUI.Action.RETRY, MainUI.Action.HOME)
            CaptureEventBridge.STATE_CANCELLED -> setOf(MainUI.Action.HOME)
            else -> emptySet()
        }

        ui.render(MainUI.ViewState(
            stateLabel = state,
            hint = hint,
            progress = lastProgress,
            diag = "${getString(R.string.ui_diag_label)}：$arrivedFrames / $ingestedFrames / $rejectedFrames" +
                   if (lastExportNote.isNotEmpty()) "\n$lastExportNote" else "",
            capability = formatCapability(),
            stats = formatStats(),
            result = formatResult(),
            visibleActions = actions))
    }

    private fun formatCapability(): String {
        val c = lastCapability ?: return getString(R.string.ui_cap_unknown)
        val branch = if (c.branch == PipelineBranch.RGBD) getString(R.string.ui_branch_rgbd)
                     else getString(R.string.ui_branch_mono)
        return listOf(
            "${getString(R.string.ui_cap_branch)}：$branch",
            "${getString(R.string.ui_cap_depth)}：${c.depthTier}",
            "${getString(R.string.ui_cap_camera)}：${yn(c.hasCamera)}",
            "${getString(R.string.ui_cap_imu)}：${yn(c.hasImu)}",
            "${getString(R.string.ui_cap_resolution)}：${c.maxImageWidth}×${c.maxImageHeight}",
            // recon_capability 不含可用内存字段。如实显示"探测中"，不伪造数字。
            "${getString(R.string.ui_cap_memory)}：${getString(R.string.ui_cap_unknown)}",
        ).joinToString("\n")
    }

    private fun yn(v: Boolean): String = if (v) getString(R.string.ui_cap_yes) else getString(R.string.ui_cap_no)

    private fun formatStats(): String {
        val s = lastStats ?: return "—"
        return listOf(
            "${getString(R.string.ui_stat_frames)}：${s.framesCaptured}",
            "${getString(R.string.ui_stat_accepted)}：${s.framesAccepted}",
            "${getString(R.string.ui_stat_keyframes)}：${s.keyframes}",
            "${getString(R.string.ui_stat_imu)}：${s.imuSamples}",
            "${getString(R.string.ui_stat_coverage)}：${(s.azimuthCoverage * 100).toInt()}%",
        ).joinToString("\n")
    }

    private fun formatResult(): String {
        val m = lastMesh ?: return "—"
        return listOf(
            "${getString(R.string.ui_result_vertices)}：${m.vertexCount}",
            "${getString(R.string.ui_result_triangles)}：${m.triangleCount}",
            "${getString(R.string.ui_result_nonmanifold)}：${m.nonManifoldEdges}",
            "${getString(R.string.ui_result_scale)}：%.4f".format(m.scaleFactor),
        ).joinToString("\n")
    }

    private fun toast(msg: String) {
        lastExportNote = msg
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
        renderNow()
    }

    override fun onPause() {
        super.onPause()
        onEvent(CaptureEventBridge.APP_BACKGROUNDED)
    }

    override fun onResume() {
        super.onResume()
        CaptureEventBridge.clearPaused()
    }

    override fun onDestroy() {
        stopStatsPolling()
        reconJob?.cancel()
        if (captureContext != 0L) {
            NativeBridge.destroy(captureContext)
            captureContext = 0L
        }
        super.onDestroy()
    }

    companion object {
        private const val TAG = "ReconCapture"
    }
}
