// SPDX-License-Identifier: BSD-3-Clause
//
// CoverageTracker.kt — 采集方位覆盖追踪（旋转矢量 → yaw → 方位桶）
//
// 上一版用陀螺仪积分在真机上覆盖率恒 0：阈值过滤 + 积分漂移 + 轴向判断
// 都不可靠。改用 TYPE_ROTATION_VECTOR（融合加速度计/磁力计/陀螺仪），
// 由四元数直接求设备罗盘方位角 yaw，用户环绕物体时 yaw 单调变化，
// 落到 36 个方位桶 → 覆盖率与球面包裹染色。真机可验证、无漂移。
//
// 纯设备端、零网络。无该传感器时如实报 0（不伪造覆盖）。
package com.recon.scan

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import kotlin.math.atan2
import kotlin.math.asin
import kotlin.math.PI

internal class CoverageTracker(context: Context) : SensorEventListener {
    private val sensorManager = context.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
    private val rotationVector = sensorManager?.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR)

    private var buckets = BooleanArray(BUCKETS)
    private var yaw = 0.0
    private var covered = 0
    private var lastMarkedYaw = Double.NaN   // 上次计桶的 yaw；避免静止漂移乱涨
    var active = false
        private set

    fun start() {
        if (rotationVector == null) return
        reset()
        sensorManager?.registerListener(this, rotationVector, SensorManager.SENSOR_DELAY_UI)
        active = true
    }

    fun stop() {
        sensorManager?.unregisterListener(this)
        active = false
    }

    fun reset() {
        java.util.Arrays.fill(buckets, false)
        yaw = 0.0
        covered = 0
        lastMarkedYaw = Double.NaN
    }

    /** 帧到达时按当前 yaw 标记方位桶，返回覆盖率 [0,1]。 */
    fun markFrame(): Float {
        if (!active) return 0f
        val norm = ((yaw % TWO_PI) + TWO_PI) % TWO_PI
        // 运动门限：yaw 从上次计桶点转过至少 4° 才计入新方位桶。
        // 静止时磁力计/传感器噪声会让 yaw 微漂移，旧版因此"没转也涨覆盖"=乱报。
        if (!lastMarkedYaw.isNaN()) {
            var d = kotlin.math.abs(norm - lastMarkedYaw)
            if (d > PI) d = TWO_PI - d
            if (d < MIN_YAW_STEP) return coverage()
        }
        val idx = ((norm / TWO_PI) * BUCKETS).toInt() % BUCKETS
        if (!buckets[idx]) { buckets[idx] = true; covered++ }
        lastMarkedYaw = norm
        return coverage()
    }

    fun coverage(): Float = if (active) covered.toFloat() / BUCKETS else 0f
    fun bucketStates(): BooleanArray = buckets.copyOf()
    fun currentYaw(): Double = yaw

    override fun onSensorChanged(e: SensorEvent) {
        if (e.sensor.type != Sensor.TYPE_ROTATION_VECTOR || e.values.size < 4) return
        // 旋转矢量四元数 (x,y,z,w) → 世界坐标 yaw（绕竖直轴方位角）
        val x = e.values[0].toDouble(); val y = e.values[1].toDouble()
        val z = e.values[2].toDouble(); val w = e.values[3].toDouble()
        // 由旋转矩阵第二行第一列/第二列求 azimuth（Android getRotationMatrixFromVector 等价）
        val siny = 2.0 * (w * z + x * y)
        val cosy = 1.0 - 2.0 * (y * y + z * z)
        yaw = atan2(siny, cosy)
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit

    companion object {
        const val BUCKETS = 36
        const val TWO_PI = 2.0 * PI
        val MIN_YAW_STEP = Math.toRadians(4.0)
    }
}
