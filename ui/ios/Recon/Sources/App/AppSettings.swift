//
//  AppSettings.swift
//  Recon — 应用级设置的单一真源（iOS）
//
//  契约：docs/09_UI规格.md §3.11（U-11 设置页）、§3.11 U-S-1..3（字号档位）、
//       §6.3（深浅色）、§6.1（语言与回退链）。
//
//  ── 为什么单独一个文件 ─────────────────────────────────────────────────
//  U-11 设置页的四个可写项（语言 / 字号 / 外观 / 默认模式…）都需要一个
//  **locale 无关**的持久化位置，且被多处读取：
//    · 字号（`reconFontScale`）→ 注入视图树（U-S-1..3）；
//    · 外观（`appearance_mode`）→ 决定 WindowGroup 的 colorScheme；
//    · 语言（`locale_override`）→ 覆盖 §6.1 从系统语言推导的默认值。
//  若让各界面各自 `UserDefaults.standard` 读写，会出现「设置页改了、别处
//  读的是旧值」的分叉 —— 与 `L10n.catalog` 的两份实例风险同型。故集中在此。
//
//  ── 存储键的命名 ───────────────────────────────────────────────────────
//  键名由 **docs/09 规格指定**（U-S-3 给定 `dynamic_type_override`；
//  §3.11 元素表给定 `appearance_mode` / `locale_override`），本文件**只做
//  规格到常量的转写**，不得自行另起名字（不许代码先行）。
//

import Foundation
import SwiftUI

/// 应用级设置（docs/09 §3.11）。全 App 单一实例（`shared`）。
///
/// `ObservableObject`：设置变更须驱动 UI 重建（字号、外观、语言都会改变渲染结果）。
public final class AppSettings: ObservableObject {

    public static let shared = AppSettings()

    // ── 存储键（由规格指定，勿改）────────────────────────────────────────

    /// 字号档位（docs/09 §3.11 U-S-3 明文键名）。
    private static let fontScaleKey = ReconFontScale.defaultsKey
    /// 外观模式（docs/09 §3.11 元素表「外观」行）。
    private static let appearanceKey = "appearance_mode"
    /// 语言覆盖（docs/09 §3.11 元素表「语言」行）。
    private static let localeOverrideKey = "locale_override"

    private let defaults: UserDefaults

    // ── 可观测取值 ───────────────────────────────────────────────────────

    /// 字号档位（U-S-1..3）。写入即回写 `UserDefaults`。
    @Published public var fontScale: ReconFontScale {
        didSet { defaults.set(fontScale.rawValue, forKey: Self.fontScaleKey) }
    }

    /// 外观模式（§6.3）。`nil` = 跟随系统。
    @Published public var appearance: AppearanceMode {
        didSet { defaults.set(appearance.rawValue, forKey: Self.appearanceKey) }
    }

    /// 语言覆盖（§3.11「语言」行）。`nil` = 跟随系统（按 §6.1 回退链取）。
    @Published public var localeOverride: AppLocale? {
        didSet { defaults.set(localeOverride?.rawValue, forKey: Self.localeOverrideKey) }
    }

    /// 默认扫描模式（§3.11「默认模式」行）。
    ///
    /// 注意：本项**尚未接线到内核**（内核的模式入口是 `ScanView(mode:)`），
    /// 当前仅持久化用户偏好。**不伪造**「已生效」—— 接线由后续任务处理，
    /// 此处如实标注为「已存储、未消费」。
    @Published public var defaultScanMode: ScanModePreference {
        didSet { defaults.set(defaultScanMode.rawValue, forKey: Self.defaultModeKey) }
    }

    private static let defaultModeKey = "default_scan_mode"

    // ── 初始化 ───────────────────────────────────────────────────────────

    /// - Parameter defaults: 允许注入（测试用）；生产走 `.standard`。
    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        // 读盘：缺省值与 §3.11 元素表的「默认」列一致（跟随系统 / 跟随系统 / 定点引导）。
        self.fontScale = ReconFontScale(
            rawValue: defaults.string(forKey: Self.fontScaleKey) ?? ""
        ) ?? .system
        self.appearance = AppearanceMode(
            rawValue: defaults.string(forKey: Self.appearanceKey) ?? ""
        ) ?? .system
        self.localeOverride = defaults.string(forKey: Self.localeOverrideKey)
            .flatMap(AppLocale.init(rawValue:))
        self.defaultScanMode = ScanModePreference(
            rawValue: defaults.string(forKey: Self.defaultModeKey) ?? ""
        ) ?? .guided
    }

    /// 供 SwiftUI 根视图消费的配色方案（§6.3）。
    ///
    /// `nil` = 不强制，跟随系统 —— 这不是「未设置」，而是**明确的**跟随语义，
    /// 故与 `AppearanceMode.system` 一一对应（不引入第二个 nil 含义）。
    public var preferredColorScheme: ColorScheme? {
        switch appearance {
        case .system:  return nil
        case .light:   return .light
        case .dark:    return .dark
        }
    }
}

// ===========================================================================
// MARK: - §6.3 外观模式
// ===========================================================================

/// 外观模式（docs/09 §6.3）。
public enum AppearanceMode: String, CaseIterable, Sendable {
    /// `跟随系统`（§6.3 默认）。
    case system
    /// `始终浅色`
    case light
    /// `始终深色`
    case dark

    /// 设置页「外观」行显示的值所对应的 locale 键。
    ///
    /// 注意 `light` / `dark` 有专门键（`msg.settings.appearance_light` /
    /// `_dark`），`system` 复用通用的「跟随系统」键（`msg.settings.font_system`）
    /// —— 依据 §5.9 的复用辨析：同一文案（`跟随系统`）在同一区块内只造一个键。
    public var labelKey: L10nKey {
        switch self {
        case .system: return .settingsFontSystem
        case .light:  return .settingsAppearanceLight
        case .dark:   return .settingsAppearanceDark
        }
    }
}

// ===========================================================================
// MARK: - §3.11 默认扫描模式
// ===========================================================================

/// 默认扫描模式偏好（docs/09 §3.11「默认模式」行 / §2 U-03|U-04）。
///
/// 与 `ScanView.ScanMode` 的区别：本枚举是**设置项**（表示"下次默认用哪个"），
/// `ScanMode` 是**运行时状态**（表示"当前在扫哪个"）。二者刻意不合并 ——
/// 设置项要持久化且是 `String` 原始值，运行时状态不需要（也**不应**）持久化。
public enum ScanModePreference: String, CaseIterable, Sendable {
    case guided
    case freeOrbit

    /// 设置页「默认模式」行显示的值所对应的 locale 键。
    public var labelKey: L10nKey {
        switch self {
        case .guided:    return .scanModeGuided
        case .freeOrbit: return .scanModeFree
        }
    }
}
