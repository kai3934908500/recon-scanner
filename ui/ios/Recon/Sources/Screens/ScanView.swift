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
    ///
    /// ⚠️ **当前恒为 `nil`（零写入点）** —— 这不是遗漏，而是**数据源尚不存在**。
    /// 该文案的来源是 docs/03 §4.2.1（方向文字表）× §4.2.2（偏差档位）的拼接，
    /// 而档位判据需要「物体包围盒中心 `C`（世界系）」算出的方位角/俯仰角/距离
    /// （03 §4.3.1）。`C` 在**采集期尚未可知** —— 它要靠重建（或用点云估计）
    /// 才能确定。故此处不填任何近似值：§3.4 的引导文字若拿不到真实角度就显示，
    /// 就是**伪造引导**（会真实误导用户移动方向）。
    /// 补齐路径：由内核提供「容器中心估计」（当前 C ABI 无此出口）。
    @State private var guidanceText: String?
    /// 当前采集期异常文案（§5.2）；`nil` 表示无异常。
    ///
    /// ⚠️ **当前恒为 `nil`（零写入点）** —— 同上，**数据源尚不存在**。
    /// §5.2 的 M1..M17 不是「有什么显示什么」，而是由 docs/02 §4.8 的
    /// **优先级引擎**裁决：D1 > D2 > … > D6 取当前最高优先级项（T-4）、
    /// 全局最小间隔 1200 ms、同 `(检测项, 级别)` 60 s 内不重复（S-3 / S-4）。
    /// 该引擎在 `core/` 中**零实现**（`grep "priority" core/src/` 无命中），
    /// C ABI 亦无上报通道。故此处**不自造一个「按帧随机挑一条」的伪引擎**
    /// —— 那会绕开 1200 ms 间隔与 60 s 去重，产生规格明确禁止的提示轰炸。
    /// 补齐路径：优先级引擎（属 §5.2 的判据实现）。
    @State private var noticeText: String?

    /// 当前是否处于 `Previewing`（预览不采帧，等用户点「开始」）。
    ///
    /// ── 为什么按状态机状态分流，而不是另设一个布尔量 ──────────────────────
    ///
    /// 「现在该显示「开始」还是「暂停/完成扫描」」是**状态机状态的函数**，
    /// 不是独立事实。若在这里自己维护一个 `isPreviewing` 布尔量，它就必须
    /// 与 `session.state` 保持同步 —— 而同步点散布在启动、暂停、结束、失败
    /// 各条路径上，漏一处就会出现「状态已是 Capturing 但界面还在等开始」
    /// 或反之（与 T-14 类问题同构）。故这里**只读状态机**，不新增状态。
    ///
    /// §4.1 状态图：`Previewing`（预览不采帧；模式选择、放置物体）
    ///           → 用户点「开始」→ `Capturing`（采帧 / 门限 / 关键帧写入）。
    ///
    /// ⚠️ 两个渲染点（`coverageHeader` / `controls`）都必须走**同一个**三分支
    /// 结构（`previewing` / `capturing` / 其余）。用 `if isPreviewing { } else { }`
    /// 会把「权限申请中」「失败」等状态**误并入 `capturing` 分支** ——
    /// 那会在用户一帧都没采时显示「完成扫描」与「覆盖方位 0%」。
    private var isPreviewing: Bool { session.state == .previewing }
    /// 是否已开始采集（`Capturing`，含 `paused` 变体 —— T-14 只置标志不改状态）。
    private var isCapturing: Bool { session.state == .capturing }

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
            if controller.cameraPermissionDenied {
                // §5.7 P-01 + P-02：相机被拒是**可恢复**的（用户去设置里打开即可），
                // 故必须给出原因与唯一出口，而不是让界面静默停住。
                //
                // 为什么提示条 + 按钮都在本页而非弹窗：P-01 的展示位置规格为
                // 「居中提示区」，且权限状态会在用户从设置页返回后被重查
                // （`ReconSessionController.cameraPermissionDenied` 的两个写入
                // 时机之一），本页常驻才能立刻反映变化。
                VStack(spacing: ReconMetric.space2) {
                    InlineNotice(L10n.t(.permCameraDenied), level: .error)
                    SecondaryButton(L10n.t(.permGoSettings)) {
                        // 经控制器持有的**同一个**权限协调器打开设置
                        // （`PermissionCoordinator()` 每次新建实例会与
                        //  「权限这条链只有一个发起者」的前提相抵触）。
                        controller.openAppSettings()
                    }
                }
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
            if isPreviewing {
                // §5.3 G-01：进入 `Previewing` 时提示放置物体（预览上方）。
                // 此时尚无覆盖数据，若仍走下面的分支会显示「覆盖方位 0%」
                // —— 那是**采集阶段**的读数，在预览阶段是没有意义的数字。
                SpecText(L10n.t(.scanCenterObject), spec: ReconFont.footnote,
                         color: ReconColor.txtPrimary, maxLines: 2)
            } else if isCapturing {
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
            }
            // 其余状态（`CapabilityProbe` / `RequestingPermission` / 终态）不显示
            // 覆盖方位或引导文字 —— 两者都是采集期读数，此时均无真实值可显示。
            Spacer(minLength: 0)
        }
        .padding(.horizontal, ReconMetric.space4)
        .padding(.vertical, ReconMetric.space2)
    }

    // --- 预览主体（§1.4：文字一律放 scrim 上，纯色遮罩，禁止模糊）---------

    private var previewArea: some View {
        ZStack {
            // 相机实时预览层 —— 与采集层**共用同一个** `ARSession`
            // （`controller.arSession`，实现见 platform/ios/Recon/ARCameraPreview.swift）。
            //
            // 2026-11-02：此前这里是一块 `Color.black.aspectRatio(4.0/3.0, ...)`
            // 占位（注释「此处留出接口，不伪造画面」），真机表现是进扫描页**只有
            // 黑屏** —— 相机一直在采，但没有任何视图渲染它。
            //
            // ⚠️ 比例的来源已改为 `controller.previewAspectRatio`（读采集格式
            // 的实际值），**不再**硬编码 4:3：契约 docs/08 声明的是 1920×1080
            // （16:9），写死 4:3 会让画面被拉伸或 letterbox 与真实画面不符。
            // ARCameraPreview 自身用 `.resizeAspect` 保证画面不变形，
            // 比例只决定**容器**的形状（§1.4：`bg.preview` 承载 letterbox 底色）。
            ARCameraPreview(session: controller.arSession)
                .aspectRatio(controller.previewAspectRatio, contentMode: .fit)

            if controller.arTrackingUnavailable {
                // §3.3：AR 不可用时上方叠加一行提示。
                //
                // 读控制器而非本视图 `@State`：该事实有**两个来源**
                // （探测期的 `ARCapability.worldTrackingSupported` 与运行期的
                // `ARCamera.TrackingState.notAvailable`），后者由 ARKit 串行
                // 队列上的回调提供 —— 视图的 `@State` 与那条队列之间没有通路，
                // 故留在视图侧就永远只有探测期那一个来源（且此前连那一个都
                // 没接，是彻底的死状态）。详见控制器内该属性的注释。
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
        Group {
            if isPreviewing {
                // §4.1 / §4.2 T-09：`Previewing` 态的主操作是「开始」——
                // 它是**进入 `Capturing` 的唯一事件**（docs/01 A-05 同）。
                // 此态下不显示「暂停 / 完成扫描」：还没开始采帧，暂停无对象，
                // 完成也无帧可提交（`canFinishScan` 恒 false，显示即恒灰）。
                PrimaryButton(L10n.t(.scanStart)) {
                    // ⚠️ 必须经编排器 `beginCapturing()`：它内部完成
                    //    `session.send(.userStart)` → 恢复帧投递 →
                    //    `recon_start_session` → 启动统计轮询。
                    //    只改本地状态会让相机继续停着（零帧入内核）。
                    controller.beginCapturing()
                }
            } else if isCapturing {
                // `Capturing` 含 `paused` 变体（T-14 只置 paused 标志，状态不变）。
                capturingControls
            }
            // 其余状态（`CapabilityProbe` / `RequestingPermission` / 终态）：
            // **刻意留空**，而不是回退到 `capturingControls` ——
            // 权限申请期间若显示「完成扫描」，用户会在**一帧都没采**时看到一个
            // 可点的按钮，点了就把空会话推进 U-06。同理「暂停」也无对象可暂停。
            // 规格 §3.3/§3.4 只定义了 `Capturing` 态的元素，未定义本阶段的控制条，
            // 故此处**不自造文案、不显示按钮**（符合「不伪造 UI」）。
            // 失败态的用户指引由上方 `cameraPermissionDenied` 提示条 + P-02
            // 「去设置」承担，不依赖本控制条。
        }
        .padding(.horizontal, ReconMetric.space4)
        .padding(.vertical, ReconMetric.space3)
        .background(ReconColor.bg.page)
    }

    /// `Capturing`（含 `paused` 变体）态的底部控制条（§3.3 / §3.4 元素表）。
    private var capturingControls: some View {
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
                // 经编排器结束采集（停帧 + 冻结统计 + 通知内核）。
                controller.finishCapture()
                // ⚠️ 进的是 **`Reviewing`（自检清单）**，不是 U-06 —— 2026-11-01 修正。
                //
                // 原实现直接 `replaceTop(.rebuild)`，等于**跳过了 `Reviewing`**：
                //   ① §4.2 T-11 明确 `FinishScan` 的落点是 `Reviewing`（`Processing`
                //      要等 T-17 的 `UserConfirmReconstruct` 才到达）；
                //   ② docs/03 §7.1.2 CK-1..CK-7 规定的清单页从此**永不出现**；
                //   ③ `confirmReconstruct()`（T-17 唯一实现）此前**零调用点** ——
                //      跳级看起来"能跑到底"，实际是把状态机的必经节点挖掉了。
                // 传**真实**会话 ID（此前是 `UUID()` —— 每次求值都不同，
                // 且与内核里的 `session_id` 无关，U-06/U-07 拿不到本次会话）。
                // `currentSessionID` 为 nil 说明从未 `open()` 成功 —— 此时
                // 不推入下一界面（没有会话可展示），留在本页由异常提示呈现。
                if let id = controller.currentSessionID {
                    router.replaceTop(with: .reviewing(sessionID: id))
                }
            }
        }
    }
}
