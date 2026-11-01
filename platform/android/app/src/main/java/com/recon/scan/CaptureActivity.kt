// SPDX-License-Identifier: BSD-3-Clause
//
// CaptureActivity.kt — 采集主界面。
// 契约：docs/09_UI规格.md §4.1/§4.2（采集状态机）、
//       docs/11_平台权限与集成.md §2.3（运行时申请时序）、§4.2（前后台切换）。

package com.recon.scan

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * 采集界面。
 *
 * 本类刻意保持为**薄壳**：真正的状态迁移规则在 native 侧
 * `platform/common/src/capture_state.cpp`（跨端共享，见 docs/09 §4.1），
 * 这里只负责把平台事件（生命周期、权限结果）翻译成状态机事件并执行副作用。
 *
 * 线程模型（docs/11 §3.7 + 记忆铁律）：native 的 recon_* 全部同步阻塞，
 * 一律经 [Dispatchers.IO] 调用；**相机帧回调绝不直接调重计算**。
 */
class CaptureActivity : ComponentActivity() {

    private var captureContext: Long = 0L

    private val cameraPermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) {
                // T-05：相机 + 运动权限已授予 → Previewing
                // （运动权限在本项目为可选，见 AND 侧 T-07）
                onEvent(CaptureEventBridge.PERMISSIONS_GRANTED)
            } else {
                // T-06：相机权限被拒 → Failed
                onEvent(CaptureEventBridge.CAMERA_PERMISSION_DENIED)
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Idle → CapabilityProbe（T-01），随后探测 → RequestingPermission（T-02）。
        onEvent(CaptureEventBridge.START_SCAN)
    }

    override fun onPause() {
        super.onPause()
        // T-14：切后台 → 暂停相机与 AR，已采集帧保留。
        onEvent(CaptureEventBridge.APP_BACKGROUNDED)
    }

    override fun onResume() {
        super.onResume()
        // 回前台：清暂停标志（docs/09 §4.2 未定义独立事件，状态本不变）。
        CaptureEventBridge.clearPaused()
    }

    // ------------------------------------------------------------------- //

    /** 把平台事件交给 native 状态机，并在主线程执行返回的副作用。 */
    private fun onEvent(event: Int) {
        val nextState = CaptureEventBridge.apply(event)
        when (nextState) {
            CaptureEventBridge.STATE_CAPABILITY_PROBE -> probeCapability()
            CaptureEventBridge.STATE_REQUESTING_PERMISSION -> requestPermissions()
            CaptureEventBridge.STATE_PREVIEWING -> startPreview()
            CaptureEventBridge.STATE_CAPTURING -> startCapture()
            CaptureEventBridge.STATE_PROCESSING -> runReconstruction()
            CaptureEventBridge.STATE_CANCELLED -> cleanupSession()
            else -> Unit
        }
    }

    private fun probeCapability() {
        lifecycleScope.launch {
            val cap = withContext(Dispatchers.IO) {
                if (captureContext == 0L) {
                    captureContext = NativeBridge.create(
                        sessionDir = filesDir.resolve("sessions/current").absolutePath,
                        preset = 1, maxThreads = 0
                    )
                }
                if (captureContext == 0L) null else NativeBridge.probeCapability(captureContext)
            }
            // 探测完成 → T-02。相机不可用 → T-03（此处简化：以 cap 为空判定）。
            onEvent(
                if (cap == null) CaptureEventBridge.PROBE_CAMERA_UNAVAILABLE
                else CaptureEventBridge.PROBE_COMPLETE
            )
        }
    }

    private fun requestPermissions() {
        // docs/11 §2.3：先查 CAMERA；POST_NOTIFICATIONS 不在此申请。
        val granted = ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) ==
            PackageManager.PERMISSION_GRANTED
        if (granted) {
            onEvent(CaptureEventBridge.PERMISSIONS_GRANTED)
        } else {
            cameraPermissionLauncher.launch(Manifest.permission.CAMERA)
        }
    }

    private fun startPreview() {
        // 启动 Camera2 会话 + 订阅 SensorManager（docs/11 §3.4/§3.6）。
        // 骨架阶段：真实实现见 docs/11 §3.4.2；此处仅占位以保证状态机闭环。
        //
        // ── IMU 缓冲的生命周期（第三十六轮接线）────────────────────────────
        //
        // 进入预览即 `reset()`：丢弃上一轮会话残留的 IMU 样本与诊断计数
        // （Activity 重建 / T-24 重试都会重新走到这里）。规则本体在
        // `platform/common/src/imu_window.cpp`，本处只调用、不实现。
        ImuRingBuffer.reset()

        // 订阅 IMU（docs/11 §3.6）—— **尚未实现**，接线点如下：
        //
        //   onSensorChanged(e) {
        //     ImuRingBuffer.append(e.timestamp, ax, ay, az, gx, gy, gz)
        //   }
        //
        // 需要 `SensorManager` + 专用 `HandlerThread("recon.capture.imu")`，且必须
        // `maxReportLatencyUs = 0`（**关闭 batching**，见 §3.6「决策」行）：开启
        // batching 会丢中间采样，破坏 02 §3.6.4 的 `σ_t` 误差预算（以功耗换精度）。
        // 回调只做「值拷贝 + 入缓冲」，切窗规则全在规则本体（docs/02 §2.4.1）。
        //
        // ⚠️ **accel 口径未定，故此处刻意不写**：§3.6 AND-IMU-2（须先减重力）与
        // §3.7 对接表（`TYPE_ACCELEROMETER` 即含重力比力）相互矛盾，登记于
        // docs/14 §5.1.3 缺口 O。在裁决前写入任何一种口径都会固化一个可能错误的
        // 物理约定 —— 故留空，而不是「随便接一个先跑起来」。
    }

    private fun startCapture() {
        // T-09：recon_start_session + 开始落盘。
        lifecycleScope.launch { withContext(Dispatchers.IO) {
            if (captureContext != 0L) {
                NativeBridge.startSession(captureContext, System.currentTimeMillis().toString())
            }
        } }
    }

    private fun runReconstruction() {
        // 长时重建走前台服务（docs/11 §2.5），此处不直接在本 Activity 内跑。
        ReconstructionService.start(this)
    }

    private fun cleanupSession() {
        lifecycleScope.launch { withContext(Dispatchers.IO) {
            if (captureContext != 0L) {
                NativeBridge.destroy(captureContext)
                captureContext = 0L
            }
        } }
    }

    override fun onDestroy() {
        super.onDestroy()
        // docs/11 §4.2：释放平台资源，**不删除**会话目录（保留给恢复）。
        if (captureContext != 0L) {
            NativeBridge.destroy(captureContext)
            captureContext = 0L
        }
    }
}
