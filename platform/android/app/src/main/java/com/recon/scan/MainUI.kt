// SPDX-License-Identifier: BSD-3-Clause
//
// MainUI.kt — 企业级扫描工作台主界面（Android，程序化视图）
//
// 设计目标（参赛作品 · 企业风，非液态玻璃）：
//   · 深色专业配色 + 克制的强调色，信息层级清晰，绝不"AI 化廉价感"；
//   · 顶部品牌栏 + 状态胶囊、四段式流程进度条、分区卡片、统计磁贴、底部主操作栏；
//   · 相机预览铺满底层，信息面板以半透明卡片叠加，采集时既能看清物体又可读数据。
//
// 为什么仍程序化构建（不引入 Material / XML layout）：
//   1. 零新依赖、零 AAPT 资源类型风险 —— 把编译失败面降到最小；
//   2. 全部用 Android framework 基础 API + 自绘 Drawable（圆角/渐变/描边），
//      在 API 24..35 上行为一致；
//   3. 视图只负责呈现，不含状态判断（规则本体在 platform/common 状态机）。
//
// 契约：docs/09_UI规格.md §4.1/§4.2（采集状态机与各态界面）。
package com.recon.scan

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Shader
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import kotlin.math.roundToInt

/**
 * 主界面视图树。由 [CaptureActivity] 持有并驱动。
 *
 * 只负责**呈现**：状态文案、流程进度、能力、统计、结果、按钮可见性由 Activity
 * 调用 [render] 更新；按钮点击经 [onAction] 回调翻译成状态机事件。
 */
internal class MainUI(
    private val context: Context,
    private val onAction: (Action) -> Unit,
) {
    enum class Action {
        START, FINISH, RECONSTRUCT, CONTINUE,
        EXPORT_GLB, EXPORT_STL, RECAPTURE, RETRY, HOME,
    }

    // ---- 企业级配色（克制、专业、高对比）--------------------------------
    private object C {
        val BG_TOP = Color.rgb(0x0B, 0x11, 0x17)
        val BG_BOTTOM = Color.rgb(0x12, 0x1B, 0x24)
        val SURFACE = Color.argb(0xE6, 0x14, 0x1E, 0x28)
        val SURFACE_SOLID = Color.rgb(0x14, 0x1E, 0x28)
        val BORDER = Color.argb(0x80, 0x2A, 0x3A, 0x48)
        val TEXT_PRIMARY = Color.rgb(0xF2, 0xF6, 0xF9)
        val TEXT_SECOND = Color.rgb(0x9F, 0xB2, 0xC2)
        val TEXT_MUTED = Color.rgb(0x6E, 0x82, 0x92)
        val ACCENT = Color.rgb(0x2E, 0xD6, 0xBF)
        val ACCENT_DARK = Color.rgb(0x12, 0x8F, 0x86)
        val ACCENT_SOFT = Color.argb(0x24, 0x2E, 0xD6, 0xBF)
        val WARN = Color.rgb(0xF5, 0xA6, 0x3D)
        val DANGER = Color.rgb(0xEF, 0x6A, 0x5E)
        val OK = Color.rgb(0x3D, 0xD6, 0x8F)
    }

    // ---- 视图引用 ---------------------------------------------------------
    val root: FrameLayout
    private val previewHost: FrameLayout
    private val statusDot: DotView
    private val statusText: TextView
    private val stepper: StepperView
    private val hintText: TextView
    private val progressWrap: LinearLayout
    private val progressBar: ProgressBarView
    private val progressText: TextView
    private val capCard: Card
    private val capBody: LinearLayout
    private val statCard: Card
    private val statGrid: LinearLayout
    private val resultCard: Card
    private val resultBody: LinearLayout
    private val diagText: TextView
    private val buttonBar: LinearLayout
    private val buttons: Map<Action, Button>

    private val dp = { v: Int -> (v * context.resources.displayMetrics.density).roundToInt() }

    init {
        root = FrameLayout(context).apply {
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT)
            setBackgroundColor(C.BG_TOP)
        }

        // 1) 相机预览铺满底层
        previewHost = FrameLayout(context).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT)
            background = verticalGradient(C.BG_TOP, C.BG_BOTTOM)
        }
        root.addView(previewHost)

        // 2) 信息面板覆盖层（顶部到底部的暗化渐变，保证文字可读）
        val overlay = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT)
            background = scrimGradient()
        }
        root.addView(overlay)

        // ---- 顶部品牌栏 ----
        val header = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(dp(18), dp(16), dp(18), dp(6))
            }
        }
        header.addView(LogoMark(context, dp(44)).apply {
            layoutParams = LinearLayout.LayoutParams(dp(44), dp(44))
        })
        val titleCol = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                setMargins(dp(12), 0, dp(8), 0)
            }
        }
        titleCol.addView(TextView(context).apply {
            text = context.getString(R.string.ui_title)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(24).toFloat())
            setTextColor(C.TEXT_PRIMARY)
            typeface = android.graphics.Typeface.DEFAULT_BOLD
            setLetterSpacingCompat(0.06f)
        })
        titleCol.addView(TextView(context).apply {
            text = context.getString(R.string.ui_subtitle)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(11).toFloat())
            setTextColor(C.TEXT_SECOND)
        })
        header.addView(titleCol)

        // 状态胶囊
        val statusPill = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = pillBackground(C.SURFACE_SOLID, C.BORDER)
            setPadding(dp(12), dp(8), dp(12), dp(8))
        }
        statusDot = DotView(context, C.ACCENT).apply {
            layoutParams = LinearLayout.LayoutParams(dp(8), dp(8)).apply { setMargins(0, 0, dp(7), 0) }
        }
        statusPill.addView(statusDot)
        statusText = TextView(context).apply {
            text = context.getString(R.string.state_idle)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(12).toFloat())
            setTextColor(C.TEXT_PRIMARY)
            typeface = android.graphics.Typeface.DEFAULT_BOLD
        }
        statusPill.addView(statusText)
        header.addView(statusPill)
        overlay.addView(header)

        // ---- 四段式流程进度条 ----
        stepper = StepperView(context, dp).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(dp(18), dp(10), dp(18), dp(2))
            }
        }
        overlay.addView(stepper)

        // ---- 可滚动内容区 ----
        val scroll = ScrollView(context).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f).apply {
                setMargins(dp(18), dp(8), dp(18), dp(4))
            }
            overScrollMode = View.OVER_SCROLL_NEVER
        }
        val content = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
        }

        // 提示卡（强调左边框）
        hintText = TextView(context).apply {
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(13).toFloat())
            setTextColor(C.TEXT_PRIMARY)
            background = hintBackground()
            setPadding(dp(14), dp(12), dp(14), dp(12))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(0, 0, 0, dp(10))
            }
        }
        content.addView(hintText)

        // 进度（重建态显示）
        progressWrap = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
            background = cardBackground()
            setPadding(dp(14), dp(12), dp(14), dp(12))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(0, 0, 0, dp(10))
            }
        }
        progressText = TextView(context).apply {
            text = context.getString(R.string.ui_progress_label)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(12).toFloat())
            setTextColor(C.ACCENT)
            typeface = android.graphics.Typeface.DEFAULT_BOLD
        }
        progressWrap.addView(progressText)
        progressBar = ProgressBarView(context, dp).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(6)).apply { setMargins(0, dp(10), 0, 0) }
        }
        progressWrap.addView(progressBar)
        content.addView(progressWrap)

        // 采集概览卡（统计磁贴）
        statCard = Card(context, dp, context.getString(R.string.ui_section_overview), C.ACCENT)
        statGrid = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
        statCard.body.addView(statGrid)
        statCard.layoutParams = cardLp()
        content.addView(statCard)

        // 设备能力卡
        capCard = Card(context, dp, context.getString(R.string.ui_section_capability), C.TEXT_SECOND)
        capBody = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        capCard.body.addView(capBody)
        capCard.layoutParams = cardLp()
        content.addView(capCard)

        // 重建结果卡
        resultCard = Card(context, dp, context.getString(R.string.ui_section_result), C.OK)
        resultBody = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        resultCard.body.addView(resultBody)
        resultCard.layoutParams = cardLp()
        content.addView(resultCard)

        // 诊断行（小字）
        diagText = TextView(context).apply {
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(11).toFloat())
            setTextColor(C.TEXT_MUTED)
            setPadding(dp(4), dp(6), dp(4), dp(12))
            typeface = android.graphics.Typeface.MONOSPACE
        }
        content.addView(diagText)

        scroll.addView(content)
        overlay.addView(scroll)

        // ---- 底部主操作栏 ----
        val bottom = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            background = bottomBackground()
            setPadding(dp(16), dp(12), dp(16), dp(16))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        }
        buttonBar = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
        }
        buttons = linkedMapOf(
            Action.START to makeButton(R.string.ui_btn_start, primary = true),
            Action.FINISH to makeButton(R.string.ui_btn_finish, primary = true),
            Action.RECONSTRUCT to makeButton(R.string.ui_btn_reconstruct, primary = true),
            Action.CONTINUE to makeButton(R.string.ui_btn_continue, primary = false),
            Action.RECAPTURE to makeButton(R.string.ui_btn_recapture, primary = true),
            Action.EXPORT_GLB to makeButton(R.string.ui_btn_export_glb, primary = false),
            Action.EXPORT_STL to makeButton(R.string.ui_btn_export_stl, primary = false),
            Action.RETRY to makeButton(R.string.ui_btn_retry, primary = true),
            Action.HOME to makeButton(R.string.ui_btn_home, primary = false),
        )
        buttons.forEach { (action, btn) ->
            btn.setOnClickListener { onAction(action) }
            btn.visibility = View.GONE
            buttonBar.addView(btn)
        }
        bottom.addView(buttonBar)
        overlay.addView(bottom)
    }

    private fun cardLp() = LinearLayout.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
        setMargins(0, 0, 0, dp(10))
    }

    /** 由 Activity 把相机预览视图挂到预览承载区。 */
    fun attachPreview(view: View) {
        previewHost.removeAllViews()
        previewHost.addView(view)
    }

    /** 根据当前状态渲染整屏。纯呈现，无状态判断。 */
    fun render(v: ViewState) {
        statusText.text = v.stateLabel
        statusDot.setColor(tintForState(v.stateTone))
        stepper.setCurrent(v.step)
        hintText.text = v.hint

        progressWrap.visibility = if (v.progressFraction >= 0f) View.VISIBLE else View.GONE
        if (v.progressFraction >= 0f) {
            progressText.text = v.progress.ifEmpty { context.getString(R.string.ui_progress_label) }
            progressBar.setFraction(v.progressFraction)
        }

        renderTiles(statGrid, v.stats, tiles = true)
        renderPairs(capBody, v.capability)
        renderPairs(resultBody, v.result)
        resultCard.visibility = if (v.result.isEmpty() || v.result == "—") View.GONE else View.VISIBLE

        diagText.text = v.diag

        buttons.forEach { (action, btn) ->
            btn.visibility = if (action in v.visibleActions) View.VISIBLE else View.GONE
        }
    }

    private fun renderPairs(host: LinearLayout, contentText: String) {
        host.removeAllViews()
        if (contentText.isEmpty() || contentText == "—") {
            host.addView(TextView(context).apply {
                text = "—"
                setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(13).toFloat())
                setTextColor(C.TEXT_MUTED)
            })
            return
        }
        contentText.split("\n").forEach { line ->
            val p = splitLabelValue(line)
            host.addView(LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                    setMargins(0, 0, 0, dp(6))
                }
                addView(TextView(context).apply {
                    text = p.first
                    setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(12).toFloat())
                    setTextColor(C.TEXT_SECOND)
                    layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                })
                addView(TextView(context).apply {
                    text = p.second
                    setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(12).toFloat())
                    setTextColor(C.TEXT_PRIMARY)
                    typeface = android.graphics.Typeface.DEFAULT_BOLD
                    gravity = Gravity.END
                    layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                })
            })
        }
    }

    /** 把 "帧数：123" 拆成 (帧数, 123)，兼容中文/英文冒号。 */
    private fun splitLabelValue(line: String): Pair<String, String> {
        val i = line.indexOf('：').let { if (it >= 0) it else line.indexOf(':') }
        return if (i >= 0) line.substring(0, i).trim() to line.substring(i + 1).trim()
        else line to ""
    }

    /** 统计区渲染成两列磁贴（大数字）。 */
    private fun renderTiles(host: LinearLayout, contentText: String, tiles: Boolean) {
        host.removeAllViews()
        if (contentText.isEmpty() || contentText == "—") {
            host.addView(TextView(context).apply {
                text = "等待采集数据"
                setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(12).toFloat())
                setTextColor(C.TEXT_MUTED)
            })
            return
        }
        val pairs = contentText.split("\n").map { splitLabelValue(it) }
        var i = 0
        while (i < pairs.size) {
            val row = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                    setMargins(0, 0, 0, dp(8))
                }
            }
            val end = minOf(i + 2, pairs.size)
            for (j in i until end) {
                val (label, value) = pairs[j]
                row.addView(tile(label, value, weight = 1f,
                    marginEnd = if (j == i && end - i == 2) dp(6) else 0))
            }
            host.addView(row)
            i += 2
        }
    }

    private fun tile(label: String, value: String, weight: Float, marginEnd: Int): View =
        LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            background = tileBackground()
            setPadding(dp(12), dp(10), dp(12), dp(10))
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, weight).apply {
                setMargins(0, 0, marginEnd, 0)
            }
            addView(TextView(context).apply {
                text = value.ifEmpty { "—" }
                setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(19).toFloat())
                setTextColor(C.ACCENT)
                typeface = android.graphics.Typeface.DEFAULT_BOLD
                maxLines = 1
                ellipsize = android.text.TextUtils.TruncateAt.END
            })
            addView(TextView(context).apply {
                text = label
                setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(10).toFloat())
                setTextColor(C.TEXT_SECOND)
                setPadding(0, dp(3), 0, 0)
                maxLines = 1
                ellipsize = android.text.TextUtils.TruncateAt.END
            })
        }

    private fun tintForState(tone: Int): Int = when (tone) {
        1 -> C.WARN      // processing
        2 -> C.OK        // completed
        3 -> C.DANGER    // failed
        else -> C.ACCENT // idle/probe/preview/capture
    }

    private fun makeButton(labelRes: Int, primary: Boolean): Button =
        Button(context).apply {
            text = context.getString(labelRes)
            isAllCaps = false
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(14).toFloat())
            setTextColor(if (primary) Color.rgb(0x06, 0x1A, 0x17) else C.TEXT_PRIMARY)
            background = if (primary) primaryButtonBackground() else secondaryButtonBackground()
            minHeight = dp(48)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(dp(4), 0, dp(4), 0)
            }
            setPadding(dp(20), dp(10), dp(20), dp(10))
        }

    // ---- 一帧渲染所需的数据 ------------------------------------------------
    data class ViewState(
        val stateLabel: String,
        val hint: String,
        val progress: String = "",
        val progressFraction: Float = -1f,
        val diag: String = "",
        val capability: String = "",
        val stats: String = "",
        val result: String = "",
        val step: Int = 0,
        val stateTone: Int = 0, // 0 常态 1 处理 2 完成 3 失败
        val visibleActions: Set<Action> = emptySet(),
    )

    // ---- 绘制辅助 ----------------------------------------------------------

    private fun TextView.setLetterSpacingCompat(v: Float) {
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.LOLLIPOP) {
            letterSpacing = v
        }
    }

    private fun verticalGradient(top: Int, bottom: Int) =
        android.graphics.drawable.GradientDrawable(
            android.graphics.drawable.GradientDrawable.Orientation.TOP_BOTTOM,
            intArrayOf(top, bottom))

    private fun scrimGradient() =
        android.graphics.drawable.GradientDrawable(
            android.graphics.drawable.GradientDrawable.Orientation.TOP_BOTTOM,
            intArrayOf(
                Color.argb(0xF2, 0x0B, 0x11, 0x17),
                Color.argb(0xA8, 0x0B, 0x11, 0x17),
                Color.argb(0xD9, 0x0B, 0x11, 0x17)))

    private fun bottomBackground() = android.graphics.drawable.GradientDrawable().apply {
        setColor(Color.rgb(0x0B, 0x11, 0x17))
        setStroke(dp(1), C.BORDER)
        setCornerRadius(dp(0).toFloat())
    }

    private fun cardBackground() = android.graphics.drawable.GradientDrawable().apply {
        setColor(C.SURFACE)
        setStroke(dp(1), C.BORDER)
        cornerRadius = dp(14).toFloat()
    }

    private fun tileBackground() = android.graphics.drawable.GradientDrawable().apply {
        setColor(Color.argb(0xB3, 0x1B, 0x2A, 0x36))
        cornerRadius = dp(10).toFloat()
    }

    private fun pillBackground(solid: Int, border: Int) =
        android.graphics.drawable.GradientDrawable().apply {
            setColor(solid)
            setStroke(dp(1), border)
            cornerRadius = dp(999).toFloat()
        }

    private fun hintBackground() = android.graphics.drawable.GradientDrawable().apply {
        setColor(C.SURFACE)
        setStroke(dp(1), C.ACCENT_SOFT)
        cornerRadius = dp(12).toFloat()
    }

    private fun primaryButtonBackground() = android.graphics.drawable.GradientDrawable(
        android.graphics.drawable.GradientDrawable.Orientation.LEFT_RIGHT,
        intArrayOf(C.ACCENT_DARK, C.ACCENT)).apply {
        cornerRadius = dp(12).toFloat()
    }

    private fun secondaryButtonBackground() = android.graphics.drawable.GradientDrawable().apply {
        setColor(Color.argb(0xE6, 0x1B, 0x2A, 0x36))
        setStroke(dp(1), C.BORDER)
        cornerRadius = dp(12).toFloat()
    }

    // ---- 自绘视图：品牌 Logo ----------------------------------------------
    private class LogoMark(context: Context, val size: Int) : View(context) {
        private val line = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = C.ACCENT
            style = Paint.Style.STROKE
            strokeWidth = (size * 0.045f).coerceAtLeast(2f)
            strokeJoin = Paint.Join.ROUND
            strokeCap = Paint.Cap.ROUND
        }
        private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = C.ACCENT_SOFT }

        override fun onDraw(canvas: Canvas) {
            super.onDraw(canvas)
            val pad = size * 0.22f
            val x0 = pad; val y0 = pad * 1.1f
            val s = size - pad * 2
            val dx = s * 0.28f; val dy = s * 0.16f
            // 顶面
            val top = Path().apply {
                moveTo(x0 + dx, y0)
                lineTo(x0 + s, y0 + dy)
                lineTo(x0 + s - dx, y0 + dy * 2)
                lineTo(x0, y0 + dy)
                close()
            }
            canvas.drawPath(top, fill); canvas.drawPath(top, line)
            // 左面 + 右面
            val left = Path().apply {
                moveTo(x0, y0 + dy)
                lineTo(x0 + s - dx, y0 + dy * 2)
                lineTo(x0 + s - dx, y0 + dy * 2 + s * 0.5f)
                lineTo(x0, y0 + dy + s * 0.5f)
                close()
            }
            val right = Path().apply {
                moveTo(x0 + s - dx, y0 + dy * 2)
                lineTo(x0 + s, y0 + dy)
                lineTo(x0 + s, y0 + dy + s * 0.5f)
                lineTo(x0 + s - dx, y0 + dy * 2 + s * 0.5f)
                close()
            }
            canvas.drawPath(left, line); canvas.drawPath(right, line)
        }
    }

    // ---- 自绘视图：状态圆点 ------------------------------------------------
    private class DotView(context: Context, color: Int) : View(context) {
        private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { this.color = color }
        fun setColor(c: Int) { paint.color = c; invalidate() }
        override fun onDraw(canvas: Canvas) {
            super.onDraw(canvas)
            canvas.drawCircle(width / 2f, height / 2f, minOf(width, height) / 2f, paint)
        }
    }

    // ---- 自绘视图：四段式流程进度条 ---------------------------------------
    private class StepperView(context: Context, private val dp: (Int) -> Int) : View(context) {
        private val labels = intArrayOf(
            R.string.ui_step_prepare, R.string.ui_step_capture,
            R.string.ui_step_reconstruct, R.string.ui_step_result)
        private var current = 0

        private val lineDone = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = C.ACCENT; strokeWidth = dp(2).toFloat() }
        private val lineTodo = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = C.BORDER; strokeWidth = dp(2).toFloat() }
        private val circleDone = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = C.ACCENT }
        private val circleCur = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = C.ACCENT }
        private val circleTodo = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = C.SURFACE_SOLID }
        private val ringTodo = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = C.BORDER; style = Paint.Style.STROKE; strokeWidth = dp(2).toFloat()
        }
        private val checkPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.rgb(0x06, 0x1A, 0x17); style = Paint.Style.STROKE
            strokeWidth = dp(2).toFloat(); strokeCap = Paint.Cap.ROUND
        }
        private val labelDone = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = C.TEXT_PRIMARY; textSize = dp(11).toFloat(); textAlign = Paint.Align.CENTER
        }
        private val labelTodo = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = C.TEXT_MUTED; textSize = dp(11).toFloat(); textAlign = Paint.Align.CENTER
        }

        fun setCurrent(step: Int) { if (current != step) { current = step.coerceIn(0, 3); invalidate() } }

        override fun onMeasure(w: Int, h: Int) {
            super.onMeasure(w, h)
            setMeasuredDimension(MeasureSpec.getSize(w), dp(58))
        }

        override fun onDraw(canvas: Canvas) {
            super.onDraw(canvas)
            val n = 4
            val r = dp(9).toFloat()
            val seg = width.toFloat() / n
            val cy = dp(12).toFloat() + r
            for (i in 0 until n) {
                val cx = seg * i + seg / 2f
                // 连线
                if (i < n - 1) {
                    val linePaint = if (i < current) lineDone else lineTodo
                    canvas.drawLine(cx + r, cy, seg * (i + 1) + seg / 2f - r, cy, linePaint)
                }
                // 圆点
                when {
                    i < current -> {
                        canvas.drawCircle(cx, cy, r, circleDone)
                        // 对勾
                        canvas.drawLine(cx - r * 0.4f, cy, cx - r * 0.05f, cy + r * 0.4f, checkPaint)
                        canvas.drawLine(cx - r * 0.05f, cy + r * 0.4f, cx + r * 0.5f, cy - r * 0.35f, checkPaint)
                    }
                    i == current -> {
                        canvas.drawCircle(cx, cy, r, circleCur)
                        canvas.drawCircle(cx, cy, r + dp(4), ringTodo)
                    }
                    else -> {
                        canvas.drawCircle(cx, cy, r, circleTodo)
                        canvas.drawCircle(cx, cy, r, ringTodo)
                    }
                }
                // 标签
                val txt = context.getString(labels[i])
                canvas.drawText(txt, cx, dp(48).toFloat(), if (i <= current) labelDone else labelTodo)
            }
        }
    }

    // ---- 自绘视图：进度条 --------------------------------------------------
    private class ProgressBarView(context: Context, private val dp: (Int) -> Int) : View(context) {
        private var fraction = 0f
        private val bg = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(0x60, 0x2A, 0x3A, 0x48) }
        private val fg = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = C.ACCENT }
        fun setFraction(f: Float) { fraction = f.coerceIn(0f, 1f); invalidate() }
        override fun onDraw(canvas: Canvas) {
            super.onDraw(canvas)
            val r = height / 2f
            canvas.drawRoundRect(0f, 0f, width.toFloat(), height.toFloat(), r, r, bg)
            if (fraction > 0f) {
                canvas.drawRoundRect(0f, 0f, (width * fraction).coerceAtLeast(r * 2), height.toFloat(), r, r, fg)
            }
        }
    }

    // ---- 卡片容器 ----------------------------------------------------------
    private class Card(
        context: Context,
        dp: (Int) -> Int,
        title: String,
        accent: Int,
    ) : LinearLayout(context) {
        val body = LinearLayout(context)
        init {
            orientation = VERTICAL
            background = android.graphics.drawable.GradientDrawable().apply {
                setColor(C.SURFACE); setStroke(dp(1), C.BORDER); cornerRadius = dp(14).toFloat()
            }
            setPadding(dp(14), dp(12), dp(14), dp(12))
            val head = TextView(context).apply {
                text = title
                setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(12).toFloat())
                setTextColor(accent)
                typeface = android.graphics.Typeface.DEFAULT_BOLD
                setLetterSpacingCompat(0.08f)
            }
            addView(head)
            body.orientation = VERTICAL
            body.setPadding(0, dp(8), 0, 0)
            addView(body)
        }
        private fun TextView.setLetterSpacingCompat(v: Float) {
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.LOLLIPOP) letterSpacing = v
        }
    }
}
