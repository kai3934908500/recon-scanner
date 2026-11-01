// SPDX-License-Identifier: BSD-3-Clause
//
// ReconApplication.kt — 进程级初始化。
// 契约：docs/11_平台权限与集成.md §2.2（AndroidManifest 声明 .ReconApplication）。
package com.recon.scan

import android.app.Application

/**
 * 应用入口。
 *
 * 刻意保持极简：不做任何网络初始化、不上报、不注册推送。零网络是硬约束
 * （docs/11 §1.6）。唯一的启动动作是加载 native 库并做 ABI 版本自检 ——
 * 若 App 与 core 的 [RECON_ABI_VERSION] 不一致，立即在开发期暴露（release
 * 下记录日志，不抛异常中断用户）。
 */
class ReconApplication : Application() {

    override fun onCreate() {
        super.onCreate()

        // 加载 native 库。库名由 CMake target 决定（librecon_core.so + 平台门面）。
        System.loadLibrary("recon_core")
        System.loadLibrary("recon_platform_android")

        // ABI 版本自检：App 侧常量必须与 core 一致（docs/01 §3）。
        val coreAbi = NativeBridge.abiVersion()
        if (coreAbi != NativeBridge.EXPECTED_ABI_VERSION) {
            android.util.Log.e(
                TAG,
                "ABI 版本不匹配：App=$EXPECTED_ABI_VERSION core=$coreAbi。"
                    + "可能导致未定义行为，请使用与 App 同步构建的 native 库。"
            )
        }
    }

    private companion object {
        const val TAG = "ReconApplication"
        const val EXPECTED_ABI_VERSION = 1
    }
}
