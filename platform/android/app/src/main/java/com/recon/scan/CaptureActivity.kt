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
    private lateinit var shell: AppShell

    /** 相机链路诊断计数：回调到达 / 成功投递内核 / 被拒（缺口 AV 同口径）。 */
    @Volatile private var arrivedFrames = 0
    @Volatile private var ingestedFrames = 0
    @Volatile private var rejectedFrames = 0

    /** Camera2 采集链：权限通过后创建，离开扫描链路后释放。 */
    private var camera: CameraCapture? = null
    private var cameraNote: String = ""
    private var nextFrameId: Long = 1L

    /** 相机只能在 Activity 处于前台时打开；真机实测后台 openCamera 会被系统拒绝。 */
    private var activityResumed = false
    private var pendingCameraStart = false
    /** 陀螺仪方位覆盖追踪：驱动全景扫描式"球包裹"引导。 */
    private var coverageTracker: CoverageTracker? = null
    /** U²-Netp 显著性物体分割：真正识别扫描目标（端侧 ONNX 推理）。 */
    private var u2netDetector: U2NetDetector? = null
    @Volatile private var detectedObjectMask: BooleanArray? = null
    @Volatile private var detecting = false
    private var lastDetectMs = 0L

    /** 后台懒加载 U2-Netp（首次检测时触发，不阻塞 onCreate/探测）。 */
    private fun ensureU2Net() {
        if (u2netDetector != null) return
        lifecycleScope.launch(Dispatchers.IO) {
            try { u2netDetector = U2NetDetector(applicationContext) }
            catch (e: Exception) { Log.w(TAG, "U2-Netp 初始化失败，降级为无目标识别: ${e.message}") }
        }
    }

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
        coverageTracker = CoverageTracker(this)
        // U2-Netp 模型改后台懒加载：4.4MB + ONNX session 建在主线程会阻塞启动，
        // 导致能力探测状态机被拖垮（用户实测"点开始采集就失败/卡探测中"）。
        val probe = DeviceProbe.probe(this)
        val branch = if (probe.hasTof) "支持硬件深度" else "单目视觉"
        val imu = if (probe.hasImu) "惯性传感器可用" else "无惯性传感器"
        val guideSummary = "设备检测：$branch · $imu\n" +
            "最大分辨率 ${probe.maxImageWidth}×${probe.maxImageHeight}\n" +
            "推荐档位：均衡（可在参数页调整）"
        shell = AppShell(
            context = this,
            scanUI = ui,
            onPreset = { id -> applyPreset(id) },
            onParam = { key, intVal, doubleVal -> setParam(key, intVal, doubleVal) },
            onFocusMode = { mode -> setFocusMode(mode) },
            onFocusDistance = { d -> setFocusDistance(d) },
            onReplayGuide = { replayGuide() },
            onDevMode = { on -> toggleDevMode(on) },
            onGuideDone = { restartCameraIfNeeded() },
            onResetSettings = { resetAllSettings() },
            onQualityTier = { tier -> setQualityTier(tier) },
            onGuideConfig = { preset, hud, imu -> applyGuideConfig(preset, hud, imu) },
            guideSummary = guideSummary,
        )
        setContentView(shell.root)

        // T-01：Idle → CapabilityProbe（经状态机），随后探测 → T-02 申请权限。
        onEvent(CaptureEventBridge.START_SCAN)
    }

    // ---- AppShell 回调：全部连真实功能，不做假按钮 ------------------------
    private fun applyPreset(id: Int) {
        lifecycleScope.launch {
            val status = withContext(Dispatchers.IO) {
                if (captureContext == 0L) ReconStatus.ERR_INVALID_ARGUMENT
                else NativeBridge.applyPreset(captureContext, id)
            }
            toast(getString(if (status == ReconStatus.OK)
                R.string.params_applied else R.string.ui_hint_failed))
        }
    }

    private fun setParam(key: String, intVal: Int, doubleVal: Double) {
        lifecycleScope.launch {
            withContext(Dispatchers.IO) {
                if (captureContext != 0L) NativeBridge.setParam(captureContext, key, intVal, doubleVal)
            }
        }
    }

    private var focusMode = 0
    private fun setFocusMode(mode: Int) {
        focusMode = mode
        // 把当前模式重新应用到正在运行的预览请求
        camera?.setFocus(mode, lastFocusDistance)
    }

    private var lastFocusDistance = 0f
    private fun setFocusDistance(d: Float) {
        lastFocusDistance = d
        if (focusMode == 1) camera?.setFocus(focusMode, d)
    }

    private fun replayGuide() {
        // 清掉"已看过"标记并重建 AppShell 的引导层显示
        getSharedPreferences("recon_prefs", MODE_PRIVATE).edit()
            .putBoolean("guide_seen", false).apply()
        shell.showGuide()
    }

    /** 引导配置向导的结果：一次写入内核预设 + HUD/IMU 偏好。 */
    private fun applyGuideConfig(preset: Int, hud: Boolean, imu: Boolean) {
        getSharedPreferences("recon_prefs", MODE_PRIVATE).edit()
            .putBoolean("guide_hud", hud)
            .putBoolean("guide_imu", imu)
            .apply()
        lifecycleScope.launch {
            withContext(Dispatchers.IO) {
                if (captureContext != 0L) {
                    NativeBridge.applyPreset(captureContext, preset)
                    NativeBridge.setParam(captureContext, "cap_imu_enabled",
                        if (imu) 1 else 0, 0.0)
                }
            }
        }
    }

    private fun setQualityTier(tier: Int) {
        // 画质档改变需重开相机（采集 ImageReader 尺寸在 openCamera 时定）
        lifecycleScope.launch {
            val wasCapturing = CaptureEventBridge.currentState() ==
                CaptureEventBridge.STATE_CAPTURING
            withContext(Dispatchers.Main.immediate) {
                releaseCamera()
                camera = CameraCapture(
                    context = this@CaptureActivity,
                    onPreviewView = { view -> ui.attachPreview(view) },
                    onFrame = { frame -> ingest(frame) },
                    onDiagnostic = { msg -> mainHandler.post { cameraNote = msg; renderNow() } },
                )
                camera?.setQualityTier(tier)
                camera?.setIngest(wasCapturing)
                camera?.start()
            }
        }
    }

    private fun resetAllSettings() {
        lifecycleScope.launch {
            withContext(Dispatchers.IO) {
                if (captureContext != 0L) NativeBridge.applyPreset(captureContext, 1)
            }
            focusMode = 0
            lastFocusDistance = 0f
            toast(getString(R.string.settings_reset_done))
        }
    }

    private var devMode = false
    private fun toggleDevMode(on: Boolean) {
        devMode = on
        renderNow()
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
            MainUI.Action.RECAPTURE -> {
                resetSessionForNewCapture()   // 丢弃旧会话帧，避免重采混入旧数据
                onEvent(CaptureEventBridge.RECAPTURE)
            }
            MainUI.Action.RETRY -> {
                // 相机类失败重试需全新会话；重建失败重试保留已采帧（T-24）。
                if (CaptureEventBridge.failureStage() == CaptureEventBridge.FAILURE_CAMERA) {
                    resetSessionForNewCapture()
                }
                onEvent(CaptureEventBridge.RETRY)
            }
            MainUI.Action.HOME -> exitToPreview()
            MainUI.Action.EXPORT_GLB -> export(0)
            MainUI.Action.EXPORT_OBJ -> export(1)
            MainUI.Action.EXPORT_PLY -> export(2)
            MainUI.Action.EXPORT_STL -> export(3)
            MainUI.Action.EXPORT_3MF -> export(4)
        }
    }

    private fun export(format: Int) {
        lifecycleScope.launch {
            val ext = when (format) { 0 -> "glb"; 1 -> "obj"; 2 -> "ply"; 3 -> "stl"; 4 -> "3mf"; else -> "bin" }
            val path = filesDir.resolve("model.$ext").absolutePath
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
        cameraNote = ""
        stopStatsPolling()
        ensureCamera()
        camera?.setIngest(false)
        // ⚠️ 切前台后再开相机：onCreate 阶段进程可能仍是后台态，
        // 此刻 openCamera 会被 Android 以 CAMERA_DISABLED(1) 拒绝（真机实测）。
        if (activityResumed) camera?.start() else pendingCameraStart = true
        renderNow()
    }

    private fun ensureCamera() {
        if (camera != null) return
        camera = CameraCapture(
            context = this,
            onPreviewView = { view -> ui.attachPreview(view) },
            onFrame = { frame -> ingest(frame) },
            onDiagnostic = { msg -> mainHandler.post { cameraNote = msg; renderNow() } },
        )
    }

    /** 相机线程：YUV→BGRA 已完成，这里只做内核投递与诊断计数。 */
    /**
     * 单会话最多送内核的帧数。SfM 是 O(n²)，但 240 帧会明显牺牲环绕采集精度。
     * 提到 600：core 侧预览/重建还有抽稀保护，600 帧在真机是可控的；
     * 超出部分如实计入 rejectedFrames 并在诊断里可见，不静默丢弃。
     */
    private val maxFramesToCore = 600

    private fun ingest(frame: CapturedFrame) {
        arrivedFrames++
        val ctx = captureContext
        if (ctx == 0L ||
            CaptureEventBridge.currentState() != CaptureEventBridge.STATE_CAPTURING) {
            rejectedFrames++
            return
        }
        if (ingestedFrames >= maxFramesToCore) {
            rejectedFrames++
            return   // 帧上限：预览继续，但不再送内核（避免 SfM O(n²) 卡死）
        }
        val status = NativeBridge.pushFrame(
            ctx, nextFrameId++, frame.timestampNs,
            frame.bgra, frame.width, frame.height, frame.width * 4,
            null, 0, 0, null,
            0,  // RECON_DEPTH_SRC_NONE（本机型 MONO 分支）
            frame.fx, frame.fy, frame.cx, frame.cy,
            0.0, 0.0, 0.0, 0.0, 0.0,
            null, null, frame.timestampNs,
            null, 0,
            0.0, 0.0)
        if (status == ReconStatus.OK) {
            ingestedFrames++
            coverageTracker?.markFrame()
            // U²-Netp 目标识别：节流 250ms，IO 线程推理，不阻塞采集。
            // 模型不可用时保持旧 mask（不伪造识别结果）。
            val now = System.currentTimeMillis()
            if (!detecting && now - lastDetectMs >= 250) {
                lastDetectMs = now
                detecting = true
                lifecycleScope.launch(Dispatchers.IO) {
                    val det = u2netDetector?.detect(frame.bgra, frame.width, frame.height)
                    detecting = false
                    if (det?.mask != null) {
                        detectedObjectMask = det.mask
                        mainHandler.post {
                            ui.updateCaptureHud(ingestedFrames,
                                coverageTracker?.coverage() ?: 0f,
                                coverageTracker?.bucketStates(), detectedObjectMask,
                                coverageTracker?.speed() ?: 0.0)
                        }
                    }
                }
            }
        } else rejectedFrames++
    }

    private fun startCapture() {
        ensureU2Net()   // 后台预热模型，不阻塞状态机
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
            coverageTracker?.start()
            ensureCamera()
            camera?.setIngest(true)
            renderNow()
        }
    }

    private fun onReviewing() {
        stopStatsPolling()
        camera?.setIngest(false)  // 保留预览，用户可选 CONTINUE 继续扫描
        // 采集封口：通知内核结束本次采集（关 session_active + 状态迁到 Reviewing）。
        // 帧一直在 core 的 frames_ 里，此调用不搬数据，但缺了会留下 session_active=true
        // 的生命周期不一致（重建期继续 push 会被拒）。失败如实记录，不静默。
        lifecycleScope.launch {
            val st = withContext(Dispatchers.IO) {
                if (captureContext != 0L) NativeBridge.finishCapture(captureContext)
                else ReconStatus.ERR_INVALID_ARGUMENT
            }
            if (st != ReconStatus.OK) {
                Log.w(TAG, "finishCapture 返回非 OK status=$st（可能已封口，幂等）")
            }
        }
        // native 读取必须离开主线程（否则可能 ANR）；结果回到主线程再渲染。
        lifecycleScope.launch {
            val s = withContext(Dispatchers.IO) {
                if (captureContext != 0L) NativeBridge.getScanStats(captureContext)
                    ?.let { ScanStats.fromArray(it) } else null
            }
            lastStats = s
            renderNow()
        }
    }

    /** 重建在 Activity 内闭环：完成/失败显式回推状态机（修 Processing 死态）。 */
    private fun runReconstruction() {
        ui.setProcessingMode(true)   // 切到独立重建进度页（隐藏取景器/信息卡）
        reconJob?.cancel()
        reconJob = lifecycleScope.launch {
            val status = withContext(Dispatchers.IO) {
                if (captureContext == 0L) ReconStatus.ERR_INVALID_ARGUMENT
                else NativeBridge.runReconstruction(captureContext,
                    object : NativeBridge.ProgressCallback {
                        override fun onProgress(stage: Int, progress: Double, message: String) {
                            mainHandler.post {
                                lastProgressFraction = progress.coerceIn(0.0, 1.0).toFloat()
                                lastProgress = "阶段 $stage · ${(progress * 100).toInt()}% · $message"
                                ui.renderStageDetail(stage, progress, message)
                                renderNow()
                            }
                        }

                        override fun onLog(level: Int, message: String) {
                            // 内核日志一行行实时滚动显示在重建页
                            mainHandler.post { ui.appendLog(level, message) }
                        }
                    })
            }
            if (status == ReconStatus.OK) {
                lastMesh = withContext(Dispatchers.IO) {
                    NativeBridge.getMeshInfo(captureContext)?.let { MeshInfo.fromArray(it) }
                }
                shell.setResultSummary(lastMesh?.let {
                    "${it.vertexCount} 顶点 · ${it.triangleCount} 三角面 · 非流形边 ${it.nonManifoldEdges}"
                } ?: getString(R.string.projects_empty_body))
                // 取预览网格几何（C ABI recon_get_preview_mesh），供 3D 线框预览
                val preview = withContext(Dispatchers.IO) {
                    NativeBridge.getPreviewMesh(captureContext, 200000)
                }
                val pos = preview?.get(0) as? FloatArray
                val idx = (preview?.get(1) as? IntArray)
                shell.setPreviewMesh(pos, idx)
                ui.clearStageDetail()
                ui.setProcessingMode(false)
                onEvent(CaptureEventBridge.RECONSTRUCTION_SUCCEEDED)
            } else {
                Log.e(TAG, "重建失败 status=$status")
                ui.clearStageDetail()
                ui.setProcessingMode(false)
                CaptureEventBridge.setFailureStage(CaptureEventBridge.FAILURE_RECONSTRUCTION)
                onEvent(CaptureEventBridge.RECONSTRUCTION_FAILED)
            }
        }
    }

    /**
     * 退出当前任务回到预览态：重建中先通知内核取消（否则后台线程还在跑），
     * 再销毁会话、丢弃帧，让用户能立刻重新开始。任何时候都有出口。
     */
    private fun exitToPreview() {
        lifecycleScope.launch {
            val st = CaptureEventBridge.currentState()
            if (st == CaptureEventBridge.STATE_PROCESSING && captureContext != 0L) {
                withContext(Dispatchers.IO) { NativeBridge.cancel(captureContext) }
                reconJob?.cancel()
            }
            withContext(Dispatchers.Main.immediate) {
                ui.setProcessingMode(false)
                resetSessionForNewCapture()
                onEvent(CaptureEventBridge.RECAPTURE)
            }
        }
    }

    /**
     * 重新采集前彻底重置会话：销毁旧 context（内含上一轮所有帧与重建结果），
     * 下一轮 onPreviewing → startCapture 时新建。否则旧帧会混进新采集，结果错乱。
     */
    private fun resetSessionForNewCapture() {
        stopStatsPolling()
        reconJob?.cancel()
        if (captureContext != 0L) {
            NativeBridge.destroy(captureContext)
            captureContext = 0L
        }
        lastStats = null
        lastMesh = null
        lastProgress = ""
        lastProgressFraction = -1f
        nextFrameId = 1L
        arrivedFrames = 0; ingestedFrames = 0; rejectedFrames = 0
        coverageTracker?.reset()
        detectedObjectMask = null
        lastDetectMs = 0L
        ui.clearStageDetail()
        shell.setResultSummary(getString(R.string.projects_empty_body))
        shell.setPreviewMesh(null, null)
    }

    private fun onCompleted() {
        stopStatsPolling()
        releaseCamera()
        renderNow()
    }

    private fun onFailed() {
        stopStatsPolling()
        releaseCamera()
        renderNow()
    }

    private fun onCancelled() {
        stopStatsPolling()
        releaseCamera()
        renderNow()
    }

    private fun releaseCamera() {
        camera?.shutdown()
        camera = null
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
                    // 采集全屏 HUD 实时反馈：帧数 + 方位覆盖
                    ui.updateCaptureHud(ingestedFrames, coverageTracker?.coverage() ?: 0f,
                        coverageTracker?.bucketStates(), detectedObjectMask,
                        coverageTracker?.speed() ?: 0.0)
                    renderNow()
                }
                delay(500)
            }
        }
    }

    private fun stopStatsPolling() {
        coverageTracker?.stop()
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
    private var lastProgressFraction: Float = -1f
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
            CaptureEventBridge.STATE_CAPTURING -> setOf(
                MainUI.Action.FINISH, MainUI.Action.HOME)
            CaptureEventBridge.STATE_REVIEWING -> setOf(
                MainUI.Action.CONTINUE, MainUI.Action.RECONSTRUCT, MainUI.Action.HOME)
            CaptureEventBridge.STATE_PROCESSING -> setOf(MainUI.Action.HOME)
            CaptureEventBridge.STATE_COMPLETED -> setOf(
                MainUI.Action.EXPORT_GLB, MainUI.Action.EXPORT_OBJ, MainUI.Action.EXPORT_PLY,
                MainUI.Action.EXPORT_STL, MainUI.Action.EXPORT_3MF, MainUI.Action.RECAPTURE,
                MainUI.Action.HOME)
            CaptureEventBridge.STATE_FAILED -> setOf(MainUI.Action.RETRY, MainUI.Action.HOME)
            CaptureEventBridge.STATE_CANCELLED -> setOf(MainUI.Action.HOME)
            else -> emptySet()
        }

        val current = CaptureEventBridge.currentState()
        // 采集态进入全屏竖屏取景（大厂扫描体验）；其余态回到信息面板。
        val isCapturing = current == CaptureEventBridge.STATE_CAPTURING
        ui.setCaptureFullscreen(isCapturing)
        if (::shell.isInitialized) shell.setCaptureFullscreen(isCapturing)
        // 四段式流程：0 设备准备 / 1 环绕采集 / 2 三维重建 / 3 成果导出
        val step = when (current) {
            CaptureEventBridge.STATE_IDLE,
            CaptureEventBridge.STATE_CAPABILITY_PROBE,
            CaptureEventBridge.STATE_REQUESTING_PERMISSION,
            CaptureEventBridge.STATE_PREVIEWING -> 0
            CaptureEventBridge.STATE_CAPTURING,
            CaptureEventBridge.STATE_REVIEWING -> 1
            CaptureEventBridge.STATE_PROCESSING -> 2
            CaptureEventBridge.STATE_COMPLETED,
            CaptureEventBridge.STATE_FAILED,
            CaptureEventBridge.STATE_CANCELLED -> 3
            else -> 0
        }
        val tone = when (current) {
            CaptureEventBridge.STATE_PROCESSING -> 1
            CaptureEventBridge.STATE_COMPLETED -> 2
            CaptureEventBridge.STATE_FAILED -> 3
            else -> 0
        }
        val progressFraction =
            if (current == CaptureEventBridge.STATE_PROCESSING) lastProgressFraction else -1f

        ui.render(MainUI.ViewState(
            stateLabel = state,
            hint = hint,
            progress = lastProgress,
            progressFraction = progressFraction,
            step = step,
            stateTone = tone,
            diag = buildString {
                append(getString(R.string.ui_diag_label)).append("：")
                    .append(arrivedFrames).append(" / ")
                    .append(ingestedFrames).append(" / ")
                    .append(rejectedFrames)
                if (cameraNote.isNotEmpty()) append("\n").append(cameraNote)
                if (lastExportNote.isNotEmpty()) append("\n").append(lastExportNote)
            },
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
        activityResumed = false
        onEvent(CaptureEventBridge.APP_BACKGROUNDED)
    }

    override fun onResume() {
        super.onResume()
        activityResumed = true
        CaptureEventBridge.clearPaused()
        restartCameraIfNeeded()
    }

    /**
     * 前台后确保相机可用：待启动标志消费，或对曾打开失败的相机完整重建。
     * 引导层遮挡期间的相机启动会被系统判后台而拒绝，引导结束后必须重试。
     */
    private fun restartCameraIfNeeded() {
        if (!activityResumed) return
        if (pendingCameraStart) {
            pendingCameraStart = false
            camera?.start()
        } else if (camera?.hasError() == true || camera == null) {
            val st = CaptureEventBridge.currentState()
            if (st == CaptureEventBridge.STATE_PREVIEWING ||
                st == CaptureEventBridge.STATE_CAPTURING) {
                val ingesting = st == CaptureEventBridge.STATE_CAPTURING
                cameraNote = ""
                releaseCamera()
                ensureCamera()
                camera?.setIngest(ingesting)
                camera?.start()
                arrivedFrames = 0; ingestedFrames = 0; rejectedFrames = 0
                renderNow()
            }
        }
    }

    override fun onDestroy() {
        coverageTracker?.stop()
        stopStatsPolling()
        reconJob?.cancel()
        releaseCamera()
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
