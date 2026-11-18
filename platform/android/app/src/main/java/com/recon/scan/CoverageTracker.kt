// SPDX-License-Identifier: BSD-3-Clause
//
// CoverageTracker.kt — 采集方位覆盖追踪（陀螺仪积分 yaw → 方位桶）
//
// 用途：全景扫描式引导的"球包裹"可视化。用户环绕物体时，实时计算已覆盖
// 的方位角比例；UI 把 36 个方位桶映射到球面上的扇区，逐个染绿，形成
// "球被慢慢包裹"的效果（对标 Polycam / Scaniverse 的覆盖反馈）。
//
// 数据源：Android SensorManager.TYPE_GYROSCOPE 的 Z 轴角速度积分（本机真值）。
// 纯设备端、零网络。无陀螺仪时降级为 0（不伪造覆盖）。
package com.recon.scan

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import kotlin.math.PI
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.sin

internal class CoverageTracker(context: Context) : SensorEventListener {
    private val sensorManager = context.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
    private val gyro = sensorManager?.getDefaultSensor(Sensor.TYPE_GYROSCOPE)

    private val buckets = BooleanArray(BUCKETS)
    private var yaw = 0.0          // 累积偏航角（弧度）
    private var lastTsNs = 0L
    private var covered = 0

    /** 是否已真正开始追踪（有陀螺仪且在采集中）。 */
    var active = false
        private set

    fun start() {
        if (gyro == null) return
        reset()
        // SENSOR_DELAY_UI：60Hz 级足够方位积分，省电
        sensorManager?.registerListener(this, gyro, SensorManager.SENSOR_DELAY_UI)
        active = true
    }

    fun stop() {
        sensorManager?.unregisterListener(this)
        active = false
    }

    fun reset() {
        java.util.Arrays.fill(buckets, false)
        yaw = 0.0
        lastTsNs = 0L
        covered = 0
    }

    /** 帧到达时按当前 yaw 标记一个方位桶。返回当前覆盖率 [0,1]。 */
    fun markFrame(): Float {
        if (!active) return 0f
        val idx = (((yaw mod TWO_PI) / TWO_PI) * BUCKETS).toInt() % BUCKETS
        val i = if (idx < 0) idx + BUCKETS else idx
        if (!buckets[i]) { buckets[i] = true; covered++ }
        return coverage()
    }

    fun coverage(): Float = if (active) covered.toFloat() / BUCKETS else 0f

    /** 各方位桶是否已覆盖，供球面扇区染色。 */
    fun bucketStates(): BooleanArray = buckets.copyOf()

    /** 当前朝向（弧度），供箭头指示。 */
    fun currentYaw(): Double = yaw

    override fun onSensorChanged(e: SensorEvent) {
        if (e.sensor.type != Sensor.TYPE_GYROSCOPE) return
        val ts = e.timestamp
        if (lastTsNs == 0L) { lastTsNs = ts; return }
        val dt = (ts - lastTsNs) / 1_000_000_000.0
        lastTsNs = ts
        if (dt <= 0.0 || dt > 1.0) return
        // 竖屏持握时设备 Y 轴（沿屏幕上下）即世界偏航轴；Z 轴是屏幕法线（俯仰相关）。
        val wy = e.values[1].toDouble()
        if (kotlin.math.abs(wy) > 6.0) return
        yaw = (yaw + wy * dt) mod TWO_PI
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit

    companion object {
        const val BUCKETS = 36
        const val TWO_PI = 2.0 * PI
    }
}

private infix fun Double.mod(m: Double): Double = ((this % m) + m) % m
