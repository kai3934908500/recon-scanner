//
//  SettingsView.swift
//  Recon — U-11 设置页（iOS）
//
//  契约：docs/09_UI规格.md §3.11（元素表、线框图、U-S-1..6）、§2（U-NAV-1 的
//       Tab4 根 / U-NAV-7）、§5.9（32 个新增键的唯一真值表）、§6.2（无障碍）、
//       §6.3（外观模式）、§6.1（语言与三级回退链）。
//
//  ── 本页为什么此前不存在（登记说明，同缺口 AF 型）────────────────────────
//  v1.1 及以前，U-01 导航栏右的「设置」按钮早已存在（`HomeView` 的
//  `homeSettings` 键），但**无目的地**（空实现），且 §2 界面清单没有它的行。
//  §3.11 的登记说明把这一点写明了。本页建于 v1.3。
//
//  ── 四处规格差异的处置（2026-11-18，均**不擅自裁决**，仅登记 + 保守实现）─
//
//  【差异 1】「尺度参照物」的值 `无` **在 §5.9 键表中无对应键**，且内核
//     **无标定状态可读**：`CalibrationView` 的参照物类型是页内 `@State`
//     （`referenceType`），既不持久化、也不上报内核；`ReconSessionController`
//     无任何 calibration 相关的 `@Published`。故本页：
//       · 值取**既有键** `msg.result.scale_none`（`未标定`）—— 语义为该行
//         「尚未设参照物」，且该键已存在、20 locale 可回退，**不新造键**
//         （规格 §5.9 未登记该值键，代码不先行）；
//       · 交互仍为「推入 U-09」（目标页已存在）。
//     **待裁决**：① 值键的正式键名；② 标定状态是否需要上提到 controller。
//
//  【差异 2】「导出记录」行：值键有（`export_count_zero` / `fmt.settings.export_count`），
//     但**目标页「导出历史列表」从未定义**（`AppRoute` 无该 case，§2 清单无该行）。
//     且全仓**无导出历史数据结构**（`ReconSessionRecord` 不含导出信息）——
//     故无记录时如实显示 `0 条`，并**禁用**该行（与 `HomeView` 底部栏
//     「导出记录」按钮的处置**完全一致**：那里也是 `.disabled(true)`）。
//     **不**凭空造一个空列表页，也**不**伪造历史条目。
//
//  【差异 3】§3.11 线框图的「液体玻璃支持情况」行尾有 `›`，但同页元素表写
//     「交互：无（只读，推 U-12 有完整说明）」，且 §6.2 明文「`液体玻璃支持情况`
//     为只读，无 trait」。**线框与表格冲突**，本实现**以表格 + §6.2 为准**：
//     只读、不显示 `›`、无 `.button` trait（否则读屏会播报成可点元素）。
//
//  【差异 4】§3.11 元素表要求「推入模式选择」「展开 20 个 locale 列表」，
//     但这两个**目标页/路由从未定义**（§2 清单无 Picker 页、无语言列表页）。
//     本实现**不擅自新增路由**（那会扩 `AppRoute` 契约且规格未登记），改用
//     内联 `Menu` / 分段 `Picker` 承载选择 —— 选择结果照规格写入 `UserDefaults`
//     （`locale_override` / `default_scan_mode`）。**待裁决**：是否要为 20 项
//     语言单列一个列表页。
//
//  ── 「默认模式」行如实标注为「已存储、未消费」────────────────────────────
//  `AppSettings.defaultScanMode` 仅持久化用户偏好，**尚未接线到内核**
//  （内核的模式入口是 `ScanView(mode:)`，由 U-01 / Tab2 的入口决定）。本页
//  **不伪造**「改了就生效」——行值直接显示所存偏好，接线由后续任务处理。
//

import SwiftUI

public struct SettingsView: View {

    @EnvironmentObject private var router: AppRouter
    /// 精度预设置的真实来源（`recon_apply_preset` 的镜像）。
    @EnvironmentObject private var controller: ReconSessionController

    /// 设置的单一真源（与 `RootView` 指向**同一个** `shared` 实例）。
    @StateObject private var settings = AppSettings.shared

    public init() {}

    // =======================================================================
    // MARK: - 主体
    // =======================================================================

    public var body: some View {
        ScrollView {
            VStack(spacing: 0) {
                scanSection
                sectionDivider
                displaySection
                sectionDivider
                aboutSection
                sectionDivider
                dataSection
            }
            .padding(.vertical, ReconMetric.space4)
        }
        .background(ReconColor.bg.page)
        .navigationTitle(L10n.t(.settingsTitle))
        .navigationBarTitleDisplayMode(.inline)
        // 根视图是深层页面，从 Tab4 根进入时无需返回按钮；经 U-01 推入时由
        // `NavigationStack` 自动提供返回。此处不做自定义返回按钮（U-NAV-3 的
        // 拦截只适用于 U-03/U-04/U-05b/U-06，本页不属该组）。
    }

    // =======================================================================
    // MARK: - 第一区块：扫描默认（§3.11）
    // =======================================================================

    private var scanSection: some View {
        VStack(spacing: 0) {
            sectionHeader(L10n.t(.settingsSectionScan))

            // 默认模式 —— 二档内联选择（差异 4）。
            Menu {
                ForEach(ScanModePreference.allCases, id: \.self) { mode in
                    Button {
                        settings.defaultScanMode = mode
                    } label: {
                        Text(L10n.t(mode.labelKey))
                    }
                }
            } label: {
                keyValueRow(L10n.t(.settingsDefaultMode),
                            value: L10n.t(settings.defaultScanMode.labelKey),
                            chevron: true)
            }
            .buttonStyle(.plain)

            // 精度预设 —— 目标页已存在（U-08），照规格推入。
            Button {
                router.push(.params)
            } label: {
                keyValueRow(L10n.t(.paramsPreset),
                            value: controller.preset.label,
                            chevron: true)
            }
            .buttonStyle(.plain)

            // 尺度参照物 —— 值键待裁决（差异 1），交互推入 U-09（目标页已存在）。
            Button {
                router.push(.calibration)
            } label: {
                keyValueRow(L10n.t(.settingsScaleReference),
                            value: L10n.t(.resultScaleNone),
                            chevron: true)
            }
            .buttonStyle(.plain)
        }
        .background(ReconColor.bg.secondary)
        .clipShape(RoundedRectangle(cornerRadius: ReconMetric.radiusCard))
        .padding(.horizontal, ReconMetric.space4)
    }

    // =======================================================================
    // MARK: - 第二区块：显示（§3.11）
    // =======================================================================

    private var displaySection: some View {
        VStack(spacing: 0) {
            sectionHeader(L10n.t(.settingsSectionDisplay))

            // 语言 —— 20 项内联菜单（差异 4）。缺省 = 跟随系统（§6.1 三级回退链）。
            Menu {
                Button {
                    settings.localeOverride = nil
                } label: {
                    Text(L10n.t(.settingsFontSystem))
                }
                Divider()
                ForEach(AppLocale.allCases, id: \.self) { loc in
                    Button {
                        settings.localeOverride = loc
                    } label: {
                        Text(loc.displayName)
                    }
                }
            } label: {
                keyValueRow(L10n.t(.settingsLanguage),
                            value: currentLanguageText,
                            chevron: true,
                            valueAccent: settings.localeOverride != nil)
            }
            .buttonStyle(.plain)

            // 字号 —— 四档（含「跟随系统」）分段切换。
            segmentedRow(L10n.t(.settingsFontSize)) {
                Picker(L10n.t(.settingsFontSize), selection: $settings.fontScale) {
                    ForEach(ReconFontScale.allCases, id: \.self) { scale in
                        Text(L10n.t(Self.labelKey(for: scale))).tag(scale)
                    }
                }
                .pickerStyle(.segmented)
                .tint(ReconColor.accent)
            }

            // 外观 —— 三档（§6.3）。
            segmentedRow(L10n.t(.settingsAppearance)) {
                Picker(L10n.t(.settingsAppearance), selection: $settings.appearance) {
                    ForEach(AppearanceMode.allCases, id: \.self) { mode in
                        Text(L10n.t(mode.labelKey)).tag(mode)
                    }
                }
                .pickerStyle(.segmented)
                .tint(ReconColor.accent)
            }
        }
        .background(ReconColor.bg.secondary)
        .clipShape(RoundedRectangle(cornerRadius: ReconMetric.radiusCard))
        .padding(.horizontal, ReconMetric.space4)
    }

    // =======================================================================
    // MARK: - 第三区块：关于（§3.11）
    // =======================================================================

    private var aboutSection: some View {
        VStack(spacing: 0) {
            // 区块标题复用 `msg.about.title`（§5.9 复用辨析第 2 条：与 U-12 导航
            // 标题同文案，只造一个键）。
            sectionHeader(L10n.t(.aboutTitle))

            // 版本与许可 —— 推入 U-12（目标页已存在）。
            Button {
                router.push(.about)
            } label: {
                keyValueRow(L10n.t(.settingsVersionLicense), value: "", chevron: true)
            }
            .buttonStyle(.plain)

            // 液体玻璃支持情况 —— **只读**（差异 3：以元素表 + §6.2 为准）。
            glassSupportRow
        }
        .background(ReconColor.bg.secondary)
        .clipShape(RoundedRectangle(cornerRadius: ReconMetric.radiusCard))
        .padding(.horizontal, ReconMetric.space4)
    }

    /// LG-4：回退时值用 `txt.secondary`，并在行下补一行 `footnote` `txt.secondary`
    /// 的降级说明 —— **不允许静默降级**（U-D-7）。
    ///
    /// 说明文案复用 `msg.about.glass_fallback_note`：§3.11 表里那句短文案
    /// （`当前系统版本低于 iOS 26，使用毛玻璃回退`）**未被 §5.9 登记为独立键**，
    /// 而 U-12 的那条正式键文案**恰好覆盖同一语义**。故复用正式键，**不新造键**
    /// （同一逻辑不写两遍）。**待裁决**：是否要为设置页的短句单列键。
    @ViewBuilder
    private var glassSupportRow: some View {
        let native = ReconGlass.isNativeAvailable
        VStack(alignment: .leading, spacing: 0) {
            keyValueRow(L10n.t(.settingsGlassSupport),
                        value: L10n.t(native ? .settingsGlassNative : .settingsGlassFallback),
                        chevron: false,
                        valueColor: native ? ReconColor.txtPrimary : ReconColor.txtSecondary)

            if !native {
                SpecText(L10n.t(.aboutGlassFallbackNote),
                         spec: ReconFont.footnote,
                         color: ReconColor.txtSecondary,
                         maxLines: 0,          // 长文案不截断（§6.4 容器不固定高度）
                         applyCharLimit: false)
                    .padding(.horizontal, ReconMetric.space3)
                    .padding(.bottom, ReconMetric.space3)
            }
        }
    }

    // =======================================================================
    // MARK: - 第四区块：数据（§3.11）
    // =======================================================================

    private var dataSection: some View {
        VStack(spacing: 0) {
            sectionHeader(L10n.t(.settingsSectionData))

            // 导出记录 —— 无真实数据结构、且目标页未定义（差异 2）→ 如实空态 +
            // 禁用，与 `HomeView` 底部栏同处置。禁用时值用 `txt.tertiary`（§3.11 状态列）。
            keyValueRow(L10n.t(.homeExportHistory),
                        value: L10n.t(.settingsExportCountZero),
                        chevron: true,
                        valueColor: ReconColor.txtTertiary,
                        enabled: false)
        }
        .background(ReconColor.bg.secondary)
        .clipShape(RoundedRectangle(cornerRadius: ReconMetric.radiusCard))
        .padding(.horizontal, ReconMetric.space4)
    }

    // =======================================================================
    // MARK: - 行 / 区块构件
    // =======================================================================

    private var sectionDivider: some View {
        Hairline().padding(.vertical, ReconMetric.space5)
    }

    /// 区块标题（`footnote` + `txt.secondary`，§3.11 线框图的区块行）。
    private func sectionHeader(_ title: String) -> some View {
        HStack {
            SpecText(title, spec: ReconFont.footnote, color: ReconColor.txtSecondary)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, ReconMetric.space3)
        .padding(.top, ReconMetric.space3)
        .padding(.bottom, ReconMetric.space1)
    }

    /// 键值行：左「键名」，右「值（+ 可选 `›`）」。
    ///
    /// `enabled == false` 时整行置灰且**不**作为可点元素（差异 3 的判据延伸：
    /// 只有真正有下级的行才给 `.button` trait，§6.2）。
    @ViewBuilder
    private func keyValueRow(_ label: String,
                             value: String,
                             chevron: Bool,
                             valueColor: Color = ReconColor.txtSecondary,
                             valueAccent: Bool = false,
                             enabled: Bool = true) -> some View {
        HStack(spacing: ReconMetric.space2) {
            SpecText(label, spec: ReconFont.body,
                     color: enabled ? ReconColor.txtPrimary : ReconColor.txtTertiary)
            Spacer(minLength: ReconMetric.space2)
            if !value.isEmpty {
                SpecText(value, spec: ReconFont.subhead,
                         color: valueAccent ? ReconColor.txtAccent : valueColor)
            }
            if chevron {
                // 文字符号 `›`（非图标）—— 与 `ParamsView` 的 `▸`/`▾` 同一先例。
                SpecText("›", spec: ReconFont.footnote,
                         color: enabled ? ReconColor.txtTertiary : ReconColor.txtTertiary)
            }
        }
        .padding(.horizontal, ReconMetric.space3)
        // §1.7：热区取严者 48×48（不是 controlHeight 的 44）。
        .frame(minHeight: ReconMetric.minTouchTarget)
        .contentShape(Rectangle())
        .opacity(enabled ? 1 : 0.6)
    }

    /// 分段控件行：键名在上，控件在下（分段控件宽度不足以与键名同行）。
    @ViewBuilder
    private func segmentedRow<Content: View>(_ label: String,
                                            @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: ReconMetric.space2) {
            SpecText(label, spec: ReconFont.body, color: ReconColor.txtPrimary)
            content()
        }
        .padding(.horizontal, ReconMetric.space3)
        .padding(.vertical, ReconMetric.space2)
    }

    // =======================================================================
    // MARK: - 显示映射（视图层，不写进设计令牌）
    // =======================================================================

    /// 「语言」行当前值：覆盖值优先，否则「跟随系统」（§6.1 三级回退链的起点）。
    private var currentLanguageText: String {
        guard let override = settings.localeOverride else {
            return L10n.t(.settingsFontSystem)
        }
        return override.displayName
    }

    /// 字号档 → locale 键。
    ///
    /// 为什么不放进 `ReconFontScale`：`DesignTokens.swift` 是 docs/09 §1 各表的
    /// **逐条转写**，不得引入规格外成员；且「哪一档对应哪个文案」是**视图映射**
    /// （§3.11 元素表），属视图层职责。此逻辑全仓仅此一处，不构成重复实现。
    private static func labelKey(for scale: ReconFontScale) -> L10nKey {
        switch scale {
        case .system:   return .settingsFontSystem
        case .small:    return .settingsFontSmall
        case .standard: return .settingsFontStandard
        case .large:    return .settingsFontLarge
        }
    }
}
