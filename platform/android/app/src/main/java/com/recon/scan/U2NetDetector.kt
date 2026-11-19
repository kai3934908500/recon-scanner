// SPDX-License-Identifier: MIT
//
// ObjectSegDetector.kt — 显著性物体分割（端侧 ONNX 推理，模型无关）
//
// 用途：真正"识别扫描目标"——显著性/二值分割模型自动找出画面主体，
// 对人/杯子/键盘/任意摆件都有效，替代旧的纯算术亮度差分。
//
// 支持模型（输入尺寸从 session 自动读取，输出取最后一个张量）：
//   - u2net.onnx（320×320，输出 d0）
//   - BiRefNet-tiny.onnx（1024×1024，输出 matte，sigmoid [0,1]）
// 归一化统一 mean=[0.485,0.456,0.406] std=[0.229,0.224,0.225]。
//
// 纯本地推理、零网络。推理在调用方提供的后台线程执行（不阻塞采集）。
package com.recon.scan

import ai.onnxruntime.OnnxTensor
import ai.onnxruntime.OrtEnvironment
import ai.onnxruntime.OrtSession
import android.content.Context
import java.nio.FloatBuffer
import android.util.Log

internal class U2NetDetector(context: Context) {
    private val env = OrtEnvironment.getEnvironment()
    private val session: OrtSession?
    private val inputName: String
    private var inputH = 320
    private var inputW = 320
    private var needSigmoid = false   // BiRefNet 输出 logits 需 sigmoid；u2net 输出已是 [0,1]
    private var closed = false

    companion object {
        private val MEAN = floatArrayOf(0.485f, 0.456f, 0.406f)
        private val STD = floatArrayOf(0.229f, 0.224f, 0.225f)
    }

    init {
        var s: OrtSession? = null
        var name = "input.1"
        try {
            // 优先加载更高精度的 BiRefNet，缺失时回退 u2net
            val modelName = if (context.assets.list("")?.contains("BiRefNet-tiny.onnx") == true)
                "BiRefNet-tiny.onnx" else "u2net.onnx"
            // 【OOM 修复】绝不能用 assets.readBytes() 把 214MB 模型一次性读进
            // Java heap（heap 上限 384MB，直接 OOM 闪退）。改为流式复制到
            // filesDir，再用文件路径让 ORT native 直接 mmap 加载，heap 峰值几 MB。
            // 按文件名判断：模型名变（换模型）时天然重新复制；同名只复制一次。
            // 不用 openFd()——debug 包 .onnx 被 aapt 压缩，openFd 会失败。
            val localFile = java.io.File(context.filesDir, modelName)
            if (!localFile.exists() || localFile.length() < 1000000L) {
                val tmp = java.io.File(context.filesDir, modelName + ".tmp")
                var copied = 0L
                context.assets.open(modelName).use { input ->
                    java.io.FileOutputStream(tmp).use { out ->
                        val buf = ByteArray(64 * 1024)
                        var n: Int
                        while (input.read(buf).also { n = it } > 0) { out.write(buf, 0, n); copied += n }
                    }
                }
                if (copied > 1000000L) tmp.renameTo(localFile) else tmp.delete()
                Log.i("U2NetDetector", "模型已复制到 filesDir: $copied bytes")
            }
            val opt = OrtSession.SessionOptions().apply {
                setOptimizationLevel(OrtSession.SessionOptions.OptLevel.ALL_OPT)
            }
            s = env.createSession(localFile.absolutePath, opt)
            // 取真实输入名（以模型为准）
            name = s.inputNames.iterator().next()
            // 输入尺寸按模型名设定（ORT Android 1.18 的 shape 查询 API 不稳定，
            // 两个目标模型尺寸已知：BiRefNet=1024²，u2net=320²）。
            if (modelName.startsWith("BiRefNet")) {
                inputH = 1024; inputW = 1024; needSigmoid = true
            } else {
                inputH = 320; inputW = 320; needSigmoid = false
            }
        } catch (e: Exception) {
            Log.e("U2NetDetector", "模型加载失败: ${e.message}", e)
            s?.close()
            s = null
        }
        Log.i("U2NetDetector", "初始化: session=${s != null}, input=$name, size=${inputW}x${inputH}")
        session = s
        inputName = name
    }

    val available: Boolean get() = session != null && !closed

    /**
     * 检测结果：128×128 软前景概率图（保留 U-Net 平滑边缘）+ 前景占比。
     * 软值 [0,1]，渲染时用它画羽化边缘和平滑轮廓，替代旧的 64×64 布尔格子。
     * @return softMask 为 null 表示未识别到显著物体。
     */
    data class Result(val softMask: FloatArray?, val ratio: Float)

    private var loggedOnce = false
    fun detect(bgra: ByteArray, w: Int, h: Int): Result {
        val sess = session ?: return Result(null, 0f)
        if (closed || w <= 0 || h <= 0 || bgra.size < w * h * 4) return Result(null, 0f)
        try {
            // 1. BGRA → 模型输入尺寸 RGB float，归一化 NCHW
            val W = inputW; val H = inputH
            val input = FloatArray(W * H * 3)
            val plane = W * H
            for (y in 0 until H) {
                val sy = (y.toFloat() / H * h).toInt().coerceIn(0, h - 1)
                for (x in 0 until W) {
                    val sx = (x.toFloat() / W * w).toInt().coerceIn(0, w - 1)
                    val i = (sy * w + sx) * 4
                    val dst = y * W + x
                    // BGRA → RGB，除以 255，归一化
                    input[dst] = (bgra[i + 2].toFloat() / 255f - MEAN[0]) / STD[0]
                    input[plane + dst] = (bgra[i + 1].toFloat() / 255f - MEAN[1]) / STD[1]
                    input[2 * plane + dst] = (bgra[i].toFloat() / 255f - MEAN[2]) / STD[2]
                }
            }
            // 2. 推理（计时：判断是否能承受多尺度 TTA）
            val tInfer = System.nanoTime()
            val tensor = OnnxTensor.createTensor(env, FloatBuffer.wrap(input),
                longArrayOf(1, 3, H.toLong(), W.toLong()))
            val outputs = sess.run(mapOf(inputName to tensor))
            tensor.close()
            // ORT Java 对 float[1][1][320][320] 返回 float[][][][]（原始多维数组），
            // Kotlin 里强转 Array<Array<Array<FloatArray>>>。旧的三层 Array<*> 解包
            // 类型不匹配，静默返回 null → 永远"未识别"。
            // BiRefNet 官方 demo 取 result[-1]（最后一个张量才是 matte）；
            // u2net 的 d0 是第 0 个。取最后一个对两者都正确。
            val raw = outputs[outputs.size() - 1].value
            // ORT 返回的 float[][][][] 各层可能是原始数组或 Object[]，
            // 用反射递归下钻到基本类型 float[]，类型完全无关，最稳。
            val d0: FloatArray? = flattenToFloatArray(raw)
            outputs.close()
            if (d0 == null || d0.size < W * H) {
                if (!loggedOnce) { Log.e("U2NetDetector", "输出张量解析失败 raw=${raw?.javaClass?.name}"); loggedOnce = true }
                return Result(null, 0f)
            }
            if (!loggedOnce) {
                Log.i("U2NetDetector", "推理OK size=${d0.size} 耗时=${(System.nanoTime()-tInfer)/1000000}ms ratio待测")
                loggedOnce = true
            }

            // 3. 模型输出 → 128×128 软概率图（格内 2×2 采样均值，保留平滑边缘）。
            // 软值是羽化边缘/平滑轮廓的关键，绝不能再粗暴阈值化成布尔格子。
            val small = 128
            val soft = FloatArray(small * small)
            var fg = 0
            val cellW = (W / small).coerceAtLeast(2)  // 每格宽（320→2, 1024→8）
            val cellH = (H / small).coerceAtLeast(2)
            for (gy in 0 until small) for (gx in 0 until small) {
                val sx0 = gx * cellW; val sy0 = gy * cellH
                var acc = 0f; var cnt = 0
                for (dy in 0..1) for (dx in 0..1) {
                    // 在格子内取 2 个采样点（格宽的 1/3 和 2/3 处）
                    val x = (sx0 + (dx + 1) * cellW / 3).coerceAtMost(W - 1)
                    val y = (sy0 + (dy + 1) * cellH / 3).coerceAtMost(H - 1)
                    acc += d0[y * W + x]; cnt++
                }
                var v = acc / cnt.coerceAtLeast(1)
                // rembg 的 BiRefNet 输出是 logits，需 sigmoid 映射到 [0,1]；
                // u2net 的 d0 已经是概率，直接沿用。
                if (needSigmoid) v = 1f / (1f + kotlin.math.exp(-v))
                soft[gy * small + gx] = v
                if (v >= 0.5f) fg++
            }
            val ratio = fg.toFloat() / (small * small)
            if (ratio < 0.02f || ratio > 0.90f) return Result(null, ratio)
            return Result(soft, ratio)
        } catch (e: Exception) {
            return Result(null, 0f)
        }
    }


    /** 递归展平任意维数组到一整条 float[]（ORT 返回类型多变，反射最稳）。 */
    private fun flattenToFloatArray(v: Any?): FloatArray? {
        if (v == null) return null
        if (v is FloatArray) return v
        if (v.javaClass.isArray) {
            val len = java.lang.reflect.Array.getLength(v)
            val parts = ArrayList<FloatArray>()
            for (i in 0 until len) {
                val part = flattenToFloatArray(java.lang.reflect.Array.get(v, i)) ?: return null
                parts.add(part)
            }
            val total = parts.sumOf { it.size }
            val out = FloatArray(total)
            var off = 0
            for (part in parts) { System.arraycopy(part, 0, out, off, part.size); off += part.size }
            return out
        }
        return null
    }

    fun close() {
        if (!closed) { closed = true; session?.close() }
    }
}
