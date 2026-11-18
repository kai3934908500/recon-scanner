// SPDX-License-Identifier: BSD-3-Clause
//
// MainUI.kt — 采集主界面的**程序化视图**（Android）
//
// 为什么程序化构建而不是 layout XML：
//   1. 本机无 Android 工具链，UI 的正确性只能靠 CI 编译验证。程序化视图只依赖
//      Android framework 的基础类（LinearLayout/TextView/Button/TextureView），
//      **不引入任何新依赖、不新增资源类型** —— 把 AAPT 资源链接失败的风险降到 0。
//   2. G-AND-3 只校验 Manifest 引用的资源，程序化视图不触碰该面。
//   3. 与 iOS 侧"权威 UI 源单一"同一精神：界面结构只在一处定义。
//
// 契约：docs/09_UI规格.md §4.1/§4.2（采集状态机与各态界面）、§3.2（能力展示）。

package com.recon.scan

import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
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
 * 只负责**呈现**：状态文案、能力、统计、按钮可见性由 Activity 调用 [render] 更新；
 * 按钮点击经 [onAction] 回调交给 Activity 翻译成状态机事件。视图内不含任何
 * 状态判断逻辑（与 native 侧 capture_state.cpp 的"规则单一来源"一致）。
 */
internal class MainUI(
    context: Context,
    private val onAction: (Action) -> Unit,
) {
    /** 按钮动作（与状态机事件解耦，便于测试与阅读）。 */
    enum class Action {
        START, FINISH, RECONSTRUCT, CONTINUE,
        EXPORT_GLB, EXPORT_STL, RECAPTURE, RETRY, HOME,
    }

    // ---- 视图引用 ---------------------------------------------------------
    val root: FrameLayout
    private val previewHost: FrameLayout
    private val stateText: TextView
    private val hintText: TextView
    private val progressText: TextView
    private val diagText: TextView
    private val capText: TextView
    private val statText: TextView
    private val resultText: TextView
    private val buttonBar: LinearLayout
    private val buttons: Map<Action, Button>

    init {
        val dp = { v: Int -> (v * context.resources.displayMetrics.density).roundToInt() }

        root = FrameLayout(context).apply {
            setBackgroundColor(Color.rgb(0x10, 0x14, 0x18))
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT)
        }

        // 相机预览承载区（TextureView 由 CaptureActivity 注入，见 attachPreview）。
        previewHost = FrameLayout(context).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT)
        }
        root.addView(previewHost)

        // 半透明信息面板（滚动），叠在预览之上。
        val panel = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.argb(0xD0, 0x08, 0x0B, 0x0E))
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP).apply { topMargin = dp(8); leftMargin = dp(12); rightMargin = dp(12) }
            setPadding(dp(12), dp(10), dp(12), dp(10))
        }

        stateText = makeText(context, dp, size = 20, color = Color.rgb(0xEA, 0xF0, 0xF5), bold = true)
        hintText = makeText(context, dp, size = 13, color = Color.rgb(0x9A, 0xA8, 0xB5))
        progressText = makeText(context, dp, size = 13, color = Color.rgb(0x4C, 0xAF, 0xE0))
        diagText = makeText(context, dp, size = 12, color = Color.rgb(0x6E, 0x7C, 0x88))
        capText = makeText(context, dp, size = 13, color = Color.rgb(0xC6, 0xD2, 0xDC))
        statText = makeText(context, dp, size = 13, color = Color.rgb(0xC6, 0xD2, 0xDC))
        resultText = makeText(context, dp, size = 13, color = Color.rgb(0x8C, 0xE0, 0xA8))

        panel.addView(stateText)
        panel.addView(hintText)
        panel.addView(progressText)
        panel.addView(diagText)
        panel.addView(section(context, dp, R.string.ui_section_capability))
        panel.addView(capText)
        panel.addView(section(context, dp, R.string.ui_section_stats))
        panel.addView(statText)
        panel.addView(section(context, dp, R.string.ui_section_result))
        panel.addView(resultText)

        val scroller = ScrollView(context).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP).apply { topMargin = dp(8); leftMargin = dp(12); rightMargin = dp(12); bottomMargin = dp(96) }
            setBackgroundColor(Color.argb(0xD0, 0x08, 0x0B, 0x0E))
            setPadding(dp(12), dp(10), dp(12), dp(10))
            addView(panel)
        }
        root.addView(scroller)

        // 底部按钮栏。
        buttonBar = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM).apply { bottomMargin = dp(16) }
        }
        buttons = linkedMapOf(
            Action.START to makeButton(context, dp, R.string.ui_btn_start),
            Action.FINISH to makeButton(context, dp, R.string.ui_btn_finish),
            Action.RECONSTRUCT to makeButton(context, dp, R.string.ui_btn_reconstruct),
            Action.CONTINUE to makeButton(context, dp, R.string.ui_btn_continue),
            Action.EXPORT_GLB to makeButton(context, dp, R.string.ui_btn_export_glb),
            Action.EXPORT_STL to makeButton(context, dp, R.string.ui_btn_export_stl),
            Action.RECAPTURE to makeButton(context, dp, R.string.ui_btn_recapture),
            Action.RETRY to makeButton(context, dp, R.string.ui_btn_retry),
            Action.HOME to makeButton(context, dp, R.string.ui_btn_home),
        )
        buttons.forEach { (action, btn) ->
            btn.setOnClickListener { onAction(action) }
            btn.visibility = View.GONE
            buttonBar.addView(btn)
        }
        root.addView(buttonBar)
    }

    /** 由 Activity 把相机预览视图挂到预览承载区。 */
    fun attachPreview(view: View) {
        previewHost.removeAllViews()
        previewHost.addView(view)
    }

    /** 根据当前状态渲染整屏。纯呈现，无状态判断。 */
    fun render(v: ViewState) {
        stateText.text = v.stateLabel
        hintText.text = v.hint
        progressText.text = v.progress
        diagText.text = v.diag
        capText.text = v.capability
        statText.text = v.stats
        resultText.text = v.result

        buttons.forEach { (action, btn) -> btn.visibility = if (action in v.visibleActions) View.VISIBLE else View.GONE }
    }

    /** 一帧渲染所需的数据（Activity 组装）。 */
    data class ViewState(
        val stateLabel: String,
        val hint: String,
        val progress: String = "",
        val diag: String = "",
        val capability: String = "",
        val stats: String = "",
        val result: String = "",
        val visibleActions: Set<Action> = emptySet(),
    )
}

// ---- 视图工厂 -------------------------------------------------------------

private fun makeText(
    context: Context,
    dp: (Int) -> Int,
    size: Int,
    color: Int,
    bold: Boolean = false,
): TextView = TextView(context).apply {
    setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(size).toFloat())
    setTextColor(color)
    if (bold) setTypeface(typeface, Typeface.BOLD)
    layoutParams = LinearLayout.LayoutParams(
        LinearLayout.LayoutParams.MATCH_PARENT,
        LinearLayout.LayoutParams.WRAP_CONTENT).apply { bottomMargin = dp(3) }
}

private fun section(context: Context, dp: (Int) -> Int, labelRes: Int): TextView =
    TextView(context).apply {
        text = context.getString(labelRes)
        setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(14).toFloat())
        setTextColor(Color.rgb(0x4C, 0xAF, 0xE0))
        setTypeface(typeface, Typeface.BOLD)
        layoutParams = LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(8); bottomMargin = dp(2) }
    }

private fun makeButton(context: Context, dp: (Int) -> Int, labelRes: Int): Button =
    Button(context).apply {
        text = context.getString(labelRes)
        isAllCaps = false
        textSize = 13f
        layoutParams = LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT).apply { leftMargin = dp(3); rightMargin = dp(3) }
        minHeight = dp(44)
    }
