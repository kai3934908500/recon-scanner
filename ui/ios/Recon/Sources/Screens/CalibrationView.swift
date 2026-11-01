//
//  CalibrationView.swift
//  Recon — U-09 尺度标定界面（iOS）
//
//  契约：docs/09_UI规格.md §3.9（线框图、元素表、标定失败文案表、置信度呈现）、
//       §5.6（返回/跳过规则）、07 §5.1 F1–F6（失败码文案，**直接取值不改写**）、
//       07 §2.10（置信度高/中/低三档）。
//

import SwiftUI

public struct CalibrationView: View {
    @EnvironmentObject private var router: AppRouter
    /// 内核编排器：手动尺度与尺度结果的**唯一来源**。
    @EnvironmentObject private var controller: ReconSessionController

    /// 参照物类型（分段控件 4 段）。
    @State private var referenceType: ReferenceType = .bankCard
    /// 实际尺寸（写 `scale_ref_width_mm` / `scale_ref_height_mm`）。
    @State private var widthMM: String = "85.60"
    @State private var heightMM: String = "54.00"
    /// 手动输入尺寸（写 `scale_manual_value_mm`）。
    @State private var manualLongMM: String = ""
    @State private var manualShortMM: String = ""
    /// 当前步骤 1..3（步骤条）。
    @State private var step: Int = 1
    /// 检测结果；`nil` 表示未检出。
    @State private var detection: Detection?
    /// 标定失败码（07 §5.1）。
    @State private var failure: CalibrationFailure?
    /// 置信度（07 §2.10）。
    @State private var confidence: Confidence?
    /// 手动尺度应用后的真实结果（`nil` 表示尚未应用或应用失败）。
    @State private var lastScale: ReconScaleResult?
    /// 是否仅 RGBD 分支（决定「跳过」是否可用）。
    ///
    /// 取内核能力探测的真实分支；未探测时保守按 MONO（`跳过` 禁用），
    /// **不默认放开** —— 放开会让无深度机型的用户跳过一个必需的步骤。
    private var rgbdBranch: Bool {
        controller.capability?.branch == .rgbd
    }

    public init() {}

    public var body: some View {
        VStack(spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: ReconMetric.space5) {
                    referenceSection
                    captureSection
                    manualSection
                }
                .padding(ReconMetric.space4)
            }
            Hairline()
            useButton
        }
        .background(ReconColor.bg.page)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarLeading) {
                Button { router.pop() } label: {
                    HStack(spacing: ReconMetric.space1) {
                        Image(systemName: "chevron.left")
                        SpecText(L10n.t(.calibTitle), spec: ReconFont.headline,
                                 color: ReconColor.txtPrimary)
                    }
                }
                .foregroundStyle(ReconColor.txtAccent)
            }
            ToolbarItem(placement: .topBarTrailing) {
                VStack(alignment: .trailing, spacing: ReconMetric.space1) {
                    Button(L10n.t(.calibSkip)) { router.pop() }
                        .font(.system(size: ReconFont.footnote.size))
                        .foregroundStyle(ReconColor.txtAccent)
                        .disabled(!canSkip)          // BRANCH_MONO 且无手动值时禁用
                    if !canSkip {
                        // §3.9「跳过」状态列：禁用时在其下补 footnote txt.warning。
                        SpecText(L10n.t(.calibMustCalibrate),
                                 spec: ReconFont.footnote, color: ReconColor.txtWarning,
                                 maxLines: 2)
                            .frame(maxWidth: 180)
                    }
                }
            }
        }
    }

    /// `跳过` 可用条件（§3.9）：仅 `BRANCH_RGBD` 或已有手动值时可用。
    private var canSkip: Bool {
        rgbdBranch || !manualLongMM.isEmpty
    }

    // --- 参照物类型 + 实际尺寸（第一区块）---------------------------------

    private var referenceSection: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space3) {
            SpecText(L10n.t(.calibRefType), spec: ReconFont.title2, color: ReconColor.txtPrimary)

            // 分段控件用系统原生（其内建胶囊外形由系统绘制，不属 U-X-3 违规）。
            Picker(L10n.t(.calibRefType), selection: $referenceType) {
                ForEach(ReferenceType.allCases, id: \.self) { t in
                    Text(t.label).tag(t)
                }
            }
            .pickerStyle(.segmented)
            .onChange(of: referenceType) { t in
                if let dims = t.defaultDimensions {
                    widthMM = dims.width
                    heightMM = dims.height
                }
            }

            // `实际尺寸  宽 85.60 mm   高 54.00 mm`
            HStack(spacing: ReconMetric.space3) {
                SpecText(L10n.t(.calibActualSize), spec: ReconFont.subhead,
                         color: ReconColor.txtSecondary)
                HStack(spacing: ReconMetric.space1) {
                    SpecText(L10n.t(.resultWidth), spec: ReconFont.footnote,
                             color: ReconColor.txtSecondary)
                    DimensionField(text: $widthMM)
                }
                HStack(spacing: ReconMetric.space1) {
                    SpecText(L10n.t(.resultHeight), spec: ReconFont.footnote,
                             color: ReconColor.txtSecondary)
                    DimensionField(text: $heightMM)
                }
            }
        }
    }

    // --- 拍摄 / 检测 / 解算（第二区块）------------------------------------

    private var captureSection: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space3) {
            // 步骤条：当前步 txt.primary，其余 txt.tertiary。
            // §3.9：**不做步骤动画、不做对勾**（U-X-4）。
            VStack(alignment: .leading, spacing: ReconMetric.space1) {
                StepLine(L10n.t(.calibStep1), active: step == 1)
                StepLine(L10n.t(.calibStep2), active: step == 2)
                StepLine(L10n.t(.calibStep3), active: step == 3)
            }

            ZStack {
                Color.black.frame(height: 200)
                // 检出的四边形用 2px sys.accent 折线叠加；角点用 6px 实心方块。
                if detection != nil {
                    RoundedRectangle(cornerRadius: 0)
                        .stroke(ReconColor.accent, lineWidth: 2)
                        .frame(width: 220, height: 140)
                }
            }

            detectionResult

            if let failure {
                // 07 §5.1 F1–F6：主文案 + 补充说明，直接取值不改写。
                InlineNotice("\(failure.mainText)。\(failure.supplement)", level: .warning)
            }
            if let confidence {
                confidenceView(confidence)
            }

            SecondaryButton(L10n.t(.calibRetake), enabled: true) {
                detection = nil
                failure = nil
                confidence = nil
                lastScale = nil
                step = 1
            }
        }
    }

    @ViewBuilder
    private var detectionResult: some View {
        if let d = detection {
            // `检测结果  找到 1 个候选 · 重投影误差 1.2 px`
            HStack(spacing: ReconMetric.space2) {
                SpecText(L10n.t(.calibDetectionResult), spec: ReconFont.footnote,
                         color: ReconColor.txtSecondary)
                SpecText(L10n.fill2(.fmtDetectionFound, String(d.candidates),
                                   String(format: "%.1f", d.reprojError)),
                         spec: ReconFont.footnote, color: ReconColor.txtPrimary, mono: true)
            }
        } else {
            SpecText(L10n.t(.calibNotFound), spec: ReconFont.footnote,
                     color: ReconColor.txtSecondary)
        }
    }

    /// 置信度呈现（07 §2.10 三档）。
    private func confidenceView(_ c: Confidence) -> some View {
        VStack(alignment: .leading, spacing: ReconMetric.space1) {
            switch c {
            case .high(let sigma):
                SpecText(L10n.t(.calibConfidenceHigh) + " · " +
                         L10n.fill(.fmtUncertainty, ScanSessionModel.percentText(sigma)),
                         spec: ReconFont.footnote, color: ReconColor.txtSuccess)
            case .medium(let sigma):
                SpecText(L10n.fill(.fmtConfidenceMedium,
                                   ScanSessionModel.percentText(sigma)),
                         spec: ReconFont.footnote, color: ReconColor.txtSecondary)
                SpecText(L10n.t(.calibRetryRecommend), spec: ReconFont.footnote,
                         color: ReconColor.txtSecondary)
            case .low:
                SpecText(L10n.t(.calibConfidenceLow),
                         spec: ReconFont.footnote, color: ReconColor.txtError)
            }
        }
    }

    // --- 手动输入尺寸（第三区块）------------------------------------------

    private var manualSection: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space3) {
            SpecText(L10n.t(.calibManualInput), spec: ReconFont.title2,
                     color: ReconColor.txtPrimary)
            HStack(spacing: ReconMetric.space3) {
                HStack(spacing: ReconMetric.space1) {
                    SpecText(L10n.t(.calibLongEdge), spec: ReconFont.footnote,
                             color: ReconColor.txtSecondary)
                    DimensionField(text: $manualLongMM)
                }
                HStack(spacing: ReconMetric.space1) {
                    SpecText(L10n.t(.calibShortEdge), spec: ReconFont.footnote,
                             color: ReconColor.txtSecondary)
                    DimensionField(text: $manualShortMM)
                }
                // §3.9「应用」：把长边毫米值写入内核（recon_set_manual_scale）。
                // 真实结果记入 `lastScale`；失败时不改 UI 状态（不静默）。
                Button(L10n.t(.calibApply)) { applyManualScale() }
                    .font(.system(size: ReconFont.footnote.size))
                    .foregroundStyle(ReconColor.txtAccent)
                    .disabled(!manualInputValid)     // 输入非法时禁用
                    .frame(minWidth: ReconMetric.minTouchTarget,
                           minHeight: ReconMetric.minTouchTarget)
            }
        }
    }

    /// 把「长边毫米」交给编排器换算成尺度因子并写入内核。
    ///
    /// 换算（长边 → 尺度因子）的唯一实现点在
    /// `ReconSessionController.applyManualScale` —— 本视图**不重算**
    /// （同一逻辑写两遍必然漂移）。
    private func applyManualScale() {
        guard let mm = Double(manualLongMM) else { return }
        lastScale = controller.applyManualScale(longEdgeMM: mm)
    }

    /// 长度单位 mm，最小 10，最大 10000（§3.9）。
    private var manualInputValid: Bool {
        guard let v = Double(manualLongMM) else { return false }
        return v >= 10 && v <= 10000
    }

    private var useButton: some View {
        PrimaryButton(L10n.t(.calibUseScale), enabled: canUseScale) { router.pop() }
            .padding(.horizontal, ReconMetric.space4)
            .padding(.vertical, ReconMetric.space3)
    }

    /// 禁用条件（§3.9）：`BRANCH_MONO` 且既无标定结果又无手动值。
    private var canUseScale: Bool {
        rgbdBranch || detection != nil || lastScale?.ok == true || !manualLongMM.isEmpty
    }
}

// --- 子组件 ---------------------------------------------------------------

private struct DimensionField: View {
    @Binding var text: String

    var body: some View {
        HStack(spacing: ReconMetric.space1) {
            TextField("", text: $text)
                .font(.system(size: ReconFont.footnote.size, design: .monospaced))
                .keyboardType(.decimalPad)
                .multilineTextAlignment(.trailing)
                .frame(width: 72, height: ReconMetric.controlHeight)
                .overlay(
                    RoundedRectangle(cornerRadius: ReconMetric.radiusInput)
                        .stroke(ReconColor.separator, lineWidth: 1)
                )
                .foregroundStyle(ReconColor.txtPrimary)
            SpecText("mm", spec: ReconFont.footnote, color: ReconColor.txtSecondary)
        }
    }
}

/// 步骤条行（§3.9：当前步 `txt.primary`，其余 `txt.tertiary`；无动画、无对勾）。
private struct StepLine: View {
    let text: String
    let active: Bool

    init(_ text: String, active: Bool) {
        self.text = text
        self.active = active
    }

    var body: some View {
        SpecText(text, spec: ReconFont.footnote,
                 color: active ? ReconColor.txtPrimary : ReconColor.txtTertiary)
    }
}

// --- 模型 -----------------------------------------------------------------

/// 参照物类型（§3.9 分段控件 4 段）。默认尺寸用于预填「实际尺寸」。
public enum ReferenceType: String, CaseIterable {
    case bankCard   // 银行卡
    case idCard     // 身份证
    case a4Paper    // A4 纸
    case custom     // 自定义

    public var label: String {
        switch self {
        case .bankCard: return L10n.t(.calibBankCard)
        case .idCard:   return L10n.t(.calibIdCard)
        case .a4Paper:  return L10n.t(.calibA4)
        case .custom:   return L10n.t(.calibCustom)
        }
    }

    /// 默认实际尺寸（mm）。`自定义` 无默认值（展开两行输入）。
    public var defaultDimensions: (width: String, height: String)? {
        switch self {
        // ISO/IEC 7810 ID-1：85.60 × 53.98
        case .bankCard: return ("85.60", "53.98")
        // GB 11643：85.60 × 54.00
        case .idCard:   return ("85.60", "54.00")
        // ISO 216 A4：210 × 297
        case .a4Paper:  return ("210.00", "297.00")
        case .custom:   return nil
        }
    }
}

public struct Detection {
    public let candidates: Int
    public let reprojError: Double   // px
}

/// 置信度（07 §2.10）。高：`σ_s/s ≤ 0.02`；中：`≤ 0.05`；低：`> 0.05` 或 `ρ > 0.03`。
public enum Confidence {
    case high(sigma: Double)
    case medium(sigma: Double)
    case low
}

/// 标定失败（07 §5.1 F1–F6）。
///
/// 文案**直接取值，不改写**。补充说明为该表第 3 列；无补充说明者为 `nil`。
public enum CalibrationFailure {
    case cardNotFound          // `W_CALIB_CARD_NOT_FOUND`
    case cardOccluded          // `W_CALIB_CARD_OCCLUDED`
    case scaleIllConditioned   // `W_CALIB_SCALE_ILL_CONDITIONED`
    case noDepth               // `W_CALIB_NO_DEPTH`

    public var mainText: String {
        switch self {
        case .cardNotFound:        return L10n.t(.calibNotFound)
        case .cardOccluded:        return L10n.t(.calibCardOccluded)
        case .scaleIllConditioned: return L10n.t(.calibScaleUnstable)
        case .noDepth:             return L10n.t(.calibNoDepth)
        }
    }

    public var supplement: String {
        switch self {
        case .cardNotFound:        return L10n.t(.calibCardNotFoundHint)
        case .cardOccluded:        return L10n.t(.calibCardOccludedHint)
        case .scaleIllConditioned: return L10n.t(.calibScaleUnstableHint)
        case .noDepth:             return L10n.t(.calibNoDepthHint)
        }
    }
}
