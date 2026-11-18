// SPDX-License-Identifier: BSD-3-Clause
//
// ObjectDetector.kt — 物体前景分割（纯本地，零依赖）
//
// 用途：识别画面中央的物体，输出前景 mask（低分辨率布尔网格）。
// HUD 据此沿**真实轮廓**画绿边 + 像涂层一样逐格覆盖，而不是矩形框/方格。
//
// 算法（轻量，64×64 下采样，单帧 <1ms）：
//   1. BGRA → 灰度下采样到 64×64
//   2. 背景色取四角均值；与背景色差异显著的像素 = 前景
//   3. 形态学：保留最大连通主体，去掉散点噪声
//   4. 前景太少/太多如实判"未识别"，不硬凑
//
// 无 ML 模型、无网络——纯算术，符合纯本地铁律。
package com.recon.scan

import kotlin.math.abs

internal class ObjectDetector {
    val gridW = 64
    val gridH = 64
    private val gray = FloatArray(gridW * gridH)
    private val mask = BooleanArray(gridW * gridH)
    private val visited = BooleanArray(gridW * gridH)

    data class Result(val mask: BooleanArray?, val ratio: Float)

    fun detect(bgra: ByteArray, w: Int, h: Int): Result {
        if (w <= 0 || h <= 0 || bgra.size < w * h * 4) return Result(null, 0f)
        for (gy in 0 until gridH) {
            val sy = gy * h / gridH
            for (gx in 0 until gridW) {
                val sx = gx * w / gridW
                val i = (sy * w + sx) * 4
                gray[gy * gridW + gx] = bgra[i] * 0.11f + bgra[i + 1] * 0.59f + bgra[i + 2] * 0.30f
            }
        }
        var bg = 0f; var cnt = 0
        for (gy in 0 until 10) for (gx in 0 until 10) { bg += gray[gy * gridW + gx]; cnt++ }
        for (gy in gridH - 10 until gridH) for (gx in gridW - 10 until gridW) {
            bg += gray[gy * gridW + gx]; cnt++
        }
        bg /= cnt.coerceAtLeast(1)

        val x0 = gridW / 8; val x1 = gridW * 7 / 8
        val y0 = gridH / 8; val y1 = gridH * 7 / 8
        var fg = 0
        for (gy in 0 until gridH) for (gx in 0 until gridW) {
            val inside = gx in x0 until x1 && gy in y0 until y1
            val on = inside && abs(gray[gy * gridW + gx] - bg) > 16f
            mask[gy * gridW + gx] = on
            if (on) fg++
        }
        val ratio = fg.toFloat() / (gridW * gridH)
        if (ratio < 0.02f || ratio > 0.80f) { mask.fill(false); return Result(null, ratio) }

        java.util.Arrays.fill(visited, false)
        var bestSeed = -1; var bestSize = 0
        for (gy in 0 until gridH) for (gx in 0 until gridW) {
            val idx = gy * gridW + gx
            if (mask[idx] && !visited[idx]) {
                val size = floodFill(gx, gy)
                if (size > bestSize) { bestSize = size; bestSeed = idx }
            }
        }
        if (bestSeed < 0 || bestSize < 12) { mask.fill(false); return Result(null, ratio) }

        val keep = BooleanArray(mask.size)
        java.util.Arrays.fill(visited, false)
        floodMark(bestSeed % gridW, bestSeed / gridW, keep)
        System.arraycopy(keep, 0, mask, 0, mask.size)
        return Result(mask.copyOf(), bestSize.toFloat() / (gridW * gridH))
    }

    private fun floodFill(sx: Int, sy: Int): Int {
        val stack = ArrayDeque<Int>()
        stack.add(sy * gridW + sx)
        visited[sy * gridW + sx] = true
        var n = 0
        while (stack.isNotEmpty()) {
            val idx = stack.removeLast()
            n++
            val x = idx % gridW; val y = idx / gridW
            if (x > 0) visit(idx - 1, stack)
            if (x < gridW - 1) visit(idx + 1, stack)
            if (y > 0) visit(idx - gridW, stack)
            if (y < gridH - 1) visit(idx + gridW, stack)
        }
        return n
    }

    private fun floodMark(sx: Int, sy: Int, out: BooleanArray) {
        val stack = ArrayDeque<Int>()
        stack.add(sy * gridW + sx)
        visited[sy * gridW + sx] = true
        while (stack.isNotEmpty()) {
            val idx = stack.removeLast()
            out[idx] = true
            val x = idx % gridW; val y = idx / gridW
            if (x > 0) visit(idx - 1, stack)
            if (x < gridW - 1) visit(idx + 1, stack)
            if (y > 0) visit(idx - gridW, stack)
            if (y < gridH - 1) visit(idx + gridW, stack)
        }
    }

    private fun visit(idx: Int, stack: ArrayDeque<Int>) {
        if (mask[idx] && !visited[idx]) { visited[idx] = true; stack.add(idx) }
    }
}
