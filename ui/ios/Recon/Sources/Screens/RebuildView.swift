//
//  RebuildView.swift
//  Recon — U-06 重建进度界面（iOS）
//
//  契约：docs/09_UI规格.md §3.6（线框图、元素表、§3.6.1 阶段标题表、
//       §3.6.2 降级提示）、§5.5（R-01..R-14）、§5.6（Q-03 取消确认）、
//       §2 U-NAV-3（U-06 返回需确认）、§1.6（进度条 200ms 线性插值）。
//
//  ── 关键约束（§3.6.2 末）────────────────────────────────────────────────
//  降级提示**只允许**涉及 00 §1.2 的白名单项（并行度 / 分块 / 落盘）。
//  出现任何与精度阈值相关的降级说明即为违规 —— 因为本产品的铁律是
//  「可以降速度、降内存，绝不降精度」。本文件据此只暴露白名单四类文案。
//

import SwiftUI

public struct RebuildView: View {
    let sessionID: UUID

    @EnvironmentObject private var router: AppRouter
    @EnvironmentObject private var session: ScanSessionModel
    /// 内核编排器：进度 / 结局 / 取消的**唯一来源**（不再本地各存一份 `@State`）。
    @EnvironmentObject private var controller: ReconSessionController

    /// 重建进度（0...1）—— 直接取内核回调值，**不做平滑**（U-D-7 不伪造）。
    private var fraction: Double { controller.progress?.fraction ?? 0 }
    /// 当前段号 1..14。内核未给出可映射段时保持上一段（不回退到 1 —— 那会倒退）。
    @State private var lastStageIndex: Int = 1
    private var stageIndex: Int {
        controller.progress?.uiSegment ?? lastStageIndex
    }
    /// 已用时 / 预计剩余（不可计算时预计剩余为 `nil`）。
    @State private var elapsedText: String = ""
    @State private var remainingText: String?
    /// 已入库关键帧 / 当前峰值内存。
    @State private var ingestedKeyframes: Int = 0
    @State private var peakMemoryGB: Double = 0
    /// §3.6.2 降级提示（白名单项）。多条时全部展示。
    ///
    /// ⚠️ 当前恒为空 —— 内核尚未提供降级通知通道（C ABI 无对应回调）。
    /// **不伪造**降级提示（那会谎报系统状态）。待 C ABI 增加降级上报后填充。
    @State private var degradations: [Degradation] = []
    /// 用户已关闭终局提示条（只收起 UI，不改内核状态）。
    @State private var dismissedOutcome = false
    /// §5.5 终局提示（R-02..R-13）—— 由 `controller.outcome` 派生。
    private var outcome: RebuildView.Outcome? {
        dismissedOutcome ? nil : Outcome.from(controller.outcome)
    }
    /// 取消中（按钮显示 `正在取消` 并禁用）。
    @State private var cancelling = false
    @State private var showCancelConfirm = false

    public init(sessionID: UUID) { self.sessionID = sessionID }

    public var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: ReconMetric.space5) {
                    stageHeader
                    statsSection
                    if !degradations.isEmpty { degradationSection }
                    cancelButton
                }
                .padding(ReconMetric.space4)
            }
            Hairline()
            // 底部常驻提示。
            SpecText(L10n.t(.rebuildKeepForeground),
                     spec: ReconFont.footnote, color: ReconColor.txtSecondary, maxLines: 2)
                .padding(.horizontal, ReconMetric.space4)
                .padding(.vertical, ReconMetric.space3)
        }
        .background(ReconColor.bg.page)
        .navigationBarBackButtonHidden(true)             // U-NAV-3
        .toolbar {
            ToolbarItem(placement: .topBarLeading) {
                Button { showCancelConfirm = true } label: {
                    HStack(spacing: ReconMetric.space1) {
                        Image(systemName: "chevron.left")
                        SpecText(L10n.t(.rebuildTitle), spec: ReconFont.headline,
                                 color: ReconColor.txtPrimary)
                    }
                }
                .foregroundStyle(ReconColor.txtAccent)
            }
        }
        // §5.6 Q-03：`取消重建？` / `已采集的数据会保留，可以稍后重新重建`
        //                                / `继续重建` / `取消重建`
        .confirmationDialog(L10n.t(.confirmCancelRebuildTitle), isPresented: $showCancelConfirm,
                            titleVisibility: .visible) {
            Button(L10n.t(.btnContinueRebuild), role: .cancel) { }
            Button(L10n.t(.btnCancelRebuild), role: .destructive) {
                // T-22：取消重建经编排器 —— 它会调用 C ABI `recon_cancel`
                // （线程安全，可打断后台重建），并让状态机迁移。
                controller.cancelReconstruction()
                cancelling = true
            }
        } message: {
            Text(L10n.t(.confirmCancelRebuildMsg))
        }
        // 段号保持：内核给出可映射段时记录，供「无段」的中间帧沿用。
        .onChange(of: controller.progress?.uiSegment) { seg in
            if let seg { lastStageIndex = seg }
        }
        .onChange(of: controller.outcome) { _ in
            // 终局后不再显示「正在取消」。
            if case .running = controller.outcome {} else { cancelling = false }
        }
    }

    // --- 阶段标题 + 进度条 + 耗时（§3.6）----------------------------------

    private var stageHeader: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space2) {
            HStack(spacing: ReconMetric.space3) {
                // `阶段 3/14  稠密匹配`
                SpecText(L10n.fill2(.fmtRebuildStage, String(stageIndex),
                                   RebuildStage.name(stageIndex)),
                         spec: ReconFont.title3, color: ReconColor.txtPrimary)
            }
            HStack(spacing: ReconMetric.space3) {
                LinearProgress(fraction: fraction)
                SpecText(ScanSessionModel.percentText(fraction), spec: ReconFont.footnote,
                         color: ReconColor.txtSecondary, mono: true)
            }
            HStack(spacing: ReconMetric.space4) {
                SpecText(elapsedText, spec: ReconFont.footnote,
                         color: ReconColor.txtPrimary, mono: true)
                if let remainingText {
                    SpecText(remainingText, spec: ReconFont.footnote,
                             color: ReconColor.txtPrimary, mono: true)
                }
            }
        }
    }

    // --- 统计区（§3.6：已入库关键帧 / 当前峰值内存）------------------------

    private var statsSection: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space2) {
            SpecText(L10n.fill(.fmtIngestedKeyframes,
                               MonoNumber.group(ingestedKeyframes)),
                     spec: ReconFont.footnote, color: ReconColor.txtSecondary, mono: true)
            SpecText(L10n.fill(.fmtPeakMemory,
                               String(format: "%.1f", peakMemoryGB)),
                     spec: ReconFont.footnote, color: ReconColor.txtSecondary, mono: true)

            // §5.5 终局提示（常驻，可关闭者给关闭按钮）。
            //
            // 结局来源是内核的**真实** `ReconOutcome`（不再由本视图 `@State`
            // 猜）。可关闭者仍给关闭按钮 —— 关闭只收起提示条，不改内核状态。
            if let outcome {
                InlineNotice(outcome.text, level: outcome.level)
                    .padding(.top, ReconMetric.space2)
                if outcome.dismissible {
                    Button(L10n.t(.commonClose)) { dismissedOutcome = true }
                        .font(.system(size: ReconFont.footnote.size))
                        .foregroundStyle(ReconColor.txtAccent)
                        .frame(minWidth: ReconMetric.minTouchTarget,
                               minHeight: ReconMetric.minTouchTarget, alignment: .leading)
                }
            }
        }
    }

    // --- 降级提示（§3.6.2 白名单）-----------------------------------------

    private var degradationSection: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space2) {
            ForEach(degradations, id: \.rawValue) { d in
                SpecText(d.text, spec: ReconFont.footnote,
                         color: ReconColor.txtWarning, maxLines: 2)
            }
        }
    }

    // --- 取消按钮（§3.6：次按钮描边、`txt.error` 文字）--------------------

    private var cancelButton: some View {
        Button {
            showCancelConfirm = true
        } label: {
            Text(cancelling ? L10n.t(.rebuildCancelling) : L10n.t(.rebuildCancel))
                .font(.system(size: ReconFont.headline.size, weight: .semibold))
                .foregroundStyle(ReconColor.txtError)
                .frame(maxWidth: .infinity)
                .frame(height: ReconMetric.controlHeight)
                .overlay(
                    RoundedRectangle(cornerRadius: ReconMetric.radiusButton)
                        .stroke(ReconColor.txtError, lineWidth: 1)   // 描边，非阴影
                )
        }
        .buttonStyle(.plain)
        .disabled(cancelling)
    }
}

// ===========================================================================
// MARK: - §3.6.1 阶段标题取值表（14 段）
// ===========================================================================

/// 重建阶段名（docs/09 §3.6.1）。
///
/// 段号 = 阶段序号 − 1。分支差异（同表）：
/// `BRANCH_MONO` 的段 4 为「无深度数据，跳过深度融合」（占位，立即完成）。
/// 该差异由 `name(_:branch:)` 体现 —— 不在这里写第二套阶段表。
public enum RebuildStage {
    /// 14 段（与 §3.6.1 表逐条对应）。
    public static let count = 14

    private static let names: [String] = [
        L10n.t(.rebuildStage1),   // 1  读取采集数据
        L10n.t(.rebuildStage2),   // 2  特征匹配与几何验证
        L10n.t(.paramsGroupMvs),  // 3  稠密匹配
        L10n.t(.paramsGroupRgbd), // 4  深度融合
        L10n.t(.rebuildStage5),   // 5  全局优化
        L10n.t(.rebuildStage6),   // 6  输入准备
        L10n.t(.rebuildStage7),   // 7  离群点剔除
        L10n.t(.rebuildStage8),   // 8  网格裁剪
        L10n.t(.rebuildStage9),   // 9  非流形边修复
        L10n.t(.rebuildStage10),  // 10 孔洞缝合
        L10n.t(.rebuildStage11),  // 11 法向统一与规整
        L10n.t(.rebuildStage12),  // 12 UV 展开
        L10n.t(.rebuildStage13),  // 13 纹理烘焙
        L10n.t(.rebuildStage14),  // 14 纹理校验与简化
    ]

    /// 该段名是否需要按管线分支替换（仅段 4）。
    public static func isBranchDependent(_ index: Int) -> Bool { index == 4 }

    public static func name(_ index: Int, branch: ReconPipelineBranch = .rgbd) -> String {
        guard index >= 1, index <= count else { return "—" }
        if index == 4, branch == .mono {
            return L10n.t(.rebuildStage4Mono)
        }
        return names[index - 1]
    }
}

/// 管线分支（与 C ABI `recon_pipeline_branch` 对应）。
public enum ReconPipelineBranch {
    case rgbd
    case mono
}

// ===========================================================================
// MARK: - §3.6.2 降级提示（白名单，四个确定取值）
// ===========================================================================

/// 允许出现的降级提示。
///
/// **白名单**：仅这 4 项可出现在重建界面（00 §1.2）。任何涉及精度阈值
/// （`mesh_poisson_depth` / `mvs_*` 等）的降级说明都是违规 —— 本枚举刻意
/// 不提供对应 case，使违规在类型层面无法表达。
public enum Degradation: String, CaseIterable {
    /// 并行度被下调
    case parallelismReduced
    /// 分块策略启用
    case chunkingEnabled
    /// 内存保护触发落盘
    case spillToDisk
    /// 高精度档不可行
    case highPresetInfeasible

    public var text: String {
        switch self {
        case .parallelismReduced:   return L10n.t(.degradeParallelism)
        case .chunkingEnabled:      return L10n.t(.degradeChunking)
        case .spillToDisk:          return L10n.t(.degradeSpill)
        case .highPresetInfeasible: return L10n.t(.degradeHighPreset)
        }
    }
}

// ===========================================================================
// MARK: - §5.5 终局提示
// ===========================================================================

extension RebuildView {
    /// §5.5 R-02..R-13 的呈现态。`dismissible` 对应规格「可否关闭」列。
    public struct Outcome: Equatable {
        public let text: String
        public let level: InlineNotice.Level
        public let dismissible: Bool

        /// R-02 `未获取到有效几何数据`（`RECON_ERR_MESH_EMPTY_INPUT`）
        public static let meshEmptyInput = Outcome(
            text: L10n.t(.rebuildMeshEmptyInput), level: .error, dismissible: true)
        /// R-03 `内存不足` + 建议（`RECON_ERR_MESH_POISSON_OOM`）
        public static let poissonOOM = Outcome(
            text: "\(L10n.t(.rebuildPoissonOom))。\(L10n.t(.rebuildPoissonOomHint))",
            level: .error, dismissible: true)
        /// R-04 `泊松重建未收敛`
        public static let poissonFailed = Outcome(
            text: L10n.t(.rebuildPoissonFailed), level: .error, dismissible: true)
        /// R-05 `拓扑修复失败`
        public static let nonManifoldUnfixed = Outcome(
            text: L10n.t(.rebuildNonmanifoldUnfixed), level: .error, dismissible: true)
        /// R-06 `UV 展开失败`
        public static let uvFailed = Outcome(
            text: L10n.t(.rebuildUvFailed), level: .error, dismissible: true)
        /// R-07 `纹理没有有效覆盖`
        public static let bakeNoCoverage = Outcome(
            text: L10n.t(.rebuildBakeNoCoverage), level: .error, dismissible: true)
        /// R-13 `重建已取消`
        public static let cancelled = Outcome(
            text: L10n.t(.rebuildCancelled), level: .info, dismissible: true)

        /// 由内核真实结局派生提示；`running` / `succeeded` 时无提示。
        ///
        /// ── 这一层为什么必须存在（而不是让视图自己 switch）──────────────────
        ///  `ReconOutcome` 携带 `ReconStatus`（47 项枚举），而 UI 只该关心
        ///  §5.5 那几条**规格文案**。若不在这里收敛，视图就得自己写一遍
        ///  「哪个 status 对应哪条文案」—— 那就成了 `ReconStatusPresentation`
        ///  的第二份实现（本项目铁律：同一逻辑写两遍必然漂移）。
        ///  故此处**只**消费已解析好的 `messageKey`，不重复判定 status。
        ///
        ///  未命中专用文案的错误（`messageKey == nil`）→ 呈现通用失败 +
        ///  真实错误码（U-D-7 不隐藏失败；U-E-2 思路同）。
        public static func from(_ o: ReconOutcome) -> Outcome? {
            switch o {
            case .running, .succeeded:
                return nil
            case .cancelled:
                return .cancelled
            case .failed(let status, let messageKey):
                if let key = messageKey {
                    return Outcome(text: L10n.t(key), level: .error, dismissible: true)
                }
                // 通用失败：附内核原始错误码，不隐藏（供截图进 bug 报告）。
                return Outcome(text: "\(L10n.t(.rebuildGenericFailure)) \(status.rawValue)",
                               level: .error, dismissible: true)
            }
        }
    }
}
