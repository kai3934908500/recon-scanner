// SPDX-License-Identifier: BSD-3-Clause
//
// AppShell.kt — 应用外壳：底部 4 Tab + 首次引导层（Android，程序化视图）
//
// 参照 Polycam / RealityScan 等主流 3D 扫描 App 的信息架构：
//   扫描 / 项目 / 参数 / 设置 四个常驻 Tab + 首次使用引导。
// 扫描页复用 [MainUI]（保持其状态机渲染接口不变），其余三页在此实现。
//
// 纯 framework API + 自绘，零新依赖。所有控件回调给 Activity 调真实 C ABI，
// 不做任何"看起来能点实际无效"的假控件。
package com.recon.scan

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.TextView
import kotlin.math.roundToInt

/**
 * 顶层容器。持有一个 [MainUI] 作为扫描页。
 *
 * @param onPreset 精度预设被选（0 快速 / 1 均衡 / 2 高精度）
 * @param onParam 参数变更 (key, intValue, doubleValue)
 * @param onFocusMode 对焦模式 (0 自动 / 1 手动)
 * @param onFocusDistance 手动对焦距离（屈光度，0=无穷远）
 * @param onReplayGuide 重新观看引导
 * @param onDevMode 开发者模式开关
 */
internal class AppShell(
    private val context: Context,
    val scanUI: MainUI,
    private val onPreset: (Int) -> Unit,
    private val onParam: (String, Int, Double) -> Unit,
    private val onFocusMode: (Int) -> Unit,
    private val onFocusDistance: (Float) -> Unit,
    private val onReplayGuide: () -> Unit,
    private val onDevMode: (Boolean) -> Unit,
    private val onGuideDone: () -> Unit,
    private val onResetSettings: () -> Unit,
    private val onQualityTier: (Int) -> Unit,
    private val onGuideConfig: (Int, Boolean, Boolean) -> Unit,
    private val guideSummary: String = "",
) {
    private object C {
        val SURFACE = Color.rgb(0xFF, 0xFF, 0xFF)
        val BG = Color.rgb(0xF7, 0xF8, 0xFA)
        val BORDER = Color.rgb(0xE5, 0xE7, 0xEB)
        val PRIMARY = Color.rgb(0x11, 0x18, 0x27)
        val SECOND = Color.rgb(0x6B, 0x72, 0x80)
        val MUTED = Color.rgb(0x9C, 0xA3, 0xAF)
        val ACCENT = Color.rgb(0x25, 0x63, 0xEB)
        val ACCENT_SOFT = Color.rgb(0xEF, 0xF6, 0xFF)
        val OK = Color.rgb(0x05, 0x96, 0x69)
    }

    enum class Tab { SCAN, PROJECTS, PARAMS, SETTINGS }

    val root: FrameLayout
    private val pages: FrameLayout
    private val guide: GuideOverlay
    private val tabViews = mutableMapOf<Tab, View>()
    private var currentTab = Tab.SCAN

    // 参数页/项目页/设置页控件引用
    private var presetButtons = mutableMapOf<Int, Button>()
    private var paramValueTexts = mutableMapOf<String, TextView>()
    private var focusAutoBtn: Button? = null
    private var focusManualBtn: Button? = null
    private var focusSeek: SeekBar? = null
    private var focusValueText: TextView? = null
    private var devToggle: Button? = null
    private var devModeOn = false
    private var resultText: TextView? = null
    private var meshPreview: MeshPreviewView? = null
    private var previewHint: TextView? = null

    private lateinit var bottomNav: LinearLayout
    private val dp = { v: Int -> (v * context.resources.displayMetrics.density).roundToInt() }

    init {
        root = FrameLayout(context).apply {
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
            setBackgroundColor(C.SURFACE)
        }

        val column = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
        }
        root.addView(column)

        pages = FrameLayout(context).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
        }
        column.addView(pages)

        // 扫描页：直接嵌入 MainUI.root
        pages.addView(scanUI.root, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))

        // 其余三页（懒创建，首次访问时填充）
        tabViews[Tab.PROJECTS] = buildProjectsPage()
        tabViews[Tab.PARAMS] = buildParamsPage()
        tabViews[Tab.SETTINGS] = buildSettingsPage()
        tabViews.values.forEach { page ->
            page.visibility = View.GONE
            pages.addView(page, FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        }

        // 底部分隔线 + Tab 栏
        column.addView(View(context).apply {
            setBackgroundColor(C.BORDER)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(1))
        })
        bottomNav = buildBottomNav()
        column.addView(bottomNav)

        // 引导层（覆盖全屏，首次显示）
        guide = GuideOverlay(context, dp, guideSummary,
            onConfig = { preset, hud, imu -> onGuideConfig(preset, hud, imu) }
        ) {
            guide.visibility = View.GONE
            prefs().edit().putBoolean("guide_seen", true).apply()
            onGuideDone()   // 引导遮挡期相机被后台拒绝，结束后主动重开
        }
        root.addView(guide, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        guide.visibility = if (prefs().getBoolean("guide_seen", false)) View.GONE else View.VISIBLE
    }

    /** 采集全屏时隐藏底部导航，回到纯沉浸取景。 */
    fun setCaptureFullscreen(on: Boolean) {
        bottomNav.visibility = if (on) View.GONE else View.VISIBLE
    }

    private fun prefs() = context.getSharedPreferences("recon_prefs", Context.MODE_PRIVATE)

    /** 重新显示引导层（设置页"重新观看"用）。 */
    fun showGuide() {
        guide.visibility = View.VISIBLE
        guide.reset()
    }

    // ---- 底部导航 ----
    private fun buildBottomNav(): LinearLayout {
        val nav = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(C.SURFACE)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            setPadding(0, dp(6), 0, dp(10))
        }
        val items = listOf(
            Tab.SCAN to R.string.tab_scan,
            Tab.PROJECTS to R.string.tab_projects,
            Tab.PARAMS to R.string.tab_params,
            Tab.SETTINGS to R.string.tab_settings,
        )
        items.forEach { (tab, labelRes) ->
            nav.addView(LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                gravity = Gravity.CENTER
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                setPadding(dp(4), dp(6), dp(4), dp(6))
                isClickable = true
                setOnClickListener { selectTab(tab) }
                addView(NavIcon(context, dp, tab).apply {
                    layoutParams = LinearLayout.LayoutParams(dp(22), dp(22))
                })
                addView(TextView(context).apply {
                    text = context.getString(labelRes)
                    setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(11).toFloat())
                    layoutParams = LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                        topMargin = dp(3)
                    }
                })
            })
        }
        return nav
    }

    fun selectTab(tab: Tab) {
        if (tab == currentTab) return
        currentTab = tab
        scanUI.root.visibility = if (tab == Tab.SCAN) View.VISIBLE else View.GONE
        tabViews.forEach { (t, v) -> v.visibility = if (t == tab) View.VISIBLE else View.GONE }
        // M3 motion：页面淡入 + 轻微上移，150ms（对标大厂页面切换）
        val shown = if (tab == Tab.SCAN) scanUI.root else tabViews[tab]
        shown?.apply {
            alpha = 0f
            translationY = dp(8).toFloat()
            animate().alpha(1f).translationY(0f).setDuration(150)
                .setInterpolator(android.view.animation.DecelerateInterpolator()).start()
        }
        refreshNavTint()
    }

    private fun refreshNavTint() {
        // 选中态 tint：简单地在各 Tab 根 view 加一层背景色区分
        val nav = (root.getChildAt(0) as? LinearLayout)?.getChildAt(2) as? LinearLayout ?: return
        for (i in 0 until nav.childCount) {
            val child = nav.getChildAt(i)
            val tab = Tab.values().getOrNull(i)
            val selected = tab == currentTab
            child.background = if (selected) roundedBg(C.ACCENT_SOFT) else null
            // 图标 + 文字同步着色（M3 选中态）
            (child as? ViewGroup)?.getChildAt(0)?.let { (it as? NavIcon)?.setActive(selected) }
            (child as? ViewGroup)?.getChildAt(1)?.let { (it as? TextView)?.setTextColor(if (selected) C.ACCENT else C.SECOND) }
        }
    }

    // ---- 项目页 ----
    private fun buildProjectsPage(): View {
        val scroll = ScrollView(context).apply { setBackgroundColor(C.BG) }
        val page = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(C.BG)
            setPadding(dp(16), dp(20), dp(16), dp(24))
        }
        page.addView(TextView(context).apply {
            text = context.getString(R.string.projects_title)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(22).toFloat())
            setTextColor(C.PRIMARY)
            typeface = Typeface.DEFAULT_BOLD
        })
        val card = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            background = roundedBg(C.SURFACE, C.BORDER)
            setPadding(dp(16), dp(16), dp(16), dp(16))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                topMargin = dp(16)
            }
            if (android.os.Build.VERSION.SDK_INT >= 21) elevation = dp(1).toFloat()
        }
        card.addView(TextView(context).apply {
            text = context.getString(R.string.projects_current)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(15).toFloat())
            setTextColor(C.PRIMARY)
            typeface = Typeface.DEFAULT_BOLD
        })
        resultText = TextView(context).apply {
            text = context.getString(R.string.projects_empty_body)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(13).toFloat())
            setTextColor(C.SECOND)
            setPadding(0, dp(10), 0, dp(12))
        }
        card.addView(resultText)
        // 3D 线框预览（GLES2）；无模型时上层提示可见，预览区留浅色占位
        meshPreview = MeshPreviewView(context).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(240)).apply { topMargin = dp(6) }
            setBackgroundColor(Color.rgb(0xF2, 0xF5, 0xF9))
        }
        card.addView(meshPreview)
        previewHint = TextView(context).apply {
            text = context.getString(R.string.projects_preview_hint)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(11).toFloat())
            setTextColor(C.MUTED)
            gravity = Gravity.CENTER
            setPadding(0, dp(6), 0, dp(4))
        }
        card.addView(previewHint)
        val goBtn = makeSecondaryButton(R.string.projects_go_scan).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                topMargin = dp(10)
            }
            setOnClickListener { selectTab(Tab.SCAN) }
        }
        card.addView(goBtn)
        page.addView(card)
        scroll.addView(page)
        return scroll
    }

    /** 由 Activity 在重建完成后更新项目页的结果摘要。 */
    fun setResultSummary(summary: String) {
        resultText?.text = summary
    }

    /** 设置 3D 预览网格几何；positions=XYZ交错，indices=三角面。 */
    fun setPreviewMesh(positions: FloatArray?, indices: IntArray?) {
        if (positions == null || indices == null) {
            meshPreview?.clear()
            previewHint?.visibility = View.VISIBLE
        } else {
            meshPreview?.setMesh(positions, indices)
            previewHint?.visibility = View.GONE
        }
    }

    // ---- 参数页 ----
    private fun buildParamsPage(): View {
        val scroll = ScrollView(context).apply { setBackgroundColor(C.BG) }
        val page = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(20), dp(16), dp(24))
        }
        scroll.addView(page)
        page.addView(TextView(context).apply {
            text = context.getString(R.string.params_title)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(20).toFloat())
            setTextColor(C.PRIMARY); typeface = Typeface.DEFAULT_BOLD
        })

        // 精度预设
        page.addView(sectionLabel(R.string.params_preset))
        val presetRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                topMargin = dp(4)
            }
        }
        val presets = listOf(
            0 to R.string.params_preset_fast,
            1 to R.string.params_preset_balanced,
            2 to R.string.params_preset_quality,
        )
        presets.forEach { (id, label) ->
            val b = makeSecondaryButton(label).apply {
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                    setMargins(dp(2), 0, dp(2), 0)
                }
                setOnClickListener {
                    onPreset(id)
                    presetButtons.forEach { (k, v) -> v.setBackgroundDrawable(
                        if (k == id) solidBg(C.ACCENT) else outlineBg()) }
                    presetButtons.forEach { (k, v) -> v.setTextColor(if (k == id) Color.WHITE else C.PRIMARY) }
                }
            }
            presetButtons[id] = b
            presetRow.addView(b)
        }
        page.addView(presetRow)

        // 参数组：全部连真实 C ABI key
        page.addView(sectionLabel(R.string.params_section_sfm))
        val sfmParams = listOf(
            "sfm_feature_count" to R.string.params_sfm_features,
            "sfm_min_track_length" to R.string.params_sfm_track,
        )
        sfmParams.forEach { (key, label) -> page.addView(paramRow(key, label)) }
        page.addView(sectionLabel(R.string.params_section_vio))
        page.addView(paramRow("vio_feature_count", R.string.params_vio_features))
        page.addView(sectionLabel(R.string.params_section_mesh))
        page.addView(paramRow("mesh_poisson_depth", R.string.params_mesh_poisson))

        page.addView(TextView(context).apply {
            text = context.getString(R.string.params_hint)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(11).toFloat())
            setTextColor(C.MUTED)
            setPadding(0, dp(14), 0, 0)
        })
        return scroll
    }

        private fun sectionLabel(res: Int): TextView = TextView(context).apply {
        text = context.getString(res)
        setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(13).toFloat())
        setTextColor(C.ACCENT); typeface = Typeface.DEFAULT_BOLD
        setPadding(0, dp(16), 0, dp(6))
    }

    private fun paramRow(key: String, labelRes: Int): View {
        val row = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = roundedBg(C.SURFACE, C.BORDER)
            setPadding(dp(12), dp(10), dp(12), dp(10))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(0, 0, 0, dp(8))
            }
        }
        row.addView(TextView(context).apply {
            text = context.getString(labelRes)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(12).toFloat())
            setTextColor(C.SECOND)
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        })
        val minus = TextView(context).apply {
            text = "−"; setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(20).toFloat())
            setTextColor(C.ACCENT); setPadding(dp(10), 0, dp(10), 0)
            setOnClickListener { adjustParam(key, -1) }
        }
        val value = TextView(context).apply {
            text = "—"
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(13).toFloat())
            setTextColor(C.PRIMARY); typeface = Typeface.DEFAULT_BOLD
            layoutParams = LinearLayout.LayoutParams(dp(52), ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                gravity = Gravity.CENTER
            }
        }
        val plus = TextView(context).apply {
            text = "+"; setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(20).toFloat())
            setTextColor(C.ACCENT); setPadding(dp(10), 0, dp(10), 0)
            setOnClickListener { adjustParam(key, 1) }
        }
        paramValueTexts[key] = value
        row.addView(minus); row.addView(value); row.addView(plus)
        return row
    }

    private val defaultParamValues = mutableMapOf(
        "sfm_feature_count" to 3000.0,
        "sfm_min_track_length" to 3.0,
        "vio_feature_count" to 300.0,
        "mesh_poisson_depth" to 10.0,
        "cap_resolution_width" to 1280.0,
        "cap_frame_rate" to 30.0,
        "cap_exposure_time_us" to 10000.0,
        "cap_iso" to 100.0,
        "sfm_ransac_max_iterations" to 10000.0,
        "sfm_ba_max_iterations" to 100.0,
        "mvs_patch_size" to 7.0,
        "mvs_num_views" to 5.0,
        "mvs_subpixel_iterations" to 3.0,
        "mesh_poisson_samples_per_node" to 1.0,
    )
    private val paramDefaults = defaultParamValues.toMutableMap()
    private val paramSteps = mutableMapOf(
        "sfm_feature_count" to 500.0,
        "sfm_min_track_length" to 1.0,
        "vio_feature_count" to 50.0,
        "mesh_poisson_depth" to 1.0,
        "cap_resolution_width" to 160.0,
        "cap_frame_rate" to 5.0,
        "cap_exposure_time_us" to 1000.0,
        "cap_iso" to 50.0,
        "sfm_ransac_max_iterations" to 5000.0,
        "sfm_ba_max_iterations" to 25.0,
        "mvs_patch_size" to 2.0,
        "mvs_num_views" to 1.0,
        "mvs_subpixel_iterations" to 1.0,
        "mesh_poisson_samples_per_node" to 1.0,
    )

    private fun adjustParam(key: String, dir: Int) {
        val cur = paramDefaults[key] ?: return
        val step = paramSteps[key] ?: 1.0
        val next = (cur + dir * step).coerceAtLeast(0.0)
        paramDefaults[key] = next
        paramValueTexts[key]?.text = if (next == next.toInt().toDouble()) next.toInt().toString()
                                     else String.format("%.1f", next)
        onParam(key, next.toInt(), next)
    }

    /** Activity 读回内核真实值后回填显示。 */
    fun setParamValue(key: String, intValue: Int, doubleValue: Double) {
        val v = if (doubleValue != 0.0 && doubleValue != intValue.toDouble()) doubleValue else intValue.toDouble()
        paramDefaults[key] = v
        paramValueTexts[key]?.text = if (v == v.toInt().toDouble()) v.toInt().toString()
                                     else String.format("%.1f", v)
    }

    // ---- 设置页 ----
    private fun buildSettingsPage(): View {
        val scroll = ScrollView(context).apply { setBackgroundColor(C.BG) }
        val page = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(20), dp(16), dp(24))
        }
        scroll.addView(page)
        page.addView(TextView(context).apply {
            text = context.getString(R.string.settings_title)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(20).toFloat())
            setTextColor(C.PRIMARY); typeface = Typeface.DEFAULT_BOLD
        })

        page.addView(sectionLabel(R.string.settings_section_capture))
        val focusCard = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            background = roundedBg(C.SURFACE, C.BORDER)
            setPadding(dp(14), dp(12), dp(14), dp(14))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(0, dp(6), 0, dp(12))
            }
        }
        focusCard.addView(TextView(context).apply {
            text = context.getString(R.string.settings_focus_mode)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(13).toFloat())
            setTextColor(C.SECOND)
        })
        val focusRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(0, dp(8), 0, dp(4))
            }
        }
        focusAutoBtn = makeSecondaryButton(R.string.settings_focus_auto).apply {
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                setMargins(0, 0, dp(4), 0)
            }
            setOnClickListener { setFocusUi(auto = true); onFocusMode(0) }
        }
        focusManualBtn = makeSecondaryButton(R.string.settings_focus_manual).apply {
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                setMargins(dp(4), 0, 0, 0)
            }
            setOnClickListener { setFocusUi(auto = false); onFocusMode(1) }
        }
        focusRow.addView(focusAutoBtn); focusRow.addView(focusManualBtn)
        focusCard.addView(focusRow)

        focusValueText = TextView(context).apply {
            text = "∞"
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(12).toFloat())
            setTextColor(C.PRIMARY)
            setPadding(0, dp(10), 0, dp(2))
        }
        focusCard.addView(focusValueText)
        focusSeek = SeekBar(context).apply {
            max = 100
            progress = 0
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(sb: SeekBar?, p: Int, fromUser: Boolean) {
                    if (!fromUser) return
                    val diopters = p / 10f   // 0..10 屈光度
                    focusValueText?.text = if (p == 0) "∞" else "${String.format("%.1f", diopters)} D"
                    onFocusDistance(diopters)
                }
                override fun onStartTrackingTouch(sb: SeekBar?) = Unit
                override fun onStopTrackingTouch(sb: SeekBar?) = Unit
            })
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        }
        focusCard.addView(focusSeek)
        page.addView(focusCard)
        setFocusUi(auto = true)

        // 采集质量：全部是 params.h 里的真实 key，改值直接 setParam 进内核
        page.addView(sectionLabel(R.string.settings_section_quality))
        // 采集画质档位（取景始终最高，此项只影响送内核帧）
        val tierCard = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            background = roundedBg(C.SURFACE, C.BORDER)
            setPadding(dp(12), dp(10), dp(12), dp(10))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(0, dp(6), 0, dp(8))
            }
        }
        tierCard.addView(TextView(context).apply {
            text = context.getString(R.string.settings_quality_tier)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(12).toFloat())
            setTextColor(C.SECOND)
        })
        val tierRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(0, dp(8), 0, 0)
            }
        }
        val tierLabels = listOf(R.string.settings_tier_smooth,
            R.string.settings_tier_balanced, R.string.settings_tier_quality)
        var currentTier = 1
        tierLabels.forEachIndexed { idx, label ->
            val b = makeSecondaryButton(label).apply {
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                    setMargins(dp(2), 0, dp(2), 0)
                }
                setButtonActive(this, idx == currentTier)
                setOnClickListener {
                    currentTier = idx
                    tierLabels.forEachIndexed { i, _ -> setButtonActive(tierRow.getChildAt(i) as? Button, i == idx) }
                    onQualityTier(idx)
                }
            }
            tierRow.addView(b)
        }
        tierCard.addView(tierRow)
        val tierHint = TextView(context).apply {
            text = context.getString(R.string.settings_tier_hint)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(10).toFloat())
            setTextColor(C.MUTED)
            setPadding(0, dp(8), 0, 0)
        }
        tierCard.addView(tierHint)
        page.addView(tierCard)
        val qualityCard = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            background = roundedBg(C.SURFACE, C.BORDER)
            setPadding(dp(12), dp(8), dp(12), dp(10))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(0, dp(6), 0, dp(10))
            }
        }
        qualityCard.addView(paramRow("cap_resolution_width", R.string.settings_resolution))
        qualityCard.addView(paramRow("cap_frame_rate", R.string.settings_framerate))
        qualityCard.addView(paramRow("cap_exposure_time_us", R.string.settings_exposure))
        qualityCard.addView(paramRow("cap_iso", R.string.settings_iso))
        val imuBtn = makeSecondaryButton(R.string.settings_imu_enabled).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                topMargin = dp(4)
            }
        }
        var imuOn = true
        imuBtn.text = "${context.getString(R.string.settings_imu_enabled)}：${context.getString(R.string.settings_on)}"
        setButtonActive(imuBtn, true)
        imuBtn.setOnClickListener {
            imuOn = !imuOn
            val stateRes = if (imuOn) R.string.settings_on else R.string.settings_off
            imuBtn.text = context.getString(R.string.settings_imu_enabled) + "：" + context.getString(stateRes)
            setButtonActive(imuBtn, imuOn)
            onParam("cap_imu_enabled", if (imuOn) 1 else 0, 0.0)
        }
        qualityCard.addView(imuBtn)
        val imuHint = TextView(context).apply {
            text = context.getString(R.string.settings_imu_hint)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(10).toFloat())
            setTextColor(C.MUTED)
            setPadding(dp(2), dp(6), dp(2), 0)
        }
        qualityCard.addView(imuHint)
        page.addView(qualityCard)

        // 重建精度：全部是内核真实 key（默认/范围来自 params.cpp 注册）
        page.addView(sectionLabel(R.string.settings_section_recon))
        val reconCard = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            background = roundedBg(C.SURFACE, C.BORDER)
            setPadding(dp(12), dp(8), dp(12), dp(10))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(0, dp(6), 0, dp(10))
            }
        }
        reconCard.addView(paramRow("sfm_ransac_max_iterations", R.string.settings_ransac_iter))
        reconCard.addView(paramRow("sfm_ba_max_iterations", R.string.settings_ba_iter))
        reconCard.addView(paramRow("mvs_patch_size", R.string.settings_mvs_patch))
        reconCard.addView(paramRow("mvs_num_views", R.string.settings_mvs_views))
        reconCard.addView(paramRow("mvs_subpixel_iterations", R.string.settings_mvs_subpixel))
        reconCard.addView(paramRow("mesh_poisson_samples_per_node", R.string.settings_poisson_samples))
        page.addView(reconCard)

        page.addView(sectionLabel(R.string.settings_section_guide))
        page.addView(makeSecondaryButton(R.string.settings_replay_guide).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            setOnClickListener { onReplayGuide() }
        })

        page.addView(sectionLabel(R.string.settings_section_advanced))
        devToggle = makeSecondaryButton(R.string.settings_dev_mode).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            setOnClickListener {
                devModeOn = !devModeOn
                setButtonActive(this, devModeOn)
                onDevMode(devModeOn)
            }
        }
        page.addView(devToggle)
        page.addView(TextView(context).apply {
            text = context.getString(R.string.settings_dev_hint)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(11).toFloat())
            setTextColor(C.MUTED); setPadding(0, dp(4), 0, 0)
        })

        // 一键还原：所有参数/预设/对焦恢复出厂默认
        page.addView(sectionLabel(R.string.settings_section_reset))
        val resetBtn = makeSecondaryButton(R.string.settings_reset).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(0, dp(6), 0, dp(4))
            }
            setTextColor(Color.rgb(0xDC, 0x26, 0x26))
            background = android.graphics.drawable.GradientDrawable().apply {
                setColor(Color.WHITE)
                setStroke(1, Color.rgb(0xDC, 0x26, 0x26)); cornerRadius = dp(8).toFloat() }
            setOnClickListener {
                // 复位本 UI 的所有控件状态
                paramDefaults.putAll(defaultParamValues)
                paramDefaults.keys.forEach { k -> paramValueTexts[k]?.text = paramDefaults[k]?.toInt()?.toString() ?: "—" }
                setFocusUi(auto = true)
                focusSeek?.progress = 0
                devModeOn = false
                setButtonActive(devToggle, false)
                presetButtons.forEach { (k, v) ->
                    setButtonActive(v, k == 1)   // 默认均衡档
                }
                onResetSettings()
            }
        }
        page.addView(resetBtn)
        page.addView(TextView(context).apply {
            text = context.getString(R.string.settings_reset_hint)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(11).toFloat())
            setTextColor(C.MUTED); setPadding(0, dp(4), 0, 0)
        })

        page.addView(sectionLabel(R.string.settings_section_about))
        page.addView(TextView(context).apply {
            text = context.getString(R.string.settings_about_body)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(12).toFloat())
            setTextColor(C.SECOND)
        })
        return scroll
    }

    private fun setFocusUi(auto: Boolean) {
        setButtonActive(focusAutoBtn, auto)
        setButtonActive(focusManualBtn, !auto)
        focusSeek?.isEnabled = !auto
        focusValueText?.alpha = if (auto) 0.4f else 1f
    }

    // ---- 控件工厂 ----
    private fun makeSecondaryButton(res: Int): Button = Button(context).apply {
        text = context.getString(res)
        isAllCaps = false
        setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(13).toFloat())
        setTextColor(C.PRIMARY)
        background = outlineBg()
        minHeight = dp(40)
        stateListAnimator = null
        setPadding(dp(12), dp(8), dp(12), dp(8))
        isHapticFeedbackEnabled = true
        setOnTouchListener { v, ev ->
            when (ev.actionMasked) {
                android.view.MotionEvent.ACTION_DOWN -> {
                    v.performHapticFeedback(android.view.HapticFeedbackConstants.VIRTUAL_KEY)
                    v.animate().scaleX(0.97f).scaleY(0.97f).setDuration(90).start()
                }
                android.view.MotionEvent.ACTION_UP,
                android.view.MotionEvent.ACTION_CANCEL ->
                    v.animate().scaleX(1f).scaleY(1f).setDuration(140).start()
            }
            false
        }
    }

    private fun setButtonActive(b: Button?, active: Boolean) {
        if (b == null) return
        b.setBackgroundDrawable(if (active) solidBg(C.ACCENT) else outlineBg())
        b.setTextColor(if (active) Color.WHITE else C.PRIMARY)
    }
    private fun outlineBg() = GradientDrawableCompat(C.SURFACE, C.BORDER, dp(8).toFloat())
    private fun solidBg(color: Int) = GradientDrawableCompat(color, color, dp(8).toFloat())
    private fun roundedBg(fill: Int, stroke: Int? = null) =
        GradientDrawableCompat(fill, stroke, dp(10).toFloat())

    private class GradientDrawableCompat(
        fill: Int, stroke: Int?, radius: Float,
    ) : android.graphics.drawable.GradientDrawable() {
        init {
            setColor(fill)
            if (stroke != null) setStroke(dp1, stroke)
            cornerRadius = radius
        }
        // setStroke 的宽需要在 init 前算；用一个固定 1px 近似即可（企业细边框）
        companion object { const val dp1 = 1 }
    }

    // ---- 自绘：导航图标 ----
    private class NavIcon(context: Context, val dp: (Int) -> Int, val tab: Tab) : View(context) {
        private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = C.SECOND; style = Paint.Style.STROKE; strokeWidth = 1.6f * context.resources.displayMetrics.density
        }
        fun setActive(on: Boolean) { paint.color = if (on) C.ACCENT else C.SECOND; invalidate() }
        override fun onDraw(canvas: Canvas) {
            super.onDraw(canvas)
            val w = width.toFloat(); val h = height.toFloat()
            when (tab) {
                Tab.SCAN -> {
                    // 取景框四角
                    val l = dp(2).toFloat()
                    val seg = w * 0.28f
                    val pts = listOf(
                        floatArrayOf(l,l,l+seg,l), floatArrayOf(l,l,l,l+seg),
                        floatArrayOf(w-l,l,w-l-seg,l), floatArrayOf(w-l,l,w-l,l+seg),
                        floatArrayOf(l,h-l,l+seg,h-l), floatArrayOf(l,h-l,l,h-l-seg),
                        floatArrayOf(w-l,h-l,w-l-seg,h-l), floatArrayOf(w-l,h-l,w-l,h-l-seg))
                    pts.forEach { canvas.drawLine(it[0], it[1], it[2], it[3], paint) }
                    canvas.drawCircle(w/2, h/2, w*0.13f, paint)
                }
                Tab.PROJECTS -> {
                    canvas.drawRect(w*0.12f, h*0.2f, w*0.88f, h*0.85f, paint)
                    canvas.drawLine(w*0.12f, h*0.4f, w*0.88f, h*0.4f, paint)
                }
                Tab.PARAMS -> {
                    canvas.drawLine(w*0.12f, h*0.3f, w*0.88f, h*0.3f, paint)
                    canvas.drawLine(w*0.12f, h*0.7f, w*0.88f, h*0.7f, paint)
                    canvas.drawCircle(w*0.35f, h*0.3f, h*0.1f, paint)
                    canvas.drawCircle(w*0.68f, h*0.7f, h*0.1f, paint)
                }
                Tab.SETTINGS -> {
                    canvas.drawCircle(w/2, h/2, h*0.22f, paint)
                    canvas.drawCircle(w/2, h/2, h*0.08f, paint)
                    for (i in 0..7) {
                        val ang = Math.toRadians(i * 45.0).toFloat()
                        val x1 = w/2 + Math.cos(ang.toDouble()).toFloat() * h*0.28f
                        val y1 = h/2 + Math.sin(ang.toDouble()).toFloat() * h*0.28f
                        val x2 = w/2 + Math.cos(ang.toDouble()).toFloat() * h*0.42f
                        val y2 = h/2 + Math.sin(ang.toDouble()).toFloat() * h*0.42f
                        canvas.drawLine(x1, y1, x2, y2, paint)
                    }
                }
            }
        }
    }

    // ---- 首次引导层 ----
    private class GuideOverlay(
        context: Context,
        dp: (Int) -> Int,
        private val summary: String,
        private val onConfig: (Int, Boolean, Boolean) -> Unit,
        onDone: () -> Unit,
    ) : FrameLayout(context) {
        private val pages = listOf(
            R.string.guide_1_title to R.string.guide_1_body,
            R.string.guide_2_title to R.string.guide_2_body,
            R.string.guide_tips_title to R.string.guide_tips_body,
            R.string.guide_cfg_title to R.string.guide_cfg_body,
            R.string.guide_3_title to R.string.guide_3_body,
            R.string.guide_4_title to R.string.guide_4_body,
        )
        private val tips = listOf(
            R.string.guide_tip_1, R.string.guide_tip_2,
            R.string.guide_tip_3, R.string.guide_tip_4)
        private val tipsBox: LinearLayout
        private val configBox: LinearLayout
        private val summaryCard: TextView
        private var cfgPreset = 1
        private var cfgHud = true
        private var cfgImu = true
        private var index = 0
        private val title: TextView
        private val body: TextView
        private val nextBtn: Button
        private val skipBtn: TextView

        init {
            setBackgroundColor(Color.rgb(0xFA, 0xFB, 0xFC))
            val box = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                gravity = Gravity.CENTER
                layoutParams = LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
                setPadding(dp(32), dp(40), dp(32), dp(40))
            }
            title = TextView(context).apply {
                setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(24).toFloat())
                setTextColor(Color.rgb(0x11, 0x18, 0x27)); typeface = Typeface.DEFAULT_BOLD
                gravity = Gravity.CENTER
            }
            body = TextView(context).apply {
                setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(14).toFloat())
                setTextColor(Color.rgb(0x6B, 0x72, 0x80)); gravity = Gravity.CENTER
                setLineSpacing(dp(6).toFloat(), 1f)
                setPadding(0, dp(16), 0, 0)
            }
            summaryCard = TextView(context).apply {
                setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(12).toFloat())
                setTextColor(Color.rgb(0x25, 0x63, 0xEB))
                gravity = Gravity.CENTER
                setPadding(dp(14), dp(12), dp(14), dp(12))
                background = android.graphics.drawable.GradientDrawable().apply {
                    setColor(Color.rgb(0xEF, 0xF6, 0xFF))
                    setStroke(1, Color.rgb(0xDB, 0xEA, 0xFE)); cornerRadius = dp(8).toFloat() }
                visibility = View.GONE
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(18) }
            }
            tipsBox = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                visibility = View.GONE
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(16) }
            }
            tips.forEachIndexed { i, res ->
                tipsBox.addView(LinearLayout(context).apply {
                    orientation = LinearLayout.HORIZONTAL
                    gravity = Gravity.CENTER_VERTICAL
                    setPadding(dp(10), dp(7), dp(10), dp(7))
                    addView(TextView(context).apply {
                        text = "${i + 1}"
                        setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(12).toFloat())
                        setTextColor(Color.WHITE); typeface = Typeface.DEFAULT_BOLD
                        gravity = Gravity.CENTER
                        background = android.graphics.drawable.GradientDrawable().apply {
                            setColor(Color.rgb(0x25, 0x63, 0xEB)); cornerRadius = dp(999).toFloat() }
                        layoutParams = LinearLayout.LayoutParams(dp(24), dp(24))
                    })
                    addView(TextView(context).apply {
                        text = context.getString(res)
                        setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(12).toFloat())
                        setTextColor(Color.rgb(0x6B, 0x72, 0x80))
                        setPadding(dp(10), 0, 0, 0)
                        layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                    })
                })
            }
            configBox = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                visibility = View.GONE
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(12) }
            }
            // 精度档 3 选 1
            configBox.addView(TextView(context).apply {
                text = context.getString(R.string.guide_cfg_precision)
                setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(12).toFloat())
                setTextColor(Color.rgb(0x25, 0x63, 0xEB)); typeface = Typeface.DEFAULT_BOLD
                setPadding(0, dp(8), 0, dp(4))
            })
            val presetRow = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            }
            val presetLabels = listOf(R.string.params_preset_fast,
                R.string.params_preset_balanced, R.string.params_preset_quality)
            presetLabels.forEachIndexed { idx, label ->
                val b = Button(context).apply {
                    text = context.getString(label); isAllCaps = false
                    setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(11).toFloat())
                    layoutParams = LinearLayout.LayoutParams(0, dp(38), 1f).apply { setMargins(dp(2),0,dp(2),0) }
                    stateListAnimator = null
                    background = android.graphics.drawable.GradientDrawable().apply {
                        setColor(if (idx == cfgPreset) Color.rgb(0x25,0x63,0xEB) else Color.WHITE)
                        setStroke(1, Color.rgb(0xDB,0xEA,0xFE)); cornerRadius = dp(8).toFloat() }
                    setTextColor(if (idx == cfgPreset) Color.WHITE else Color.rgb(0x6B,0x72,0x80))
                    setOnClickListener {
                        cfgPreset = idx
                        for (i in 0 until presetRow.childCount) {
                            val cb = presetRow.getChildAt(i) as Button
                            cb.setBackgroundDrawable(android.graphics.drawable.GradientDrawable().apply {
                                setColor(if (i == cfgPreset) Color.rgb(0x25,0x63,0xEB) else Color.WHITE)
                                setStroke(1, Color.rgb(0xDB,0xEA,0xFE)); cornerRadius = dp(8).toFloat() })
                            cb.setTextColor(if (i == cfgPreset) Color.WHITE else Color.rgb(0x6B,0x72,0x80))
                        }
                    }
                }
                presetRow.addView(b)
            }
            configBox.addView(presetRow)
            // 两个开关：HUD / IMU
            fun addSwitch(labelRes: Int, hintRes: Int, initial: Boolean, onToggle: (Boolean) -> Unit) {
                val row = LinearLayout(context).apply {
                    orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
                    setPadding(dp(8), dp(9), dp(8), dp(9))
                    layoutParams = LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(6) }
                    background = android.graphics.drawable.GradientDrawable().apply {
                        setColor(Color.rgb(0xF2,0xF5,0xF9)); cornerRadius = dp(8).toFloat() }
                }
                val labels = LinearLayout(context).apply {
                    orientation = LinearLayout.VERTICAL
                    layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                }
                labels.addView(TextView(context).apply {
                    text = context.getString(labelRes); setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(12).toFloat())
                    setTextColor(Color.rgb(0x11,0x18,0x27)); typeface = Typeface.DEFAULT_BOLD })
                labels.addView(TextView(context).apply {
                    text = context.getString(hintRes); setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(10).toFloat())
                    setTextColor(Color.rgb(0x9C,0xA3,0xAF)) })
                row.addView(labels)
                val sw = Button(context).apply {
                    text = context.getString(if (initial) R.string.settings_on else R.string.settings_off)
                    isAllCaps = false; setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(12).toFloat())
                    minWidth = dp(56); stateListAnimator = null
                    background = android.graphics.drawable.GradientDrawable().apply {
                        setColor(if (initial) Color.rgb(0x25,0x63,0xEB) else Color.WHITE)
                        setStroke(1, Color.rgb(0xDB,0xEA,0xFE)); cornerRadius = dp(999).toFloat() }
                    setTextColor(if (initial) Color.WHITE else Color.rgb(0x6B,0x72,0x80))
                }
                var on = initial
                sw.setOnClickListener {
                    on = !on
                    sw.text = context.getString(if (on) R.string.settings_on else R.string.settings_off)
                    sw.setBackgroundDrawable(android.graphics.drawable.GradientDrawable().apply {
                        setColor(if (on) Color.rgb(0x25,0x63,0xEB) else Color.WHITE)
                        setStroke(1, Color.rgb(0xDB,0xEA,0xFE)); cornerRadius = dp(999).toFloat() })
                    sw.setTextColor(if (on) Color.WHITE else Color.rgb(0x6B,0x72,0x80))
                    onToggle(on)
                }
                row.addView(sw)
                configBox.addView(row)
            }
            addSwitch(R.string.guide_cfg_hud, R.string.guide_cfg_hud_hint, true) { cfgHud = it }
            addSwitch(R.string.guide_cfg_imu, R.string.guide_cfg_imu_hint, true) { cfgImu = it }
            nextBtn = Button(context).apply {
                isAllCaps = false
                setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(15).toFloat())
                setTextColor(Color.WHITE)
                background = android.graphics.drawable.GradientDrawable().apply {
                    setColor(Color.rgb(0x25, 0x63, 0xEB)); cornerRadius = dp(8).toFloat() }
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, dp(48)).apply { topMargin = dp(40) }
                setOnClickListener {
                    index++
                    if (index >= pages.size) { onConfig(cfgPreset, cfgHud, cfgImu); onDone() } else render()
                }
            }
            skipBtn = TextView(context).apply {
                text = context.getString(R.string.guide_skip)
                setTextSize(TypedValue.COMPLEX_UNIT_PX, dp(13).toFloat())
                setTextColor(Color.rgb(0x9C, 0xA3, 0xAF))
                gravity = Gravity.CENTER
                setPadding(0, dp(16), 0, 0)
                setOnClickListener { onConfig(cfgPreset, cfgHud, cfgImu); onDone() }
            }
            // 装饰立方体
            box.addView(CubeArt(context, dp).apply {
                layoutParams = LinearLayout.LayoutParams(dp(96), dp(96)).apply { bottomMargin = dp(32) }
            })
            box.addView(title)
            box.addView(body)
            box.addView(tipsBox)
            box.addView(configBox)
            box.addView(summaryCard)
            box.addView(nextBtn)
            box.addView(skipBtn)
            addView(box)
            render()
        }
        fun reset() { index = 0; render() }
        private fun render() {
            title.text = context.getString(pages[index].first)
            body.text = context.getString(pages[index].second)
            tipsBox.visibility = if (pages[index].first == R.string.guide_tips_title) View.VISIBLE else View.GONE
            configBox.visibility = if (pages[index].first == R.string.guide_cfg_title) View.VISIBLE else View.GONE
            val showSummary = index == pages.size - 1 && summary.isNotEmpty()
            summaryCard.visibility = if (showSummary) View.VISIBLE else View.GONE
            if (showSummary) summaryCard.text = summary
            nextBtn.text = context.getString(
                if (index == pages.size - 1) R.string.guide_start else R.string.guide_next)
        }
        private class CubeArt(context: Context, dp: (Int) -> Int) : View(context) {
            private val p = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = Color.rgb(0x25, 0x63, 0xEB); style = Paint.Style.STROKE
                strokeWidth = dp(2).toFloat(); strokeJoin = Paint.Join.ROUND
            }
            override fun onDraw(c: Canvas) {
                super.onDraw(c)
                val s = minOf(width, height).toFloat(); val x = (width-s)/2; val y = (height-s)/2
                val dx = s*0.28f; val dy = s*0.16f
                val top = android.graphics.Path().apply {
                    moveTo(x+dx,y); lineTo(x+s,y+dy); lineTo(x+s-dx,y+dy*2); lineTo(x,y+dy); close() }
                val left = android.graphics.Path().apply {
                    moveTo(x,y+dy); lineTo(x+s-dx,y+dy*2); lineTo(x+s-dx,y+dy*2+s*0.5f); lineTo(x,y+dy+s*0.5f); close() }
                val right = android.graphics.Path().apply {
                    moveTo(x+s-dx,y+dy*2); lineTo(x+s,y+dy); lineTo(x+s,y+dy+s*0.5f); lineTo(x+s-dx,y+dy*2+s*0.5f); close() }
                c.drawPath(top,p); c.drawPath(left,p); c.drawPath(right,p)
            }
        }
    }
}
