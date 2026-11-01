// SPDX-License-Identifier: BSD-3-Clause
//
// ReconstructionService.kt — 长时重建前台服务（dataSync 类型）。
// 契约：docs/11_平台权限与集成.md §2.5（前台服务决策）、§4.2（AND-L-1）。
//
// 关键点：Android 上重建**可以且应该**在后台继续（与 iOS 相反，iOS 无可靠的
// 长时后台执行保证）。故用前台服务承载 recon_run_reconstruction。

package com.recon.scan

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder
import androidx.core.app.NotificationCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

class ReconstructionService : Service() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    override fun onCreate() {
        super.onCreate()
        createChannel()
        startForeground(NOTIF_ID, buildNotification(0.0))
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        scope.launch {
            val ctx = NativeBridge.create(
                sessionDir = filesDir.resolve("sessions/current").absolutePath,
                preset = 1, maxThreads = 0
            )
            if (ctx == 0L) {
                stopSelf()
                return@launch
            }
            NativeBridge.runReconstruction(ctx, object : NativeBridge.ProgressCallback {
                override fun onProgress(stage: Int, progress: Double, message: String) {
                    // 前台服务运行在后台线程；更新通知需切主线程。
                    scope.launch(Dispatchers.Main) {
                        updateNotification(progress)
                    }
                }
            })
            NativeBridge.destroy(ctx)
            stopSelf()
        }
        // AND-L-1：被系统回收后重启应继续重建；这里选择 not sticky（会话检查点可恢复）。
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        scope.cancel()
        super.onDestroy()
    }

    // ------------------------------------------------------------------- //

    private fun createChannel() {
        val mgr = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        val ch = NotificationChannel(
            CHANNEL_ID, "重建进度", NotificationManager.IMPORTANCE_LOW
        )
        mgr.createNotificationChannel(ch)
    }

    private fun buildNotification(progress: Double): Notification =
        NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("正在重建")
            .setContentText("${(progress * 100).toInt()}%")
            .setSmallIcon(android.R.drawable.stat_sys_download)
            .setOngoing(true)
            .setProgress(100, (progress * 100).toInt(), false)
            .build()

    private fun updateNotification(progress: Double) {
        val mgr = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        mgr.notify(NOTIF_ID, buildNotification(progress))
    }

    companion object {
        private const val CHANNEL_ID = "recon_reconstruction"
        private const val NOTIF_ID = 1001

        fun start(context: Context) {
            val intent = Intent(context, ReconstructionService::class.java)
            context.startForegroundService(intent)
        }
    }
}
