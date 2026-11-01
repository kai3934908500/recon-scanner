//
//  ReviewingView.swift
//  Recon — U-05b 自检清单界面（iOS）
//
//  契约：
//    · docs/09_UI规格.md §4.1（状态图：`Capturing` —确认→ `Reviewing`
//      「自检清单」—清单通过 → 确认重建→ `Processing`）、§4.2 T-11 / T-16 /
//      T-17 / T-18 / T-19、§4.3（异常态）、§5.6 Q-05、§5.5 R-*
//    · docs/03_扫描模式与关键帧.md §7.1.1（S1..S14 清单项与判据）、
//      §7.1.2（CK-1..CK-7 呈现规则）、§7.3（HO-1..HO-4 确认后移交）
//
//  ── 本视图为什么必须存在（2026-11-01 缺口 AF）────────────────────────────
//
//  规格**三处**都规定 `Reviewing` 是一个**必须存在**的界面态：
//    ① §4.1 状态图把它画成一个节点（`自检清单`），且注明
//       「清单通过 → 确认重建」——有出边、有用户动作；
//    ② §4.2 有**五条**迁移以它为源或目标：T-11（进入）、T-16（severe 未过，
//       确认按钮禁用）、T-17（**确认重建 → `Processing`**）、T-18（继续扫描）、
//       T-19（✕ → `Cancelled`）；
//    ③ docs/03 §7.1.2 给出了完整的呈现规则 CK-1..CK-7（含「确认按钮禁用 /
//       文案`无法确认`」「仍有 N 项未通过…」）与 §5.6 Q-05 的确认框文案。
//
//  但 docs/09 **§3 界面规格没有它的小节**（`### 3.x` 从 3.5 U-05 直接跳到
//  3.6 U-06），§2 界面清单**没有它这一行**，导航结构图也**没有它**。
//  缺口形态与已修的「缺口 AD（`Previewing` 态元素表漏登记）」**完全同构**：
//  仍是「状态迁移表要求存在、界面规格未列」，而不是「两份契约冲突」。
//
//  ── 缺了它的实际后果（这不是纸面问题）──────────────────────────────────
//
//  `ReconSessionController.confirmReconstruct()`（T-17 的唯一实现）**全仓零
//  调用点**：`Capturing → Reviewing` 之后，`Reviewing` 的其余四条出边没有
//  任何按钮。用户点「完成扫描」后**只能**看着一个没有主操作的界面 ——
//  状态机彻底卡死，重建永不开始。这正是 #109 的根因。
//
//  ── 诚实的实现边界（不伪造自检结果）────────────────────────────────────
//
//  docs/03 §7.1.1 的 S1..S14 判据**在内核中零实现**：
//    · `azimuth_coverage` / `elevation_coverage` 全仓**零赋值**
//      （`recon_scan_stats` 只被写入 `frames_captured` / `imu_samples`
//       ——见 `c_api.cpp:220-227`）；
//    · 全部 `anomaly_*` 计数**零赋值**；
//    · `q_total`（质量分中位数）**不存在**（`grep "q_total" core/` 零命中）；
//    · `scan_review_*` 的 8 个阈值键**未注册**（只有 2 个同名异键
//      `scan_review_min_coverage` / `scan_review_max_anomalies`）；
//    · C ABI 21 个导出函数**无任何自检查询接口**。
//
//  故本视图**不伪造一个看起来完整的 14 项清单**（那会在每一项上给出假
//  「✓/!」）。它**只呈现能真实判定的那一项**：S1「关键帧总数」，其数据来源
//  `recon_scan_stats.keyframes` 是**真实可读**的（`orchestrator.cpp:537`
//  在阶段 1 末尾写入）。其余 13 项**显式标注为「未接入」**并说明原因，
//  与 `ResultView` 的 `WireframePlaceholder`（`线框预览尚未接入…`）
//  同一方针 ——「宁可如实告知功能未接入，也不画一个假结果」。
//

import SwiftUI

public struct ReviewingView: View {

    let sessionID: UUID

    @EnvironmentObject private var router: AppRouter
    @EnvironmentObject private var session: ScanSessionModel
    /// 内核编排器：自检数据、确认重建、取消清理的唯一来源。
    @EnvironmentObject private var controller: ReconSessionController

    /// Q-05 的确认框显隐（warn 未通过时点「确认重建」先弹）。
    @State private var showWarnConfirm = false
    /// Q-02 的取消确认框显隐（✕）。
    @State private var showCancelConfirm = false

    public init(sessionID: UUID) { self.sessionID = sessionID }

    // =======================================================================
    // MARK: - 自检清单模型
    // =======================================================================

    /// 清单项（docs/03 §7.1.1 的 S1..S14）。
    ///
    /// `state` 三态，**不**用 `Bool` —— 因为「未接入」与「未通过」是两件
    /// 完全不同的事：前者是**我们测不了**，后者是**测了且不合格**。
    /// 用 `Bool` 会把前者伪装成后者（`false` = 未通过），那正是「伪造」。
    struct ChecklistItem {
        enum State {
            /// 已判定且通过（`✓`）。
            case passed
            /// 已判定且未通过；`detail` 为 §7.1.1 的补充信息。
            case failed(detail: String)
            /// **判据在内核中尚无实现** —— 如实标注，不猜测。
            case notWired
        }
        let id: String
        let title: String
        let severity: Severity
        let state: State

        /// 严重级（§7.1.1「严重级」列）。决定是否阻断确认。
        enum Severity: Int, Comparable {
            /// 阻断（severe）—— §7.1.2 CK-4：确认按钮禁用。
            case severe = 2
            /// 阻断（warn）—— CK-5：可用但先弹确认。
            case warn = 1
            /// 仅警告 —— 不阻断。
            case notice = 0
            static func < (a: Severity, b: Severity) -> Bool { a.rawValue < b.rawValue }
        }
    }

    /// 自检清单（S1..S14，共 14 项；顺序与 §7.1.1 表一致）。
    ///
    /// ── 唯一被真实判定的一项：S1 ──────────────────────────────────────────
    ///  判据 `关键帧总数 ≥ scan_review_min_keyframes（默认 60）`。
    ///  当前值取 `controller.scanStats?.keyframes`，**缺失时标 notWired**
    ///  —— 不把 `nil` 当 0（那会凭空报「有效帧数过少」）。
    ///
    /// ⚠️ 阈值 `60` 按 docs/03 §7.1.1 S1 表值取：该键尚未注册（见下表
    ///    `notWired` 说明），故这里**不能**经 `controller.paramInt` 读 ——
    ///    若那样写，读不到就会静默退化为某个默认值，而 UI 会把它显示成
    ///    「按内核阈值判定」。本视图选择**显式写死规格表值并注明来源**，
    ///    使「阈值目前是规格常量、不是可调参数」这一事实在代码里可见。
    private var checklist: [ChecklistItem] {
        var items: [ChecklistItem] = []

        // S1 关键帧总数（唯一可真实判定项）
        items.append(ChecklistItem(
            id: "S1", title: L10n.t(.reviewItemKeyframes),
            severity: .warn, state: s1State()))

        // S2..S14 —— 判据无实现，**逐条如实标注**（不省略项号，
        // 否则用户会以为规格里只有 1 项自检）。
        items.append(ChecklistItem(id: "S2", title: L10n.t(.reviewItemViewpoints),
                                   severity: .warn, state: .notWired))
        items.append(ChecklistItem(id: "S3", title: L10n.t(.reviewItemAzimuth),
                                   severity: .warn, state: .notWired))
        items.append(ChecklistItem(id: "S4", title: L10n.t(.reviewItemElevation),
                                   severity: .notice, state: .notWired))
        items.append(ChecklistItem(id: "S5", title: L10n.t(.reviewItemBlindSectors),
                                   severity: .notice, state: .notWired))
        items.append(ChecklistItem(id: "S6", title: L10n.t(.reviewItemQualityMedian),
                                   severity: .notice, state: .notWired))
        items.append(ChecklistItem(id: "S7", title: L10n.t(.reviewItemLowQuality),
                                   severity: .notice, state: .notWired))
        items.append(ChecklistItem(id: "S8", title: L10n.t(.reviewItemTexture),
                                   severity: .notice, state: .notWired))
        items.append(ChecklistItem(id: "S9", title: L10n.t(.reviewItemTimeSync),
                                   severity: .notice, state: .notWired))
        items.append(ChecklistItem(id: "S10", title: L10n.t(.reviewItemOnDisk),
                                   severity: .severe, state: .notWired))
        items.append(ChecklistItem(id: "S11", title: L10n.t(.reviewItemDiskWritable),
                                   severity: .severe, state: .notWired))
        items.append(ChecklistItem(id: "S12", title: L10n.t(.reviewItemBottomFace),
                                   severity: .notice, state: .notWired))
        items.append(ChecklistItem(id: "S13", title: L10n.t(.reviewItemSpecular),
                                   severity: .notice, state: .notWired))
        items.append(ChecklistItem(id: "S14", title: L10n.t(.reviewItemPoseConverged),
                                   severity: .warn, state: .notWired))
        return items
    }

    /// S1 的判定。关键帧数来自内核 `recon_scan_stats.keyframes`。
    private func s1State() -> ChecklistItem.State {
        guard let n = controller.scanStats?.keyframes else {
            // 体统计缺失（未开始采集 / 句柄缺失）—— **不当作 0**。
            return .notWired
        }
        // 阈值 = docs/03 §7.1.1 S1 表值（`scan_review_min_keyframes` 默认 60）。
        let threshold: UInt64 = 60
        if n >= threshold { return .passed }
        return .failed(detail: L10n.fill(.reviewFewKeyframes, String(n)))
    }

    /// 未通过的 warn 项数（§7.1.2 CK-5 的 `N`）。
    private var warnFailureCount: Int {
        checklist.filter {
            if case .failed = $0.state { return $0.severity == .warn || $0.severity == .notice }
            return false
        }.count
    }
    /// 是否存在 severe 未通过（CK-4 → 禁用确认）。
    ///
    /// ⚠️ `notWired` **不**计入 severe 阻断：那会把「我们没测」当成
    /// 「测了且不合格」，从而**永久禁用**确认按钮（S10/S11 恰是 severe）
    /// ——用户将永远无法重建。故只统计**已判定**的 severe 失败。
    private var hasSevereFailure: Bool {
        checklist.contains {
            if case .failed = $0.state { return $0.severity == .severe }
            return false
        }
    }
    /// 确认按钮是否可用（CK-4：severe 未通过时禁用）。
    private var canConfirm: Bool {
        !hasSevereFailure && (controller.scanStats?.keyframes ?? 0) > 0
    }

    // =======================================================================
    // MARK: - 主体
    // =======================================================================

    public var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: ReconMetric.space5) {
                    summarySection
                    checklistSection
                    noticesSection
                }
                .padding(ReconMetric.space4)
            }
            Hairline()
            controls
        }
        .background(ReconColor.bg.page)
        .navigationBarBackButtonHidden(true)          // §7.2.3 CU-2 由本页自管
        .toolbar { reviewingToolbar }
        // §5.6 Q-05：`仍有 N 项未通过，确认后将按当前数据重建？`
        //                      / `未通过的项目会写入导出报告`
        //                      / `继续扫描` / `确认重建`
        .confirmationDialog(L10n.fill(.confirmSelfcheckTitle, String(warnFailureCount)),
                            isPresented: $showWarnConfirm, titleVisibility: .visible) {
            Button(L10n.t(.btnContinueScan), role: .cancel) { }
            Button(L10n.t(.confirmRebuild)) { startReconstruction() }
        } message: {
            Text(L10n.t(.confirmSelfcheckMsg))
        }
        // §5.6 Q-02：`取消扫描并删除数据？` / `已采集的 N 帧将被删除`
        //                          / `继续扫描` / `删除并退出`
        .confirmationDialog(L10n.t(.confirmScanCancelTitle),
                            isPresented: $showCancelConfirm, titleVisibility: .visible) {
            Button(L10n.t(.btnContinueScan), role: .cancel) { }
            Button(L10n.t(.btnDeleteAndExit), role: .destructive) {
                controller.cancelFromReview()
                router.popToHome()
            }
        } message: {
            Text(L10n.fill(.fmtFramesDeleted,
                           MonoNumber.group(session.capturedFrames)))
        }
    }

    // --- 导航栏（§7.2.3 CU-1：取消入口为文字按钮）--------------------------

    @ToolbarContentBuilder
    private var reviewingToolbar: some ToolbarContent {
        ToolbarItem(placement: .topBarLeading) {
            Button {
                showCancelConfirm = true
            } label: {
                Image(systemName: "xmark")
                    .font(.system(size: ReconFont.headline.size))
            }
            .foregroundStyle(ReconColor.txtPrimary)
            .frame(width: ReconMetric.minTouchTarget, height: ReconMetric.minTouchTarget)
            .accessibilityLabel(Text(L10n.t(.a11yScanEnd)))
        }
        ToolbarItem(placement: .principal) {
            SpecText(L10n.t(.reviewTitle), spec: ReconFont.headline,
                     color: ReconColor.txtPrimary)
        }
    }

    // --- 汇总区 ------------------------------------------------------------

    private var summarySection: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space2) {
            SpecText(L10n.t(.reviewTitle), spec: ReconFont.title3,
                     color: ReconColor.txtPrimary)
            if let s = controller.scanStats {
                SpecText(L10n.fill(.fmtCapturedFrames,
                                   ScanSessionModel.frameCountText(Int(s.framesCaptured))),
                         spec: ReconFont.footnote, color: ReconColor.txtSecondary, mono: true)
                SpecText(L10n.fill(.fmtIngestedKeyframes,
                                   MonoNumber.group(Int(s.keyframes))),
                         spec: ReconFont.footnote, color: ReconColor.txtSecondary, mono: true)
            }
        }
    }

    // --- 清单区（§7.1.2 CK-1 / CK-2 / CK-3 / CK-6）------------------------

    /// 清单呈现。
    ///
    /// CK-2「最多 6 行」的实现取舍：**未通过项优先**（按严重级降序），
    /// 其后是未接入项，最后才是通过项。「全部通过时只显示『检查通过』」
    /// 在本实现中**不可达**（14 项里 13 项未接入），故不为其留分支。
    private var checklistSection: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space2) {
            // 排序：failed(severe) > failed(warn) > failed(notice) > notWired > passed
            ForEach(sortedForDisplay, id: \.id) { item in
                HStack(alignment: .top, spacing: ReconMetric.space2) {
                    SpecText(marker(for: item), spec: ReconFont.body,
                             color: color(for: item), mono: true)
                    VStack(alignment: .leading, spacing: 2) {
                        SpecText(item.title, spec: ReconFont.body,
                                 color: ReconColor.txtPrimary, maxLines: 2)
                        if case .failed(let detail) = item.state {
                            SpecText(detail, spec: ReconFont.footnote,
                                     color: ReconColor.txtSecondary, maxLines: 2)
                        }
                    }
                }
            }
        }
    }

    /// 展示顺序（CK-2 精神：把「需要用户注意的」放前面）。
    private var sortedForDisplay: [ChecklistItem] {
        checklist.sorted { a, b in rank(a) > rank(b) }
    }
    private func rank(_ i: ChecklistItem) -> Int {
        switch i.state {
        case .failed:   return 100 + i.severity.rawValue
        case .notWired: return 50
        case .passed:   return 10
        }
    }
    /// CK-1：`✓ 项目名` / `! 项目名`；未接入项用 `—` 并附说明。
    private func marker(for i: ChecklistItem) -> String {
        switch i.state {
        case .passed:   return "✓"
        case .failed:   return "!"
        case .notWired: return "—"
        }
    }
    private func color(for i: ChecklistItem) -> Color {
        switch i.state {
        case .passed:   return ReconColor.txtSecondary
        case .failed:   return i.severity == .severe
                            ? ReconColor.txtError : ReconColor.txtWarning
        case .notWired: return ReconColor.txtSecondary
        }
    }

    // --- 说明区（诚实的未接入披露）-----------------------------------------

    private var noticesSection: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space2) {
            InlineNotice(L10n.t(.reviewNotWiredNotice), level: .info)
                .accessibilityIdentifier("review.notWired")
        }
    }

    // --- 底部控制（§7.1.2 CK-4 / CK-5）------------------------------------

    private var controls: some View {
        HStack(spacing: ReconMetric.space3) {
            // CK-5：`继续扫描` → T-18（回到 Capturing，**不清空已采帧**）。
            SecondaryButton(L10n.t(.btnContinueScan)) {
                controller.continueScanFromReview()
                router.pop()
            }
            // CK-4：severe 未通过时禁用，文案 `无法确认`。
            PrimaryButton(hasSevereFailure ? L10n.t(.reviewCannotConfirm)
                                           : L10n.t(.confirmRebuild),
                          enabled: canConfirm) {
                if warnFailureCount > 0 {
                    showWarnConfirm = true     // CK-5：先弹纯文字确认
                } else {
                    startReconstruction()
                }
            }
        }
        .padding(.horizontal, ReconMetric.space4)
        .padding(.vertical, ReconMetric.space3)
        .background(ReconColor.bg.page)
    }

    /// T-17：确认重建 → `Processing`，推入 U-06。
    private func startReconstruction() {
        controller.confirmReconstruct()
        router.replaceTop(with: .rebuild(sessionID: sessionID))
    }
}
