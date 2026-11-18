// SPDX-License-Identifier: BSD-3-Clause
//
// MainUI.kt — 企业级工作台主界面（Android，程序化视图）
//
// 设计参照（2026-11-18 上网核实的真实企业风）：
//   Linear / Stripe / Notion 的设计语言 —— 白底或极浅灰底、黑白灰为主、
//   **单一冷蓝强调色**、细边框、大留白、信息层级靠排版而非花哨色彩。
//   状态色（红/绿/橙）只以**小面积**（圆点、文字）出现，绝不铺满屏。
//
// 与本轮之前错误版本的差异（如实登记）：
//   之前是深色底 + 青/橙/红多色强调 = 用户指出"糖果色、小气、不是企业风"。
//   本版完全重写为白底极简风：4px 栅格、12/16px 对称内边距、8px 圆角、
//   borders-only 深度（细边框，无大阴影、无渐变、无多彩）。
//
// 为什么仍程序化构建（不引入 Material / XML layout）：
//   零新依赖、零 AAPT 资源风险；全部用 Android framework 基础 API + 自绘
//   Drawable，在 API 24..35 上一致。视图只负责呈现，无状态判断。
//
// 契约：docs/09_UI规格.md §4.1/§4.2（采集状态机与各态界面）。
package com.recon.scan

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
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

internal class MainUI(
    private val context: Context,
    private val onAction: (Action) -> Unit,
) {
    enum class Action {
        START, FINISH, RECONSTRUCT, CONTINUE,
        EXPORT_GLB, EXPORT_STL, RECAPTURE, RETRY, HOME,
    }

    // ---- Linear 风格配色：黑白灰结构 + 单一冷蓝强调 ------------------------
    private object C {
        val BG = Color.rgb(0xF7, 0xF8, 0xFA)
        val SURFACE = Color.rgb(0xFF, 0xFF, 0xFF)
        val BORDER = Color.rgb(0xE5, 0xE7, 0xEB)
        val TRACK = Color.rgb(0xEB, 0xEE, 0xF2)
        val TEXT_PRIMARY = Color.rgb(0x11, 0x18, 0x27)
        val TEXT_SECOND = Color.rgb(0x6B, 0x72, 0x80)
        val TEXT_MUTED = Color.rgb(0x9C, 0xA3, 0xAF)
        val ACCENT = Color.rgb(0x25, 0x63, 0xEB)
        val ACCENT_SOFT = Color.rgb(0xEF, 0xF6, 0xFF)
        val OK = Color.rgb(0x05, 0x96, 0x69)
        val WARN = Color.rgb(0xB4, 0x53, 0x09)
        val DANGER = Color.rgb(0xDC, 0x26, 0x26)
        val PREVIEW_BG = Color.rgb(0x1F, 0x29, 0x37)
    }

    val root: FrameLayout
    private val previewHost: FrameLayout
    private val statusDot: DotView
    private val statusText: TextView
    private val stepper: StepperView
    private val hintText: TextView
    private val progressWrap: LinearLayout
    private val progressBar: ProgressBarView
    private val progressText: TextView
    private val stageList: LinearLayout
    private val statCard: Card
    private val statGrid: LinearLayout
    private val capCard: Card
    private val capBody: LinearLayout
    private val resultCard: Card
    private val resultBody: LinearLayout
    private val diagText: TextView
    private lateinit var headerBar: LinearLayout
    private lateinit var infoScroll: ScrollView
    private lateinit var bottomBar: LinearLayout
    private var fullscreen = false
    private val buttonBar: LinearLayout
    private val buttons: Map<Action, Button>

    private val dp = { v: Int -> (v * context.resources.displayMetrics.density).roundToInt() }

    init {
        root = FrameLayout(context).apply {
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
            setBackgroundColor(C.BG)
        }
        // root 公开为 val（Activity setContentView 用）
        val column = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
        }
        root.addView(column)

        // ---- 顶部品牌栏 ----
        val header = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setBackgroundColor(C.SURFACE)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            setPadding(dp(16), dp(14), dp(16), dp(14))
        }
        header.addView(LogoMark(context, dp(36)).apply {
            layoutParams = LinearLayout.LayoutParams(dp(36), dp(36))
        })
        val titleCol = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                setMargins(dp(12), 0, dp(8), 0)
            }
        }
        titleCol.addView(TextView(context).apply {
            text = "recon"
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(19).toFloat())
            setTextColor(C.TEXT_PRIMARY)
            typeface = android.graphics.Typeface.DEFAULT_BOLD
        })
        titleCol.addView(TextView(context).apply {
            text = context.getString(R.string.ui_subtitle)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(11).toFloat())
            setTextColor(C.TEXT_SECOND)
        })
        header.addView(titleCol)

        val statusPill = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = pillBackground()
            setPadding(dp(11), dp(7), dp(11), dp(7))
        }
        statusDot = DotView(context, C.TEXT_SECOND).apply {
            layoutParams = LinearLayout.LayoutParams(dp(7), dp(7)).apply { setMargins(0, 0, dp(7), 0) }
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
        headerBar = header
        column.addView(header)
        column.addView(View(context).apply {
            setBackgroundColor(C.BORDER)
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1))
        })

        // ---- 四段式流程进度条 ----
        stepper = StepperView(context, dp).apply {
            setBackgroundColor(C.SURFACE)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        }
        column.addView(stepper)

        // ---- 相机取景器（白底页里的独立取景区）----
        previewHost = FrameLayout(context).apply {
            setBackgroundColor(C.PREVIEW_BG)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(196)).apply {
                setMargins(dp(16), dp(12), dp(16), dp(4))
            }
            clipToOutline = true
            outlineProvider = object : android.view.ViewOutlineProvider() {
                override fun getOutline(view: View, outline: android.graphics.Outline) {
                    outline.setRoundRect(0, 0, view.width, view.height, dp(10).toFloat())
                }
            }
        }
        column.addView(previewHost)

        // ---- 可滚动信息区 ----
        val scroll = ScrollView(context).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f).apply {
                setMargins(dp(16), dp(8), dp(16), dp(4))
            }
            overScrollMode = View.OVER_SCROLL_NEVER
        }
        val content = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }

        hintText = TextView(context).apply {
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(13).toFloat())
            setTextColor(C.TEXT_PRIMARY)
            background = hintBackground()
            setPadding(dp(14), dp(12), dp(14), dp(12))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(0, 0, 0, dp(12))
            }
        }
        content.addView(hintText)

        progressWrap = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
            background = cardBackground()
            setPadding(dp(14), dp(12), dp(14), dp(12))
            layoutParams = cardLp()
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
        stageList = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(0, dp(14), 0, 0)
            }
        }
        progressWrap.addView(stageList)
        content.addView(progressWrap)

        statCard = Card(context, dp, context.getString(R.string.ui_section_overview))
        statGrid = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
        statCard.body.addView(statGrid)
        statCard.layoutParams = cardLp()
        content.addView(statCard)

        capCard = Card(context, dp, context.getString(R.string.ui_section_capability))
        capBody = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        capCard.body.addView(capBody)
        capCard.layoutParams = cardLp()
        content.addView(capCard)

        resultCard = Card(context, dp, context.getString(R.string.ui_section_result))
        resultBody = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        resultCard.body.addView(resultBody)
        resultCard.layoutParams = cardLp()
        content.addView(resultCard)

        diagText = TextView(context).apply {
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(11).toFloat())
            setTextColor(C.TEXT_MUTED)
            setPadding(dp(2), dp(4), dp(2), dp(10))
            typeface = android.graphics.Typeface.MONOSPACE
        }
        content.addView(diagText)

        scroll.addView(content)
        infoScroll = scroll
        column.addView(scroll)

        // ---- 底部主操作栏 ----
        column.addView(View(context).apply {
            setBackgroundColor(C.BORDER)
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1))
        })
        val bottom = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(C.SURFACE)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        }
        buttonBar = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
            setPadding(dp(12), dp(12), dp(12), dp(14))
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
        bottomBar = bottom
        column.addView(bottom)
    }

    private fun cardLp() = LinearLayout.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
        setMargins(0, 0, 0, dp(12))
    }

    fun attachPreview(view: View) {
        previewHost.removeAllViews()
        previewHost.addView(view)
    }

    /**
     * 采集态切换为**全屏竖屏取景**：真正的大厂扫描体验。
     * 全屏时隐藏品牌栏/信息流/流程条，取景器铺满整屏，只留顶部极简状态条与底部操作栏。
     */
    fun setCaptureFullscreen(on: Boolean) {
        if (fullscreen == on) return
        fullscreen = on
        headerBar.visibility = if (on) View.GONE else View.VISIBLE
        infoScroll.visibility = if (on) View.GONE else View.VISIBLE
        stepper.visibility = if (on) View.GONE else View.VISIBLE
        bottomBar.setPadding(0, dp(12), 0, if (on) dp(30) else dp(14))
        previewHost.layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            if (on) 0 else dp(196)).apply {
            weight = if (on) 1f else 0f
            setMargins(if (on) 0 else dp(16), if (on) 0 else dp(12),
                       if (on) 0 else dp(16), 0)
        }
        previewHost.invalidate()
    }

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

        renderTiles(statGrid, v.stats)
        renderPairs(capBody, v.capability)
        renderPairs(resultBody, v.result)
        resultCard.visibility = if (v.result.isEmpty() || v.result == "—") View.GONE else View.VISIBLE
        diagText.text = v.diag
        buttons.forEach { (action, btn) ->
            btn.visibility = if (action in v.visibleActions) View.VISIBLE else View.GONE
        }
    }

    /**
     * 渲染多阶段重建明细。stage 是 recon_stage 枚举值，progress 是**总进度** [0,1]。
     * 用总进度推算各阶段：stage 之前的算完成、当前阶段按 stage 内插值、之后等待。
     * 让用户在长耗时重建时能看到"走到哪一步、每步完成度"，而不是干等一个百分比。
     */
    fun renderStageDetail(stage: Int, progress: Double, message: String) {
        val stages = intArrayOf(
            R.string.stage_preprocess, R.string.stage_vio, R.string.stage_sfm,
            R.string.stage_bundle, R.string.stage_mvs, R.string.stage_fusion,
            R.string.stage_mesh, R.string.stage_postproc, R.string.stage_texture,
            R.string.stage_scale, R.string.stage_export)
        stageList.removeAllViews()
        val activeIndex = (stage - 1).coerceIn(0, stages.size - 1)
        // 总进度按比例分配到 11 段；当前段内用 (progress*11 - activeIndex) 细分
        val global = (progress * stages.size).toFloat()
        for (i in stages.indices) {
            val frac = when {
                i < activeIndex -> 1f
                i == activeIndex -> (global - i).coerceIn(0f, 1f)
                else -> 0f
            }
            val row = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                    setMargins(0, 0, 0, dp(7))
                }
            }
            val mark = TextView(context).apply {
                text = if (frac >= 1f) "✓" else if (i == activeIndex) "●" else "○"
                setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(11).toFloat())
                setTextColor(if (frac >= 1f) C.OK else if (i == activeIndex) C.ACCENT else C.TEXT_MUTED)
                layoutParams = LinearLayout.LayoutParams(dp(18), ViewGroup.LayoutParams.WRAP_CONTENT)
            }
            val name = TextView(context).apply {
                text = context.getString(stages[i])
                setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(11).toFloat())
                setTextColor(if (i <= activeIndex) C.TEXT_PRIMARY else C.TEXT_MUTED)
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            }
            val pct = TextView(context).apply {
                text = "${(frac * 100).toInt()}%"
                setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(10).toFloat())
                setTextColor(if (i == activeIndex) C.ACCENT else C.TEXT_MUTED)
                layoutParams = LinearLayout.LayoutParams(dp(38), ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                    gravity = Gravity.END
                }
            }
            row.addView(mark); row.addView(name); row.addView(pct)
            stageList.addView(row)
        }
        if (message.isNotEmpty()) {
            stageList.addView(TextView(context).apply {
                text = message
                setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(10).toFloat())
                setTextColor(C.TEXT_MUTED)
                setPadding(0, dp(4), 0, 0)
            })
        }
    }

    /** 重建结束后清空阶段明细。 */
    fun clearStageDetail() { stageList.removeAllViews() }

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
                    setMargins(0, 0, 0, dp(8))
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

    private fun splitLabelValue(line: String): Pair<String, String> {
        val i = line.indexOf('：').let { if (it >= 0) it else line.indexOf(':') }
        return if (i >= 0) line.substring(0, i).trim() to line.substring(i + 1).trim()
        else line to ""
    }

    private fun renderTiles(host: LinearLayout, contentText: String) {
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
                row.addView(tile(label, value, 1f, if (j == i && end - i == 2) dp(8) else 0))
            }
            host.addView(row)
            i += 2
        }
    }

    private fun tile(label: String, value: String, weight: Float, marginEnd: Int): View =
        LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            background = tileBackground()
            setPadding(dp(12), dp(11), dp(12), dp(11))
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, weight).apply {
                setMargins(0, 0, marginEnd, 0)
            }
            addView(TextView(context).apply {
                text = value.ifEmpty { "—" }
                setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(18).toFloat())
                setTextColor(C.TEXT_PRIMARY)
                typeface = android.graphics.Typeface.DEFAULT_BOLD
                maxLines = 1
                ellipsize = android.text.TextUtils.TruncateAt.END
            })
            addView(TextView(context).apply {
                text = label
                setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(11).toFloat())
                setTextColor(C.TEXT_SECOND)
                setPadding(0, dp(4), 0, 0)
                maxLines = 1
                ellipsize = android.text.TextUtils.TruncateAt.END
            })
        }

    private fun tintForState(tone: Int): Int = when (tone) {
        1 -> C.WARN
        2 -> C.OK
        3 -> C.DANGER
        else -> C.TEXT_SECOND
    }

    private fun makeButton(labelRes: Int, primary: Boolean): Button =
        Button(context).apply {
            text = context.getString(labelRes)
            isAllCaps = false
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(14).toFloat())
            setTextColor(if (primary) Color.WHITE else C.TEXT_PRIMARY)
            background = if (primary) primaryButtonBackground() else secondaryButtonBackground()
            minHeight = dp(42)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(dp(4), 0, dp(4), 0)
            }
            setPadding(dp(18), dp(9), dp(18), dp(9))
            stateListAnimator = null
        }

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
        val stateTone: Int = 0,
        val visibleActions: Set<Action> = emptySet(),
    )

    private fun pillBackground() = android.graphics.drawable.GradientDrawable().apply {
        setColor(Color.rgb(0xF3, 0xF4, 0xF6))
        cornerRadius = dp(999).toFloat()
    }
    private fun cardBackground() = android.graphics.drawable.GradientDrawable().apply {
        setColor(C.SURFACE); setStroke(dp(1), C.BORDER); cornerRadius = dp(8).toFloat()
    }
    private fun tileBackground() = android.graphics.drawable.GradientDrawable().apply {
        setColor(Color.rgb(0xFA, 0xFB, 0xFC)); setStroke(dp(1), C.BORDER); cornerRadius = dp(8).toFloat()
    }
    private fun hintBackground() = android.graphics.drawable.GradientDrawable().apply {
        setColor(C.ACCENT_SOFT); setStroke(dp(1), Color.rgb(0xDB, 0xEA, 0xFE)); cornerRadius = dp(8).toFloat()
    }
    private fun primaryButtonBackground() = android.graphics.drawable.GradientDrawable().apply {
        setColor(C.ACCENT); cornerRadius = dp(8).toFloat()
    }
    private fun secondaryButtonBackground() = android.graphics.drawable.GradientDrawable().apply {
        setColor(C.SURFACE); setStroke(dp(1), C.BORDER); cornerRadius = dp(8).toFloat()
    }

    // ---- 自绘：品牌 Logo（石墨细线立方体）----
    private class LogoMark(context: Context, val size: Int) : View(context) {
        private val line = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = C.TEXT_PRIMARY; style = Paint.Style.STROKE
            strokeWidth = 2.2f; strokeJoin = Paint.Join.ROUND; strokeCap = Paint.Cap.ROUND
        }
        override fun onDraw(canvas: Canvas) {
            super.onDraw(canvas)
            val pad = size * 0.22f
            val x0 = pad; val y0 = pad * 1.1f
            val s = size - pad * 2
            val dx = s * 0.28f; val dy = s * 0.16f
            val top = Path().apply {
                moveTo(x0 + dx, y0); lineTo(x0 + s, y0 + dy)
                lineTo(x0 + s - dx, y0 + dy * 2); lineTo(x0, y0 + dy); close()
            }
            val left = Path().apply {
                moveTo(x0, y0 + dy); lineTo(x0 + s - dx, y0 + dy * 2)
                lineTo(x0 + s - dx, y0 + dy * 2 + s * 0.5f); lineTo(x0, y0 + dy + s * 0.5f); close()
            }
            val right = Path().apply {
                moveTo(x0 + s - dx, y0 + dy * 2); lineTo(x0 + s, y0 + dy)
                lineTo(x0 + s, y0 + dy + s * 0.5f); lineTo(x0 + s - dx, y0 + dy * 2 + s * 0.5f); close()
            }
            canvas.drawPath(top, line); canvas.drawPath(left, line); canvas.drawPath(right, line)
        }
    }

    private class DotView(context: Context, color: Int) : View(context) {
        private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { this.color = color }
        fun setColor(c: Int) { paint.color = c; invalidate() }
        override fun onDraw(canvas: Canvas) {
            super.onDraw(canvas)
            canvas.drawCircle(width / 2f, height / 2f, minOf(width, height) / 2f, paint)
        }
    }

    private class StepperView(context: Context, private val dp: (Int) -> Int) : View(context) {
        private val labels = intArrayOf(
            R.string.ui_step_prepare, R.string.ui_step_capture,
            R.string.ui_step_reconstruct, R.string.ui_step_result)
        private var current = 0
        private val lineDone = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = C.ACCENT; strokeWidth = dp(2).toFloat() }
        private val lineTodo = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = C.TRACK; strokeWidth = dp(2).toFloat() }
        private val circleDone = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = C.ACCENT }
        private val circleCur = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = C.ACCENT }
        private val circleTodo = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = C.SURFACE }
        private val ringTodo = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = C.BORDER; style = Paint.Style.STROKE; strokeWidth = 1.5f * context.resources.displayMetrics.density }
        private val ringCur = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = C.ACCENT; style = Paint.Style.STROKE; strokeWidth = 1.5f * context.resources.displayMetrics.density }
        private val checkPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE; style = Paint.Style.STROKE
            strokeWidth = 1.8f * context.resources.displayMetrics.density; strokeCap = Paint.Cap.ROUND }
        private val labelDone = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = C.TEXT_PRIMARY; textSize = dp(11).toFloat(); textAlign = Paint.Align.CENTER }
        private val labelTodo = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = C.TEXT_MUTED; textSize = dp(11).toFloat(); textAlign = Paint.Align.CENTER }
        fun setCurrent(step: Int) { if (current != step) { current = step.coerceIn(0, 3); invalidate() } }
        override fun onMeasure(w: Int, h: Int) {
            super.onMeasure(w, h); setMeasuredDimension(MeasureSpec.getSize(w), dp(56))
        }
        override fun onDraw(canvas: Canvas) {
            super.onDraw(canvas)
            val n = 4; val r = dp(8).toFloat(); val seg = width.toFloat() / n; val cy = dp(12).toFloat() + r
            for (i in 0 until n) {
                val cx = seg * i + seg / 2f
                if (i < n - 1) {
                    canvas.drawLine(cx + r, cy, seg * (i + 1) + seg / 2f - r, cy,
                        if (i < current) lineDone else lineTodo)
                }
                when {
                    i < current -> {
                        canvas.drawCircle(cx, cy, r, circleDone)
                        canvas.drawLine(cx - r * 0.4f, cy, cx - r * 0.05f, cy + r * 0.4f, checkPaint)
                        canvas.drawLine(cx - r * 0.05f, cy + r * 0.4f, cx + r * 0.5f, cy - r * 0.35f, checkPaint)
                    }
                    i == current -> {
                        canvas.drawCircle(cx, cy, r, circleCur)
                        canvas.drawCircle(cx, cy, r + dp(4), ringCur)
                    }
                    else -> {
                        canvas.drawCircle(cx, cy, r, circleTodo)
                        canvas.drawCircle(cx, cy, r, ringTodo)
                    }
                }
                canvas.drawText(context.getString(labels[i]), cx, dp(46).toFloat(),
                    if (i <= current) labelDone else labelTodo)
            }
        }
    }

    private class ProgressBarView(context: Context, private val dp: (Int) -> Int) : View(context) {
        private var fraction = 0f
        private val bg = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = C.TRACK }
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

    private class Card(context: Context, dp: (Int) -> Int, title: String) : LinearLayout(context) {
        val body = LinearLayout(context)
        init {
            orientation = VERTICAL
            background = android.graphics.drawable.GradientDrawable().apply {
                setColor(C.SURFACE); setStroke(dp(1), C.BORDER); cornerRadius = dp(8).toFloat() }
            if (android.os.Build.VERSION.SDK_INT >= 21) elevation = dp(1).toFloat()
            setPadding(dp(14), dp(12), dp(14), dp(13))
            addView(TextView(context).apply {
                text = title
                setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(13).toFloat())
                setTextColor(C.TEXT_PRIMARY)
                typeface = android.graphics.Typeface.DEFAULT_BOLD
            })
            body.orientation = VERTICAL
            body.setPadding(0, dp(10), 0, 0)
            addView(body)
        }
    }
}
