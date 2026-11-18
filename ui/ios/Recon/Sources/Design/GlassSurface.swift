//
//  GlassSurface.swift
//  Recon — Liquid Glass 材质封装（iOS 26+，含 iOS 15+ 回退）
//
//  契约：docs/09_UI规格.md §1.2.1（LG-1 .. LG-10）。
//
//  ── 一个必须说清的工程约束 ─────────────────────────────────────────────
//
//  Liquid Glass 的 API（`.glassEffect(_:in:)` / `GlassEffectContainer`）**只在
//  iOS 26 SDK 中存在**。本项目 v1.3 起的 Xcode 基线是 **26（iOS 26 SDK）**，
//  CI 亦跑在 `macos-26`（docs/12 §4.1 / §4.6）。故 `#if compiler(>=6.2)` 走
//  **真分支**，原生玻璃**真正参与编译**（不是"写了但从未编过"）。
//
//  ⚠️ 但仍保留双层守卫，因为**运行期**与**编译期**是两件事：
//     · 编译期（Xcode 26 下恒真）：`#if compiler(>=6.2)` 决定原生分支是否参与编译；
//     · 运行期：设备系统可能是 iOS 17/18/25 —— 此时 `.glassEffect` **不可调用**
//       （会崩），必须由 `#available(iOS 26.0, *)` 挡下并走回退。
//
//      编译器版本门（编译期）        运行时版本门（运行期）
//      ┌──────────────────────┐    ┌──────────────────────┐
//      │ #if compiler(>=6.2)  │ →  │ #available(iOS 26.0) │ →  原生液态玻璃
//      │ (Xcode 26+ / iOS 26  │    └──────────────────────┘
//      │  SDK 存在)           │ →  else                →  .ultraThinMaterial 回退
//      └──────────────────────┘
//      #else                                              →  .ultraThinMaterial 回退
//
//  因此：**交付基线（Xcode 26）下产出的就是真玻璃**；只有部署下限（iOS 17）
//  设备在运行期才走回退。这一点必须在交付说明中如实写明。
//
//  另：`#if compiler(>=6.2)` 与「iOS 26 SDK 可用」在现实中高度相关
//  （Xcode 26 随附 Swift 6.2），用作编译期探针是可接受的近似；若将来两者脱钩，
//  应改为在构建系统注入 `-DRECON_HAS_IOS26_SDK` 由 CI 显式判定。
//

import SwiftUI

// ===========================================================================
// MARK: - 玻璃表面角色（LG-2 的机器可执行表达）
// ===========================================================================

/// 允许使用 Liquid Glass 的**系统级表面**（docs/09 §1.2.1 LG-2）。
///
/// 该枚举是白名单本身：只有这 5 种角色能构造玻璃表面，业务内容区
/// （列表 / 表单 / 参数行 / 结果统计 / 预览遮罩面）**没有对应 case**，
/// 因此无法在类型层面表达出来 —— 这是刻意的设计。
    /// 规格原文（docs/09 §1.2.1 LG-2 表格的「角色」列）：
    /// 导航栏 / 工具栏 / 浮动操作按钮（FAB）/ 底部 Sheet / Alert 附属按钮。
    ///
    /// ── 为什么这里没有 `specNote: String` 属性 ────────────────────────────
    /// 原先有一个 `specNote` 属性把这 5 个角色名做成字符串返回，声称「供代码
    /// 审查与『关于』页引用」。实测**全仓库零调用点**，且这 5 个中文名在
    /// `zh-CN.json` 里**没有对应键** —— 即它既不是真 UI 文案，又无法被本地化，
    /// 是典型的「看似规格化、实则死代码」。已删除；角色名保留为文档注释，
    /// 代码审查在此处可读。
    ///
    /// 将来「关于」页若要列出「哪些表面使用了玻璃」，应新增正式 locale 键
    /// （而非复用本注释），届时 `tools/check_ios_contract.py` 的 G6/G9 会强制
    /// 键存在 —— 不允许出现无键的可见文案。
    public enum GlassRole: String, CaseIterable, Sendable {
        /// 导航栏
        case navigationBar
        /// 工具栏
        case toolbar
        /// 浮动操作按钮（FAB）
        case floatingAction
        /// 底部 Sheet
        case sheet
        /// Alert 附属按钮
        case alertAccessory
    }

// ===========================================================================
// MARK: - 原生可用性探针（供「关于」页做降级说明，LG-4）
// ===========================================================================

public enum ReconGlass {

    /// 本机运行时是否支持原生 Liquid Glass。
    ///
    /// 「关于」页据此显示降级说明（LG-4：回退视为降级，须可见）。
    public static var isNativeAvailable: Bool {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) { return true }
        return false
        #else
        return false
        #endif
    }

    /// 当前**构建**是否具备编译原生玻璃分支的能力（由基线工具链决定）。
    /// 注意：`canCompileNative == false` 时 `isNativeAvailable` 必为 false。
    public static var canCompileNative: Bool {
        #if compiler(>=6.2)
        return true
        #else
        return false
        #endif
    }

    /// 降级说明文案（LG-4 + U-D-7「不隐藏失败」）。
    public static var degradationNote: String? {
        if isNativeAvailable { return nil }
        if !canCompileNative {
            // 只有在用一个不支持 iOS 26 SDK 的旧 Xcode 构建时才会到这里
            //（交付基线是 Xcode 26，故正常情况下本分支不出现）。
            return "当前构建所用的 Xcode 不支持 Liquid Glass（需 Xcode 26+ / iOS 26 SDK），界面使用系统半透明材质替代。"
        }
        return "当前系统版本低于 iOS 26，界面使用系统半透明材质替代 Liquid Glass。"
    }
}

// ===========================================================================
// MARK: - Liquid Glass 表面修饰器
// ===========================================================================

/// 把内容包进 Liquid Glass 表面（iOS 26+）或系统半透明材质（回退）。
///
/// 用法：
/// ```swift
/// myToolbar.glassSurface(.toolbar, in: .rect(cornerRadius: 8))
/// ```
public struct GlassSurfaceModifier<S: Shape>: ViewModifier {
    let role: GlassRole
    let shape: S
    /// `.regular.tint(_:)` 的着色（LG-6：`.regular` 系列可用）。
    let tint: Color?
    /// `.interactive()`（LG-8：仅 A17 Pro+ 生效，旧设备由系统静默降级为静态）。
    let interactive: Bool

    public func body(content: Content) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            return AnyView(nativeGlass(content))
        } else {
            return AnyView(fallbackGlass(content))
        }
        #else
        return AnyView(fallbackGlass(content))
        #endif
    }

    // --- 原生路径（LG-1：只允许系统原生实现）-----------------------------
    #if compiler(>=6.2)
    @available(iOS 26.0, *)
    private func nativeGlass(_ content: Content) -> some View {
        var glass: Glass = .regular          // LG-6：禁止 .clear
        if let tint { glass = glass.tint(tint) }
        if interactive { glass = glass.interactive() }
        // LG-5：不叠加 shadow / elevation / 渐变 / 自绘边框。
        return content.glassEffect(glass, in: shape)
    }
    #endif

    // --- 回退路径（LG-4：必须是显式指定的系统材质）-----------------------
    private func fallbackGlass(_ content: Content) -> some View {
        // 回退一律用 `.ultraThinMaterial`（系统材质），不允许自绘玻璃。
        // LG-5 同样适用：回退路径下也不叠加阴影。
        content
            .background(.ultraThinMaterial, in: shape)
    }
}

// ===========================================================================
// MARK: - 玻璃分组容器（GlassEffectContainer）
// ===========================================================================

/// 把多个玻璃表面归入同一 `GlassEffectContainer`，使其可合并、可 morph。
///
/// 约束（LG-7）：`GlassEffectContainer` **不跨 `NavigationStack` 边界**。
/// 因此本容器只应用在单页内的多个浮动元素（如预览页底部的按钮组）上。
public struct GlassGroup<Content: View>: View {
    var spacing: CGFloat
    @ViewBuilder var content: () -> Content

    public init(spacing: CGFloat = ReconMetric.space2, @ViewBuilder content: @escaping () -> Content) {
        self.spacing = spacing
        self.content = content
    }

    public var body: some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            return AnyView(GlassEffectContainer(spacing: spacing, content: content))
        } else {
            return AnyView(HStack(spacing: spacing, content: content))
        }
        #else
        return AnyView(HStack(spacing: spacing, content: content))
        #endif
    }
}

// ===========================================================================
// MARK: - 便捷入口
// ===========================================================================

extension View {
    /// 应用液态玻璃表面。`role` 必须取自 `GlassRole` 白名单（LG-2）。
    public func glassSurface<S: Shape>(
        _ role: GlassRole,
        in shape: S = .rect(cornerRadius: ReconMetric.radiusButton),
        tint: Color? = nil,
        interactive: Bool = false
    ) -> some View {
        modifier(GlassSurfaceModifier(role: role, shape: shape, tint: tint, interactive: interactive))
    }
}

/// 玻璃工具栏按钮（LG-2：`toolbar` 角色；LG-3/LG-4 由修饰器统一处理）。
///
/// 刻意不提供「把任意业务内容包成玻璃」的通用入口 —— 业务内容区必须不透明。
public struct GlassToolbarButton: View {
    let systemImage: String
    let label: String
    let action: () -> Void

    public init(systemImage: String, label: String, action: @escaping () -> Void) {
        self.systemImage = systemImage
        self.label = label
        self.action = action
    }

    public var body: some View {
        Button(action: action) {
            // 图标尺寸固定（§1.8 图标 20pt 档），不随字号缩放 —— 否则大字体会
            // 撑破 48×48 热区（§1.7）。故用 `fixedFont`。
            Image(systemName: systemImage)   // U-X-6：仅系统 SF Symbols
                .font(ReconFont.fixedFont(ReconFont.headline))
                // §1.7：热区取严者 48×48（不是 controlHeight 的 44）。
                .frame(width: ReconMetric.minTouchTarget, height: ReconMetric.minTouchTarget)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .foregroundStyle(ReconColor.txtPrimary)
        .glassSurface(.toolbar, in: .rect(cornerRadius: ReconMetric.radiusButton))
        .accessibilityLabel(Text(label))   // §6.2 无障碍标签
    }
}
