//
//  HomeView.swift
//  Recon — U-01 首页 / 项目列表（iOS）
//
//  契约：docs/09_UI规格.md §3.1（元素表、线框图、空状态）、§2（导航硬约束）、
//       §5.5 R-14（进程被杀后恢复文案）、§1.3/§1.4/§1.5/§1.7。
//

import SwiftUI

public struct HomeView: View {
    @EnvironmentObject private var router: AppRouter
    @EnvironmentObject private var session: ScanSessionModel
    /// 内核编排器：会话列表与能力摘要的**唯一来源**（不再本地各存一份 `@State`）。
    @EnvironmentObject private var controller: ReconSessionController

    /// 会话摘要（供列表渲染）。
    ///
    /// 由 `ReconSessionRecord`（控制器发布的真实记录）映射而来 ——
    /// **不重算**详情/时间戳（那是 `ReconSessionRecord` 的职责，同一逻辑
    /// 写两遍必然漂移，见 MEMORY.md）。
    public struct SessionSummary: Identifiable, Hashable {
        public let id: UUID
        public let name: String
        public let detail: String       // `1.2 MB · GLB · 标准档`
        public let timestamp: String    // `10-30 14:20`
        public let hasResult: Bool      // 决定「继续」文案为 `继续` / `继续扫描`
        public let interrupted: Bool    // R-14：上次扫描被中断，可继续

        init(_ r: ReconSessionRecord) {
            id = r.id
            name = r.name
            detail = r.detailText
            timestamp = r.timestampText
            hasResult = r.hasResult
            interrupted = r.interrupted
        }
    }

    /// 列表数据源：直接读控制器的真实记录（本次运行期内发生过的会话）。
    private var recent: [SessionSummary] { controller.records.map(SessionSummary.init) }

    /// 能力摘要；`nil` 表示探测未完成（§3.1：显示 `正在检测设备能力`）。
    ///
    /// 由「内核能力 + ARKit 能力」现算 —— 与 U-02 同一转换点
    /// （`CapabilityReport.make`），不另写一套判定。
    ///
    /// ⚠️ 判据落在 `controller.isProbing` 而**不是** `capability == nil`
    /// （2026-11-01 修正）：探测**失败**时 `capability` 同样为 nil，若按 nil
    /// 判「未完成」，按钮就永远灰着 —— 用户既进不去、也看不到失败原因。
    /// 探测结束后一律放行，让 U-02 / U-03 呈现真实结果（U-D-7 不隐藏失败）。
    private var capability: CapabilitySummary? {
        let report = CapabilityReport.make(capability: controller.capability,
                                           ar: controller.arCapability)
        guard !report.probing else { return nil }
        return CapabilitySummary(pipelineName: report.pipeline.displayName,
                                 depthTier: report.pipeline.depthTier)
    }

    /// 「开始扫描」是否可用：仅在**探测进行中**禁用（§3.1 状态列）。
    private var canStartScan: Bool { !controller.isProbing }

    /// 全部项目数 = 已知记录数。无持久化层时为 0（按钮隐藏，§3.1）。
    private var allProjectsCount: Int { controller.records.count }

    public init() {}

    // =======================================================================
    // MARK: - §3.1 首页主体
    // =======================================================================

    public var body: some View {
        VStack(spacing: 0) {
            content
            Hairline()                                  // 底部栏上方分隔线（1px separator）
            bottomBar
        }
        .background(ReconColor.bg.page)
        .navigationTitle("")                            // 标题由自定义左栏承载（display 字号）
        .toolbar {
            // LG-2：导航栏是允许使用玻璃的系统级表面之一。
            ToolbarItem(placement: .principal) {
                SpecText(L10n.t(.homeTitle), spec: ReconFont.display, color: ReconColor.txtPrimary)
                    .accessibilityAddTraits(.isHeader)
            }
            ToolbarItem(placement: .topBarTrailing) {
                Button(L10n.t(.homeSettings)) { /* 推入设置页（§2 未列入 U-01..U-10，占位） */ }
                    .font(.system(size: ReconFont.footnote.size))
                    .foregroundStyle(ReconColor.txtAccent)   // 按下态即 accent
            }
        }
    }

    @ViewBuilder
    private var content: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: ReconMetric.space5) {
                capabilityRow
                startScanButton
                recentSection
            }
            .padding(.horizontal, ReconMetric.space4)
            .padding(.vertical, ReconMetric.space4)
        }
    }

    // --- 设备能力行（§3.1）------------------------------------------------

    private var capabilityRow: some View {
        HStack(spacing: ReconMetric.space3) {
            VStack(alignment: .leading, spacing: ReconMetric.space1) {
                if let capability {
                    // 形态：`设备能力  单目+多视图 · 深度等级 0`
                    HStack(spacing: ReconMetric.space2) {
                        SpecText(L10n.t(.homeCapabilityLabel), spec: ReconFont.subhead,
                                 color: ReconColor.txtPrimary)
                        SpecText(capability.pipelineName + " · " +
                                 L10n.fill(.fmtDepthTier, String(capability.depthTier)),
                                 spec: ReconFont.subhead,
                                 color: depthTierColor(capability.depthTier))
                    }
                } else {
                    // 探测未完成（§3.1）
                    SpecText(L10n.t(.homeProbing), spec: ReconFont.subhead,
                             color: ReconColor.txtSecondary)
                }
            }
            Spacer(minLength: ReconMetric.space2)
            Button(L10n.t(.homeDetail)) { router.push(.capability) }
                .font(.system(size: ReconFont.footnote.size))
                .foregroundStyle(ReconColor.txtAccent)
        }
        .padding(.vertical, ReconMetric.space3)
    }

    /// 深度等级 0/1 用 `txt.secondary`；2/3 用 `txt.primary`（§3.1「深度等级行」）。
    private func depthTierColor(_ tier: Int) -> Color {
        tier >= 2 ? ReconColor.txtPrimary : ReconColor.txtSecondary
    }

    // --- 开始扫描（§3.1）--------------------------------------------------

    private var startScanButton: some View {
        // 探测进行中禁用（文案不变）；探测结束后**无论成败**都放行 ——
        // 失败路径由 U-02 / U-03 承载，不能用灰按钮把用户挡在门外。
        PrimaryButton(L10n.t(.homeStartScan), enabled: canStartScan) {
            // 默认模式为 MODE_GUIDED（docs/09 §2）。
            //
            // ⚠️ 必须走 `controller.beginLaunch`（唯一入口），**不能**只改路由。
            //    只 push 不建上下文 = 缺口 U/V/W 的原始形态：界面进去了，
            //    但内核句柄为 nil、能力未探测、ARKit 从不启动。
            //    `beginLaunch` 内部完成 open → 探测 → 申请权限 → 路由推进会由
            //    SessionRouterLink 依状态机新状态触发。
            controller.beginLaunch(guided: true)
            router.pushScan(guided: true)
        }
    }

    // --- 最近项目 / 空状态（§3.1）-----------------------------------------

    @ViewBuilder
    private var recentSection: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space3) {
            if recent.isEmpty {
                // 空状态：`最近项目` 区块整体替换为一行 footnote 文本。
                SpecText(L10n.t(.homeEmptyState),
                         spec: ReconFont.footnote,
                         color: ReconColor.txtSecondary,
                         maxLines: 3)
            } else {
                SpecText(L10n.t(.homeRecent), spec: ReconFont.footnote, color: ReconColor.txtSecondary)
                VStack(spacing: 0) {
                    ForEach(Array(recent.enumerated()), id: \.element.id) { idx, item in
                        if idx > 0 { Hairline() }        // 列表项之间 1px separator
                        sessionRow(item)
                    }
                }
                .background(ReconColor.bg.secondary)     // 分组底色（无阴影 U-X-2）
                .clipShape(RoundedRectangle(cornerRadius: ReconMetric.radiusCard))

                if allProjectsCount > 0 {
                    Button("\(L10n.t(.homeAllProjects)) (\(allProjectsCount))") {
                        /* 推入全部项目列表 */
                    }
                        .font(.system(size: ReconFont.footnote.size))
                        .foregroundStyle(ReconColor.txtAccent)
                }
            }
        }
    }

    private func sessionRow(_ item: SessionSummary) -> some View {
        HStack(alignment: .center, spacing: ReconMetric.space3) {
            VStack(alignment: .leading, spacing: ReconMetric.space1) {
                SpecText(item.name, spec: ReconFont.headline, color: ReconColor.txtPrimary)
                SpecText(item.detail, spec: ReconFont.footnote, color: ReconColor.txtSecondary)
                if item.interrupted {
                    // R-14：进程被杀后恢复。
                    SpecText(L10n.t(.homeInterrupted), spec: ReconFont.footnote,
                             color: ReconColor.txtWarning)
                }
            }
            Spacer(minLength: ReconMetric.space2)
            VStack(alignment: .trailing, spacing: ReconMetric.space1) {
                SpecText(item.timestamp, spec: ReconFont.caption2,
                         color: ReconColor.txtTertiary, mono: true)
                Button(item.hasResult ? L10n.t(.homeContinue) : L10n.t(.homeContinueScan)) {
                    // 已有结果推 U-07，未完成推 U-06。
                    router.push(item.hasResult ? .result(sessionID: item.id)
                                               : .rebuild(sessionID: item.id))
                }
                .font(.system(size: ReconFont.footnote.size))
                .foregroundStyle(ReconColor.txtAccent)
                // §1.7：文字按钮同样要满足 48×48 热区。
                .frame(minWidth: ReconMetric.minTouchTarget, minHeight: ReconMetric.minTouchTarget,
                       alignment: .trailing)
            }
        }
        .padding(ReconMetric.space3)
        .contentShape(Rectangle())
    }

    // --- 底部栏（§3.1：参数 / 导出记录）-----------------------------------

    private var bottomBar: some View {
        HStack {
            Button(L10n.t(.homeParams)) { router.push(.params) }
                .font(.system(size: ReconFont.footnote.size))
                .foregroundStyle(ReconColor.txtAccent)
                .frame(minWidth: ReconMetric.minTouchTarget, minHeight: ReconMetric.minTouchTarget)

            Spacer()

            Button(L10n.t(.homeExportHistory)) { /* 推导出历史 */ }
                .font(.system(size: ReconFont.footnote.size))
                .foregroundStyle(ReconColor.txtAccent)
                .frame(minWidth: ReconMetric.minTouchTarget, minHeight: ReconMetric.minTouchTarget)
                .disabled(true)                          // 无记录时禁用（§3.1）
                .opacity(1)                              // 禁用仅置灰文案，不隐藏
        }
        .padding(.horizontal, ReconMetric.space4)
        .background(ReconColor.bg.page)
    }
}

/// 首页顶部的能力摘要（§3.1 只用到管线名与深度等级两项）。
public struct CapabilitySummary: Hashable {
    public let pipelineName: String   // `单目+多视图` / `彩色+深度融合`
    public let depthTier: Int         // 0..3
    public init(pipelineName: String, depthTier: Int) {
        self.pipelineName = pipelineName
        self.depthTier = depthTier
    }
}
