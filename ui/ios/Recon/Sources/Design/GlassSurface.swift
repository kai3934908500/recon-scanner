//
//  GlassSurface.swift
//  Recon — Liquid Glass 材质封装（iOS 26+，含 iOS 15+ 回退）
//
//  契约：docs/09_UI规格.md §1.2.1（LG-1 .. LG-10）。
//
//  ── 一个必须说清的工程约束 ─────────────────────────────────────────────
//
//  Liquid Glass 的 API（`.glassEffect(_:in:)` / `GlassEffectContainer`）**只在
//  iOS 26 SDK 中存在**。本项目的 Xcode 基线是 15.4（iOS 17 SDK，docs/12 §4.1）。
//  在 iOS 17 SDK 下直接写 `.glassEffect(...)` 会**编译失败**（符号不存在），
//  写 `#available(iOS 26.0, *)` 也会触发「unexpected version」警告 —— 两者都
//  违反本项目的零警告标准。
//
//  因此本文件采用**编译期门 + 运行时门**双层守卫：
//
//      编译器版本门（编译期）        运行时版本门（运行期）
//      ┌──────────────────────┐    ┌──────────────────────┐
//      │ #if compiler(>=6.2)  │ →  │ #available(iOS 26.0) │ →  原生液态玻璃
//      │ (Xcode 26+ / iOS 26  │    └──────────────────────┘
//      │  SDK 存在)           │ →  else                →  .ultraThinMaterial 回退
//      └──────────────────────┘
//      #else                                              →  .ultraThinMaterial 回退
//
//  含义：用 Xcode 15.4 构建时，原生玻璃分支**根本不参与编译**，产物是纯回退
//  路径 —— 这**不是**缺陷，而是「基线工具链无法产出原生玻璃」的如实结果。
//  要真正产出液态玻璃效果，必须用 **Xcode 26+ 重新构建**（此时 `#if` 走真分支）。
//  这一点必须在交付说明中写明，不得声称「已实现」而实际跑的是回退。
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
            return "当前构建使用 Xcode 15.4 基线（iOS 17 SDK），无法编译 Liquid Glass 材质，界面使用系统半透明材质替代。"
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
            Image(systemName: systemImage)   // U-X-6：仅系统 SF Symbols
                .font(.system(size: ReconFont.headline.size, weight: .regular))
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
