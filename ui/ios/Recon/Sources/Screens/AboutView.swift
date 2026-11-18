//
//  AboutView.swift
//  Recon — U-12 关于页（iOS）
//
//  契约：docs/09_UI规格.md §3.12（元素表、线框图、U-A-1..3）、§1.2.1（LG-4）、
//       §5.9（键唯一真值表）、§6.2（无障碍：版本行 / 玻璃状态 / 本地处理 / 许可行）。
//
//  ── 本页为什么必须存在（登记说明，v1.2 补）──────────────────────────────
//  v1.1 及以前本页**无编号**，却被**六处**引用为「关于页要显示 X」：
//    · LG-4（玻璃回退须在关于页可见说明）；
//    · `GlassSurface.swift` 的三条注释（`ReconGlass.degradationNote` 的既定消费方）；
//    · `Localization.swift` 的三条注释。
//  一处都没有它的定义 —— 典型的「下游引用已存在、上游定义缺失」。
//
//  ── 本页是 LG-4「不允许静默降级」的落地处（U-D-7）──────────────────────
//  玻璃回退**必须**在此可见说明，且 §6.2 要求回退时**朗读降级说明全文**。
//  故本页不是「显示个状态就完了」：`msg.about.glass_fallback_note` 的存在、
//  以及与 `ReconGlass.isNativeAvailable` 的一致性，是本页的核心判据。
//
//  ── U-A-1..3（本页不得出现的内容）在本文件中的落实 ─────────────────────
//    U-A-1 无自证词：全页文案取自 §5.9 键表，无「官方/正版/认证」类词；
//    U-A-2 无评分/分享引导、无外部链接（唯一入口「开源许可」是**应用内**路由）；
//    U-A-3 零网络触发点：本页只读包内资源与 Info.plist，不含任何 URL/网络 API。
//      ⚠️ `msg.about.glass_fallback_note` 提到 iOS 26 —— 那是**文字**，不是链接；
//      U-A-2 禁的是「外部链接」，不是「提及版本号」。
//
//  ── 一处规格未指定来源的登记（2026-11-18，不擅自裁决）──────────────────
//  §3.12 线框图与元素表写「构建 `<日期>` · iOS 17.0+」，但**未指定这两个值的
//  来源**（元素表只写了版本号来源 `CFBundleShortVersionString`）。本实现的
//  保守取值，均**读真实来源、取不到则显示 `—` 而非伪造**：
//    · 构建日期 ← 主可执行文件的 `contentModificationDate`（即真实构建时刻）；
//    · 最低 iOS ← 包内 Info.plist 的 `MinimumOSVersion`（Xcode 构建时注入的
//      真值，与 docs/12 §4.6 的三处同源真源一致，**不在 Swift 侧硬编 17.0**
//      ——硬编会引入第四份字面量，违反 §5.1 C-4「同一个数字只从一处取值」）。
//  **待裁决**：① 构建日期是否改用 CI 注入的变量；② `MinimumOSVersion` 是否算
//  作规格认可的来源。
//

import SwiftUI

public struct AboutView: View {

    @EnvironmentObject private var router: AppRouter

    public init() {}

    // =======================================================================
    // MARK: - 真实取值（全部读包内真源，取不到返回 nil 而非伪造）
    // =======================================================================

    /// 短版本号（`CFBundleShortVersionString`，§3.12 元素表指定来源）。
    private var shortVersion: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String
            ?? "—"
    }

    /// 构建号（`CFBundleVersion`）。
    private var buildNumber: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String
            ?? "—"
    }

    /// 构建日期：主可执行文件的修改时刻（规格未指定来源，取真实值）。
    ///
    /// 为什么不编一个日期：规格写 `<日期>` 是**占位**，填一个写死的常量会让
    /// 关于页每次都显示同一个旧日期 —— 那是伪造。取不到就显示 `—`。
    private var buildDate: String {
        guard let url = Bundle.main.executableURL,
              let date = (try? url.resourceValues(forKeys: [.contentModificationDateKey]))?
                  .contentModificationDate
        else { return "—" }
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")   // 固定格式，不随语言变
        f.dateFormat = "yyyy-MM-dd"
        return f.string(from: date)
    }

    /// 最低 iOS 版本：包内 `MinimumOSVersion`（Xcode 注入）。
    ///
    /// 取不到时返回 `nil` → 「构建」行**不显示** `· iOS x+` 尾段，而不是编一个。
    /// 但 `fmt.about.build` 有两个占位符，必须都填 —— 故此处填 `—`。
    private var minimumOS: String {
        Bundle.main.object(forInfoDictionaryKey: "MinimumOSVersion") as? String ?? "—"
    }

    /// 可用语言数（§5.9 末明文：**不写死 20**，取 `EXPECTED_LOCALES` 等价的枚举）。
    private var availableLanguageCount: Int { AppLocale.allCases.count }

    /// 当前语言的本地化显示名。
    ///
    /// 复用 `AppLocale.displayName`（`Localization.swift` 单点定义，U-11 同样使用）
    /// ——**不**在本页重写这段映射（同一逻辑不写两遍）。
    private var currentLanguageName: String { L10n.current.displayName }

    // =======================================================================
    // MARK: - 主体
    // =======================================================================

    public var body: some View {
        ScrollView {
            VStack(spacing: 0) {
                nameSection
                sectionDivider
                glassSection
                sectionDivider
                localProcessingSection
                sectionDivider
                openSourceSection
                sectionDivider
                languageSection
            }
            .padding(.vertical, ReconMetric.space4)
        }
        .background(ReconColor.bg.page)
        .navigationTitle(L10n.t(.aboutTitle))
        .navigationBarTitleDisplayMode(.inline)
    }

    // =======================================================================
    // MARK: - 第一区块：名称与版本（§3.12）
    // =======================================================================

    private var nameSection: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space2) {
            SpecText(L10n.t(.aboutAppName), spec: ReconFont.title3, color: ReconColor.txtPrimary)
            // `版本 %@ (%@)` —— 双占位符，必须 `fill2`（`fill` 会把两个占位符
            // 填成同一值，得到「版本 1.2.0 (1.2.0)」）。
            SpecText(L10n.fill2(.fmtAboutVersion, shortVersion, buildNumber),
                     spec: ReconFont.subhead, color: ReconColor.txtSecondary, mono: true)
            SpecText(L10n.fill2(.fmtAboutBuild, buildDate, minimumOS),
                     spec: ReconFont.subhead, color: ReconColor.txtSecondary, mono: true)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, ReconMetric.space4)
        // §6.2 U-12 版本行：`版本 0.1.0，构建 2026-11-02，最低 iOS 17.0`
        // ——三段合成一句（A11Y-2：陈述句，不重复控件类型）。
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(L10n.t(.aboutAppName) + "，" +
            L10n.fill2(.fmtAboutVersion, shortVersion, buildNumber) + "，" +
            L10n.fill2(.fmtAboutBuild, buildDate, minimumOS)))
    }

    // =======================================================================
    // MARK: - 第二区块：液体玻璃（LG-4 落地处）
    // =======================================================================

    private var glassSection: some View {
        let native = ReconGlass.isNativeAvailable
        // 说明文案：原生可用时不显示降级说明（`nil`）；回退时用正式键
        // `msg.about.glass_fallback_note`（**LG-4 强制可见**）。
        let note: String? = native ? nil : L10n.t(.aboutGlassFallbackNote)

        return VStack(alignment: .leading, spacing: ReconMetric.space2) {
            SpecText(L10n.t(.aboutGlassTitle), spec: ReconFont.headline,
                     color: ReconColor.txtPrimary)
            SpecText(L10n.t(native ? .settingsGlassNative : .settingsGlassFallback),
                     spec: ReconFont.body,
                     color: native ? ReconColor.txtPrimary : ReconColor.txtSecondary)
            if let note {
                SpecText(note, spec: ReconFont.footnote, color: ReconColor.txtSecondary,
                         maxLines: 0, applyCharLimit: false)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, ReconMetric.space4)
        // §6.2 U-12 玻璃状态：`液体玻璃，毛玻璃回退`；**回退时须朗读降级说明全文**
        // （LG-4：不允许静默降级）。原生可用时不朗读额外说明。
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(glassA11yLabel(native: native, note: note)))
    }

    private func glassA11yLabel(native: Bool, note: String?) -> String {
        let state = L10n.t(native ? .settingsGlassNative : .settingsGlassFallback)
        let base = L10n.t(.aboutGlassTitle) + "，" + state
        guard let note else { return base }
        return base + "，" + note
    }

    // =======================================================================
    // MARK: - 第三区块：本地处理（零网络立场的唯一用户可见声明处）
    // =======================================================================

    private var localProcessingSection: some View {
        SpecText(L10n.t(.aboutLocalProcessing),
                 spec: ReconFont.footnote, color: ReconColor.txtSecondary,
                 maxLines: 0, applyCharLimit: false)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, ReconMetric.space4)
    }

    // =======================================================================
    // MARK: - 第四区块：开源许可入口（→ U-13）
    // =======================================================================

    private var openSourceSection: some View {
        Button {
            router.push(.licenses)
        } label: {
            HStack(spacing: ReconMetric.space2) {
                SpecText(L10n.t(.aboutOpenSource), spec: ReconFont.body,
                         color: ReconColor.txtPrimary)
                Spacer(minLength: ReconMetric.space2)
                // 文字符号 `›`（非图标）—— 与设置页、`ParamsView` 同一先例。
                SpecText("›", spec: ReconFont.footnote, color: ReconColor.txtTertiary)
            }
            .padding(.horizontal, ReconMetric.space4)
            .frame(minHeight: ReconMetric.minTouchTarget)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        // §6.2 U-12 开源许可行：trait `.button`（有下级）。
        .accessibilityAddTraits(.isButton)
    }

    // =======================================================================
    // MARK: - 第五区块：语言与文案（§3.12）
    // =======================================================================

    private var languageSection: some View {
        SpecText(L10n.fill2(.fmtAboutLanguage,
                            currentLanguageName,
                            String(availableLanguageCount)),
                 spec: ReconFont.footnote, color: ReconColor.txtSecondary,
                 maxLines: 0, applyCharLimit: false)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, ReconMetric.space4)
    }

    // =======================================================================
    // MARK: - 构件
    // =======================================================================

    private var sectionDivider: some View {
        Hairline().padding(.vertical, ReconMetric.space4)
    }
}
