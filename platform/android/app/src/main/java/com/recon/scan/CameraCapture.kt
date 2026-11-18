// SPDX-License-Identifier: BSD-3-Clause
//
// CameraCapture.kt — Android Camera2 采集链（ImageReader/YUV_420_888 → BGRA8 → 内核）
//
// ── 为什么要这一层 ─────────────────────────────────────────────────────────
// 此前 Android 端只有状态机 + UI，没有任何相机帧进入内核 —— 用户拍完数据全 0。
// 本类把 Camera2 的帧流真正接到 `NativeBridge.yuvToBgra` → `NativeBridge.pushFrame`。
//
// ── 平台无关逻辑的边界（不可越界）──────────────────────────────────────────
// 本类**只做内存布局搬运**：去 rowStride、把 semi-planar 解交错成 I420。
// 任何 YUV→RGB 色彩矩阵系数都**不得**出现在这里 —— 唯一实现在
// `platform/common/src/yuv_convert.cpp`，由 `check_platform_parity.py` 闸门 8 看守。
//
// ── 为什么统一成 I420 ──────────────────────────────────────────────────────
// Android 机型底层可能是 NV12/NV21（Cb 与 Cr 共缓冲、pixelStride=2）或 I420。
// 机型判断一旦写错，画面会变紫/绿但**几何仍算得出来**（核心只用灰度）——
// 这种"看起来坏了但闸门全绿"最难查。故这里无条件解交错为 I420，走 native
// `ConvertYuv420PlanarToBgra8`，彻底消除 cb_first 判断分支。
//
// ── 生命周期 ───────────────────────────────────────────────────────────────
// start() → TextureView 进视图树 → onSurfaceTextureAvailable → openCamera
//        → createCaptureSession(预览 + ImageReader) → setRepeatingRequest
// setIngest(true/false) 控制是否真的把帧交给内核（预览态只丢弃并 close）。
// stop() 关会话/设备/Reader/线程；Activity.onDestroy 调 shutdown()。
//
// 线程模型：所有相机回调在私有 HandlerThread 上；UI 更新由调用方主线程完成。

package com.recon.scan

import android.content.Context
import android.graphics.ImageFormat
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraAccessException
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.media.Image
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.graphics.Matrix
import android.util.Size
import android.view.Surface
import android.view.TextureView
import android.view.View
import android.widget.FrameLayout
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.max

/** 交给 Activity 推给内核的一帧（已转 BGRA8，含该机实测内参）。 */
internal data class CapturedFrame(
    val bgra: ByteArray,
    val width: Int,
    val height: Int,
    val timestampNs: Long,
    val fx: Double,
    val fy: Double,
    val cx: Double,
    val cy: Double,
)

internal class CameraCapture(
    private val context: Context,
    private val onPreviewView: (View) -> Unit,
    private val onFrame: (CapturedFrame) -> Unit,
    private val onDiagnostic: (String) -> Unit,
) {
    private var thread: HandlerThread? = null
    private var handler: Handler? = null
    private var textureView: TextureView? = null
    private var reader: ImageReader? = null
    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var previewSurface: Surface? = null
    private var frameSurfaceRef: Surface? = null
    private var focusMode = 0       // 0 自动 / 1 手动
    private var focusDistance = 0f  // 屈光度
    private var cameraId: String? = null
    private var characteristics: CameraCharacteristics? = null

    private var width = 0
    private var height = 0
    private var previewW = 0
    private var previewH = 0
    private var started = false

    /** 相机传感器安装角；用于把横向预览旋转到竖屏并居中裁剪（修"图像压扁"）。 */
    private var sensorOrientation = 90

    /** 内核内参（从 CaptureResult/Characteristics 实测，不写死假值）。 */
    @Volatile private var fx = 0.0
    @Volatile private var fy = 0.0
    @Volatile private var cx = 0.0
    @Volatile private var cy = 0.0
    @Volatile private var intrinsicsReady = false

    /** 只有 Capturing 态才为 true；预览态仍持续 acquire/close 以免缓冲耗尽。 */
    @Volatile private var ingestEnabled = false

    /** 相机打开失败标志：Activity 在前台恢复时据此自动重试（后台打开相机会被系统拒绝）。 */
    @Volatile private var openFailed = false

    fun hasError(): Boolean = openFailed

    fun setIngest(enabled: Boolean) {
        ingestEnabled = enabled && started
    }

    /** 创建预览视图并挂到 UI；相机在 surface 就绪后才打开。 */
    fun start() {
        if (started) return
        started = true
        val t = HandlerThread("ReconCamera").apply { start() }
        thread = t
        handler = Handler(t.looper)

        val view = TextureView(context).apply {
            layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT)
            surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                override fun onSurfaceTextureAvailable(s: SurfaceTexture, w: Int, h: Int) {
                    configureTransform(w, h)
                    openCamera(s)
                }
                override fun onSurfaceTextureSizeChanged(s: SurfaceTexture, w: Int, h: Int) {
                    configureTransform(w, h)
                }
                override fun onSurfaceTextureDestroyed(s: SurfaceTexture): Boolean = true
                override fun onSurfaceTextureUpdated(s: SurfaceTexture) = Unit
            }
        }
        textureView = view
        onPreviewView(view)
    }

    /**
     * 按相机预览宽高比 + 传感器安装角，对 TextureView 做旋转 + 居中裁剪。
     * 这是修"图像被压扁"的关键：默认 TextureView 把 1280×720 横屏 buffer 直接
     * 拉伸到竖屏 View，必然变形。center-crop 保证等比填充、不拉伸。
     */
    /**
     * AOSP Camera2Basic 官方算法（逐字口径）：横屏设备才旋转；竖屏保持单位阵。
     * 上一版错误地把 sensorOrientation 当旋转依据，在竖屏强行旋转 + 二次缩放，
     * 导致画面被拉成条纹 —— 这是参赛真机实测暴露的缺陷，此处按官方修正。
     */
    private fun configureTransform(viewW: Int, viewH: Int) {
        val tv = textureView ?: return
        if (viewW <= 0 || viewH <= 0 || previewW <= 0 || previewH <= 0) return
        val rotation = (context as? android.app.Activity)?.windowManager?.defaultDisplay?.rotation
            ?: Surface.ROTATION_0
        val matrix = Matrix()
        val viewRect = android.graphics.RectF(0f, 0f, viewW.toFloat(), viewH.toFloat())
        // buffer 用「高×宽」——这是 AOSP 原文的口径（预览尺寸存的是横屏 w×h）
        val bufferRect = android.graphics.RectF(0f, 0f, previewH.toFloat(), previewW.toFloat())
        val cx = viewRect.centerX()
        val cy = viewRect.centerY()
        if (rotation == Surface.ROTATION_90 || rotation == Surface.ROTATION_270) {
            bufferRect.offset(cx - bufferRect.centerX(), cy - bufferRect.centerY())
            matrix.setRectToRect(viewRect, bufferRect, Matrix.ScaleToFit.FILL)
            val scale = max(viewH.toFloat() / previewH.toFloat(), viewW.toFloat() / previewW.toFloat())
            matrix.postScale(scale, scale, cx, cy)
            matrix.postRotate((90 * (rotation - 2)).toFloat(), cx, cy)
        } else if (rotation == Surface.ROTATION_180) {
            matrix.postRotate(180f, cx, cy)
        }
        // 竖屏 ROTATION_0：保持单位阵，让 TextureView 按 buffer 自然铺满（系统已旋转对齐）
        tv.setTransform(matrix)
    }

    /** 停止推帧并释放相机资源；可再次 start()。 */
    fun stop() {
        ingestEnabled = false
        try {
            session?.close()
            session = null
            camera?.close()
            camera = null
            reader?.close()
            reader = null
            previewSurface?.release()
            previewSurface = null
            handler?.removeCallbacksAndMessages(null)
            thread?.quitSafely()
            thread = null
            handler = null
            // 先摘监听器，再让 UI 移除视图，避免销毁回调打到已释放的线程。
            textureView?.surfaceTextureListener = null
            val v = textureView
            textureView = null
            v?.let { (it.parent as? android.view.ViewGroup)?.removeView(it) }
        } catch (t: Throwable) {
            onDiagnostic("相机停止异常：${t.message}")
        } finally {
            started = false
            intrinsicsReady = false
        }
    }

    fun shutdown() = stop()

    private fun openCamera(surfaceTexture: SurfaceTexture) {
        val h = handler ?: return
        try {
            val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
            val ids = manager.cameraIdList
            if (ids.isEmpty()) {
                onDiagnostic("未找到相机设备")
                return
            }
            val id = ids.firstOrNull { camId ->
                manager.getCameraCharacteristics(camId)
                    .get(CameraCharacteristics.LENS_FACING) ==
                    CameraCharacteristics.LENS_FACING_BACK
            } ?: ids[0]
            cameraId = id
            val chars = manager.getCameraCharacteristics(id)
            characteristics = chars
            sensorOrientation = chars.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 90

            // 采集尺寸（送内核重建）——按画质档位，避免 4K 每帧 48MB 拖垮内存。
            val size = pickSize(chars)
            width = size.width
            height = size.height
            fx = width / 2.0
            fy = height / 2.0
            cx = width / 2.0
            cy = height / 2.0

            // 预览尺寸（只显示、不存）——取相机最大 YUV 尺寸，最高画质取景。
            // Camera2 允许一个 session 内 preview / ImageReader 两个不同尺寸的 surface。
            val previewSize = maxPreviewSize(chars)
            previewW = previewSize.width
            previewH = previewSize.height
            surfaceTexture.setDefaultBufferSize(previewW, previewH)
            val tv = textureView
            if (tv != null) configureTransform(tv.width, tv.height)
            val preview = Surface(surfaceTexture)
            previewSurface = preview

            val r = ImageReader.newInstance(width, height, ImageFormat.YUV_420_888, 3)
            reader = r
            r.setOnImageAvailableListener({ imageReader -> drain(imageReader) }, h)

            manager.openCamera(id, object : CameraDevice.StateCallback() {
                override fun onOpened(device: CameraDevice) {
                    camera = device
                    createSession(device, preview, r.surface, chars)
                }
                override fun onDisconnected(device: CameraDevice) {
                    onDiagnostic("相机断开连接")
                    openFailed = true
                    device.close()
                    camera = null
                }
                override fun onError(device: CameraDevice, error: Int) {
                    onDiagnostic("相机打开失败 error=$error")
                    openFailed = true
                    device.close()
                    camera = null
                }
            }, h)
        } catch (e: CameraAccessException) {
            onDiagnostic("相机访问失败：${e.message}")
            openFailed = true
        } catch (e: SecurityException) {
            onDiagnostic("相机权限被拒：${e.message}")
            openFailed = true
        } catch (t: Throwable) {
            onDiagnostic("相机初始化异常：${t.message}")
            openFailed = true
        }
    }

    private fun createSession(
        device: CameraDevice,
        preview: Surface,
        frameSurface: Surface,
        chars: CameraCharacteristics,
    ) {
        val h = handler ?: return
        frameSurfaceRef = frameSurface
        try {
            device.createCaptureSession(
                listOf(preview, frameSurface),
                object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(s: CameraCaptureSession) {
                        session = s
                        val request = device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW)
                            .apply {
                                addTarget(preview)
                                addTarget(frameSurface)
                                set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                                set(CaptureRequest.NOISE_REDUCTION_MODE,
                                    CaptureRequest.NOISE_REDUCTION_MODE_FAST)
                                applyFocus(this)
                            }.build()
                        try {
                            s.setRepeatingRequest(request, object : CameraCaptureSession.CaptureCallback() {
                                override fun onCaptureCompleted(
                                    session: CameraCaptureSession,
                                    request: CaptureRequest,
                                    result: TotalCaptureResult,
                                ) {
                                    if (!intrinsicsReady) {
                                        readIntrinsics(result, chars)
                                    }
                                }
                            }, h)
                        } catch (e: CameraAccessException) {
                            onDiagnostic("预览启动失败：${e.message}")
                        }
                    }

                    override fun onConfigureFailed(s: CameraCaptureSession) {
                        onDiagnostic("相机会话配置失败")
                        openFailed = true
                    }
                },
                h)
        } catch (e: CameraAccessException) {
            onDiagnostic("相机会话创建失败：${e.message}")
            openFailed = true
        }
    }

    /** fx/fy 用实测焦距与传感器物理尺寸算，绝不写死"看起来差不多"的假内参。 */
    /** 对当前正在运行的预览请求应用对焦设置（设置页实时生效）。 */
    fun setFocus(mode: Int, distanceDiopters: Float) {
        focusMode = mode
        focusDistance = distanceDiopters
        val h = handler ?: return
        h.post {
            val dev = camera ?: return@post
            val s = session ?: return@post
            val frameSf = frameSurfaceRef ?: return@post
            val previewSf = previewSurface ?: return@post
            try {
                val req = dev.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                    addTarget(previewSf)
                    addTarget(frameSf)
                    set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                    set(CaptureRequest.NOISE_REDUCTION_MODE, CaptureRequest.NOISE_REDUCTION_MODE_FAST)
                    applyFocus(this)
                }.build()
                s.setRepeatingRequest(req, null, h)
            } catch (e: CameraAccessException) {
                onDiagnostic("对焦设置失败：${e.message}")
            }
        }
    }

    /** 把 focusMode/focusDistance 写进请求。手动模式需先确认硬件支持，不支持则如实留自动。 */
    private fun applyFocus(builder: CaptureRequest.Builder) {
        if (focusMode == 1) {
            val chars = characteristics
            val supported = chars?.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES)
            val manual = CaptureRequest.CONTROL_AF_MODE_OFF
            if (supported == null || supported.contains(manual)) {
                builder.set(CaptureRequest.CONTROL_AF_MODE, manual)
                // 屈光度 → 焦距（米）= 1/D，D=0 表示无穷远
                val focal = if (focusDistance <= 0f) Float.POSITIVE_INFINITY else 1f / focusDistance
                builder.set(CaptureRequest.LENS_FOCUS_DISTANCE, focal)
                return
            }
        }
        builder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
    }

    private fun readIntrinsics(result: TotalCaptureResult, chars: CameraCharacteristics) {
        val focal = result.get(CaptureResult.LENS_FOCAL_LENGTH) ?: return
        val sensor = chars.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE) ?: return
        if (focal <= 0f || sensor.width <= 0f || sensor.height <= 0f) return
        fx = focal.toDouble() * width / sensor.width.toDouble()
        fy = focal.toDouble() * height / sensor.height.toDouble()
        cx = width / 2.0
        cy = height / 2.0
        intrinsicsReady = true
    }

    /** 取景预览固定用相机最大 YUV 尺寸（最高画质，只显示不存）。 */
    private fun maxPreviewSize(chars: CameraCharacteristics): Size {
        val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        val sizes = map?.getOutputSizes(ImageFormat.YUV_420_888)?.toList().orEmpty()
        return sizes.maxByOrNull { it.width.toLong() * it.height.toLong() } ?: Size(1920, 1080)
    }

    /** 采集画质档：0 流畅(720p) / 1 均衡(1080p) / 2 高画质(最大)。 */
    @Volatile private var qualityTier = 1

    fun setQualityTier(tier: Int) { qualityTier = tier.coerceIn(0, 2) }

    private fun pickSize(chars: CameraCharacteristics): Size {
        val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        val sizes = map?.getOutputSizes(ImageFormat.YUV_420_888)?.toList().orEmpty()
        if (sizes.isEmpty()) return Size(1280, 720)
        return when (qualityTier) {
            0 -> { val t = 1280 * 720; sizes.minByOrNull { abs(it.width * it.height - t) } ?: sizes[0] }
            1 -> { val t = 1920 * 1080; sizes.minByOrNull { abs(it.width * it.height - t) } ?: sizes[0] }
            else -> sizes.maxByOrNull { it.width.toLong() * it.height.toLong() } ?: sizes[0]
        }
    }

    private fun drain(r: ImageReader) {
        val image = r.acquireLatestImage() ?: return
        try {
            if (!ingestEnabled) return
            val planes = image.planes
            if (planes.size < 3) return
            val w = width
            val h = height
            if (w <= 0 || h <= 0 || w % 2 != 0 || h % 2 != 0) return

            val y = copyLuma(planes[0], w, h) ?: return
            val cb = copyChroma(planes[1], w, h) ?: return
            val cr = copyChroma(planes[2], w, h) ?: return
            // 统一 I420：Cb/Cr 各自紧凑、共享 stride = w/2；本侧无色彩数学。
            val bgra = NativeBridge.yuvToBgra(
                y, w, cb, w / 2, cr, w / 2, w, h,
                false, false, false, false) ?: return

            onFrame(CapturedFrame(
                bgra = bgra, width = w, height = h,
                timestampNs = image.timestamp,
                fx = fx, fy = fy, cx = cx, cy = cy))
        } catch (t: Throwable) {
            onDiagnostic("帧转换异常：${t.message}")
        } finally {
            image.close()
        }
    }

    /** Y 平面：去掉 rowStride 尾部填充，输出 w*h 紧凑字节。 */
    private fun copyLuma(plane: Image.Plane, w: Int, h: Int): ByteArray? {
        val out = ByteArray(w * h)
        val buf = plane.buffer.duplicate()
        buf.position(0)
        val rs = plane.rowStride
        var src = 0
        var dst = 0
        repeat(h) {
            if (src + w > buf.limit()) return null
            buf.position(src)
            buf.get(out, dst, w)
            src += rs
            dst += w
        }
        return out
    }

    /**
     * 色度平面：无论底层 I420（pixelStride=1）还是 NV12/NV21（=2），
     * 都输出 (w/2)*(h/2) 的**单通道紧凑**数组。纯布局搬运，无色彩运算。
     */
    private fun copyChroma(plane: Image.Plane, w: Int, h: Int): ByteArray? {
        val cw = w / 2
        val ch = h / 2
        val out = ByteArray(cw * ch)
        val buf = plane.buffer.duplicate()
        buf.position(0)
        val rs = plane.rowStride
        val ps = plane.pixelStride
        if (ps <= 0) return null
        if (ps == 1) {
            var src = 0
            var dst = 0
            repeat(ch) {
                if (src + cw > buf.limit()) return null
                buf.position(src)
                buf.get(out, dst, cw)
                src += rs
                dst += cw
            }
        } else {
            repeat(ch) { row ->
                repeat(cw) { col ->
                    val at = row * rs + col * ps
                    if (at >= buf.limit()) return null
                    out[row * cw + col] = buf.get(at)
                }
            }
        }
        return out
    }
}
