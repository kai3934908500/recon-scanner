// SPDX-License-Identifier: BSD-3-Clause
//
// MeshPreviewView.kt — 3D 网格线框预览（Android，GLES2 自绘，零依赖）
//
// 与 iOS `platform/ios/Recon/WireframeRenderer.swift` 对等：
//   数据来自 NativeBridge.getPreviewMesh()（C ABI recon_get_preview_mesh），
//   本类只做渲染：MVP 变换 + 线框绘制 + 触摸旋转 + 自动居中缩放。
//
// 契约：
//   · 顶点色=核心原始坐标，不施加 scale_factor（避免叠加缺口 AJ）；
//   · 只画线框，不写回网格、不改导出；
//   · 触摸拖动旋转、双指/单指缩放由外部可选调用 setZoom。
package com.recon.scan

import android.content.Context
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.view.MotionEvent
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.nio.ShortBuffer
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10
import kotlin.math.sqrt

/**
 * 3D 线框预览视图。调用 [setMesh] 传入 positions/indices 后自动渲染。
 * 无网格时显示空（由外层提示"暂无模型"）。
 */
internal class MeshPreviewView(context: Context) : GLSurfaceView(context) {
    private val rendererImpl = MeshRenderer()
    private var lastX = 0f
    private var lastY = 0f

    init {
        setEGLContextClientVersion(2)
        setRenderer(rendererImpl)
        renderMode = RENDERMODE_CONTINUOUSLY
    }

    /** positions: XYZ 交错；indices: 每 3 个一三角面。 */
    fun setMesh(positions: FloatArray?, indices: IntArray?) {
        rendererImpl.setMesh(positions, indices)
    }

    fun clear() = rendererImpl.setMesh(null, null)

    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> { lastX = e.x; lastY = e.y }
            MotionEvent.ACTION_MOVE -> {
                val dx = e.x - lastX
                val dy = e.y - lastY
                rendererImpl.rotate(dx * 0.5f, dy * 0.5f)
                lastX = e.x; lastY = e.y
            }
        }
        return true
    }

    private class MeshRenderer : Renderer {
        @Volatile private var pending: MeshData? = null
        private var mesh: MeshData? = null

        private var program = 0
        private var aPos = 0
        private var uMvp = 0
        private var uColor = 0

        private var angleX = 20f
        private var angleY = 30f
        private var zoom = 1f

        private val mvp = FloatArray(16)
        private val view = FloatArray(16)
        private val proj = FloatArray(16)

        fun setMesh(pos: FloatArray?, idx: IntArray?) {
            pending = if (pos == null || idx == null || pos.isEmpty() || idx.isEmpty()) null
                     else MeshData(pos, idx)
        }

        fun rotate(dxDeg: Float, dyDeg: Float) {
            angleY += dxDeg
            angleX = (angleX + dyDeg).coerceIn(-85f, 85f)
        }

        override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
            GLES20.glClearColor(0.96f, 0.97f, 0.98f, 1f) // 浅灰底，企业风
            GLES20.glEnable(GLES20.GL_DEPTH_TEST)
            program = buildProgram(VERTEX, FRAGMENT)
            aPos = GLES20.glGetAttribLocation(program, "aPosition")
            uMvp = GLES20.glGetUniformLocation(program, "uMVP")
            uColor = GLES20.glGetUniformLocation(program, "uColor")
        }

        override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
            GLES20.glViewport(0, 0, width, height)
            aspect = width.toFloat() / maxOf(height, 1)
        }

        override fun onDrawFrame(gl: GL10?) {
            pending?.let { mesh = it; pending = null }
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT or GLES20.GL_DEPTH_BUFFER_BIT)
            val m = mesh ?: return

            GLES20.glUseProgram(program)
            GLES20.glUniform4f(uColor, 0.15f, 0.39f, 0.92f, 1f) // M3 企业蓝

            // 模型居中 + 归一缩放（核心原始坐标，不施加 scale_factor）
            val cx = (m.minX + m.maxX) / 2f
            val cy = (m.minY + m.maxY) / 2f
            val cz = (m.minZ + m.maxZ) / 2f
            val span = maxOf(m.maxX - m.minX, m.maxY - m.minY, m.maxZ - m.minZ, 0.001f)
            val s = (1.6f / span) * zoom

            val model = FloatArray(16)
            setIdentityM(model)
            translateM(model, -cx, -cy, -cz)
            rotateM(model, angleX, 1f, 0f, 0f)
            rotateM(model, angleY, 0f, 1f, 0f)
            scaleM(model, s, s, s)

            setIdentityM(view)
            translateM(view, 0f, 0f, -3f)
            perspectiveM(proj, 45f, aspect, 0.1f, 100f)
            multiplyMM(mvp, 0, proj, 0, view, 0)
            multiplyMM(mvp, 0, mvp, 0, model, 0)

            GLES20.glUniformMatrix4fv(uMvp, 1, false, mvp, 0)
            GLES20.glVertexAttribPointer(aPos, 3, GLES20.GL_FLOAT, false, 0, m.positions)
            GLES20.glEnableVertexAttribArray(aPos)
            GLES20.glLineWidth(1.5f)
            GLES20.glDrawElements(GLES20.GL_LINES, m.lineCount, GLES20.GL_UNSIGNED_SHORT, m.lines)
        }

        private var aspect = 1f

        private class MeshData(pos: FloatArray, idx: IntArray) {
            val positions: FloatBuffer
            val lines: ShortBuffer
            val lineCount: Int
            var minX = Float.MAX_VALUE; var minY = Float.MAX_VALUE; var minZ = Float.MAX_VALUE
            var maxX = -Float.MAX_VALUE; var maxY = -Float.MAX_VALUE; var maxZ = -Float.MAX_VALUE

            init {
                val bb = ByteBuffer.allocateDirect(pos.size * 4).order(ByteOrder.nativeOrder())
                bb.asFloatBuffer().put(pos).position(0)
                positions = bb.asFloatBuffer()
                var i = 0
                while (i + 2 < pos.size) {
                    minX = minOf(minX, pos[i]); maxX = maxOf(maxX, pos[i])
                    minY = minOf(minY, pos[i+1]); maxY = maxOf(maxY, pos[i+1])
                    minZ = minOf(minZ, pos[i+2]); maxZ = maxOf(maxZ, pos[i+2])
                    i += 3
                }
                // 每个三角面 3 条边；去重后转 GL_UNSIGNED_SHORT（顶点数上限 65535，预览足够）
                val edgeSet = HashSet<Long>()
                val edgeList = ArrayList<Int>()
                var k = 0
                while (k + 2 < idx.size) {
                    val a = idx[k]; val b = idx[k+1]; val c = idx[k+2]
                    addEdge(a, b, edgeSet, edgeList)
                    addEdge(b, c, edgeSet, edgeList)
                    addEdge(c, a, edgeSet, edgeList)
                    k += 3
                }
                lineCount = minOf(edgeList.size, 65535 * 2)
                val lb = ByteBuffer.allocateDirect(lineCount * 2).order(ByteOrder.nativeOrder())
                val shortArr = ShortArray(lineCount)
                for (j in 0 until lineCount) shortArr[j] = edgeList[j].toShort()
                lb.asShortBuffer().put(shortArr).position(0)
                lines = lb.asShortBuffer()
            }
            private fun addEdge(a: Int, b: Int, set: HashSet<Long>, out: ArrayList<Int>) {
                val lo = minOf(a, b); val hi = maxOf(a, b)
                val key = (lo.toLong() shl 32) or (hi.toLong() and 0xffffffffL)
                if (set.add(key) && out.size + 2 <= 65535 * 2) { out.add(lo); out.add(hi) }
            }
        }

        companion object {
            const val VERTEX = """
                attribute vec3 aPosition;
                uniform mat4 uMVP;
                void main() { gl_Position = uMVP * vec4(aPosition, 1.0); }
            """
            const val FRAGMENT = """
                precision mediump float;
                uniform vec4 uColor;
                void main() { gl_FragColor = uColor; }
            """
            fun buildProgram(vs: String, fs: String): Int {
                val v = GLES20.glCreateShader(GLES20.GL_VERTEX_SHADER).also {
                    GLES20.glShaderSource(it, vs); GLES20.glCompileShader(it) }
                val f = GLES20.glCreateShader(GLES20.GL_FRAGMENT_SHADER).also {
                    GLES20.glShaderSource(it, fs); GLES20.glCompileShader(it) }
                return GLES20.glCreateProgram().also {
                    GLES20.glAttachShader(it, v); GLES20.glAttachShader(it, f); GLES20.glLinkProgram(it) }
            }
            fun setIdentityM(m: FloatArray) {
                m.fill(0f); m[0]=1f; m[5]=1f; m[10]=1f; m[15]=1f
            }
            fun translateM(m: FloatArray, x: Float, y: Float, z: Float) {
                m[12]=m[0]*x+m[4]*y+m[8]*z+m[12]
                m[13]=m[1]*x+m[5]*y+m[9]*z+m[13]
                m[14]=m[2]*x+m[6]*y+m[10]*z+m[14]
            }
            fun rotateM(m: FloatArray, angleDeg: Float, x: Float, y: Float, z: Float) {
                val r = Math.toRadians(angleDeg.toDouble()).toFloat()
                val c = kotlin.math.cos(r); val s = kotlin.math.sin(r)
                // 简化绕 X / 绕 Y（本类只用这两个轴）
                if (x == 1f) {
                    val m5=m[5]; val m6=m[6]; val m9=m[9]; val m10=m[10]
                    m[5]=c*m5+s*m9; m[6]=c*m6+s*m10; m[9]=-s*m5+c*m9; m[10]=-s*m6+c*m10
                } else if (y == 1f) {
                    val m0=m[0]; val m2=m[2]; val m8=m[8]; val m10b=m[10]
                    m[0]=c*m0-s*m8; m[2]=c*m2-s*m10b; m[8]=s*m0+c*m8; m[10]=s*m2+c*m10b
                }
            }
            fun scaleM(m: FloatArray, x: Float, y: Float, z: Float) {
                m[0]*=x; m[1]*=x; m[2]*=x; m[3]*=x
                m[4]*=y; m[5]*=y; m[6]*=y; m[7]*=y
                m[8]*=z; m[9]*=z; m[10]*=z; m[11]*=z
            }
            fun perspectiveM(m: FloatArray, fovy: Float, asp: Float, near: Float, far: Float) {
                m.fill(0f)
                val f = 1f / kotlin.math.tan(Math.toRadians(fovy.toDouble()).toFloat()/2f)
                m[0]=f/asp; m[5]=f; m[10]=(far+near)/(near-far); m[11]=-1f
                m[14]=(2f*far*near)/(near-far)
            }
            fun multiplyMM(out: FloatArray, oi: Int, a: FloatArray, ai: Int, b: FloatArray, bi: Int) {
                val tmp = FloatArray(16)
                for (c in 0..3) for (r in 0..3) {
                    var sum = 0f
                    for (k in 0..3) sum += a[ai+k*4+r] * b[bi+c*4+k]
                    tmp[c*4+r] = sum
                }
                System.arraycopy(tmp, 0, out, oi, 16)
            }
        }
    }
}
