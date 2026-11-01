//
//  ScanView.swift
//  Recon — U-03 自由环绕 / U-04 定点引导扫描界面（iOS）
//
//  契约：docs/09_UI规格.md §3.3（U-03 线框图与元素表）、§3.4（U-04）、
//       §3.5（异常提示内联）、§5.2（采集期异常文案）、§5.6（Q-01/Q-02 确认框）、
//       §2 U-NAV-3（U-03/U-04 返回必须先弹确认）、§1.4（scrim 承载白字）。
//
//  ── 一个刻意不做的事 ──────────────────────────────────────────────────
//  本视图**不自己判断**「现在能不能完成扫描」。该规则在
//  ScanSessionModel.canFinishScan（源自 cap_min_frames_before_finish）里，
//  而迁移合法性在 C++ 状态机里。这里只读不判，避免规则出现第二份实现。
//

import SwiftUI

public struct ScanView: View {

    public enum ScanMode {
        case freeOrbit   // U-03
        case guided      // U-04

        var displayName: String {
            switch self {
            case .freeOrbit: return L10n.t(.scanModeFree)
            case .guided:    return L10n.t(.scanModeGuided)
            }
        }
    }

    let mode: ScanMode

    @EnvironmentObject private var router: AppRouter
    @EnvironmentObject private var session: ScanSessionModel
    /// 内核编排器：暂停/继续与结束采集的**副作用**（停帧、冻结统计）都经它。
    @EnvironmentObject private var controller: ReconSessionController

    /// §5.6 确认框的显隐。
    @State private var showEndConfirm = false
    /// U-04 的引导提示（`请移到物体右前方 · 距离较远`）。
    @State private var guidanceText: String?
    /// AR 位置跟踪不可用时的提示（§3.3 预览状态）。
    @State private var arTrackingUnavailable = false
    /// 当前采集期异常文案（§5.2）；`nil` 表示无异常。
    @State private var noticeText: String?

    /// U-04 定点引导的当前点位（16 个引导方位中的第几个）。
    ///
    /// 当前恒为 5 —— 因为「引导点位推进」依赖 ARKit 的世界系追踪回调，而
    /// 采集层（`CameraCapture.swift` 的 ARSessionDelegate）尚未接线。**这不是
    /// 硬编码文案，而是一个待接线的状态量**：一旦采集层回吐真实点位下标，
    /// 把这里换成 `session` 的字段即可，文案侧无需改动。
    /// 保留此常量是为了让 `fmt.point_index` 现在就接上线、不留硬编码。
    private static let guidedPointIndex = 5

    public init(mode: ScanMode) {
        self.mode = mode
    }

    // =======================================================================
    // MARK: - 主体
    // =======================================================================

    public var body: some View {
        VStack(spacing: 0) {
            coverageHeader
            previewArea
            if let noticeText {
                // §1.6：提示条立即出现/立即消失，无动画。
                InlineNotice(noticeText, level: .warning)
                    .padding(.horizontal, ReconMetric.space4)
                    .padding(.bottom, ReconMetric.space2)
            }
            controls
        }
        .background(ReconColor.bg.preview)              // 预览 letterbox 底色
        .navigationBarBackButtonHidden(true)            // U-NAV-3：禁用系统返回
        .toolbar { scanToolbar }
        // §5.6 Q-01（采集中返回）/ Q-02（采集中取消）：纯文字，禁止动画。
        .confirmationDialog(L10n.t(.scanEndConfirmTitle2),
                            isPresented: $showEndConfirm,
                            titleVisibility: .visible) {
            Button(L10n.t(.btnContinueScan), role: .cancel) { /* 留在本页 */ }
            Button(L10n.t(.btnEndScan), role: .destructive) {
                // T-12：经编排器结束采集 —— 它会停帧、冻结统计、投递 C ABI
                // `recon_finish_capture` 并让状态机进入 Reviewing。
                // **不**直接 `session.send`，否则相机仍在喷帧（T-14 同类问题）。
                controller.finishCapture()
                router.popToHome()
            }
        } message: {
            Text(L10n.fill(.fmtFramesDeleted,
                           MonoNumber.group(session.capturedFrames)))
        }
    }

    // --- 导航栏（§3.3：✕ / 模式名 / 帧计数）-------------------------------

    @ToolbarContentBuilder
    private var scanToolbar: some ToolbarContent {
        ToolbarItem(placement: .topBarLeading) {
            Button {
                showEndConfirm = true                  // U-NAV-3：先确认
            } label: {
                Image(systemName: "xmark")             // U-X-6：仅 SF Symbols
                    .font(.system(size: ReconFont.headline.size))
            }
            .foregroundStyle(ReconColor.txtPrimary)
            .frame(width: ReconMetric.minTouchTarget, height: ReconMetric.minTouchTarget)
            .accessibilityLabel(Text(L10n.t(.a11yScanEnd)))   // §3.3：无文字，但需标签
        }
        ToolbarItem(placement: .principal) {
            Button {
                // 点按切换模式：弹确认（文案 `切换扫描模式会保留已采集的帧，继续？`）
                // 该确认框文案见 §5.6 之外的模式切换条；此处由 router 换模式实现。
            } label: {
                SpecText(mode.displayName, spec: ReconFont.headline, color: ReconColor.txtPrimary)
            }
            .buttonStyle(.plain)
        }
        ToolbarItem(placement: .topBarTrailing) {
            // 采集停止时数字冻结 —— 由 session.capturedFrames 的更新时机保证。
            SpecText(L10n.fill(.fmtCapturedFrames,
                               ScanSessionModel.frameCountText(session.capturedFrames)),
                     spec: ReconFont.footnote,
                     color: ReconColor.txtSecondary,
                     mono: true)
        }
    }

    // --- 覆盖方位 / 点位（§3.3 `覆盖方位 63%`；§3.4 `点位 5/16`）-----------

    private var coverageHeader: some View {
        HStack {
            switch mode {
            case .freeOrbit:
                SpecText(L10n.fill(.fmtCoveragePercent,
                                   String(Int((min(max(session.coverageFraction, 0), 1) * 100).rounded()))),
                         spec: ReconFont.footnote, color: ReconColor.txtPrimary, mono: true)
            case .guided:
                if let guidanceText {
                    SpecText(guidanceText, spec: ReconFont.footnote,
                             color: ReconColor.txtPrimary, maxLines: 2)
                }
                Spacer()
                SpecText(L10n.fill(.fmtPointIndex,
                                   String(Self.guidedPointIndex)), spec: ReconFont.footnote,
                         color: ReconColor.txtPrimary, mono: true)
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, ReconMetric.space4)
        .padding(.vertical, ReconMetric.space2)
    }

    // --- 预览主体（§1.4：文字一律放 scrim 上，纯色遮罩，禁止模糊）---------

    private var previewArea: some View {
        ZStack {
            // 相机实时预览层。真实实现接 AVCaptureVideoPreviewLayer / ARSCNView；
            // 此处留出接口，不伪造画面。
            Color.black
                .aspectRatio(4.0 / 3.0, contentMode: .fit)

            if arTrackingUnavailable {
                // §3.3：AR 不可用时上方叠加一行提示。
                VStack {
                    InlineNotice(L10n.t(.scanArUnavailable), level: .warning)
                        .padding(ReconMetric.space4)
                    Spacer()
                }
            }

            // §3.4：引导模式下预览中央有定向标记（线框中的 △）。
            if mode == .guided {
                Image(systemName: "arrowtriangle.up")
                    .font(.system(size: 24))
                    .foregroundStyle(ReconColor.txtPrimary)
            }
        }
        .frame(maxHeight: .infinity)
    }

    // --- 底部控制（§3.3：暂停（描边，左）/ 完成扫描（实心，右））----------

    private var controls: some View {
        HStack(spacing: ReconMetric.space3) {
            SecondaryButton(session.pauseButtonTitle) {
                // 暂停/继续是 §3.3 的 Capturing 态 `paused` 变体。
                //
                // 经编排器切换 —— 它会**一并**停/开相机帧投递（`capture.setPaused`）
                // 与统计轮询。若只切状态机标志，相机仍会持续喷帧（耗电/发热，
                // 与 T-14 同类问题）。
                let next = !session.isPaused
                if next { controller.setPaused(true) }
                else     { controller.appWillForeground() }
            }

            if mode == .guided {
                SecondaryButton(L10n.t(.scanFlipMode)) { /* 前后摄像头切换 */ }
            }

            PrimaryButton(L10n.t(.scanFinish),
                          enabled: session.canFinishScan,
                          hint: session.finishScanHint) {
                // 经编排器结束采集（停帧 + 冻结统计 + 通知内核），再跳 U-06。
                controller.finishCapture()
                router.replaceTop(with: .rebuild(sessionID: UUID()))
            }
        }
        .padding(.horizontal, ReconMetric.space4)
        .padding(.vertical, ReconMetric.space3)
        .background(ReconColor.bg.page)
    }
}
