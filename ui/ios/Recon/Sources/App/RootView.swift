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
        // ── ⚠️ 为什么不是 `NavigationStack`（2026-11-01 CI 首编暴露）────────────
        // `NavigationStack` 与 `navigationDestination(for:destination:)` 都是
        // **iOS 16.0+** 的 API，而本工程部署目标是 **iOS 15.0**
        // （platform/ios/Container/project.yml 的 IPHONEOS_DEPLOYMENT_TARGET，
        //  来源 docs/12 §4.6）。在 15.0 下用 16.0 的 API 会直接编译失败：
        //   'NavigationStack' is only available in iOS 16.0 or newer。
        //
        // 三条路（登记在此，避免后人重复推导）：
        //   ① 把部署目标提到 16.0 —— **否决**。docs/12 §4.6 的结论是产品立场
        //      （「不淘汰老旧机型」）而非技术偏好，且覆盖面数据支持它：
        //      iOS 15 累计覆盖 99.3%、iOS 16 为 97.8%，提高下限会真实砍掉约
        //      1.5% 的用户。契约冲突不擅自裁决，故保持 15.0。
        //   ② 保留 16 的 API 并用 `if #available` 分叉 —— **否决**。分叉意味着
        //      两套导航实现并存，路径驱动逻辑要写两遍（违反「同一逻辑不写两遍」）。
        //   ③ 改用 iOS 15 即可用的 `NavigationView` + 由 `router.path` 驱动的
        //      `NavigationLink(isActive:)` 链 —— **采用**（即下面的实现）。
        //      `NavigationView` 底层仍是 `UINavigationController`，故
        //      `navigationTitle` / `toolbar` / `navigationBarBackButtonHidden`
        //      （ScanView 的 U-NAV-3 拦截依赖它）全部照常工作。
        //      `NavigationLink(isActive:...)` 在 iOS 16 被软弃用，但 deprecation
        //      警告**只在部署目标 ≥ 被弃用版本时**才发出 —— 15.0 < 16.0，故在
        //      `SWIFT_TREAT_WARNINGS_AS_ERRORS` 的零警告标准下不会变成错误。
        NavigationView {
            HomeView()
                .modifier(ReconPushLink(level: 0))
        }
        // 显式声明 stack 样式：iPhone 上保证单列 push 语义（U-NAV-1 禁 Tab 栏）。
        .navigationViewStyle(.stack)
        // §1.6：仅使用系统默认转场（iOS push），不做自定义动画。
        .environment(\.l10n, l10n.catalog)
    }

    /// 路由 → 界面（与 `AppRoute` 一一对应，docs/09 §2 的 `route` 列）。
    @ViewBuilder
    fileprivate static func destination(for route: AppRoute) -> some View {
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
        }
    }
}

/// 把 `AppRouter.path[level]` 映射为一次 iOS 15 可用的 push。
///
/// 递归方式：第 `level` 层若存在路由，则挂一个 `NavigationLink` 到该层的
/// 目的界面，并在目的界面内再挂 `level + 1` 层的触发器 —— 这样任意深度的
/// 栈都由「一层一个 link」自然表达，而 `router.path` 仍是**唯一事实来源**
/// （不持有第二份栈状态，故不存在两处不一致的可能）。
///
/// ⚠️ 索引安全：`route` **先取值再进闭包**，闭包内不做任何下标访问。
///    `router.popToHome()` 会把 `path` 一次清空，此时 SwiftUI 仍可能求值
///    旧闭包 —— 若闭包里写 `router.path[level]` 就会越界崩溃。取成局部
///    `AppRoute` 值（enum，值语义）后即与 `path` 的后续变化解耦。
///
/// ⚠️ 为什么 `destination` 处要 `AnyView`：本 ViewModifier 递归引用自身，
///    不擦除类型则编译器需展开完整嵌套类型才能定型 → "circular reference"。
///    `AnyView` 在此是**必要手段**（SwiftUI 无递归 ViewBuilder 的有限类型推导）。
///
/// ⚠️ 为什么"无该层路由"分支返回**原生 `content`** 而不是 `AnyView(content)`：
///    后者会在每次 `body` 求值时重建视图身份，破坏 SwiftUI 的 diff，导致
///    下层界面（如 ScanView）状态被反复丢弃。`Group` 的 `if/else` 由
///    ViewBuilder 编译成 `_ConditionalContent`，两个分支各自保持原类型，
///    故 `content` 分支的身份是稳定的。
private struct ReconPushLink: ViewModifier {
    @EnvironmentObject private var router: AppRouter
    let level: Int

    func body(content: Content) -> some View {
        // 该层没有对应路由 → 本层不 push，原样返回（身份稳定，见上）。
        let route: AppRoute? = router.path.indices.contains(level)
            ? router.path[level] : nil

        // `isActive` 的 setter 处理「系统返回」：关闭第 level 层即意味着其后
        // 各层一并出栈（`removeSubrange`），与栈语义一致。返回按钮与滑动手势
        // 都作用于**栈顶**，故实际只会触发 `level == count-1` 的那一个 setter。
        let isActive = Binding<Bool>(
            get: { router.path.count > level },
            set: { active in
                if !active, router.path.count > level {
                    router.path.removeSubrange(level...)
                }
            }
        )

        return Group {
            if let route {
                ZStack {
                    content
                    // label 用零尺寸 `Color.clear` 而非 `EmptyView`：后者无渲染
                    // 节点，在部分 iOS 版本上会让 link 失效。
                    NavigationLink(isActive: isActive) {
                        AnyView(DestinationView(route: route, level: level + 1))
                    } label: {
                        Color.clear.frame(width: 0, height: 0)
                    }
                }
            } else {
                content
            }
        }
    }
}

/// 目的界面的薄包装：把 `RootView.destination` 的产出再挂上下一层的 push 触发器。
private struct DestinationView: View {
    let route: AppRoute
    let level: Int

    var body: some View {
        RootView.destination(for: route)
            .modifier(ReconPushLink(level: level))
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
