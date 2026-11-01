//
//  CapabilityView.swift
//  Recon — U-02 能力探测结果页（iOS）
//
//  契约：docs/09_UI规格.md §3.2（线框图、元素表、管线名取值表）、
//       §2（导航）、02 §3.9.4 CAL-2（未标定机型附加提示）。
//
//  管线名与尺度来源文案**必须**取自 §3.2 的「管线名取值表」，由 00 §5.2 决策表
//  驱动，**禁止自由文本**。故本文件用枚举承载该表，而不是拼字符串。
//

import SwiftUI

public struct CapabilityView: View {
    @EnvironmentObject private var router: AppRouter
    /// 内核编排器：能力探测结果的**唯一来源**（不再本地伪造 `report`）。
    @EnvironmentObject private var controller: ReconSessionController

    public init() {}

    /// 报告 = 由内核能力 + ARKit 能力现算（不是一份会过期的本地副本）。
    private var report: CapabilityReport {
        CapabilityReport.make(capability: controller.capability,
                              ar: controller.arCapability)
    }

    public var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: ReconMetric.space5) {
                pipelineSection
                sensorSection
                scaleSection
            }
            .padding(ReconMetric.space4)
        }
        .background(ReconColor.bg.page)
        .navigationTitle(L10n.t(.capabilityTitle))
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarLeading) {
                Button { router.pop() } label: {
                    HStack(spacing: ReconMetric.space1) {
                        Image(systemName: "chevron.left")
                        SpecText(L10n.t(.capabilityTitle), spec: ReconFont.headline,
                                 color: ReconColor.txtPrimary)
                    }
                }
                .foregroundStyle(ReconColor.txtAccent)
            }
        }
        .safeAreaInset(edge: .bottom) { bottomButtons }
    }

    // --- 重建管线（§3.2 第一区块）-----------------------------------------

    private var pipelineSection: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space2) {
            SpecText(L10n.s("重建管线"), spec: ReconFont.title2, color: ReconColor.txtPrimary)

            let p = report.pipeline
            // 管线名（title3）
            SpecText(p.displayName, spec: ReconFont.title3,
                     color: ReconColor.txtPrimary, maxLines: 2)
            // 管线说明（callout txt.secondary）
            SpecText(p.scaleNote, spec: ReconFont.callout,
                     color: ReconColor.txtSecondary, maxLines: 2)
            // 深度等级行：0/1 用 txt.secondary；2/3 用 txt.primary
            SpecText(L10n.fill2(.fmtDepthTier, String(p.depthTier), p.depthNote),
                     spec: ReconFont.footnote,
                     color: p.depthTier >= 2 ? ReconColor.txtPrimary : ReconColor.txtSecondary)

            // 02 §3.9.4 CAL-2：未标定机型附加提示（区块之间，footnote txt.warning）
            if report.isUncalibratedModel {
                SpecText(L10n.s("该机型未标定，精度可能下降"),
                         spec: ReconFont.footnote, color: ReconColor.txtWarning, maxLines: 2)
            }
        }
    }

    // --- 探测到的传感器（§3.2 第二区块）-----------------------------------

    private var sensorSection: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space2) {
            SpecText(L10n.s("探测到的传感器"), spec: ReconFont.title2, color: ReconColor.txtPrimary)

            // 缺失的传感器整行隐藏；「否」用 txt.secondary（§3.2）。
            ForEach(report.sensors, id: \.name) { s in
                HStack(spacing: ReconMetric.space3) {
                    SpecText(s.name, spec: ReconFont.subhead, color: ReconColor.txtPrimary)
                    Spacer(minLength: ReconMetric.space2)
                    SpecText(s.available ? L10n.s("是") : L10n.s("否"), spec: ReconFont.subhead,
                             color: s.available ? ReconColor.txtPrimary : ReconColor.txtSecondary)                    SpecText(s.detail, spec: ReconFont.subhead,
                             color: ReconColor.txtSecondary, mono: true)
                        .frame(minWidth: 96, alignment: .trailing)
                }
                .frame(minHeight: 24)
            }
        }
    }

    // --- 尺度来源（§3.2 第三区块）-----------------------------------------

    private var scaleSection: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space2) {
            SpecText(L10n.s("尺度来源"), spec: ReconFont.title2, color: ReconColor.txtPrimary)
            SpecText(report.pipeline.scaleSourceText,
                     spec: ReconFont.callout, color: ReconColor.txtPrimary, maxLines: 2)
        }
    }

    // --- 底部（§3.2：使用此配置 / 重新探测）-------------------------------

    private var bottomButtons: some View {
        HStack(spacing: ReconMetric.space3) {
            PrimaryButton(L10n.s("使用此配置"), enabled: !report.probing) {
                router.popToHome()
            }
            SecondaryButton(L10n.s("重新探测"), enabled: !report.probing) {
                // 重跑能力探测。
            }
        }
        .padding(.horizontal, ReconMetric.space4)
        .padding(.vertical, ReconMetric.space3)
        .background(ReconColor.bg.page)
    }
}

// ===========================================================================
// MARK: - §3.2 管线名取值表（由 00 §5.2 决策表驱动）
// ===========================================================================

/// 重建管线呈现。四种组合**逐条**取自 §3.2 的取值表。
///
/// 说明：表中 `BRANCH_RGBD` 的三种深度源（`HW_LIDAR` / `HW_TOF` / `ALGO_DEPTH`）
/// 共享同一对比管线名与尺度来源文案，故合并为 `.rgbd(depthTier:)`；
/// 深度等级由深度源决定，用于「深度等级行」的着色与后缀。
public enum CapabilityPipeline: Equatable {
    /// `BRANCH_RGBD` + `HW_LIDAR`
    case rgbdLidar
    /// `BRANCH_RGBD` + `HW_TOF`
    case rgbdToF
    /// `BRANCH_RGBD` + `ALGO_DEPTH`
    case rgbdAlgoDepth
    /// `BRANCH_MONO`
    case monoMVS

    /// 管线名文案（§3.2 表第 3 列）。
    public var displayName: String {
        switch self {
        case .rgbdLidar, .rgbdToF, .rgbdAlgoDepth:
            return L10n.s("彩色 + 深度融合 + 多视图立体")
        case .monoMVS:
            return L10n.s("单目 + 多视图立体（MVS）")
        }
    }

    /// 管线说明文案（§3.2 元素表「管线说明」行）。
    public var scaleNote: String {
        switch self {
        case .rgbdLidar, .rgbdToF, .rgbdAlgoDepth:
            return L10n.s("尺度由深度传感器锚定")
        case .monoMVS:
            return L10n.s("尺度来自参照物标定或手动输入")
        }
    }

    /// 尺度来源文案（§3.2 表第 4 列）。
    public var scaleSourceText: String {
        switch self {
        case .rgbdLidar, .rgbdToF, .rgbdAlgoDepth:
            return L10n.s("深度传感器锚定（可用参照物复核）")
        case .monoMVS:
            return L10n.s("参照物标定 或 手动输入")
        }
    }

    /// 深度等级（0..3）。RGBD 分支按深度源定级。
    public var depthTier: Int {
        switch self {
        case .rgbdLidar:     return 3
        case .rgbdToF:       return 2
        case .rgbdAlgoDepth: return 1
        case .monoMVS:       return 0
        }
    }

    /// 深度等级行的后缀（§3.2 线框图：`深度等级 0 · 无深度传感器`）。
    public var depthNote: String {
        switch self {
        case .rgbdLidar:     return L10n.s("激光雷达")
        case .rgbdToF:       return L10n.t(.capDepthTof)
        case .rgbdAlgoDepth: return L10n.t(.capDepthAlgo)
        case .monoMVS:       return L10n.t(.capDepthNone)
        }
    }
}

// ===========================================================================
// MARK: - 探测报告模型
// ===========================================================================

public struct CapabilityReport {
    public var pipeline: CapabilityPipeline = .monoMVS
    public var sensors: [SensorRow] = []
    public var isUncalibratedModel: Bool = false
    /// 探测进行中（按钮禁用，文案不变 —— §3.2「使用此配置」状态列）。
    public var probing: Bool = false

    public struct SensorRow: Equatable {
        public let name: String      // `相机` / `陀螺仪` / `加速度计` / `激光雷达` / `ARKit 追踪`
        public let available: Bool
        public let detail: String    // `1920×1080 @ 60fps` / `200 Hz` / `worldTracking`

        public init(name: String, available: Bool, detail: String) {
            self.name = name
            self.available = available
            self.detail = detail
        }
    }
}

// ===========================================================================
// MARK: - 探测源 → 报告（唯一转换点）
// ===========================================================================
//
//  为什么把转换放在这里而不是 Controller：转换**只依赖探测数据**，
//  与内核句柄无关，属纯粹的数据整形。放这里可单测（Controller 依赖 C ABI）。

extension CapabilityReport {

    /// 由内核能力 + ARKit 能力合成 U-02 的报告。
    ///
    /// 两个来源的**分工**（不重复采集同一事实）：
    ///  · 管线分支以**内核** `capability.branch` 为准（那是产品级决策）；
    ///  · 传感器可用性以 **ARKit** 探测为准（那是设备级事实，内核看不到）。
    ///
    /// `capability` 为 `nil` 时（未 open）由 ARKit 结果单独合成，`probing = true`。
    public static func make(capability: ReconCapability?,
                            ar: ARCapability?) -> CapabilityReport {
        guard let ar else {
            return CapabilityReport(probing: true)
        }

        // 管线：内核说 RGBD 且 ARKit 有深度 → 取 ARKit 的深度来源细分；
        // 内核说 MONO 或 ARKit 无深度 → MONO。
        let pipeline: CapabilityPipeline
        let branchIsRGBD = capability.map { $0.branch == .rgbd }
            ?? (ar.sceneDepthSupported)
        if branchIsRGBD && ar.sceneDepthSupported {
            // LiDAR 与 ToF 的区分需读 ARWorldTrackingConfiguration 的设备标志；
            // ARCapability 尚未暴露该标志，故**保守**按 LiDAR（与 ARCapability.pipeline 一致），
            // 不猜 ToF。实机验证后再细化。
            pipeline = .rgbdLidar
        } else {
            pipeline = .monoMVS
        }

        // 传感器行（§3.2 第二区块）。**缺失的整行隐藏** —— 故只列真实存在的项。
        //
        // ── 关于「可用性」这一列 ────────────────────────────────────────────
        //  §3.2 线框图的列语义是 `名称  是/否  详情`，其中 `是/否` 是**可用性**列
        //  （由上方 `s.available` 渲染 `msg.common.yes|no`）；`详情` 列只承载
        //  **技术标识**（`1920×1080 @ 60fps` / `200 Hz` / `worldTracking`）或
        //  `—`。因此这里**不得**把「可用/不可用」再写进 detail —— 那会让
        //  不可用行显示成 `激光雷达  否  不可用`（同一事实说两遍，且 detail 里
        //  的中文无对应键、非中文用户会看到中文）。此前正是这个写法触发了 G9。
        var rows: [CapabilityReport.SensorRow] = []
        if let v = ar.videoResolution {
            rows.append(.init(name: L10n.t(.capSensorCamera), available: true,
                              detail: "\(v.w)×\(v.h)"))
        } else {
            // 分辨率需会话运行时读；探测阶段未知 → 显示 `—` 而非编造。
            rows.append(.init(name: L10n.t(.capSensorCamera), available: true, detail: "—"))
        }
        if ar.gyroAvailable {
            rows.append(.init(name: L10n.t(.capSensorGyro), available: true,
                              detail: "200 Hz"))
        }
        if ar.accelerometerAvailable {
            rows.append(.init(name: L10n.t(.capSensorAccel), available: true,
                              detail: "100 Hz"))
        }
        rows.append(.init(name: L10n.t(.capSensorLidar), available: ar.sceneDepthSupported,
                          detail: ar.sceneDepthSupported ? "LiDAR" : "—"))
        rows.append(.init(name: L10n.t(.capSensorArkit), available: ar.worldTrackingSupported,
                          detail: ar.worldTrackingSupported ? "worldTracking" : "—"))

        return CapabilityReport(pipeline: pipeline,
                                sensors: rows,
                                isUncalibratedModel: !ar.sceneDepthSupported,
                                probing: capability == nil)
    }
}
