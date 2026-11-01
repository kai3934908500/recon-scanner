//
//  DesignTokens.swift
//  Recon — L6 UI 层设计令牌（iOS）
//
//  契约：docs/09_UI规格.md §1.3（色板）、§1.4（字体字号）、§1.5（尺寸间距圆角）、
//       §1.6（动效）、§1.7（触控与对比度）。
//
//  本文件是 docs/09 §1 各表的**逐条转写**，不引入任何规格外的取值。
//  任何新增/修改必须先改 docs/09，再改本文件（不许代码先行）。
//
//  深浅色策略（docs/09 §1.3.3）：成对手写 Light/Dark 两套值，
//  **禁止**代码内做 dynamicColor 反色计算。此处用 UIColor 的 trait 闭包只为
//  「按当前外观选择已手写好的那一套常量」，不含任何算术推导。
//

import SwiftUI
import UIKit

// ===========================================================================
// MARK: - §1.3 色板
// ===========================================================================

/// 色板令牌（docs/09 §1.3.1 Light / §1.3.2 Dark）。
///
/// 命名与规格表一一对应：`bg.page` → `.page`（嵌套在 `ReconColor.Bg` 下）。
public enum ReconColor {

    /// 按外观二选一取色。两套值均**手写自** docs/09 §1.3，不做反色计算。
    private static func pair(light: String, dark: String) -> Color {
        Color(uiColor: UIColor { trait in
            trait.userInterfaceStyle == .dark ? UIColor(hex: dark) : UIColor(hex: light)
        })
    }

    // --- bg.* -------------------------------------------------------------
    public enum Bg {
        /// `bg.page`
        public static let page = pair(light: "#FFFFFF", dark: "#000000")
        /// `bg.secondary` — 设置分组底色、结果页统计区底色
        public static let secondary = pair(light: "#F2F2F7", dark: "#1C1C1E")
        /// `bg.preview` — 相机预览 letterbox 底色（两套均为纯黑）
        public static let preview = pair(light: "#000000", dark: "#000000")
        /// `bg.third` — `bg.secondary` 上的抬升区块（仅用分隔线区分，不用阴影）
        public static let third = pair(light: "#FFFFFF", dark: "#2C2C2E")
    }

    // --- separator --------------------------------------------------------
    /// `separator` — 1px 分隔线
    public static let separator = pair(light: "#C6C6C8", dark: "#38383A")
    /// `separator.opaque` — 需要压过 `bg.secondary` 的分隔线
    public static let separatorOpaque = pair(light: "#C6C6C8", dark: "#545458")

    /// `scrim` — `#000000` @ 60%。预览上的**纯色**遮罩，用于承载白字。
    /// 注意：docs/09 §1.3.3 明确「禁止用模糊，仅用纯色叠加」；§1.2.1 补充
    /// 「不允许被 Liquid Glass 替代」。
    public static let scrim = Color.black.opacity(0.60)

    // --- sys.* ------------------------------------------------------------
    /// 选中态、进度条前景、开关开启、滑块轨道
    public static let accent = pair(light: "#007AFF", dark: "#0A84FF")
    /// 实心主按钮底色
    public static let accentSolid = pair(light: "#0066CC", dark: "#0A84FF")
    /// 主按钮按下态
    public static let accentPressed = pair(light: "#0052A8", dark: "#409CFF")
    /// 成功图标填充、已完成标记
    public static let success = pair(light: "#34C759", dark: "#30D158")
    /// 警告图标填充
    public static let warning = pair(light: "#FF9500", dark: "#FF9F0A")
    /// 错误图标填充、失败标记
    public static let error = pair(light: "#FF3B30", dark: "#FF453A")

    // --- txt.* ------------------------------------------------------------
    /// 标题、正文
    public static let txtPrimary = pair(light: "#000000", dark: "#FFFFFF")
    /// 次要说明
    public static let txtSecondary = pair(light: "#616161", dark: "#98989D")
    /// 极次要、时间戳
    public static let txtTertiary = pair(light: "#757575", dark: "#76767B")
    /// 可点击文字
    public static let txtAccent = pair(light: "#005FBF", dark: "#409CFF")
    /// 成功文字
    public static let txtSuccess = pair(light: "#1F7A33", dark: "#30D158")
    /// 警告文字
    public static let txtWarning = pair(light: "#A65300", dark: "#FFD60A")
    /// 错误文字
    public static let txtError = pair(light: "#C62828", dark: "#FF6961")
    /// 实心按钮上的文字
    public static let txtOnSolid = pair(light: "#FFFFFF", dark: "#FFFFFF")
}

// ===========================================================================
// MARK: - §1.4 字体与字号阶梯
// ===========================================================================

/// 字体令牌（docs/09 §1.4）。
///
/// 规则约束（同表下方）：
/// - 字体族：`Font.system`（SF Pro Text）；数值用 SF Mono。**不使用自定义字体**。
/// - 字重仅 3 档：Regular / Medium(Android) / Semibold。**禁止 Bold、Black**。
/// - 行高固定，不随内容自适应；字距 0，不使用 tracking。
public enum ReconFont {

    /// 尺寸 / 行高（pt）。行高固定，由调用方通过 `lineSpacing` 落实。
    public struct Spec {
        public let size: CGFloat
        public let lineHeight: CGFloat
        public let weight: Font.Weight

        /// SwiftUI `Text.lineSpacing` 需要的是**行间距增量**，即 (行高 − 字号)。
        public var lineSpacing: CGFloat { max(0, lineHeight - size) }
    }

    /// 仅首页顶部项目名
    public static let display = Spec(size: 34, lineHeight: 40, weight: .regular)
    /// 结果页、导出页标题
    public static let title1 = Spec(size: 28, lineHeight: 34, weight: .regular)
    /// 区块标题
    public static let title2 = Spec(size: 22, lineHeight: 28, weight: .regular)
    /// 界面标题
    public static let title3 = Spec(size: 20, lineHeight: 25, weight: .regular)
    /// 列表主标题、按钮文字
    public static let headline = Spec(size: 17, lineHeight: 22, weight: .semibold)
    /// 正文
    public static let body = Spec(size: 17, lineHeight: 22, weight: .regular)
    /// 长正文、次要说明
    public static let callout = Spec(size: 16, lineHeight: 21, weight: .regular)
    /// 列表副标题、控件标签
    public static let subhead = Spec(size: 15, lineHeight: 20, weight: .regular)
    /// 辅助说明、单位、阈值
    public static let footnote = Spec(size: 13, lineHeight: 18, weight: .regular)
    /// 统计数值标签
    public static let caption = Spec(size: 12, lineHeight: 16, weight: .regular)
    /// 时间戳、文件路径尾段
    public static let caption2 = Spec(size: 11, lineHeight: 13, weight: .regular)

    /// 数值专用等宽字体（§1.4「数字」行：所有统计数值必须等宽 + 右对齐 + 千分位）。
    public static func mono(_ spec: Spec) -> Font {
        .system(size: spec.size, weight: spec.weight, design: .monospaced)
    }
}

// ===========================================================================
// MARK: - §1.5 尺寸、间距、圆角
// ===========================================================================

/// 尺寸令牌（docs/09 §1.5）。iOS 用 pt。
public enum ReconMetric {
    /// `radius.button` — 主/次按钮
    public static let radiusButton: CGFloat = 8
    /// `radius.input` — 文本输入框
    public static let radiusInput: CGFloat = 6
    /// `radius.card` — 统计区块容器（仅描边或底色，**无阴影**）
    public static let radiusCard: CGFloat = 8
    /// `radius.chip` — 参数行的「已修正/锁定」小标签
    public static let radiusChip: CGFloat = 4

    /// `space.1` — 行内元素间距
    public static let space1: CGFloat = 4
    /// `space.2` — 控件内元素间距
    public static let space2: CGFloat = 8
    /// `space.3` — 列表项内边距
    public static let space3: CGFloat = 12
    /// `space.4` — 区块内边距
    public static let space4: CGFloat = 16
    /// `space.5` — 区块之间
    public static let space5: CGFloat = 24
    /// `space.6` — 页面级分隔
    public static let space6: CGFloat = 32

    /// `hairline` — 1px（0.5pt）
    public static let hairline: CGFloat = 0.5
    /// `control.height` — 输入框、开关所在行高
    public static let controlHeight: CGFloat = 44
    /// `progress.height` — 进度条
    public static let progressHeight: CGFloat = 4

    /// §1.7 最小触控热区。规格原文：「iOS 44×44 pt，Android 48×48 dp。
    /// 两者取严者：**48×48**」。—— 注意这与 `controlHeight`（44，行高）是两个
    /// 不同的量：行高 44 是布局尺寸，热区下限 48 是可点面积，不得混用。
    public static let minTouchTarget: CGFloat = 48
    /// §1.7 相邻热区间距下界
    public static let minTouchSpacing: CGFloat = 8

    /// 圆角禁令判定式（§1.5）：任何控件的圆角半径**不得超过其短边的一半**。
    /// 这是禁止胶囊（U-X-3）的通用判据，供组件自查用。
    public static func radiusAllowed(_ radius: CGFloat, shortSide: CGFloat) -> Bool {
        radius <= shortSide / 2
    }
}

// ===========================================================================
// MARK: - §1.6 动效
// ===========================================================================

/// 动效令牌（docs/09 §1.6）。
///
/// 铁律：除规格表列出项外，**全 App 零自定义动画**（U-M-1）。
public enum ReconMotion {

    /// 唯一允许的自定义插值：进度条数值变化时的 200ms 线性插值。
    /// 规格明确「无循环、无呼吸、无光泽」。
    public static let progressInterpolation: Double = 0.200

    /// `prefers-reduced-motion` 等效判定（U-M-2）：开启时所有自定义插值时长归零，
    /// 功能不受影响。
    public static func duration(_ seconds: Double, reduceMotion: Bool) -> Double {
        reduceMotion ? 0 : seconds
    }

    /// 页面转场：使用系统默认（iOS push），由 `NavigationStack` 提供。
    /// **禁止**自定义滑入、缩放、淡入（§1.6 表）。
    public static let allowsCustomTransitions = false
}

// ===========================================================================
// MARK: - 十六进制解析
// ===========================================================================

extension UIColor {
    /// 解析 `#RRGGBB` / `RRGGBB` / `#AARRGGBB`。仅接受 6 位或 8 位。
    /// 解析失败即编程错误（令牌写错），故用 `preconditionFailure` 而非静默返回。
    convenience init(hex: String) {
        var s = hex
        if s.hasPrefix("#") { s.removeFirst() }
        guard s.count == 6 || s.count == 8, let v = UInt64(s, radix: 16) else {
            preconditionFailure("非法色值字面量 '\(hex)'：应为 #RRGGBB 或 #AARRGGBB")
        }
        let r, g, b, a: CGFloat
        if s.count == 8 {
            r = CGFloat((v >> 24) & 0xFF) / 255
            g = CGFloat((v >> 16) & 0xFF) / 255
            b = CGFloat((v >> 8) & 0xFF) / 255
            a = CGFloat(v & 0xFF) / 255
        } else {
            r = CGFloat((v >> 16) & 0xFF) / 255
            g = CGFloat((v >> 8) & 0xFF) / 255
            b = CGFloat(v & 0xFF) / 255
            a = 1
        }
        self.init(red: r, green: g, blue: b, alpha: a)
    }
}
