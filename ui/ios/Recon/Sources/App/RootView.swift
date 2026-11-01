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

    public init() {}

    public var body: some View {
        NavigationStack(path: $router.path) {
            HomeView()
                .navigationDestination(for: AppRoute.self) { route in
                    destination(for: route)
                }
        }
        // §1.6：仅使用系统默认转场（iOS push），不做自定义动画。
        .environment(\.l10n, l10n.catalog)
    }

    @ViewBuilder
    private func destination(for route: AppRoute) -> some View {
        switch route {
        case .home:
            HomeView()
        case .capability:
            CapabilityView()
        case .scanFree:
            ScanView(mode: .freeOrbit)
        case .scanGuided:
            ScanView(mode: .guided)
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
