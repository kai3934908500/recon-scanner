// SPDX-License-Identifier: MIT
//
// U2NetDetector.kt — U²-Netp 显著性物体分割（端侧 ONNX 推理）
//
// 用途：真正"识别扫描目标"——U²-Netp 是显著性物体检测（salient object
// detection）模型，自动找出画面主体，对人/杯子/键盘/任意摆件都有效。
// 替代旧的纯算术亮度差分（那个乱晃就 100%、认不出真物体）。
//
// 模型：u2netp.onnx（4.4MB，MIT），输入 [1,3,320,320] NCHW RGB，
//       归一化 mean=[0.485,0.456,0.406] std=[0.229,0.224,0.225]，
//       输出 d0 = [1,1,320,320] 显著性 [0,1]。
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
    private var closed = false

    companion object {
        private const val SIZE = 320
        private val MEAN = floatArrayOf(0.485f, 0.456f, 0.406f)
        private val STD = floatArrayOf(0.229f, 0.224f, 0.225f)
    }

    init {
        var s: OrtSession? = null
        var name = "input.1"
        try {
            val model = context.assets.open("u2netp.onnx").readBytes()
            val opt = OrtSession.SessionOptions().apply {
                setOptimizationLevel(OrtSession.SessionOptions.OptLevel.ALL_OPT)
                // 线程数用 ORT 默认（该版本 SessionOptions 不暴露 setNumThreads）
            }
            s = env.createSession(model, opt)
            // 取真实输入名（官方是 input.1，但以模型为准）
            name = s.inputNames.iterator().next()
        } catch (e: Exception) {
            Log.e("U2NetDetector", "模型加载失败: ${e.message}", e)
            s?.close()
            s = null
        }
        Log.i("U2NetDetector", "初始化: session=${s != null}, input=$name")
        session = s
        inputName = name
    }

    val available: Boolean get() = session != null && !closed

    /**
     * 检测结果：64×64 前景 mask + 前景占比。
     * @param bgra BGRA8 像素（相机帧）
     * @return mask 为 null 表示未识别到显著物体。
     */
    data class Result(val mask: BooleanArray?, val ratio: Float)

    private var loggedOnce = false
    fun detect(bgra: ByteArray, w: Int, h: Int): Result {
        val sess = session ?: return Result(null, 0f)
        if (closed || w <= 0 || h <= 0 || bgra.size < w * h * 4) return Result(null, 0f)
        try {
            // 1. BGRA → 320×320 RGB float，归一化 NCHW
            val input = FloatArray(SIZE * SIZE * 3)
            val plane = SIZE * SIZE
            for (y in 0 until SIZE) {
                val sy = (y.toFloat() / SIZE * h).toInt().coerceIn(0, h - 1)
                for (x in 0 until SIZE) {
                    val sx = (x.toFloat() / SIZE * w).toInt().coerceIn(0, w - 1)
                    val i = (sy * w + sx) * 4
                    val dst = y * SIZE + x
                    // BGRA → RGB，除以 255，归一化
                    input[dst] = (bgra[i + 2].toFloat() / 255f - MEAN[0]) / STD[0]
                    input[plane + dst] = (bgra[i + 1].toFloat() / 255f - MEAN[1]) / STD[1]
                    input[2 * plane + dst] = (bgra[i].toFloat() / 255f - MEAN[2]) / STD[2]
                }
            }
            // 2. 推理
            val tensor = OnnxTensor.createTensor(env, FloatBuffer.wrap(input),
                longArrayOf(1, 3, SIZE.toLong(), SIZE.toLong()))
            val outputs = sess.run(mapOf(inputName to tensor))
            tensor.close()
            // ORT Java 对 float[1][1][320][320] 返回 float[][][][]（原始多维数组），
            // Kotlin 里强转 Array<Array<Array<FloatArray>>>。旧的三层 Array<*> 解包
            // 类型不匹配，静默返回 null → 永远"未识别"。
            val raw = outputs.get(0).value
            // ORT 返回的 float[][][][] 各层可能是原始数组或 Object[]，
            // 用反射递归下钻到基本类型 float[]，类型完全无关，最稳。
            val d0: FloatArray? = flattenToFloatArray(raw)
            outputs.close()
            if (d0 == null || d0.size < SIZE * SIZE) {
                if (!loggedOnce) { Log.e("U2NetDetector", "输出张量解析失败 raw=${raw?.javaClass?.name}"); loggedOnce = true }
                return Result(null, 0f)
            }
            if (!loggedOnce) { Log.i("U2NetDetector", "推理OK size=${d0.size} outputType=${raw?.javaClass?.name}"); loggedOnce = true }

            // 3. 320×320 显著性 → 64×64 前景 mask（阈值 0.5）
            val mask = BooleanArray(64 * 64)
            var fg = 0
            for (gy in 0 until 64) for (gx in 0 until 64) {
                // 下采样：取 320 图中对应块中心
                val sx = (gx * SIZE / 64) + (SIZE / 128)
                val sy = (gy * SIZE / 64) + (SIZE / 128)
                val v = d0[sy * SIZE + sx]
                if (v >= 0.5f) { mask[gy * 64 + gx] = true; fg++ }
            }
            val ratio = fg.toFloat() / (64 * 64)
            // 前景太少（无物体）或太多（贴脸/纯色误判）→ 如实未识别
            if (ratio < 0.02f || ratio > 0.90f) return Result(null, ratio)
            return Result(mask, ratio)
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
