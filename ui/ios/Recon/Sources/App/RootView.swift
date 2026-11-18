//
//  RootView.swift
//  Recon — 根导航容器（iOS）
//
//  契约：docs/09_UI规格.md §2（导航结构）、§1.2.1 LG-2（玻璃仅用于导航栏/工具栏）、
//       §6.1（20 个 locale 与三级回退链）。
//

import SwiftUI

public struct RootView: View {
    @EnvironmentObject private var router: AppRouter

    /// 文案目录。用 `@StateObject` 保证整个 App 生命周期只有一份实例
    /// —— 与 `L10n.catalog` 指向同一个对象（两份实例会让「当前语言」分叉）。
    @StateObject private var l10n = L10nBootstrap.shared

    /// 字号档位（docs/09 §3.11 U-S-1..3）。初值从 `UserDefaults` 的
    /// `reconFontScale` 键读出；设置页改档后由 `AppSettings` 更新并回写。
    @StateObject private var settings = AppSettings.shared

    public init() {}

    public var body: some View {
        // ── 为什么改用 `NavigationStack`（v1.3 UI 重构）────────────────────────
        // 部署目标已从 iOS 15.0 提到 **17.0**（docs/12 §4.6，v1.3 裁决），
        // 故直接使用 iOS 16+ 的 `NavigationStack` + `navigationDestination`。
        //
        // ⚠️ 这不是"能用新的就用新的"，而是**必须换** —— 原实现
        //    （`NavigationView` + 手写 `NavigationLink(isActive:)` 递归栈）在
        //    部署目标 ≥ 16.0 后**会编译失败**：
        //      `NavigationView` / `.navigationViewStyle(_:)` /
        //      `NavigationLink(isActive:destination:label:)` 均在 iOS 16 被**弃用**，
        //      而弃用警告**只在部署目标 ≥ 被弃用版本时**才发出。旧注释记录得很清楚：
        //      「15.0 < 16.0，故不会变成错误」；一旦提到 17.0，这条前提就反转了
        //      —— 在 `SWIFT_TREAT_WARNINGS_AS_ERRORS: YES` 的零警告标准下，
        //      弃用警告 = 构建失败。
        //
        // 相比旧实现的好处（也是本次重构的目标）：
        //   · `router.path`（`[AppRoute]`）成为**唯一栈状态**，直接交给
        //     `NavigationStack(path:)` —— 删掉了 `ReconPushLink` 那套「一层一个
        //     link」的递归镜像，不再有两处状态需要保持同步；
        //   · 系统返回按钮 / 边缘滑动 / 程序化出栈三者天然一致；
        //   · 与 iOS 26 的玻璃工具栏（`.toolbarBackground` / `.glassEffect`）协同，
        //     这是老 `NavigationView` 做不到的（docs/09 §1.2.1）。
        //   U-NAV-3 的返回拦截不受影响：仍由 ScanView 用
        //   `.navigationBarBackButtonHidden(true)` + 自定义返回按钮实现。
        NavigationStack(path: $router.path) {
            HomeView()
                .navigationDestination(for: AppRoute.self) { route in
                    RootView.destination(for: route)
                }
        }
        // §1.6：仅使用系统默认转场（iOS push），不做自定义动画。
        .environment(\.l10n, l10n.catalog)
        // §3.11 U-S-1..3：字号档位注入全树，`reconFont` 据此乘系数。
        .environment(\.reconFontScale, settings.fontScale)
    }

    /// 路由 → 界面（与 `AppRoute` 一一对应，docs/09 §2 的 `route` 列）。
    ///
    /// 由 `NavigationStack` 的 `navigationDestination(for:)` 消费 —— 每个
    /// `push` 一次求值，不再需要旧实现里的 `AnyView` 类型擦除（那是为递归
    /// `ViewModifier` 准备的，现已随 `ReconPushLink` 一并删除）。
    @ViewBuilder
    private static func destination(for route: AppRoute) -> some View {
        switch route {
        case .home:
            HomeView()
        case .capability:
            CapabilityView()
        case .scanFree:
            ScanView(mode: .freeOrbit)
        case .scanGuided:
            ScanView(mode: .guided)
        case .reviewing(let id):
            ReviewingView(sessionID: id)
        case .rebuild(let id):
            RebuildView(sessionID: id)
        case .result(let id):
            ResultView(sessionID: id)
        case .params:
            ParamsView()
        case .calibration:
            CalibrationView()
        case .export(let id):
            ExportView(sessionID: id)
        case .settings:
            SettingsView()
        case .about:
            AboutView()
        case .licenses:
            LicensesView()
        }
    }
}

/// 启动期文案目录的初始化。
///
/// 单独抽出来是为了让「加载」这件事**只发生一次**，且发生在 UI 构建之前：
/// 若放在某个 View 的 `init` 里，SwiftUI 重建视图时可能重复加载（重复 IO）；
/// 若放在 `L10n` 的静态初始化里，则无法注入测试用的 bundle。
public final class L10nBootstrap: ObservableObject {

    public static let shared = L10nBootstrap()

    public let catalog: LocaleCatalog

    private init() {
        let c = LocaleCatalog()
        c.loadAll()
        c.buildSourceIndex()
        catalog = c

        // 与视图构建期用的那一份对齐（`Text(...)` 实参与非 View 辅助函数
        // 无法访问 EnvironmentValues，故保留这份同一实例的引用）。
        L10n.catalog = c
        L10n.current = Self.systemLocale()
    }

    /// 把系统的 `Locale` 映射到 §6.1 的 20 个 locale。
    ///
    /// 规则：先按完整标识匹配（`zh-Hans-CN` → `zh-CN`），再按语言码匹配
    /// （`de-AT` → `de`），都失败则回退到源语言 `zh-CN`（§6.1 回退链的
    /// 起点，而不是空 —— 空会让整个界面显示键名）。
    static func systemLocale() -> AppLocale {
        let preferred = Locale.preferredLanguages.first ?? "zh-CN"
        // 归一化：下划线转连字符、去掉脚本子标记的常见写法
        let norm = preferred.replacingOccurrences(of: "_", with: "-")

        // 1) 完整标识（大小写不敏感）
        for l in AppLocale.allCases where
            norm.compare(l.rawValue, options: .caseInsensitive) == .orderedSame {
            return l
        }
        // 2) 中文特判：zh-Hans* → zh-CN，zh-Hant* → zh-TW
        if norm.lowercased().hasPrefix("zh") {
            if norm.lowercased().contains("hant") || norm.lowercased().contains("tw")
                || norm.lowercased().contains("hk") || norm.lowercased().contains("mo") {
                return .zhTW
            }
            return .zhCN
        }
        // 3) 仅有语言码
        let lang = norm.split(separator: "-").first.map(String.init)?.lowercased() ?? ""
        for l in AppLocale.allCases {
            let lc = l.rawValue.lowercased()
            if lc == lang || lc.hasPrefix(lang + "-") { return l }
        }
        // 4) 兜底：源语言（不是空，也不是英文 —— 保证一定有文案可显示）
        return .zhCN
    }
}
