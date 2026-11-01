//
//  ResultView.swift
//  Recon — U-07 重建结果预览（iOS）
//
//  契约：docs/09_UI规格.md §3.7（线框图、元素表、U-R-1..U-R-4 预览技术约束）、
//       §5.5 R-08/R-09/R-10（拓扑/纹理警告）、§1.4（数值等宽右对齐千分位）。
//
//  ── U-R-1 / U-R-3 的实现边界（诚实声明）────────────────────────────────
//  预览只做**线框**渲染，不做材质/光照/阴影/IBL（U-R-1）；旋转**无自动旋转、
//  无惯性、无阻尼**（U-R-3）。这两条是**渲染层**的约束，本文件只负责声明入口
//  与保证不加动画修饰符；真正的线框渲染需接入 Metal（shaders/metal/，尚未实现）。
//  因此预览区当前为**未接入状态**，按 U-D-7 必须显式告知用户，不得静默留白。
//

import SwiftUI

public struct ResultView: View {
    let sessionID: UUID

    @EnvironmentObject private var router: AppRouter
    /// 内核编排器：结果摘要与网格统计的**唯一来源**。
    @EnvironmentObject private var controller: ReconSessionController

    /// 网格统计（来自 `controller.result`，即 `recon_get_mesh_info`）。
    ///
    /// 由 `ReconResultSummary` 转换 —— **不重算尺寸/体积**（那是摘要类型的
    /// 职责，见 `ReconResultSummary.sortedExtentsMM` / `volumeCM3`）。
    private var info: MeshInfo? {
        controller.result.map(MeshInfo.init)
    }
    /// R-08 / R-09 / R-10 警告（由真实结果字段派生，非臆测）。
    private var warnings: [String] {
        guard let r = controller.result else { return [] }
        var out: [String] = []
        if r.nonManifoldEdges > 0 { out.append(L10n.t(.rebuildNonmanifoldUnfixed)) }
        if r.holeCount > 0 { out.append(L10n.t(.rebuildHoleUnfilled)) }
        if r.degraded { out.append(L10n.t(.rebuildSimplifyReverted)) }
        return out
    }

    public init(sessionID: UUID) { self.sessionID = sessionID }

    public var body: some View {
        VStack(spacing: 0) {
            previewArea
            Hairline()
            ScrollView {
                VStack(alignment: .leading, spacing: ReconMetric.space5) {
                    if let info {
                        topologySection(info)
                        dimensionSection(info)
                        if info.hasTexture { textureSection(info) }
                        scaleSection(info)
                    }
                }
                .padding(ReconMetric.space4)
            }
            Hairline()
            bottomBar
        }
        .background(ReconColor.bg.page)
        .toolbar {
            ToolbarItem(placement: .principal) {
                SpecText(sessionName, spec: ReconFont.headline, color: ReconColor.txtPrimary)
            }
            ToolbarItem(placement: .topBarTrailing) {
                Button(L10n.t(.exportTitle)) { router.push(.export(sessionID: sessionID)) }
                    .font(.system(size: ReconFont.headline.size, weight: .semibold))
                    .foregroundStyle(ReconColor.txtAccent)
                    .disabled(!(info?.exportable ?? false))   // 重建未成功则禁用
            }
        }
    }

    /// 会话名：取控制器记录中该 id 的展示名；找不到时回退到占位（数据，非 UI 文案）。
    ///
    /// 之前这里写死 `陶瓷杯` —— 那是**数据**却硬编码在视图里，属伪造。
    /// 现在从真实记录取；记录不存在（例如深链进入）时显示 `—` 而非编造名字。
    private var sessionName: String {
        controller.records.first(where: { $0.id == sessionID })?.name ?? "—"
    }

    // --- 线框预览（U-R-1..U-R-4）------------------------------------------

    private var previewArea: some View {
        ZStack {
            Color.black
            if info == nil {
                // §3.7：加载中显示一行 footnote，**禁止**骨架屏闪烁（U-X-8/U-X-4）。
                SpecText(L10n.t(.resultPreviewPending), spec: ReconFont.footnote,
                         color: ReconColor.txtSecondary)
            } else {
                // 线框渲染入口（Metal）。U-R-3：不加任何动画修饰符。
                WireframePlaceholder()
            }
        }
        .frame(height: 260)
        // 单指旋转 / 双指缩放由渲染层处理；此处不添加惯性或阻尼动画。
        .gesture(DragGesture())      // 占位：真实旋转交给 Metal 层，无惯性
    }

    // --- 拓扑（§3.7 元素表）-----------------------------------------------

    private func topologySection(_ info: MeshInfo) -> some View {
        VStack(alignment: .leading, spacing: ReconMetric.space2) {
            SpecText(L10n.t(.resultTopo), spec: ReconFont.title2, color: ReconColor.txtPrimary)
            StatRow(L10n.t(.resultTriangles), MonoNumber.group(info.triangles))
            StatRow(L10n.t(.resultVertices), MonoNumber.group(info.vertices))
            // 非流形边 > 0 用 txt.error；孔洞 > 0 用 txt.warning（§3.7）。
            StatRow(L10n.t(.resultNonManifoldEdges), "\(info.nonManifoldEdges)",
                    valueColor: info.nonManifoldEdges > 0 ? ReconColor.txtError : ReconColor.txtPrimary)
            StatRow(L10n.t(.resultHoles), "\(info.holes)",
                    valueColor: info.holes > 0 ? ReconColor.txtWarning : ReconColor.txtPrimary)
            StatRow(L10n.t(.resultBoundaryLoops), "\(info.boundaryLoops)")

            ForEach(warnings, id: \.self) { w in
                SpecText(w, spec: ReconFont.footnote,
                         color: ReconColor.txtWarning, maxLines: 3)
            }
        }
    }

    // --- 尺寸（§3.7）------------------------------------------------------

    private func dimensionSection(_ info: MeshInfo) -> some View {
        VStack(alignment: .leading, spacing: ReconMetric.space2) {
            SpecText(L10n.t(.resultDimension), spec: ReconFont.title2, color: ReconColor.txtPrimary)
            HStack(spacing: ReconMetric.space4) {
                DimCell(L10n.t(.resultLength), info.lengthMM)
                DimCell(L10n.t(.resultWidth), info.widthMM)
                DimCell(L10n.t(.resultHeight), info.heightMM)
            }
            StatRow(L10n.t(.resultVolume), String(format: "%.2f", info.volumeCM3), unit: "cm³")
        }
    }

    // --- 纹理（§3.7：无纹理模型整块隐藏）----------------------------------

    private func textureSection(_ info: MeshInfo) -> some View {
        VStack(alignment: .leading, spacing: ReconMetric.space2) {
            SpecText(L10n.t(.resultTexture), spec: ReconFont.title2, color: ReconColor.txtPrimary)
            HStack(spacing: ReconMetric.space3) {
                SpecText(L10n.fill2(.fmtAtlasSize, "\(info.atlasWidth)", "\(info.atlasHeight)"),
                         spec: ReconFont.subhead, color: ReconColor.txtPrimary, mono: true)
                // 覆盖率低于阈值用 txt.warning。
                SpecText(L10n.fill(.fmtCoverageRatio,
                                   ScanSessionModel.percentText(info.textureCoverage)),
                         spec: ReconFont.subhead,
                         color: info.textureCoverage < MeshInfo.coverageWarnThreshold
                                ? ReconColor.txtWarning : ReconColor.txtPrimary,
                         mono: true)
            }
        }
    }

    // --- 尺度（§3.7：可点按推 U-09）---------------------------------------

    private func scaleSection(_ info: MeshInfo) -> some View {
        Button {
            router.push(.calibration)
        } label: {
            HStack(spacing: ReconMetric.space2) {
                SpecText(L10n.t(.resultScale), spec: ReconFont.subhead, color: ReconColor.txtSecondary)
                SpecText(info.scaleDescription, spec: ReconFont.subhead,
                         color: info.scaleSource == .none ? ReconColor.txtWarning
                                                          : ReconColor.txtPrimary)
                Spacer(minLength: 0)
            }
            .frame(minHeight: ReconMetric.minTouchTarget, alignment: .leading)
        }
        .buttonStyle(.plain)
    }

    // --- 底部（§3.7：重新采集 / 导出模型）---------------------------------

    private var bottomBar: some View {
        HStack(spacing: ReconMetric.space3) {
            SecondaryButton(L10n.t(.resultRecapture)) {
                router.pushScan(guided: true)   // 新建会话（§3.7）
            }
            PrimaryButton(L10n.t(.btnExportModel), enabled: info?.exportable ?? false) {
                router.push(.export(sessionID: sessionID))
            }
        }
        .padding(.horizontal, ReconMetric.space4)
        .padding(.vertical, ReconMetric.space3)
    }
}

// ===========================================================================
// MARK: - 键值行（§1.4：数值等宽 + 右对齐 + 单位前半角空格）
// ===========================================================================

/// 统计键值行：左侧 `subhead` 键名，右侧 `subhead` 等宽数值。
public struct StatRow: View {
    let key: String
    let value: String
    let unit: String?
    let valueColor: Color

    public init(_ key: String, _ value: String, unit: String? = nil,
                valueColor: Color = ReconColor.txtPrimary) {
        self.key = key
        self.value = value
        self.unit = unit
        self.valueColor = valueColor
    }

    public var body: some View {
        HStack(spacing: ReconMetric.space3) {
            SpecText(key, spec: ReconFont.subhead, color: ReconColor.txtSecondary)
            Spacer(minLength: ReconMetric.space2)
            MonoNumber(value, unit: unit, spec: ReconFont.subhead, color: valueColor)
        }
        .frame(minHeight: 24)
    }
}

/// 尺寸单元格：`长 182.4 mm`（键名 footnote + 数值等宽 subhead）。
private struct DimCell: View {
    let key: String
    let valueMM: Double

    init(_ key: String, _ valueMM: Double) {
        self.key = key
        self.valueMM = valueMM
    }

    var body: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space1) {
            SpecText(key, spec: ReconFont.caption, color: ReconColor.txtSecondary)
            MonoNumber(String(format: "%.1f", valueMM), unit: "mm",
                       spec: ReconFont.subhead, color: ReconColor.txtPrimary)
        }
    }
}

/// 线框渲染占位。真实实现接入 Metal（shaders/metal/）。
/// 刻意保留为**显式未接入**状态，而不是画一个假网格。
private struct WireframePlaceholder: View {
    var body: some View {
        SpecText(L10n.t(.resultPreviewPending), spec: ReconFont.footnote,
                 color: ReconColor.txtSecondary, maxLines: 2)
    }
}

// ===========================================================================
// MARK: - 网格统计模型
// ===========================================================================

/// 与 C ABI `recon_mesh_info` 对应的 UI 侧模型。
public struct MeshInfo: Equatable {
    public var triangles: Int = 0
    public var vertices: Int = 0
    public var nonManifoldEdges: Int = 0
    public var holes: Int = 0
    public var boundaryLoops: Int = 0
    public var lengthMM: Double = 0
    public var widthMM: Double = 0
    public var heightMM: Double = 0
    public var volumeCM3: Double = 0
    public var hasTexture: Bool = true
    /// 贴图数量（来自 `recon_mesh_info.texture_count`；用于体积估算的口径统一）。
    public var textureCount: Int = 0
    public var atlasWidth: Int = 0
    public var atlasHeight: Int = 0
    public var textureCoverage: Double = 0
    public var scaleSource: ScaleSource = .none
    public var scaleUncertainty: Double? = nil    // `σ_s/s`
    public var exportable: Bool = false

    /// 覆盖率警告阈值（`post_tex_coverage_max_ratio`，08 参数体系）。
    public static let coverageWarnThreshold: Double = 0.95

    /// 尺度来源（§3.7 的取值表）。
    public enum ScaleSource: String {
        case referenceObject = "reference_object"
        case manual          = "manual"
        case depth           = "depth"
        case imuPrior        = "imu_prior"
        case none            = "none"

        var label: String {
            switch self {
            case .referenceObject: return L10n.t(.resultScaleReference)
            case .manual:          return L10n.t(.resultScaleManual)
            case .depth:           return L10n.t(.resultScaleDepth)
            case .imuPrior:        return L10n.t(.resultScaleImu)
            case .none:            return L10n.t(.resultScaleNone)
            }
        }
    }

    /// 由内核结果摘要构造（U-07 的唯一转换点）。
    ///
    /// 尺寸取 `sortedExtentsMM`（降序 = 长/宽/高，见该属性注释）；
    /// 体积取 `volumeCM3`。**不在这里重算** —— 否则就是同一逻辑的第二份实现。
    init(_ r: ReconResultSummary) {
        triangles = Int(r.triangleCount)
        vertices = Int(r.vertexCount)
        nonManifoldEdges = Int(r.nonManifoldEdges)
        holes = Int(r.holeCount)
        boundaryLoops = Int(r.boundaryLoops)
        let e = r.sortedExtentsMM
        lengthMM = e.count > 0 ? e[0] : 0
        widthMM  = e.count > 1 ? e[1] : 0
        heightMM = e.count > 2 ? e[2] : 0
        volumeCM3 = r.volumeCM3
        textureCount = Int(r.textureCount)
        hasTexture = r.textureCount > 0
        // 图集尺寸 / 覆盖率 / 尺度来源目前不在 `recon_mesh_info` 的摘要字段里
        // （内核尚未导出）→ 保持 0 / .none，UI 按「未提供」呈现，**不编造**。
        scaleSource = .none
        scaleUncertainty = nil
        // 可导出条件：有三角面且拓扑硬判据通过（非流形边为 0，见 MEMORY 结论）。
        exportable = r.triangleCount > 0 && r.nonManifoldEdges == 0
    }

    /// 尺度行文案。`IMU 估计` 追加 `（误差约 ±10%）`（§3.7）。
    public var scaleDescription: String {
        var s = scaleSource.label
        if scaleSource == .imuPrior {
            s += L10n.t(.resultImuErrorHint)
        }
        if let u = scaleUncertainty, scaleSource != .none {
            s += " · " + L10n.fill(.fmtUncertainty,
                                   String(format: "%.1f", u * 100))
        }
        return s
    }
}
