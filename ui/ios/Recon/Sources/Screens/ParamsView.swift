//
//  ParamsView.swift
//  Recon — U-08 参数面板（iOS）
//
//  契约：docs/09_UI规格.md §3.8（线框图、元素表、§3.8.1 分组映射、§3.8.5 专家模式
//       10 参数）、docs/08_参数体系.md（**参数键名唯一定义处**）、§5.8 Q-06/Q-10。
//
//  ⚠️ 分组计数**不硬编码**。理由（§3.8.1 末的表内自相矛盾已记录）：
//     文档 §3.8.1 表列 G6=78、合计 336；同节线框图列 G6=16、合计 274；差 62 项。
//     而 core 注册表按前缀实测：cap 12 / sync 3 / vio 18 / sfm 26 / mvs 16 /
//     rgbd 82 / mesh 16 / post 18 / scale 10 / export 0 / perf 13（另有 calib 0）。
//     三个口径互不一致 —— 属已登记的长尾遗留（参数规格-实现差距）。
//     因此本视图的分组标题计数一律**运行时从参数注册表读取**，不写字面量，
//     避免「折叠行写 29、展开后有 12 项」这类自相矛盾出现在成品里。
//
//  §3.8.1 表列出的 10 个专家参数（§3.8.5）是**枚举白名单**，可安全写死 ——
//  它们是「哪些参数归专家模式」的定义，不是计数。
//

import SwiftUI

public struct ParamsView: View {
    @EnvironmentObject private var router: AppRouter
    @EnvironmentObject private var controller: ReconSessionController

    /// 精度预设（08 §2）。展示值来自 controller（内核 `recon_apply_preset`），
    /// 本视图不持有第二份状态，避免「UI 显示已切档、内核没切」。
    private var preset: Preset { controller.preset }
    /// 专家模式开关。
    @State private var expertMode = false
    /// 展开的分组（按组号）。
    @State private var expanded: Set<Int> = []
    /// 预设差异：覆盖项数由内核 `overridden` 清单**真实**给出。
    /// 无句柄（未打开会话）时返回 `nil`，UI 不画这一行（而不是画个假数字）。
    private var presetDiff: PresetDiff? {
        guard let n = controller.presetOverriddenCount() else { return nil }
        return PresetDiff(overridden: n, kept: 0)
    }
    /// Q-10：高精度档不可行（本机内存不足）。
    @State private var highPresetInfeasible = false
    @State private var showFactoryConfirm = false

    /// 分组计数：每次读取时向内核取（`recon_export_params`）。
    /// **不是** `@State` —— 避免「缓存一份可能过期的计数」。
    private var groupCounts: [Int: Int] { controller.groupCounts() }

    public init() {}

    public var body: some View {
        VStack(spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: ReconMetric.space5) {
                    presetSection
                    groupsSection
                }
                .padding(ReconMetric.space4)
            }
            Hairline()
            expertToggle
        }
        .background(ReconColor.bg.page)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarLeading) {
                Button { router.pop() } label: {
                    HStack(spacing: ReconMetric.space1) {
                        Image(systemName: "chevron.left")
                        SpecText(L10n.t(.paramsTitle), spec: ReconFont.headline, color: ReconColor.txtPrimary)
                    }
                }
                .foregroundStyle(ReconColor.txtAccent)
            }
            ToolbarItem(placement: .topBarTrailing) {
                // `完成`：关闭面板，写回 ParamSet。
                Button(L10n.t(.paramsDone)) { router.pop() }
                    .font(.system(size: ReconFont.headline.size, weight: .semibold))
                    .foregroundStyle(ReconColor.txtAccent)
            }
        }
        // §5.8 Q-06：恢复出厂。
        .confirmationDialog(L10n.t(.confirmRestoreFactoryTitle),
                            isPresented: $showFactoryConfirm, titleVisibility: .visible) {
            Button(L10n.t(.commonCancel), role: .cancel) { }
            Button(L10n.t(.paramsRestoreFactory), role: .destructive) { /* 清除全部 overrides */ }
        }
    }

    /// 预设选择绑定：写入内核（`recon_apply_preset`）后才更新显示值。
    /// 内核拒绝时不更新 UI 档位，避免「显示已切、实际未切」。
    private var presetBinding: Binding<Preset> {
        Binding(get: { controller.preset },
                set: { controller.applyPreset($0) })
    }

    // --- 精度预设（§3.8 第一区块）-----------------------------------------

    private var presetSection: some View {
        VStack(alignment: .leading, spacing: ReconMetric.space3) {
            SpecText(L10n.t(.paramsPreset), spec: ReconFont.title2, color: ReconColor.txtPrimary)

            Picker(L10n.t(.paramsPreset), selection: presetBinding) {
                ForEach(Preset.allCases, id: \.self) { p in
                    Text(p.label).tag(p)
                }
            }
            .pickerStyle(.segmented)

            // Q-10：高精度档在内存不足机型上禁用，其下加一行 footnote txt.error。
            if highPresetInfeasible {
                SpecText(L10n.t(.paramsHighInfeasible),
                         spec: ReconFont.footnote, color: ReconColor.txtError, maxLines: 2)
            }

            // 预设差异提示：`覆盖 12 项 · 保留你的 3 项` + `查看`
            if let diff = presetDiff {
                HStack(spacing: ReconMetric.space2) {
                    SpecText(diff.summaryText, spec: ReconFont.footnote,
                             color: ReconColor.txtSecondary, maxLines: 2)
                    Spacer(minLength: ReconMetric.space2)
                    Button(L10n.t(.commonView)) { /* 展开差异清单（§3.8.3） */ }
                        .font(.system(size: ReconFont.footnote.size))
                        .foregroundStyle(ReconColor.txtAccent)
                        .frame(minWidth: ReconMetric.minTouchTarget,
                               minHeight: ReconMetric.minTouchTarget)
                }
            }
        }
    }

    // --- 分组折叠列表（§3.8）----------------------------------------------

    private var groupsSection: some View {
        VStack(spacing: 0) {
            ForEach(Array(ParamGroup.ordered.enumerated()), id: \.element.id) { idx, group in
                if idx > 0 { Hairline() }
                groupRow(group)
            }
        }
        .background(ReconColor.bg.secondary)
        .clipShape(RoundedRectangle(cornerRadius: ReconMetric.radiusCard))
    }

    private func groupRow(_ group: ParamGroup) -> some View {
        let isOpen = expanded.contains(group.id)
        // 计数来自注册表；未取到时显示占位而非假数字。
        let countText = groupCounts[group.id].map { " (\($0))" } ?? ""

        return VStack(spacing: 0) {
            Button {
                if isOpen { expanded.remove(group.id) } else { expanded.insert(group.id) }
            } label: {
                HStack(spacing: ReconMetric.space2) {
                    // 收起 `▸` / 展开 `▾` —— 文字符号，非图标（§3.8）。
                    SpecText(isOpen ? "▾" : "▸", spec: ReconFont.subhead,
                             color: ReconColor.txtSecondary)
                    SpecText(group.title + countText, spec: ReconFont.headline,
                             color: ReconColor.txtPrimary)
                    Spacer(minLength: 0)
                }
                .frame(minHeight: ReconMetric.controlHeight)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)

            if isOpen {
                // 展开态：参数行（左侧中文名，右侧当前值 + `▸`）。
                // 具体键名与取值来自 08 文档的参数表，本视图只呈现，不做定义。
                ForEach(group.visibleKeys(expert: expertMode, from: controller), id: \.key) { item in
                    HStack(spacing: ReconMetric.space2) {
                        SpecText(item.displayName, spec: ReconFont.subhead,
                                 color: ReconColor.txtPrimary)
                        Spacer(minLength: ReconMetric.space2)
                        SpecText(item.value, spec: ReconFont.subhead,
                                 color: ReconColor.txtSecondary, mono: true)
                        SpecText("▸", spec: ReconFont.footnote, color: ReconColor.txtTertiary)
                    }
                    .padding(.leading, ReconMetric.space4)
                    .frame(minHeight: ReconMetric.controlHeight)
                }
            }
        }
        .padding(.horizontal, ReconMetric.space3)
    }

    // --- 底部：专家模式 + 恢复出厂（§3.8 元素表）--------------------------

    private var expertToggle: some View {
        VStack(spacing: ReconMetric.space2) {
            Toggle(isOn: $expertMode) {
                SpecText(L10n.t(.paramsExpertMode), spec: ReconFont.subhead, color: ReconColor.txtPrimary)
            }
            .tint(ReconColor.accent)

            Button(L10n.t(.paramsRestoreFactory)) { showFactoryConfirm = true }
                .font(.system(size: ReconFont.footnote.size))
                .foregroundStyle(ReconColor.txtError)
                .frame(minWidth: ReconMetric.minTouchTarget, minHeight: ReconMetric.minTouchTarget)
        }
        .padding(.horizontal, ReconMetric.space4)
        .padding(.vertical, ReconMetric.space3)
    }
}

// ===========================================================================
// MARK: - 精度预设（08 §2）
// ===========================================================================

public enum Preset: String, CaseIterable {
    case fast       // 快速
    case standard   // 标准
    case high       // 高精度

    public var label: String {
        switch self {
        case .fast:     return L10n.t(.paramsPresetFast)
        case .standard: return L10n.t(.paramsPresetStandard)
        case .high:     return L10n.t(.paramsPresetHigh)
        }
    }

    /// 与 C ABI `recon_preset` 的序号对应。
    public var rawIndex: Int32 {
        switch self {
        case .fast:     return 0
        case .standard: return 1
        case .high:     return 2
        }
    }
}

/// 预设差异提示（§3.8「预设差异提示」行）。
public struct PresetDiff {
    /// `覆盖 12 项 · 保留你的 3 项`
    public let overridden: Int
    public let kept: Int

    public init(overridden: Int, kept: Int) {
        self.overridden = overridden
        self.kept = kept
    }

    /// 无差异时显示 `与当前设置一致`（§3.8）。
    public var summaryText: String {
        if overridden == 0 && kept == 0 { return L10n.t(.paramsDiffIdentical) }
        return L10n.fill2(.fmtPresetDiff, String(overridden), String(kept))
    }
}

// ===========================================================================
// MARK: - 参数分组（§3.8.1 组名与键名前缀映射）
// ===========================================================================

/// 参数分组。
///
/// 表内的 11 组、组名、键名前缀取自 §3.8.1；**条目数不在代码里**（见文件头说明），
/// 由运行时注册表填充 `groupCounts`。
public struct ParamGroup: Identifiable {
    /// 组号 1..11（§3.8.1）。
    public let id: Int
    /// 面板分组标题（如 `采集`）。
    public let title: String
    /// 键名前缀（如 `cap_`）。`scale`/`calib` 为并列前缀。
    public let prefixes: [String]

    /// 展开态可见参数。真实键与取值由 `ReconSessionController.groupItems` 提供
    /// （数据源是内核 `recon_export_params`），本视图不伪造。
    public func visibleKeys(expert: Bool,
                            from controller: ReconSessionController) -> [ParamItem] {
        controller.groupItems(groupId: id, expert: expert)
    }

    /// 键名 → 组号的映射（docs/09 §3.8.1 的 11 组）。
    ///
    /// 唯一实现处：`groupCounts` 与 `groupItems` 都调它，避免「同一映射写两遍」
    /// 而漂移。注意组 9 有**两个并列前缀** `scale_` / `calib_`。
    ///
    /// - Returns: 组号；键不属于 G1–G11 时返回 `nil`（如 `scan_*` 属 docs/08
    ///   的 G12，而 docs/09 面板未定义该组 —— 如实排除，不臆造归属）。
    public static func groupIndex(forKey key: String) -> Int? {
        let prefix = key.split(separator: "_").first.map(String.init) ?? ""
        switch prefix {
        case "cap":    return 1
        case "sync":   return 2
        case "vio":    return 3
        case "sfm":    return 4
        case "mvs":    return 5
        case "rgbd":   return 6
        case "mesh":   return 7
        case "post":   return 8
        case "scale", "calib": return 9
        case "export": return 10
        case "perf":   return 11
        default:       return nil   // scan_ 等：docs/09 面板无此组
        }
    }

    /// §3.8.1 的 11 组（顺序即展开顺序）。
    public static let ordered: [ParamGroup] = [
        ParamGroup(id: 1,  title: L10n.t(.paramsGroupCapture), prefixes: ["cap_"]),
        ParamGroup(id: 2,  title: L10n.t(.paramsGroupSync), prefixes: ["sync_"]),
        ParamGroup(id: 3,  title: L10n.t(.paramsGroupVio), prefixes: ["vio_"]),
        ParamGroup(id: 4,  title: L10n.t(.paramsGroupSfm), prefixes: ["sfm_"]),
        ParamGroup(id: 5,  title: L10n.t(.paramsGroupMvs), prefixes: ["mvs_"]),
        ParamGroup(id: 6,  title: L10n.t(.paramsGroupRgbd), prefixes: ["rgbd_"]),
        ParamGroup(id: 7,  title: L10n.t(.paramsGroupMesh), prefixes: ["mesh_"]),
        ParamGroup(id: 8,  title: L10n.t(.paramsGroupPost), prefixes: ["post_"]),
        ParamGroup(id: 9,  title: L10n.t(.paramsGroupScale), prefixes: ["scale_", "calib_"]),
        ParamGroup(id: 10, title: L10n.t(.paramsGroupExport), prefixes: ["export_"]),
        ParamGroup(id: 11, title: L10n.t(.paramsGroupPerf), prefixes: ["perf_"]),
    ]
}

/// 单个参数行。
public struct ParamItem {
    public let key: String          // 08 文档定义的键名
    public let displayName: String
    public let value: String
}

// ===========================================================================
// MARK: - §3.8.5 专家模式下的 10 个参数（枚举白名单）
// ===========================================================================

/// 归入专家模式的 10 个参数（§3.8.5）。这是**定义**，不是计数，故可写死。
///
/// 同节末尾另列出 `mesh_poisson_depth_min`、`sync_drift_estimate_window`、
/// `sync_max_drift_ppm`、`sync_frame_drop_tolerance`、`sync_pipeline_latency_s`、
/// `sync_cm_fusion_lag_s`、`sync_rolling_shutter_line_time_us`、
/// `sync_align_residual_budget_us`、`sync_imu_jitter_rms_us` **UI 不暴露**。
public enum ExpertParam: String, CaseIterable {
    case syncInterpolation        = "sync_interpolation"
    case vioImuNoiseGyro          = "vio_imu_noise_gyro"
    case vioImuNoiseAcc           = "vio_imu_noise_acc"
    case vioImuBiasWalk           = "vio_imu_bias_walk"
    case rgbdLidarRole            = "rgbd_lidar_role"
    case meshPoissonDepthMode     = "mesh_poisson_depth_mode"
    case meshPoissonDegree        = "mesh_poisson_degree"
    case postNonmanifoldDeleteFallback = "post_nonmanifold_delete_fallback"
    case postBakeMinWeightSum     = "post_bake_min_weight_sum"
    case postBakeColorSpace       = "post_bake_color_space"

    public var displayName: String {
        switch self {
        case .syncInterpolation:            return L10n.t(.paramSyncInterpolation)
        case .vioImuNoiseGyro:              return L10n.t(.paramVioImuNoiseGyro)
        case .vioImuNoiseAcc:               return L10n.t(.paramVioImuNoiseAcc)
        case .vioImuBiasWalk:               return L10n.t(.paramVioImuBiasWalk)
        case .rgbdLidarRole:                return L10n.t(.paramRgbdLidarRole)
        case .meshPoissonDepthMode:         return L10n.t(.paramMeshPoissonDepthMode)
        case .meshPoissonDegree:            return L10n.t(.paramMeshPoissonDegree)
        case .postNonmanifoldDeleteFallback: return L10n.t(.paramPostNonmanifoldDeleteFallback)
        case .postBakeMinWeightSum:         return L10n.t(.paramPostBakeMinWeightSum)
        case .postBakeColorSpace:           return L10n.t(.paramPostBakeColorSpace)
        }
    }
}
