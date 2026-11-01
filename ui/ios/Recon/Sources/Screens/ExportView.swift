//
//  ExportView.swift
//  Recon — U-10 导出界面（iOS）
//
//  契约：docs/09_UI规格.md §3.10（线框图、元素表、U-E-1..U-E-4）、
//       §5.7 P-04（相册权限被拒文案）、§5.8 Q-09（校验失败重试）。
//
//  ── 三条硬约束（§3.10 末表）────────────────────────────────────────────
//  U-E-2：失败时必须列出对应错误码 `RECON_ERR_EXPORT_*`，**不隐藏错误码**。
//  U-E-3：校验未通过时两个导出按钮均禁用；**不提供「忽略并导出」**。
//  U-E-4：导出完成后提示一行 `已导出 186 MB`，**不弹 Toast、不做动画**。
//
//  因此本文件刻意没有「强制导出」入口，也不使用任何 toast 组件。
//

import SwiftUI

public struct ExportView: View {
    let sessionID: UUID

    @EnvironmentObject private var router: AppRouter
    /// 内核编排器：导出动作与完成提示的**唯一来源**。
    @EnvironmentObject private var controller: ReconSessionController

    /// 导出格式（写 `export_format`）。
    @State private var format: ExportFormat = .glb
    /// 校验项结果（17 项）。
    ///
    /// ⚠️ 当前恒为「全部通过」的**惰性初值** —— 真实 17 项校验在导出时由内核
    /// 执行（`recon_export` 内部），本视图尚无读取校验明细的 C ABI 通道。
    /// 因此**不**把它渲染成「刚校验过」的结论：`ValidationStatus` 另标
    /// `hasRun`，未跑过时显示「尚未校验」而非勾。
    @State private var checks: [ExportCheck] = ExportCheck.allPassing
    /// 校验是否已真实运行过（见上）。`false` 时列表显示为「未校验」。
    @State private var hasRunValidation = false
    /// 保存路径。默认落在应用 Documents 下，文件名随会话名（**数据**，非文案）。
    @State private var path: String = ""
    /// 可用空间（字节）。真实值需 `FileManager` 查询 —— 未接入前置 0 并禁用导出。
    @State private var freeBytes: Int64 = 0
    /// 相册权限是否被拒（P-04）。
    @State private var photoPermissionDenied = false
    /// 导出完成提示（U-E-4：一行文字，非常驻）—— 直接读编排器发布值。
    private var completedNote: String? { controller.lastExportNote }

    public init(sessionID: UUID) { self.sessionID = sessionID }

    /// U-E-3：校验全部通过才允许导出。
    private var validationPassed: Bool { checks.allSatisfy(\.passed) }

    /// 预计文件大小（字节）：与 U-01 列表同一个估算口径（唯一实现）。
    private var estimatedBytes: Int64 {
        ReconSessionRecord.estimateBytes(triangles: controller.result?.triangleCount ?? 0,
                                         textures: controller.result?.textureCount ?? 0)
    }

    /// 空间是否充足（低于预估 × 1.2 报错，§3.10「可用空间」行）。
    private var spaceSufficient: Bool {
        Double(freeBytes) >= Double(estimatedBytes) * 1.2
    }

    public var body: some View {
        VStack(spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: ReconMetric.space5) {
                    formatSection
                    locationSection
                    validationSection
                    if let completedNote {
                        // U-E-4：一行 footnote txt.success，不弹 Toast、不做动画。
                        SpecText(completedNote, spec: ReconFont.footnote,
                                 color: ReconColor.txtSuccess)
                    }
                    // U-E-2：导出失败时列出内核原始错误码，不隐藏。
                    if !controller.lastExportStatus.isOK {
                        SpecText("\(L10n.t(.exportFailed)) · "
                                 + "\(controller.lastExportStatus.rawValue)",
                                 spec: ReconFont.footnote,
                                 color: ReconColor.txtError, maxLines: 3)
                    }
                }
                .padding(ReconMetric.space4)
            }
            Hairline()
            bottomBar
        }
        .background(ReconColor.bg.page)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarLeading) {
                Button { router.pop() } label: {
                    HStack(spacing: ReconMetric.space1) {
                        Image(systemName: "chevron.left")
                        SpecText(L10n.t(.exportTitle), spec: ReconFont.headline,
                                 color: ReconColor.txtPrimary)
                    }
                }
                .foregroundStyle(ReconColor.txtAccent)
            }
        }
        .onAppear {
            // 默认文件名取当前会话名（数据），扩展名随格式。
            let name = controller.records.first(where: { $0.id == sessionID })?.name
                ?? "model"
            path = "/Documents/\(name).\(format.fileExtension)"
        }
        .onDisappear { controller.clearExportNote() }
    }

    // --- 格式（§3.10 第一区块）--------------------------------------------

    private var formatSection: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space3) {
            SpecText(L10n.t(.exportFormat), spec: ReconFont.title2, color: ReconColor.txtPrimary)

            Picker(L10n.t(.exportFormat), selection: $format) {
                ForEach(ExportFormat.allCases, id: \.self) { f in
                    Text(f.label).tag(f)
                }
            }
            .pickerStyle(.segmented)

            // 格式附属选项（键值行）。
            VStack(alignment: .leading, spacing: ReconMetric.space2) {
                SpecText("\(L10n.t(.exportTextureFormat)) \(format.textureFormatLabel)",
                         spec: ReconFont.subhead,
                         color: ReconColor.txtPrimary)
                SpecText("\(L10n.t(.exportIndexMode)) \(L10n.t(.commonAuto))",
                         spec: ReconFont.subhead, color: ReconColor.txtPrimary)
                SpecText("\(L10n.t(.exportVertexColor)) \(L10n.t(.commonOn))",
                         spec: ReconFont.subhead, color: ReconColor.txtPrimary)
                // OBJ+MTL 时追加 `法线 开` `UV 开` `切线 开`
                if format == .objMtl {
                    SpecText("\(L10n.t(.exportNormals)) \(L10n.t(.commonOn))",
                             spec: ReconFont.subhead, color: ReconColor.txtPrimary)
                    SpecText("\(L10n.t(.exportUv)) \(L10n.t(.commonOn))",
                             spec: ReconFont.subhead, color: ReconColor.txtPrimary)
                    SpecText("\(L10n.t(.exportTangents)) \(L10n.t(.commonOn))",
                             spec: ReconFont.subhead, color: ReconColor.txtPrimary)
                }
            }
        }
    }

    // --- 保存位置 + 可用空间（§3.10 第二区块）-----------------------------

    private var locationSection: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space2) {
            SpecText(L10n.t(.exportSaveLocation), spec: ReconFont.title2,
                     color: ReconColor.txtPrimary)

            Button {
                // 点按打开系统文件选择器（iOS：UIDocumentPickerViewController 导出模式）。
            } label: {
                SpecText(path, spec: ReconFont.footnote,
                         color: ReconColor.txtSecondary, maxLines: 2)
                    .frame(minHeight: ReconMetric.minTouchTarget, alignment: .leading)
            }
            .buttonStyle(.plain)

            // 空间不足时改为 txt.error `可用空间不足`。
            SpecText(spaceSufficient
                     ? L10n.fill(.fmtFreeSpace, Self.byteText(freeBytes))
                     : L10n.t(.exportNoSpace),
                     spec: ReconFont.footnote,
                     color: spaceSufficient ? ReconColor.txtSecondary : ReconColor.txtError)
        }
    }

    // --- 校验结果（§3.10 第三区块）----------------------------------------

    private var validationSection: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space2) {
            SpecText(hasRunValidation
                     ? L10n.fill(.fmtValidationCount, String(ExportCheck.allNames.count))
                     : L10n.t(.exportValidation),
                     spec: ReconFont.title2, color: ReconColor.txtPrimary)

            // 未跑过校验时不画勾（§3.10 的勾表示「已校验且通过」，画了就是伪造）。
            // U-D-7：不隐藏状态 —— 明确写「尚未校验，导出时由内核执行」。
            if !hasRunValidation {
                SpecText(L10n.t(.exportValidationPending), spec: ReconFont.footnote,
                         color: ReconColor.txtSecondary, maxLines: 3)
            }

            ForEach(checks) { c in
                VStack(alignment: .leading, spacing: ReconMetric.space1) {
                    HStack(spacing: ReconMetric.space2) {
                        // 通过 = sys.success 图标；失败 = sys.error 图标。U-X-6：SF Symbols。
                        Image(systemName: hasRunValidation
                              ? (c.passed ? "checkmark.circle" : "xmark.circle")
                              : "circle.dashed")
                            .font(.system(size: ReconFont.footnote.size))
                            .foregroundStyle(hasRunValidation
                                             ? (c.passed ? ReconColor.success : ReconColor.error)
                                             : ReconColor.txtTertiary)
                        SpecText(c.name, spec: ReconFont.subhead,
                                 color: (hasRunValidation && !c.passed)
                                        ? ReconColor.txtError : ReconColor.txtPrimary)
                        Spacer(minLength: 0)
                    }
                    // U-E-2：失败时列出错误码 + 原因，不隐藏。
                    if hasRunValidation && !c.passed {
                        SpecText("\(c.errorCode) · \(c.reason)", spec: ReconFont.footnote,
                                 color: ReconColor.txtError, maxLines: 3)
                            .padding(.leading, ReconMetric.space4)
                    }
                }
                .frame(minHeight: 28)
            }

            SpecText(L10n.fill(.fmtEstimatedFile, Self.byteText(estimatedBytes)),
                     spec: ReconFont.footnote, color: ReconColor.txtSecondary, mono: true)
                .padding(.top, ReconMetric.space2)
        }
    }

    // --- 底部（§3.10：导出到文件 / 导出到相册）----------------------------

    private var bottomBar: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space2) {
            if photoPermissionDenied {
                // P-04：未授予相册权限，无法导出到相册。
                SpecText(L10n.t(.permPhotoDenied),
                         spec: ReconFont.footnote, color: ReconColor.txtWarning, maxLines: 2)
            }
            HStack(spacing: ReconMetric.space3) {
                PrimaryButton(L10n.t(.exportToFile),
                              enabled: validationPassed && spaceSufficient) {
                    // 经编排器导出 —— 它会调用 C ABI `recon_export` 并把
                    // 真实 `ReconStatus` 与完成提示发布回来（**不静默吞错**）。
                    _ = controller.exportModel(format: format, outPath: path)
                }
                SecondaryButton(L10n.t(.exportToPhotos),
                                enabled: validationPassed && !photoPermissionDenied) {
                    // 相册导出走 PHPhotoLibrary；写入成功后提示同 U-E-4。
                    // ⚠️ 未接入：此处不伪造成功提示 —— 保留按钮但由 enabled 门控。
                }
            }
        }
        .padding(.horizontal, ReconMetric.space4)
        .padding(.vertical, ReconMetric.space3)
    }

    /// 字节 → 人类可读（`186 MB` / `3.2 GB`）。
    static func byteText(_ bytes: Int64) -> String {
        let gb = Double(bytes) / 1_073_741_824
        if gb >= 1 { return String(format: "%.1f GB", gb) }
        let mb = Double(bytes) / 1_048_576
        return String(format: "%.0f MB", mb)
    }
}

// ===========================================================================
// MARK: - §3.10 导出格式
// ===========================================================================

public enum ExportFormat: String, CaseIterable {
    case glb       // GLB
    case objMtl    // OBJ+MTL
    case ply       // PLY

    public var label: String {
        switch self {
        case .glb:    return "GLB"
        case .objMtl: return "OBJ+MTL"
        case .ply:    return "PLY"
        }
    }

    /// `export_texture_format` 的呈现（线框图：`贴图 PNG`）。
    public var textureFormatLabel: String {
        switch self {
        case .glb:    return "PNG"
        case .objMtl: return "PNG"
        case .ply:    return "PNG"
        }
    }

    /// 文件扩展名（供默认保存路径用）。
    public var fileExtension: String {
        switch self {
        case .glb:    return "glb"
        case .objMtl: return "obj"
        case .ply:    return "ply"
        }
    }
}

// ===========================================================================
// MARK: - U-E-1 的 17 项校验名（06 §4.1）
// ===========================================================================

/// 一项导出校验结果。
///
/// U-E-1 固定了 17 项校验名的中文缩写，**顺序与取值均取自规格**，不得增删。
public struct ExportCheck: Identifiable, Equatable {
    public var id: String { name }
    public let name: String
    public let passed: Bool
    public let errorCode: String     // U-E-2：失败时必填
    public let reason: String

    public init(name: String, passed: Bool, errorCode: String = "", reason: String = "") {
        self.name = name
        self.passed = passed
        self.errorCode = errorCode
        self.reason = reason
    }

    /// U-E-1 的 17 项校验名（顺序即规格表列出的顺序）。
    ///
    /// 全部走 `L10nKey` 具名键 —— 这些键**早已存在于 `zh-CN.json`**
    /// （`msg.export.check.*`，共 17 条，与本节条目一一对应）。
    public static let allNames: [String] = [
        L10n.t(.exportCheckIndexRange), L10n.t(.exportCheckTriangleWinding),
        L10n.t(.exportCheckSignedVolume), L10n.t(.exportCheckNormalNormalized),
        L10n.t(.exportCheckNormalConsistent),
        L10n.t(.exportCheckUvRange), L10n.t(.exportCheckUvDegenerate),
        L10n.t(.exportCheckIsolatedVertex), L10n.t(.exportCheckDuplicateVertex),
        L10n.t(.exportCheckDuplicateFace),
        L10n.t(.exportCheckIndexWidth), L10n.t(.exportCheckAtlasSize),
        L10n.t(.exportCheckTexturePresent), L10n.t(.exportCheckBoundingBox),
        L10n.t(.exportCheckNonmanifold),
        L10n.t(.exportCheckNonFinite), L10n.t(.exportCheckRoundtrip),
    ]

    /// 全部通过（默认态）。
    public static let allPassing: [ExportCheck] = allNames.map {
        ExportCheck(name: $0, passed: true)
    }
}
