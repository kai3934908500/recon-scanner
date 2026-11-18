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

    /// `bg` 命名空间别名（**小写**，与 docs/09 §1.3 的令牌名 `bg.page` 逐字对应）。
    ///
    /// 为什么需要它：Swift 的嵌套类型**不能**用小写成员访问（`ReconColor.bg`
    /// 不是 `Bg` 的合法写法，编译器报 `type 'ReconColor' has no member 'bg'`）。
    /// 但 docs/09 §1.3 的表里令牌名就是 `bg.page` / `bg.secondary` / …
    /// 若强制调用点改写成 `ReconColor.Bg.page`，令牌名与规格表就不再逐字对应，
    /// 后续按规格核对时会产生「代码里找不到 `bg.page`」的假缺口。
    ///
    /// 故在此提供**同一份值**的小写入口：`public static let bg = Bg.self`。
    /// 它不复制任何色值（`Bg` 仍是唯一真源），只是暴露一个符合规格命名的访问路径。
    /// 两种写法 `ReconColor.bg.page` 与 `ReconColor.Bg.page` 完全等价。
    public static let bg = Bg.self

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
///
/// ── Dynamic Type（docs/09 §3.11 U-S-1..6，v1.3 新增）────────────────────
///
/// **为什么需要 `textStyle` 字段**：v1.2 及以前，`Spec` 只有固定 `size`
/// （如 `.system(size: 17)`）—— **固定字号对系统「文字大小」设置完全无响应**，
/// 这正是 v1.1 下「字号匹配不好」的根因（U-S-4）。修法不是把 `size` 乘以某个
/// 系数（那是绝对覆盖，会破坏可访问性放大），而是**改用 `Font.TextStyle`**
/// —— 由系统按当前 Dynamic Type 设置解析出字号。
///
/// **为什么保留固定 `size`**：docs/09 §1.4 的 11 档固定字号与 Apple Dynamic Type
/// 各 `TextStyle` 的**默认（Large）字号逐值相同**（§1.4 末映射表，U-S-5），
/// 故两者**不是二选一**：`textStyle` 承载缩放基准，`size` 作为**默认档的等价真值**
/// 保留，供两处使用 —— ① 非文字场景（§1.8 图标尺寸、§6.4「图形标记不随字号缩放」）；
/// ② 单测/闸门核对「恒等映射」是否仍成立（差异列全 0 是判据）。
///
/// **禁止** `.system(size: 17, relativeTo: .body)` —— SwiftUI **不存在**该签名
/// （`relativeTo:` 仅属 `Font.custom(_:size:relativeTo:)`）。v1.2 规格初稿误写此
/// 形式，若照实现**编译不过**（U-S-4 订正登记）。
public enum ReconFont {

    /// 尺寸 / 行高（pt）+ Dynamic Type 缩放基准。
    ///
    /// - `textStyle`：缩放基准（U-S-4）。渲染文字**必须**经 `font(_:mono:)` 使用它，
    ///   不得直接读 `size` 构造 `.system(size:)`。
    /// - `size` / `lineHeight`：**默认档（Large）等价真值**，与 Apple 各
    ///   `TextStyle` 的默认字号逐值相同（U-S-5）。供图标尺寸与映射校验使用。
    public struct Spec {
        /// Dynamic Type 缩放基准（U-S-4）。
        public let textStyle: Font.TextStyle
        /// 默认档（Large）字号。**不参与系统缩放**，仅供图标与校验。
        public let size: CGFloat
        /// 默认档行高（pt）。行高随字号缩放由 `lineSpacing`/容器自适应承载（U-S-6）。
        public let lineHeight: CGFloat
        public let weight: Font.Weight

        /// SwiftUI `Text.lineSpacing` 需要的是**行间距增量**，即 (行高 − 字号)。
        ///
        /// 注意这是**默认档**的增量。字号被 Dynamic Type 放大后，实际行高由
        /// 系统按行高倍数同步放大（`Font.TextStyle` 自带），故此处无需重算
        /// （U-S-6：系数施加点在字号，不在行高）。
        public var lineSpacing: CGFloat { max(0, lineHeight - size) }
    }

    /// 仅首页顶部项目名
    public static let display = Spec(textStyle: .largeTitle, size: 34, lineHeight: 40, weight: .regular)
    /// 结果页、导出页标题
    public static let title1 = Spec(textStyle: .title, size: 28, lineHeight: 34, weight: .regular)
    /// 区块标题
    public static let title2 = Spec(textStyle: .title2, size: 22, lineHeight: 28, weight: .regular)
    /// 界面标题
    public static let title3 = Spec(textStyle: .title3, size: 20, lineHeight: 25, weight: .regular)
    /// 列表主标题、按钮文字
    public static let headline = Spec(textStyle: .headline, size: 17, lineHeight: 22, weight: .semibold)
    /// 正文
    public static let body = Spec(textStyle: .body, size: 17, lineHeight: 22, weight: .regular)
    /// 长正文、次要说明
    public static let callout = Spec(textStyle: .callout, size: 16, lineHeight: 21, weight: .regular)
    /// 列表副标题、控件标签
    public static let subhead = Spec(textStyle: .subheadline, size: 15, lineHeight: 20, weight: .regular)
    /// 辅助说明、单位、阈值
    public static let footnote = Spec(textStyle: .footnote, size: 13, lineHeight: 18, weight: .regular)
    /// 统计数值标签
    public static let caption = Spec(textStyle: .caption, size: 12, lineHeight: 16, weight: .regular)
    /// 时间戳、文件路径尾段
    public static let caption2 = Spec(textStyle: .caption2, size: 11, lineHeight: 13, weight: .regular)

    /// **非缩放**字体构造：固定字号，**仅限图标/图形标记**（§1.8 图标尺寸、
    /// §6.4「图形标记不随字号缩放」）。
    ///
    /// 与 `.reconFont(...)` 的区别是语义，不是实现：本函数**刻意**不参与 Dynamic Type，
    /// 因为规格要求这些元素尺寸固定（避免遮挡预览、避免图标随字号撑破工具栏）。
    /// 名称中带 `Fixed` 以在调用点自证意图，审查时一眼可辨。
    public static func fixedFont(_ spec: Spec, mono: Bool = false) -> Font {
        .system(size: spec.size, weight: spec.weight,
                design: mono ? .monospaced : .default)
    }

    /// **非缩放**字体构造（**裸字号**变体）。
    ///
    /// ⚠️ 仅供「规格尚未给出 token」的图形标记使用（如 §3.4 引导三角标记，
    /// 规格写 12px 而实现为 24pt，**一致性未裁决**）。调用点**必须**写注释
    /// 说明该数值的来源与为何不是 token —— 否则它就是 §1「不引入规格外取值」
    /// 的违规点。新代码**不应**使用本重载；能在 §1.4 找到对应 token 的一律用
    /// `fixedFont(_ spec:)`。
    public static func fixedFont(size: CGFloat, mono: Bool = false) -> Font {
        .system(size: size, weight: .regular,
                design: mono ? .monospaced : .default)
    }

    /// 用户档位系数的**唯一施加点**（U-S-2）。
    ///
    /// 输入是**已被系统 Dynamic Type 缩放**的字号，输出是「再乘档位系数」后的最终
    /// 字号。字体渲染与宽度估算（`SpecText` 的 40 字符截断上限）都经此函数，
    /// 保证「系数只在字号上乘一次」这一逻辑**不写第二遍**。
    ///
    /// - Parameters:
    ///   - systemScaled: 系统缩放后的字号（来自 `@ScaledMetric`）。
    ///   - scale: 用户档位。
    public static func applyScale(_ systemScaled: CGFloat, scale: ReconFontScale) -> CGFloat {
        systemScaled * scale.factor
    }
}

// ===========================================================================
// MARK: - §3.11 U-S-1..3 字号档位（Dynamic Type 系数）
// ===========================================================================

/// 字号档位（docs/09 §3.11 U-S-2）。
///
/// 三档 = **在系统缩放之上再乘一个系数**（0.9 / 1.0 / 1.15），**不做绝对字号覆盖**
/// —— 保留 Dynamic Type 的可访问性放大能力（U-S-2）。
public enum ReconFontScale: String, CaseIterable, Sendable {
    /// `跟随系统`（U-S-1）：不施加额外系数，完全采用 Dynamic Type。
    case system
    /// `小`：×0.9
    case small
    /// `标准`：×1.0
    case standard
    /// `大`：×1.15
    case large

    /// 施加在系统缩放之上的系数（U-S-2）。
    ///
    /// `system` 档也返回 1.0 —— 它的语义是「不额外乘系数」，与 `standard` 在
    /// **数值**上等价，但**语义不同**（前者表示"跟随系统"，后者表示"用户显式选了标准档"），
    /// 故保留为两个 case，不合并。
    public var factor: CGFloat {
        switch self {
        case .system, .standard: return 1.0
        case .small:             return 0.9
        case .large:             return 1.15
        }
    }

    /// §3.11 U-S-3：locale 无关的 `UserDefaults` 键 `dynamic_type_override`。
    ///
    /// 该键**不是 locale 键**，不进 §5 文案表（U-S-3 明文）。
    public static let defaultsKey = "dynamic_type_override"
}

// ===========================================================================
// MARK: - 唯一的字体缩放入口（U-S-4：文字必须走此路径）
// ===========================================================================

private struct ReconFontScaleKey: EnvironmentKey {
    static let defaultValue: ReconFontScale = .system
}

public extension EnvironmentValues {
    /// 当前字号档位（由 `RootView` 从 `UserDefaults` 读出后注入）。
    var reconFontScale: ReconFontScale {
        get { self[ReconFontScaleKey.self] }
        set { self[ReconFontScaleKey.self] = newValue }
    }
}

/// **文字的统一缩放修饰器**：系统 Dynamic Type × 用户档位系数。
///
/// 这是全 App **唯一**的字体缩放实现（U-S-4「所有 `ReconFont.*` 必须走缩放路径」），
/// `SpecText` 与所有直接挂字体的按钮/标签都经它取字体，避免各写各的。
///
/// 机制（U-S-1 + U-S-2 的合成）：
/// 1. `@ScaledMetric(relativeTo: spec.textStyle)` —— 以 `spec.size` 为**默认档初值**，
///    由系统按当前 Dynamic Type 设置缩放出实际字号。因 `spec.size` 与 Apple 该
///    `TextStyle` 的默认字号**逐值相同**（U-S-5 的恒等映射），系统处于默认档时
///    结果 == `spec.size`，与 §1.4 表逐值一致；
/// 2. 再乘 `reconFontScale.factor`（0.9 / 1.0 / 1.15）—— 这是「再乘一个系数」，
///    不是绝对覆盖，故系统放大到 200% 时仍是 200% × factor（U-S-2 可访问性保留）。
private struct ScaledFontModifier: ViewModifier {
    let spec: ReconFont.Spec
    let mono: Bool

    /// 系统缩放后的字号（默认档初值 = `spec.size`）。
    @ScaledMetric private var scaledSize: CGFloat
    /// 用户档位系数（U-S-2），由环境注入。
    @Environment(\.reconFontScale) private var scale

    init(spec: ReconFont.Spec, mono: Bool) {
        self.spec = spec
        self.mono = mono
        // `relativeTo:` 取 `spec.textStyle` —— 缩放曲线与该文本样式对齐。
        _scaledSize = ScaledMetric(wrappedValue: spec.size, relativeTo: spec.textStyle)
    }

    func body(content: Content) -> some View {
        content.font(
            .system(size: ReconFont.applyScale(scaledSize, scale: scale),
                    weight: spec.weight,
                    design: mono ? .monospaced : .default)
        )
    }
}

/// 供**宽度估算**用的「当前实际字号」（默认档初值 × 系统缩放 × 档位系数）。
///
/// 与 `ScaledFontModifier` 共用 `ReconFont.applyScale`，故「系数只乘一次」不写两遍。
/// 用途：`SpecText` 的单行 40 字符截断上限 —— 字号放大后容器上限须同步放宽，
/// 否则 §6.4「200% 时允许两行」会被过早截断破坏。
public struct ScaledFontMetrics<Content: View>: View {
    private let spec: ReconFont.Spec
    private let render: (_ renderedSize: CGFloat) -> Content

    @ScaledMetric private var scaledSize: CGFloat
    @Environment(\.reconFontScale) private var scale

    public init(spec: ReconFont.Spec,
                @ViewBuilder render: @escaping (_ renderedSize: CGFloat) -> Content) {
        self.spec = spec
        self.render = render
        _scaledSize = ScaledMetric(wrappedValue: spec.size, relativeTo: spec.textStyle)
    }

    public var body: some View {
        render(ReconFont.applyScale(scaledSize, scale: scale))
    }
}

public extension View {
    /// 按字号 token 挂字体，**自动响应系统 Dynamic Type 与用户档位**（U-S-1..4）。
    ///
    /// - Parameters:
    ///   - spec: 字号 token（`ReconFont.*`）。
    ///   - mono: 数值用等宽（§1.4「数字」行）。
    ///   - withLineSpacing: 是否同时设置 `Spec.lineSpacing`（多行正文用 `true`，
    ///     单行文本用默认 `false` 以免无谓的行距）。
    ///
    /// 图标/图形标记**不要**用本修饰器，用 `ReconFont.fixedFont(_:)`（§1.8 / §6.4）。
    func reconFont(_ spec: ReconFont.Spec, mono: Bool = false,
                   withLineSpacing: Bool = false) -> some View {
        modifier(ScaledFontModifier(spec: spec, mono: mono))
            .lineSpacing(withLineSpacing ? spec.lineSpacing : 0)
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
