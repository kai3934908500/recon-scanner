//
//  Components.swift
//  Recon — L6 UI 层基础组件（iOS）
//
//  契约：docs/09_UI规格.md §1.3（色板）、§1.4（字体/数字格式）、§1.5（尺寸圆角）、
//       §1.6（动效）、§1.7（触控与对比度）、§1.2（视觉禁令 U-X-1..U-X-10）。
//
//  设计约束（在组件层面直接落死，避免调用方各写各的）：
//   - U-X-1 渐变：本文件不使用任何 Gradient。
//   - U-X-2 阴影：本文件不使用 shadow / elevation；分组一律用 1px 分隔线或留白。
//   - U-X-3 胶囊：圆角一律走 ReconMetric.radius*，且不超过短边一半。
//   - U-X-4/U-M-1：无入场动画；仅进度条有 200ms 线性插值（ReconMotion）。
//

import SwiftUI

// ===========================================================================
// MARK: - 规格文本
// ===========================================================================

/// 按字号 token 渲染文本，自动落实行高与色板。
///
/// 集中处理三件事，避免各页面重复写：
/// 1. 行高（`Spec.lineSpacing` = 行高 − 字号）；
/// 2. 颜色 token；
/// 3. §1.4「上限」：单行 **40 个半角字符**超出即尾部 `…` 截断 —— 由
///    `truncateWidth` 参数配合 `lineLimit(1)` 落实（宽度按 `footnote` 字号
///    估算 40 个半角字符的合理宽度）；多行最多 3 行。
public struct SpecText: View {
    let text: String
    let spec: ReconFont.Spec
    let color: Color
    let mono: Bool
    let maxLines: Int
    let applyCharLimit: Bool

    public init(
        _ text: String,
        spec: ReconFont.Spec = ReconFont.body,
        color: Color = ReconColor.txtPrimary,
        mono: Bool = false,
        maxLines: Int = 1,
        applyCharLimit: Bool = true
    ) {
        self.text = text
        self.spec = spec
        self.color = color
        self.mono = mono
        self.maxLines = maxLines
        self.applyCharLimit = applyCharLimit
    }

    /// §1.4 上限：单行 40 个半角字符。
    private static let singleLineCharLimit = 40

    /// 需要截断时按当前字号估算的宽度上限（40 个半角字符 ≈ 0.5 em 宽）。
    private var truncateWidth: CGFloat? {
        guard applyCharLimit, maxLines == 1 else { return nil }
        return CGFloat(Self.singleLineCharLimit) * spec.size * 0.5
    }

    public var body: some View {
        let font: Font = mono
            ? .system(size: spec.size, weight: spec.weight, design: .monospaced)
            : .system(size: spec.size, weight: spec.weight)

        Text(text)
            .font(font)
            .foregroundStyle(color)
            .lineSpacing(spec.lineSpacing)
            .lineLimit(maxLines)
            .frame(maxWidth: truncateWidth, alignment: .leading)
            .truncationMode(.tail)
    }
}

// ===========================================================================
// MARK: - §1.4 数值格式（等宽 + 右对齐 + 千分位）
// ===========================================================================

/// 统计数值的统一渲染。
///
/// §1.4「数字」行：所有统计数值（面数、顶点、尺寸、百分比、耗时、内存）**必须**
/// 等宽 + 右对齐 + 千分位分隔（`1,284,512`）；单位前留一个半角空格（`182.4 mm`）。
public struct MonoNumber: View {
    let value: String
    let unit: String?
    let spec: ReconFont.Spec
    let color: Color

    public init(_ value: String, unit: String? = nil,
                spec: ReconFont.Spec = ReconFont.caption,
                color: Color = ReconColor.txtPrimary) {
        self.value = value
        self.unit = unit
        self.spec = spec
        self.color = color
    }

    /// 整数千分位格式化（`1284512` → `1,284,512`）。
    ///
    /// `nonisolated`：本函数是**纯格式化**，不触碰任何视图状态，却被
    /// `ScanSessionModel.frameCountText(_:)`（`nonisolated public static`）
    /// 调用 —— 而 `MonoNumber: View` 的类型成员在 Swift 6 下默认 `@MainActor`
    /// 隔离，于是非隔离上下文调用它会报
    /// 「call to main actor-isolated static method 'group' in a synchronous
    /// nonisolated context」。标记 `nonisolated` 是**语义正确**的修法
    /// （它本就不需要 actor 隔离），而非为了让编译通过而放宽调用点。
    /// `formatter` 也随之 `nonisolated(unsafe)` —— `NumberFormatter` 的
    /// `string(from:)` 在 Apple 文档中标注为线程安全；此处只在静态初始化时
    /// 配置一次、之后只读，故该标注是准确的（不是"为过编译而撒谎"）。
    nonisolated public static func group(_ n: Int) -> String {
        Self.formatter.string(from: NSNumber(value: n)) ?? "\(n)"
    }

    nonisolated(unsafe) private static let formatter: NumberFormatter = {
        let f = NumberFormatter()
        f.numberStyle = .decimal
        f.groupingSeparator = ","
        f.usesGroupingSeparator = true
        f.maximumFractionDigits = 0
        return f
    }()

    public var body: some View {
        HStack(spacing: ReconMetric.space1) {
            Text(value)
                .font(.system(size: spec.size, weight: spec.weight, design: .monospaced))
            if let unit {
                // 单位与数值之间留一个半角空格（§1.4）。
                Text(unit)
                    .font(.system(size: spec.size, weight: spec.weight))
            }
        }
        .foregroundStyle(color)
        .frame(minWidth: 0, alignment: .trailing)   // 右对齐
        .monospacedDigit()
    }
}

// ===========================================================================
// MARK: - 分隔线（U-X-2：分组只用 1px 线或留白）
// ===========================================================================

public struct Hairline: View {
    let opaque: Bool
    let vertical: Bool

    public init(opaque: Bool = false, vertical: Bool = false) {
        self.opaque = opaque
        self.vertical = vertical
    }

    public var body: some View {
        Rectangle()
            .fill(opaque ? ReconColor.separatorOpaque : ReconColor.separator)
            .frame(width: vertical ? ReconMetric.hairline : nil,
                   height: vertical ? nil : ReconMetric.hairline)
    }
}

// ===========================================================================
// MARK: - 按钮（§1.5 圆角 8、§1.7 热区 48、U-X-1 无渐变、U-X-2 无阴影）
// ===========================================================================

/// 实心主按钮。文案 `headline`。
public struct PrimaryButton: View {
    let title: String
    let enabled: Bool
    let hint: String?
    let action: () -> Void

    /// - Parameter hint: 禁用时在按钮**下方**补一行 `footnote` `txt.warning`
    ///   （§3.3「完成扫描」的状态规则：`至少需要 12 帧，当前 8 帧`）。
    public init(_ title: String, enabled: Bool = true, hint: String? = nil,
                action: @escaping () -> Void) {
        self.title = title
        self.enabled = enabled
        self.hint = hint
        self.action = action
    }

    public var body: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space1) {
            Button(action: action) {
                Text(title)
                    .font(.system(size: ReconFont.headline.size, weight: .semibold))
                    .foregroundStyle(ReconColor.txtOnSolid)
                    .frame(maxWidth: .infinity)
                    .frame(height: ReconMetric.controlHeight)
                    .background(ReconColor.accentSolid)     // 纯色，无渐变（U-X-1）
                    .clipShape(RoundedRectangle(cornerRadius: ReconMetric.radiusButton))
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .disabled(!enabled)
            .opacity(enabled ? 1 : 0.35)

            if !enabled, let hint {
                SpecText(hint, spec: ReconFont.footnote, color: ReconColor.txtWarning)
            }
        }
    }
}

/// 次按钮（描边）。文案 `headline`。
public struct SecondaryButton: View {
    let title: String
    let enabled: Bool
    let action: () -> Void

    public init(_ title: String, enabled: Bool = true, action: @escaping () -> Void) {
        self.title = title
        self.enabled = enabled
        self.action = action
    }

    public var body: some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: ReconFont.headline.size, weight: .semibold))
                .foregroundStyle(ReconColor.accent)
                .frame(maxWidth: .infinity)
                .frame(height: ReconMetric.controlHeight)
                .overlay(
                    RoundedRectangle(cornerRadius: ReconMetric.radiusButton)
                        .stroke(ReconColor.accent, lineWidth: 1)   // 描边，非阴影
                )
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.35)
    }
}

// ===========================================================================
// MARK: - 进度条（§1.5 高 4、§1.6 200ms 线性插值、无循环无呼吸无光泽）
// ===========================================================================

/// **确定性**进度条。U-X-8：加载态用文字 + 确定性进度条，禁止无限旋转指示器。
///
/// 唯一允许的自定义插值：数值变化时 200ms 线性（§1.6）。
public struct LinearProgress: View {
    let fraction: Double              // 0...1
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    public init(fraction: Double) {
        self.fraction = min(max(fraction, 0), 1)
    }

    public var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Rectangle()
                    .fill(ReconColor.separator)                  // 轨道
                Rectangle()
                    .fill(ReconColor.accent)                     // 前色，实色无光泽
                    .frame(width: geo.size.width * fraction)
            }
        }
        .frame(height: ReconMetric.progressHeight)
        .clipShape(Capsule())   // 进度条自身高度 4，圆角 = 高/2，不违反 U-X-3（非按钮）
        .animation(
            .linear(duration: ReconMotion.duration(ReconMotion.progressInterpolation,
                                                   reduceMotion: reduceMotion)),
            value: fraction
        )
    }
}

// ===========================================================================
// MARK: - 内联异常提示条（§3.5 U-05）
// ===========================================================================

/// 内联提示条：文案 + 图标，**无动画**（§1.6「提示条出现：立即出现/立即消失」）。
///
/// U-D-7：不隐藏失败 —— 任何失败/降级/回退/夹取都必须有文字说明。
public struct InlineNotice: View {
    public enum Level { case info, warning, error }

    let text: String
    let level: Level

    public init(_ text: String, level: Level) {
        self.text = text
        self.level = level
    }

    private var symbol: String {
        switch level {
        case .info:    return "info.circle"      // 仅系统 SF Symbols（U-X-6）
        case .warning: return "exclamationmark.triangle"
        case .error:   return "xmark.octagon"
        }
    }

    private var tint: Color {
        switch level {
        case .info:    return ReconColor.txtSecondary
        case .warning: return ReconColor.txtWarning
        case .error:   return ReconColor.txtError
        }
    }

    public var body: some View {
        // §1.5：容器用 8pt 圆角 + 底色，**不用阴影**（U-X-2）；用 1px 描边区分。
        HStack(alignment: .top, spacing: ReconMetric.space2) {
            Image(systemName: symbol)
                .font(.system(size: ReconFont.footnote.size))
                .foregroundStyle(tint)
            SpecText(text, spec: ReconFont.footnote, color: tint, maxLines: 3)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, ReconMetric.space3)
        .padding(.vertical, ReconMetric.space2)
        .background(ReconColor.bg.third)
        .overlay(
            RoundedRectangle(cornerRadius: ReconMetric.radiusCard)
                .stroke(tint.opacity(0.5), lineWidth: 1)
        )
        .clipShape(RoundedRectangle(cornerRadius: ReconMetric.radiusCard))
        // §1.7：错误提示不得仅靠颜色传达，必须同时有文字（此处文字即 msg）。
        .accessibilityElement(children: .combine)
        .accessibilityLabel(Text(text))
    }
}

// ===========================================================================
// MARK: - 小标签 chip（§1.5 radius.chip = 4）
// ===========================================================================

/// 参数行的「已修正 / 锁定」小标签。
public struct Chip: View {
    let title: String

    public init(_ title: String) { self.title = title }

    public var body: some View {
        Text(title)
            .font(.system(size: ReconFont.caption2.size, weight: .regular))
            .foregroundStyle(ReconColor.txtAccent)
            .padding(.horizontal, ReconMetric.space1)
            .padding(.vertical, 1)
            .overlay(
                RoundedRectangle(cornerRadius: ReconMetric.radiusChip)
                    .stroke(ReconColor.txtAccent, lineWidth: 1)
            )
    }
}
