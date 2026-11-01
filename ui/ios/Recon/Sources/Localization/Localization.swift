//
//  Localization.swift
//  Recon — 国际化基础设施（iOS）
//
//  契约：docs/09_UI规格.md §6.1（20 个 locale、回退链、键名规范、命名空间）。
//
//  ── 为什么不用 .strings/.xcstrings 而用 JSON ────────────────────────────
//  §6.1 明确 locale 文件位于 `ui/ios/Resources/locales/<locale>.json`，且
//  Android 侧为 `values-*/`。两端共用同一套 JSON 源（键名相同），可让
//  `tools/check_locale_parity.py` 一次校验两端覆盖度 —— 避免两端各维护一份
//  文案表而漂移（铁律：同一逻辑写两遍必然漂移）。
//
//  ── 回退链（§6.1）──────────────────────────────────────────────────────
//      zh-CN → en-US → key 本身（显示 `msg.<key>`）
//  三级回退在 `LocalizedText.resolve` 里逐级实现，且缺键时显示键本身而非空串
//  —— 空串会让「漏翻译」静默不可见（与项目既往「静默丢弃」教训同构）。
//
//  ── L10nKey 原始值必须与 locale JSON 键名逐字一致 ─────────────────────
//  这是本项目第三次踩到的同一类坑：**枚举里写了一个"看起来对"的键名，但
//  JSON 里根本没有** → 运行时静默回退、UI 显示键名而非文案，且**不报错**。
//  与既往「单测硬编码绕过注册表」「静默丢弃」是同构的一类静默失效。
//  现由 `tools/check_ios_contract.py` 的 **G6 闸门**强制：L10nKey 的每个
//  原始值都必须存在于 `zh-CN.json`，否则 CI 失败。
//

import Foundation

// ===========================================================================
// MARK: - 语言清单（§6.1 的 20 个 locale）
// ===========================================================================

/// 支持的语言。**顺序与 §6.1 表一致**。
public enum AppLocale: String, CaseIterable {
    case zhCN = "zh-CN"      // 1  简体中文（源语言）
    case enUS = "en-US"      // 2  英语（美国）
    case enGB = "en-GB"      // 3  英语（英国）
    case de   = "de"         // 4  德语
    case frFR = "fr-FR"      // 5  法语
    case es   = "es"         // 6  西班牙语
    case ptBR = "pt-BR"      // 7  葡萄牙语（巴西）
    case ru   = "ru"         // 8  俄语
    case ua   = "ua"         // 9  乌克兰语
    case pl   = "pl"         // 10 波兰语
    case cs   = "cs"         // 11 捷克语
    case tr   = "tr"         // 12 土耳其语
    case ar   = "ar"         // 13 阿拉伯语（RTL）
    case he   = "he"         // 14 希伯来语（RTL）
    case ja   = "ja"         // 15 日语
    case ko   = "ko"         // 16 韩语
    case zhTW = "zh-TW"      // 17 简体中文（繁体）
    case it   = "it"         // 18 意大利语
    case nl   = "nl"         // 19 荷兰语
    case th   = "th"         // 20 泰语

    /// 源语言（§6.1）。
    public static let source: AppLocale = .zhCN
    /// 二级回退（§6.1）。
    public static let secondaryFallback: AppLocale = .enUS

    /// 是否 RTL（§6.1：阿拉伯语、希伯来语）。
    public var isRTL: Bool { self == .ar || self == .he }

    /// 布局方向提示。RTL 使用系统自动镜像（§1.4「字体覆盖」行：
    /// RTL locale 用系统的自动镜像布局，**不手工镜像**）。
    public var layoutDirection: LayoutDirectionHint {
        isRTL ? .rightToLeft : .leftToRight
    }

    public enum LayoutDirectionHint { case leftToRight, rightToLeft }

    /// 文件名（§6.1）。
    public var fileName: String { "\(rawValue).json" }
}

// ===========================================================================
// MARK: - 文案目录（JSON 加载 + 三级回退）
// ===========================================================================

/// 文案目录。
///
/// 多个 locale 的 **key 集合应当一致**；不一致由 CI 的工具校验（见文件头）。
public final class LocaleCatalog {

    /// locale → (key → 译文)
    private var tables: [AppLocale: [String: String]] = [:]

    /// 加载时某些 locale 文件缺失（非「待翻译」，是**文件本身不存在**）。
    public private(set) var missingFiles: [AppLocale] = []

    public init() {}

    /// 从 bundle 的 `Resources/locales/<locale>.json` 加载全部 locale。
    ///
    /// 缺失某个 locale 文件时不抛错、不静默 —— 记录到 `missingFiles` 供 CI 与
    /// 「关于」页暴出（U-D-7：不隐藏降级）。
    ///
    /// ── 为什么不用 `[String: String]` 解码 ──────────────────────────────
    /// 每个 locale 文件里都有一个 `_meta` **对象**（含 `rtl`、
    /// `translation_status` 等，供 §6.1 的 RTL 判定与「待翻译」声明使用）。
    /// 用 `[String: String]` 解码时，遇到对象值会**整体抛错** → 该 locale 被
    /// 判为缺失。实测 18 个待翻译 locale 全都是这种结构，即 18/20 会静默
    /// 落进 `missingFiles`。故这里用 `JSONSerialization` 只挑出**字符串值**，
    /// 忽略 `_meta` 之类的非字符串条目。
    public func loadAll(from bundle: Bundle = .main) {
        missingFiles = []
        tables = [:]
        for locale in AppLocale.allCases {
            guard let url = bundle.url(forResource: locale.rawValue,
                                       withExtension: "json",
                                       subdirectory: "locales"),
                  let data = try? Data(contentsOf: url),
                  let any = try? JSONSerialization.jsonObject(with: data),
                  let raw = any as? [String: Any]
            else {
                missingFiles.append(locale)
                continue
            }
            // 只保留字符串值；`_meta` 等非字符串条目跳过（它们不是文案）。
            var table: [String: String] = [:]
            for (k, v) in raw {
                if let s = v as? String { table[k] = s }
            }
            // 占位文件（`translation_status = "pending"`、无任何 msg.* 键）是
            // **合法**的：按 §6.1 三级回退会落到 en-US。此处不视为缺失。
            tables[locale] = table
        }
    }

    /// 供测试注入。
    public func setTable(_ table: [String: String], for locale: AppLocale) {
        tables[locale] = table
    }

    /// 解析键，按 §6.1 三级回退链：`当前` → `en-US` → `zh-CN`(源) → key 本身。
    ///
    /// 注意 §6.1 的回退链写作 `zh-CN → en-US → key 本身`，其中 `zh-CN` 是**源**
    /// 语言（所有文案的原始定义处）。当用户语言是其他 locale 时，实际顺序为
    /// 「用户语言 → en-US → zh-CN → key」。这样保证：用户语言缺键时先借英文，
    /// 英文也缺再借源文中文，最终才显示键名。
    public func resolve(_ key: String, for locale: AppLocale) -> String {
        if let v = tables[locale]?[key], !v.isEmpty { return v }
        if let v = tables[AppLocale.secondaryFallback]?[key], !v.isEmpty { return v }
        if let v = tables[AppLocale.source]?[key], !v.isEmpty { return v }
        // 最后回退：显示 `msg.<key>` 形态（§6.1「key 本身」）。
        // 不返回空串 —— 空串会让漏翻译静默不可见。
        return key.hasPrefix("msg.") || key.hasPrefix("a11y.") || key.hasPrefix("fmt.")
            ? key : "msg.\(key)"
    }

    /// 检出某 locale 相对源语言的缺键（供 CI 与「关于」页）。
    public func missingKeys(for locale: AppLocale) -> [String] {
        guard let sourceTable = tables[AppLocale.source] else { return [] }
        let table = tables[locale] ?? [:]
        return sourceTable.keys.filter { table[$0]?.isEmpty ?? true }.sorted()
    }

    /// 已加载的 locale 数（用于自检）。
    public var loadedCount: Int { tables.count }

    /// 建立「源语言原文 → 键」的索引，供迁移期按原文反查（`L10n.s(_:)`）。
    ///
    /// 必须在 `loadAll` 之后调用。同一原文对应多个键时保留**第一个**
    /// （按 `AppLocale.source` 表中键的出现顺序不稳定，故这里按字典序取，
    /// 保证跨平台/跨运行可复现 —— 确定性构建要求）。
    public func buildSourceIndex() {
        guard let sourceTable = tables[AppLocale.source] else { return }
        var index: [String: String] = [:]
        // 先按值分组，再按键名排序取首 —— 保证结果确定。
        var byValue: [String: [String]] = [:]
        for (k, v) in sourceTable where !v.isEmpty {
            byValue[v, default: []].append(k)
        }
        for (v, keys) in byValue {
            index[v] = keys.sorted().first
        }
        L10nKey.sourceIndex = index
    }
}

// ===========================================================================
// MARK: - 键名命名空间（§6.1）
// ===========================================================================

/// 文案键。遵循 §6.1 规范：`msg.<域>.<编号>` / `a11y.*` / `fmt.*`。
///
/// 本枚举只收录**已在 docs/09 §5 文案表中定义**的键，不发明新文案；
/// 表中没有的文案不得出现在成品 UI 里（少数自拟文案已在 case 注释里逐条登记
/// 为「文档缺口」，见 `retryToCamera` / `capDepthTof` / `glassNavBar` 等）。
///
/// ── 为什么是 `CaseIterable` ──────────────────────────────────────────────
/// 原先 `allRawValues` 是一份**手抄**的 case 列表，与枚举声明**两处并存**。
/// 这类「同一事实写两遍」在本项目已经反复漂移（见 `MEMORY.md`「同一逻辑写两遍
/// 必然漂移」）：本轮实测手抄表就已**漏掉 50 个新 case**、并引用了两个**已被
/// 删除**的 case 名（`a11yEndScan`/`a11yExport`）→ 编译失败。
/// 改为 `CaseIterable` 后 `allRawValues = allCases.map(\.rawValue)`，
/// **单一事实来源**，结构上不可能再漂移。
public enum L10nKey: String, CaseIterable {
    // --- §5.4 采集与落盘（N-*）------------------------------------------
    case scanMoveTooFast        = "msg.notice.move_too_fast"
    case scanDirectionUncovered = "msg.notice.direction_uncovered"

    // --- §5.5 重建（R-*）------------------------------------------------
    case rebuildDone            = "msg.rebuild.done"
    case rebuildMeshEmptyInput  = "msg.rebuild.empty_input"
    //     R-04..R-12：§5.5 文案表逐行对应（第十五轮接线时补齐 —— 此前 zh-CN.json
    //     已有这些键，但枚举未收录，UI 无从类型化引用）。**改 §5.5 必须同步此处。**
    case rebuildPoissonFailed      = "msg.rebuild.poisson_failed"     // R-04
    case rebuildNonmanifoldUnfixed = "msg.rebuild.nonmanifold_unfixed" // R-05
    case rebuildUvFailed           = "msg.rebuild.uv_failed"          // R-06
    case rebuildBakeNoCoverage     = "msg.rebuild.bake_no_coverage"   // R-07
    case rebuildHoleUnfilled       = "msg.rebuild.hole_unfilled"      // R-08
    case rebuildNormalInconsistent = "msg.rebuild.normal_inconsistent"// R-09
    case rebuildTexPartial         = "msg.rebuild.tex_partial"        // R-10
    case rebuildSimplifyReverted   = "msg.rebuild.simplify_reverted"  // R-11
    /// §5.5 R-01 通用失败兜底（附内核原始错误码）。本键为**新增**，
    /// 因 `RebuildView` 的「无 messageKey 的错误码」分支需要一句通用前缀。
    case rebuildGenericFailure     = "msg.rebuild.generic_failure"

    case rebuildPoissonChunked     = "msg.rebuild.poisson_chunked"    // R-12
    case rebuildCancelled          = "msg.rebuild.cancelled"          // R-13
    /// R-14：进程被杀后恢复（首页最近项目行）。
    case homeInterrupted           = "msg.home.interrupted"
    //     §3.6 重建页常驻文案（标题 / 取消 / 前台提示 / 正在取消）。
    case rebuildTitle              = "msg.rebuild.title"
    /// 重建阶段名（§3.5 十四段）。
    case rebuildStage1             = "msg.rebuild.stage_1"
    case rebuildStage2             = "msg.rebuild.stage_2"
    case rebuildStage3             = "msg.rebuild.stage_3"
    case rebuildStage4             = "msg.rebuild.stage_4"
    /// MONO 分支的第四段（无深度时跳过深度融合）。
    case rebuildStage4Mono         = "msg.rebuild.stage_4_mono"
    case rebuildStage5             = "msg.rebuild.stage_5"
    case rebuildStage6             = "msg.rebuild.stage_6"
    case rebuildStage7             = "msg.rebuild.stage_7"
    case rebuildStage8             = "msg.rebuild.stage_8"
    case rebuildStage9             = "msg.rebuild.stage_9"
    case rebuildStage10            = "msg.rebuild.stage_10"
    case rebuildStage11            = "msg.rebuild.stage_11"
    case rebuildStage12            = "msg.rebuild.stage_12"
    case rebuildStage13            = "msg.rebuild.stage_13"
    case rebuildStage14            = "msg.rebuild.stage_14"
    /// 内存降级提示（§13）。
    case degradeParallelism        = "msg.degrade.parallelism"
    case degradeChunking           = "msg.degrade.chunking"
    case degradeSpill              = "msg.degrade.spill"
    case degradeHighPreset         = "msg.degrade.high_preset"

    case rebuildCancel             = "msg.rebuild.cancel"
    case rebuildCancelling         = "msg.rebuild.cancelling"
    case rebuildKeepForeground      = "msg.rebuild.keep_foreground"

    // --- §5.7 权限（P-*）------------------------------------------------
    //     注意 `msg.notice.motion_denied` 与 `msg.perm.motion_denied` 是**两条
    //     独立文案**（前者是采集期轻提示、后者是权限页说明），此处取权限页那条。
    case permCameraDenied       = "msg.perm.camera_denied"
    case permMotionDenied       = "msg.perm.motion_denied"
    case permPhotoDenied        = "msg.perm.photo_denied"
    /// 无深度传感器（归属「设备能力」域，不是「权限」域 —— 它不是被拒而是不存在）。
    case capNoDepthSensor       = "msg.capability.no_depth_sensor"
    /// 机型未标定。
    case capUncalibrated        = "msg.capability.uncalibrated"

    // --- §5.8 导出（E-*）------------------------------------------------
    //     E-01 的 `已导出 186 MB` 用的是 `fmt.exported_size`（带占位符的格式
    //     串），其 case 定义在下方的 `fmt.*` 段（`fmtExportedSize`）——
    //     **不要在此处再声明一次**：本文件曾因此出现重复原始值
    //     （两 case 同键），被 G6 闸门抓出。此处只留说明。
    case exportInProgress       = "msg.export.in_progress"
    case exportTitle            = "msg.export.title"
    case exportFormat           = "msg.export.format"
    case exportSaveLocation     = "msg.export.save_location"
    case exportToFile           = "msg.export.to_file"
    case exportToPhotos         = "msg.export.to_photos"
    case exportEstimatedSize    = "msg.export.estimated_size"
    case exportFreeSpace        = "msg.export.free_space"
    case exportValidation       = "msg.export.validation"
    /// U-D-7：未跑过校验时的诚实占位（**新增键**，docs/09 §3.10 未规定）。
    case exportValidationPending = "msg.export.validation_pending"
    /// §3.10 十七项导出校验项名。
    case exportCheckIndexRange      = "msg.export.check.index_range"
    case exportCheckTriangleWinding = "msg.export.check.triangle_winding"
    case exportCheckSignedVolume    = "msg.export.check.signed_volume"
    case exportCheckNormalNormalized = "msg.export.check.normal_normalized"
    case exportCheckNormalConsistent = "msg.export.check.normal_consistent"
    case exportCheckUvRange         = "msg.export.check.uv_range"
    case exportCheckUvDegenerate    = "msg.export.check.uv_degenerate"
    case exportCheckIsolatedVertex  = "msg.export.check.isolated_vertex"
    case exportCheckDuplicateVertex = "msg.export.check.duplicate_vertex"
    case exportCheckDuplicateFace   = "msg.export.check.duplicate_face"
    case exportCheckIndexWidth      = "msg.export.check.index_width"
    case exportCheckAtlasSize       = "msg.export.check.atlas_size"
    case exportCheckTexturePresent  = "msg.export.check.texture_present"
    case exportCheckBoundingBox     = "msg.export.check.bounding_box"
    case exportCheckNonmanifold     = "msg.export.check.nonmanifold"
    case exportCheckNonFinite       = "msg.export.check.non_finite"
    case exportCheckRoundtrip       = "msg.export.check.roundtrip"

    case exportFailed           = "msg.export.failed"
    case exportNoSpace          = "msg.export.no_space"
    case exportValidationFailed = "msg.export.validation_failed"
    case exportSavedToPhotos    = "msg.export.saved_to_photos"

    // --- §5.6 确认框按钮（Q-01..Q-03）-----------------------------------
    case btnContinueScan        = "msg.confirm.continue_scan"
    case btnEndScan             = "msg.confirm.end_scan"
    case btnDeleteAndExit       = "msg.confirm.delete_and_exit"
    case btnContinueRebuild     = "msg.confirm.continue_rebuild"
    case confirmCancelRebuildTitle = "msg.confirm.cancel_rebuild_title"
    case confirmCancelRebuildMsg   = "msg.confirm.cancel_rebuild_msg"
    case confirmScanCancelTitle    = "msg.confirm.cancel_scan_title"
    case confirmSwitchModeTitle    = "msg.confirm.switch_mode_title"
    case confirmSwitchMode         = "msg.confirm.switch_mode"
    case confirmNoScaleTitle       = "msg.confirm.no_scale_title"
    case confirmNoScaleMsg         = "msg.confirm.no_scale_msg"
    case confirmGoCalibrate        = "msg.confirm.go_calibrate"
    case confirmSelfcheckTitle     = "msg.confirm.selfcheck_title"
    case confirmSelfcheckMsg       = "msg.confirm.selfcheck_msg"
    case confirmRebuild            = "msg.confirm.confirm_rebuild"
    case confirmContinue           = "msg.confirm.continue"

    case btnCancelRebuild       = "msg.confirm.cancel_rebuild"
    case btnExportModel         = "msg.result.export_model"
    /// 恢复出厂设置的二次确认标题（`msg.confirm.restore_factory_title`）。
    ///
    /// 此前 `ParamsView` 直接用 `L10n.s("恢复出厂设置会清除全部自定义参数，继续？")`
    /// 裸原文 —— 反查虽能命中，但「能具名就具名」是本项目的去硬编码要求。
    case confirmRestoreFactoryTitle = "msg.confirm.restore_factory_title"

    // --- 首页（§3.1）----------------------------------------------------
    case homeTitle              = "msg.home.title"          // 物体扫描
    case homeStartScan          = "msg.home.start_scan"     // 开始扫描
    case homeEmptyState         = "msg.home.empty_state"
    case homeProbing            = "msg.home.probe_running"  // 正在检测设备能力
    case homeCapabilityLabel    = "msg.home.capability_label"
    case homeAllProjects        = "msg.home.all_projects"
    case homeContinue           = "msg.home.continue"
    case homeContinueScan       = "msg.home.continue_scan"
    /// 首页其余元素（§3.1 元素表逐行）。这些键**早已存在于 zh-CN.json**
    /// （第十五轮接线时只补了一部分），此处补齐枚举声明以便类型化引用 ——
    /// 枚举缺声明会让视图被迫退回裸中文，从而触发 G9。
    case homeSettings           = "msg.home.settings"        // 设置
    case homeDetail             = "msg.home.detail"          // 详情
    case homeRecent             = "msg.home.recent"          // 最近项目
    case homeParams             = "msg.home.params"          // 参数
    case homeExportHistory      = "msg.home.export_history"  // 导出记录
    /// 首页会话行的**无结果副标题**：`未完成重建 · 标准档`。
    ///
    /// ── 文档缺口（如实登记）────────────────────────────────────────────
    ///  docs/09 §3.1 的元素表只给了**有结果**时该行的样子
    ///  （`1.2 MB · GLB · 标准档`），未规定**无结果**时副标题写什么。
    ///  原先此处直接写死中文 `未完成重建` → 非中文用户看到中文（G9 报出）。
    ///  按「文档是契约、但不为满足契约而谎报」，此处新增本键并登记为
    ///  **文档缺口**（需在 docs/09 §3.1 补齐「无结果」状态列）。
    case homeNotRebuilt         = "msg.home.not_rebuilt"

    // --- 通用按钮与取值（§2 元素表 / §3.8.4 枚举名映射 / 行 713·1225）------
    case commonView             = "msg.common.view"          // 查看
    case commonClose            = "msg.common.close"         // 关闭
    case commonCancel           = "msg.confirm.cancel"       // 取消（确认框统一用词）
    case commonAuto             = "msg.common.auto"          // 自动
    case commonOn               = "msg.common.on"            // 开
    case commonOff              = "msg.common.off"           // 关

    // --- 设备能力（§3.2 U-02）-------------------------------------------
    case capabilityTitle        = "msg.capability.title"
    case capDepthTof            = "msg.capability.depth_tof"
    case capDepthAlgo           = "msg.capability.depth_algo"
    case capDepthNone           = "msg.capability.depth_none"
    /// §3.2 第二区块的传感器名（第五轮接线时从裸中文改为具名键）。
    case capSensorCamera        = "msg.capability.sensor_camera"
    case capSensorGyro          = "msg.capability.sensor_gyro"
    case capSensorAccel         = "msg.capability.sensor_accel"
    case capSensorLidar         = "msg.capability.sensor_lidar"
    case capSensorArkit         = "msg.capability.sensor_arkit"
    /// U-02 底部按钮与动作文案。
    case capUseConfig           = "msg.capability.use_config"
    case capReprobe             = "msg.capability.reprobe"
    /// §3.2 第一区块的管线名（**取值表**第 3 列）。
    ///
    /// 区块标题 / 说明 / 尺度来源文案**不再另立键名** —— 分别复用既有的
    /// `msg.capability.pipeline`(`重建管线`) / `sensors` / `scale_source`、
    /// `scale_note_rgbd|mono`、`scale_text_rgbd|mono`、`available|unavailable`
    /// （`是|否` 用 `msg.common.yes|no`）、深度等级行用 `fmt.depth_tier`。
    /// 视图侧一律以 `L10n.s("中文")` 反查命中。
    ///
    /// ── 为什么删掉一批 case ─────────────────────────────────────────────
    /// 第十六轮曾为上述这些文案**再加一套键名**（`section_pipeline` 等），
    /// 实测全部**零引用**、且与既有键语义重复（同一文案两个键 =
    /// `MEMORY.md`「同一逻辑写两遍必然漂移」）。G6 闸门报出后全部删除。
    case capPipelineRgbd        = "msg.capability.pipeline_rgbd"
    case capPipelineMono        = "msg.capability.pipeline_mono"

    // --- 采集（§3.3/§3.4）-----------------------------------------------
    case scanPause              = "msg.scan.pause"
    // --- 扫描模式与操作（§3.3 U-04）-------------------------------------
    case scanModeFree           = "msg.scan.mode_free"
    case scanModeGuided         = "msg.scan.mode_guided"
    case scanArUnavailable      = "msg.scan.ar_unavailable"
    case scanFlipMode           = "msg.scan.flip_mode"
    case scanFinish             = "msg.scan.finish"
    case scanVisualFallback     = "msg.scan.visual_fallback"
    case scanCoverage           = "msg.scan.coverage"
    case scanPointIndex         = "msg.scan.point_index"
    /// 结束扫描确认框标题（复用既有 `msg.confirm.end_scan_title`）。
    case scanEndConfirmTitle2   = "msg.confirm.end_scan_title"

    case scanResume             = "msg.scan.resume"
    /// §3.3 采集界面其余文案。
    ///
    /// 带占位符的三条**不在此声明**：复用已有的 `fmtCapturedFrames` /
    /// `fmtCoveragePercent` / `fmtPointIndex` / `fmtMinFramesHint` /
    /// `fmtFramesDeleted`（同语义、同译文），避免同一文案两个键
    /// （本项目 G6 闸门会拒收重复原始值）。
    ///
    /// 页面标题 / 模式名 / 结束确认标题 / 追踪不可用提示同样**不另立键名**：
    /// 分别复用既有 `msg.scan.mode_free|guided`、`msg.confirm.end_scan_title`、
    /// `msg.scan.ar_unavailable`。第十六轮为它们造的 `scan.title` /
    /// `scan.free_orbit` / `scan.guided` / `scan.end_confirm_title` /
    /// `scan.tracking_unavailable` 实测**零引用且语义重复**，已删除。

    // --- 结果页（§3.7 U-07）---------------------------------------------
    //     统计行与区块标题**复用既有键**：区块标题用 `msg.result.topo` /
    //     `dimension` / `texture`（§3.7 线框图即 `拓扑`/`尺寸`/`纹理`），
    //     非流形边用 `msg.result.nonmanifold_edges`，尺度来源用
    //     `scale_reference|manual|depth|imu`，IMU 误差提示用
    //     `msg.result.imu_error_hint`。`resultPreviewPending` 的声明在下方既有段。
    //     第十六轮造的 `section_topology` / `section_dimension` / `section_texture` /
    //     `non_manifold` / `scale_ref` / `scale_imu_note` 实测**零引用且重复**，已删除。
    case resultTriangles       = "msg.result.triangles"
    case resultTopo            = "msg.result.topo"
    case resultDimension       = "msg.result.dimension"
    case resultTexture         = "msg.result.texture"
    case resultNonManifoldEdges = "msg.result.nonmanifold_edges"
    case resultScaleReference  = "msg.result.scale_reference"
    case resultImuErrorHint    = "msg.result.imu_error_hint"
    case resultCoverage        = "msg.result.coverage"
    case resultAtlas           = "msg.result.atlas"
    case resultUncertainty     = "msg.result.uncertainty"
    case resultScaleUncalibrated = "msg.result.scale_uncalibrated"

    case resultVertices        = "msg.result.vertices"
    case resultHoles           = "msg.result.holes"
    case resultBoundaryLoops   = "msg.result.boundary_loops"
    case resultLength          = "msg.result.length"
    case resultWidth           = "msg.result.width"
    case resultHeight          = "msg.result.height"
    case resultVolume          = "msg.result.volume"
    case resultScale           = "msg.result.scale"
    case resultScaleManual     = "msg.result.scale_manual"
    case resultScaleDepth      = "msg.result.scale_depth"
    case resultScaleImu        = "msg.result.scale_imu"
    case resultScaleNone       = "msg.result.scale_none"
    case resultRecapture       = "msg.result.recapture"

    // --- 导出页（§3.10 U-10）--------------------------------------------
    //     区块标题与权限/空间提示**复用既有键**：`msg.export.format` /
    //     `msg.export.save_location` / `msg.perm.photo_denied` /
    //     `msg.export.no_space`。第十六轮造的 `section_format` /
    //     `section_location` / `photo_denied` / `no_space_enough` 实测
    //     **零引用且重复**，已删除。格式附属选项的 6 条键在下方既有段声明。

    // --- 标定页（§3.9 U-09）---------------------------------------------
    case calibTitle            = "msg.calib.title"
    case calibA4               = "msg.calib.a4"
    case calibMustCalibrate    = "msg.calib.must_calibrate"
    case calibConfidenceLow    = "msg.calib.confidence_low"
    case calibConfidenceMedium = "msg.calib.confidence_medium"
    case calibRetryRecommend   = "msg.calib.retry_recommend"
    case calibCandidates       = "msg.calib.candidates"
    case calibCandidatesUnit   = "msg.calib.candidates_unit"
    case calibReprojError      = "msg.calib.reproj_error"

    case calibRefType          = "msg.calib.ref_type"
    case calibBankCard         = "msg.calib.bank_card"
    case calibIdCard           = "msg.calib.id_card"
    case calibCustom           = "msg.calib.custom"
    case calibActualSize       = "msg.calib.actual_size"
    /// §3.9 步骤条第 1/2/3 步（`1 拍摄参照物` / `2 检测卡片` / `3 解算尺度`）。
    ///
    /// 复用既有 `msg.calib.step_1|2|3` —— 第十六轮造过 `step_shoot|detect|solve`，
    /// 实测零引用且重复，已删。
    case calibStep1            = "msg.calib.step_1"
    case calibStep2            = "msg.calib.step_2"
    case calibStep3            = "msg.calib.step_3"
    case calibDetectionResult  = "msg.calib.detection_result"
    case calibNotFound         = "msg.calib.not_found"
    /// §3.9 标定失败四态的**主文案 + 补救提示**（`CalibrationFailure` 使用）。
    ///
    /// 这 7 个键早已存在于 zh-CN/en-US 两个 JSON，但未在 `L10nKey` 声明 ——
    /// 因此 `CalibrationFailure` 只能用 `L10n.s("未检测到卡片")` 裸原文调用。
    /// 反查虽能命中（G9 通过），但「能具名就具名」是本项目的去硬编码要求，
    /// 且裸原文一旦改动就失去编译期检查。此处补齐声明。
    case calibCardOccluded     = "msg.calib.card_occluded"
    case calibScaleUnstable    = "msg.calib.scale_unstable"
    case calibNoDepth          = "msg.calib.no_depth"
    case calibCardNotFoundHint = "msg.calib.card_not_found_hint"
    case calibCardOccludedHint = "msg.calib.card_occluded_hint"
    case calibScaleUnstableHint = "msg.calib.scale_unstable_hint"
    case calibNoDepthHint      = "msg.calib.no_depth_hint"
    case calibRetake           = "msg.calib.retake"
    case calibManualInput      = "msg.calib.manual_input"
    case calibLongEdge         = "msg.calib.long_edge"
    case calibShortEdge        = "msg.calib.short_edge"
    case calibApply            = "msg.calib.apply"
    case calibUseScale         = "msg.calib.use_scale"
    case calibSkip             = "msg.calib.skip"
    /// §3.9 置信度三档与失败码主/补文案（**直接取值，不改写**）。
    ///
    /// 宽度/高度标签复用 `msg.calib.long_edge|short_edge` 之外的
    /// `msg.result.width|height`（同义 `宽`/`高`）；参照物尺寸行整体复用
    /// `fmt.actual_size`（`实际尺寸  宽 %@ mm   高 %@ mm`）。
    /// 「跳过禁用」提示复用 `msg.calib.must_calibrate`；
    /// 「建议重测」复用 `msg.calib.retry_recommend`；
    /// 「尺度不可靠」复用 `msg.calib.confidence_low`。
    /// 第十六轮造的 `width`/`height`/`skip_disabled`/`unreliable`/
    /// `confidence_retry`/`a4_paper`/`fail_*`/`fix_*` 实测零引用且重复，已删。
    case calibConfidenceHigh   = "msg.calib.confidence_high"

    // --- 参数页（§3.8 U-08）---------------------------------------------
    case paramsTitle           = "msg.params.title"
    case paramsPreset          = "msg.params.preset"
    case paramsPresetFast      = "msg.params.preset_fast"
    case paramsPresetStandard  = "msg.params.preset_standard"
    case paramsPresetHigh      = "msg.params.preset_high"
    case paramsExpertMode      = "msg.params.expert_mode"
    case paramsRestoreFactory  = "msg.params.restore_factory"
    /// 恢复出的二次确认标题复用既有 `msg.confirm.restore_factory_title`
    /// （`恢复出厂设置会清除全部自定义参数，继续？`）；第十六轮造的
    /// `msg.params.factory_confirm` 实测零引用且重复，已删。
    case paramsDone            = "msg.params.done"           // 完成
    case paramsDiffIdentical   = "msg.params.diff_identical" // 与当前设置一致
    case paramsGroupCapture    = "msg.params.group_capture"
    case paramsGroupSync       = "msg.params.group_sync"
    case paramsGroupVio        = "msg.params.group_vio"
    case paramsGroupSfm        = "msg.params.group_sfm"
    case paramsGroupMesh       = "msg.params.group_mesh"
    case paramsGroupPost       = "msg.params.group_post"
    case paramsGroupScale      = "msg.params.group_scale"
    case paramsGroupExport     = "msg.params.group_export"
    case paramsGroupPerf       = "msg.params.group_perf"
    case paramsHighInfeasible  = "msg.params.high_infeasible"

    // --- 参数（§3.8）----------------------------------------------------
    case paramsGroupMvs         = "msg.params.group_mvs"
    case paramsGroupRgbd        = "msg.params.group_rgbd"
    // §3.8.5 专家模式的 10 个参数显示名（文档明列，故一条不落）。
    case paramSyncInterpolation          = "msg.params.p_sync_interpolation"
    case paramVioImuNoiseGyro            = "msg.params.p_vio_imu_noise_gyro"
    case paramVioImuNoiseAcc             = "msg.params.p_vio_imu_noise_acc"
    case paramVioImuBiasWalk             = "msg.params.p_vio_imu_bias_walk"
    case paramRgbdLidarRole              = "msg.params.p_rgbd_lidar_role"
    case paramMeshPoissonDepthMode       = "msg.params.p_mesh_poisson_depth_mode"
    case paramMeshPoissonDegree          = "msg.params.p_mesh_poisson_degree"
    case paramPostNonmanifoldDeleteFallback = "msg.params.p_post_nonmanifold_delete_fallback"
    case paramPostBakeMinWeightSum       = "msg.params.p_post_bake_min_weight_sum"
    case paramPostBakeColorSpace         = "msg.params.p_post_bake_color_space"

    // --- 失败重试提示（T-24 的 UI 补充说明）------------------------------
    //     注意：docs/09 §5 文案表**尚未收录**这三条，是从 §4.2 T-24 的
    //     「按失败阶段」正文派生出来的说明性文案。已登记为文档缺口。
    case retryToCamera          = "msg.fail.retry_camera"
    case retryToRebuild         = "msg.fail.retry_rebuild"
    case retryToCalibration     = "msg.fail.retry_calibration"

    // --- 重建（§3.6）----------------------------------------------------
    case rebuildPoissonOom      = "msg.rebuild.poisson_oom"
    /// §3.6 泊松 OOM 的补充建议（`msg.rebuild.poisson_oom_hint`）。
    case rebuildPoissonOomHint  = "msg.rebuild.poisson_oom_hint"

    // --- 无障碍标签（§5.10 A-*）------------------------------------------
    //     命名规范见 §6.1：`a11y.<域>.<元素>`。无文字按钮（如采集页的结束按钮
    //     只有图标）必须有标签 —— 否则 VoiceOver 只会读出「按钮」。
    case a11yScanStart          = "a11y.scan.start"     // 开始扫描
    case a11yScanEnd            = "a11y.scan.end"       // 结束扫描
    case a11yExport             = "a11y.export.model"   // 导出模型
    case a11yCalibPreview       = "a11y.calib.preview"  // 参照物预览
    case a11yResultPreview      = "a11y.result.preview" // 三维网格预览

    // --- 导出（§3.10）---------------------------------------------------
    case exportTextureFormat    = "msg.export.texture_format"
    case exportIndexMode        = "msg.export.index_mode"
    case exportVertexColor      = "msg.export.vertex_color"
    case exportNormals          = "msg.export.normals"
    case exportUv               = "msg.export.uv"
    case exportTangents         = "msg.export.tangents"

    // --- 带占位符的格式串（§6.1 `fmt.*` 命名空间）------------------------
    case fmtCapturedFrames      = "fmt.captured_frames"
    case fmtMinFramesHint       = "fmt.min_frames_hint"
    case fmtRebuildStage        = "fmt.rebuild_stage"
    case fmtCoveragePercent     = "fmt.coverage_percent"
    case fmtPointIndex          = "fmt.point_index"
    case fmtDepthTier           = "fmt.depth_tier"
    case fmtCoverageRatio       = "fmt.coverage_ratio"
    case fmtUncertainty         = "fmt.uncertainty"
    case fmtConfidenceMedium    = "fmt.confidence_medium"
    case fmtDetectionFound      = "fmt.detection_found"
    case fmtAtlasSize           = "fmt.atlas_size"
    case fmtIngestedKeyframes   = "fmt.ingested_keyframes"
    case fmtPeakMemory          = "fmt.peak_memory"
    case fmtEstimatedFile       = "fmt.estimated_file"
    case fmtFreeSpace           = "fmt.free_space"
    case fmtValidationCount     = "fmt.validation_count"
    case fmtExportedSize        = "fmt.exported_size"
    case fmtPresetDiff          = "fmt.preset_diff"
    /// 确认框：`已采集的 %d 帧将被删除`
    case fmtFramesDeleted       = "msg.confirm.frames_deleted"

    // --- 结果页（§3.7）---------------------------------------------------
    //     `线框预览尚未接入` 是**如实告知功能未接入**的占位文案（不是「正在准备」）。
    //     docs/09 §5 文案表**尚未收录**本条，已登记为文档缺口；不谎报为
    //     `msg.result.preparing_preview`（那条语义是「准备中」，会误导用户等待）。
    case resultPreviewPending   = "msg.result.preview_pending"
}

// ===========================================================================
// MARK: - 键集合自检（供 G6/G9 闸门与单测共用）
// ===========================================================================

extension L10nKey {
    /// 全部键的原始值。
    ///
    /// `tools/check_ios_contract.py` 的 G6 闸门会解析本文件、提取每个 case 的
    /// 原始值，再逐个到 `zh-CN.json` 里查在不在。**这是防止「枚举写了不存在的
    /// 键 → 运行时静默显示键名」的永久防线**。新增键必须同时改 JSON。
    ///
    /// 由 `CaseIterable` 派生 —— **不是手抄列表**（手抄表曾漏 50 项并引用已删
    /// case 导致编译失败，见枚举声明处的说明）。
    public static var allRawValues: [String] {
        allCases.map(\.rawValue)
    }
}

// ===========================================================================
// MARK: - 视图侧取值（环境注入 + 便捷方法）
// ===========================================================================
//
// ── 为什么用环境注入而不是全局单例 ──────────────────────────────────────
// 全局单例会让「当前语言」无法在测试里替换，也无法随系统语言变化重建。
// 环境注入（`\.l10n`）让视图声明依赖、让 SwiftUI 在目录变化时自动重建受
// 影响子树；测试里 `\.l10n` 也能塞一份假目录。
//
// ── 为什么提供字符串直取的 API ──────────────────────────────────────────
// 迁移期（把 241 处硬编码中文换成查表）是**逐处**进行的，不可能一次改完。
// 允许按 `L10nKey` 取值、也允许按**源语言原文**反查（`L10nCatalog.key(for:)`
// 的预建映射），这样迁移可以把「行为不变」作为每一步的验收条件：替换后
// 源语言（zh-CN）下渲染结果必须逐字符相同。

private struct L10nEnvironmentKey: EnvironmentKey {
    static let defaultValue: LocaleCatalog? = nil
}

public extension EnvironmentValues {
    /// 当前文案目录。由 `RootView` 在 `.environment` 注入。
    var l10n: LocaleCatalog? {
        get { self[L10nEnvironmentKey.self] }
        set { self[L10nEnvironmentKey.self] = newValue }
    }
}

/// 文案取值的统一入口。
///
/// 视图侧写法：
/// ```swift
/// Text(L10n.t(.homeStartScan))                 // 按枚举
/// Text(L10n.s("物体扫描"))                      // 按源语言原文反查（迁移期用）
/// ```
public enum L10n {

    /// 当前语言（由 `RootView` 依据系统 Locale 解析后注入；缺省为源语言）。
    ///
    /// 非 `@MainActor` 隔离：仅在视图构建期读取。
    public static var current: AppLocale = .zhCN

    /// 共享目录。`RootView` 在启动时 `loadAll()` 一次。
    ///
    /// 之所以既用环境注入又保留这一份：视图构建期（`Text(...)` 的实参、
    /// 非 View 的辅助函数如 `ScanSessionModel.finishScanHint`）无法访问
    /// `EnvironmentValues`。两者指向**同一个** `LocaleCatalog` 实例。
    public internal(set) static var catalog = LocaleCatalog()

    /// 按枚举键取值。
    public static func t(_ key: L10nKey) -> String {
        catalog.resolve(key.rawValue, for: current)
    }

    /// 类型化重载：直接吃枚举，**不需要写 `.rawValue`**。
    ///
    /// 为什么要有这一组重载：`.fmtDepthTier.rawValue` 这种写法依赖 Swift 的
    /// **隐式成员链类型推断**（把 `String` 形参的实参推断成 `L10nKey` 再取
    /// `.rawValue`）。该推断规则在实践里成立，但本项目**没有 macOS / Xcode**，
    /// 任何「靠推断成立」的写法都无法编译验证。这里提供 `L10nKey` 形参的重载，
    /// 调用点写成 `L10n.fill(.fmtDepthTier, …)` —— **纯类型检查，零推断**，
    /// 且把枚举名写错会变成**编译错误**而不是运行期显示键名。
    public static func fill(_ key: L10nKey, _ value: String) -> String {
        fill(key.rawValue, value)
    }

    /// 按**原始键名**取值（用于尚未收进 `L10nKey` 枚举、但 JSON 里已存在的键）。
    public static func raw(_ key: String) -> String {
        catalog.resolve(key, for: current)
    }

    /// 按源语言（zh-CN）原文反查并取值。
    ///
    /// 迁移期专用：把 `Text("物体扫描")` 直接写成 `Text(L10n.s("物体扫描"))`，
    /// 不改调用点结构、不改可见行为（zh-CN 下结果逐字符相同）。若原文在源
    /// 语言表里找不到，**原样返回并记录**（不返回空串 —— 「静默丢弃」是本项目
    /// 反复踩过的坑）。
    public static func s(_ sourceText: String) -> String {
        guard let key = L10nKey.sourceIndex[sourceText] else {
            L10n.unresolved.insert(sourceText)
            return sourceText
        }
        return catalog.resolve(key, for: current)
    }

    /// 把**已格式化好的文本**填进格式键的占位符。
    ///
    /// 为什么不直接用 `String(format:)`：`fmt.captured_frames` 的占位符是
    /// `%d`，而界面要显示的是**带千分位**的数字（§1.4 要求等宽 + 千分位），
    /// 即 `1,280` 这种**字符串**。把字符串塞给 `%d` 是未定义行为。
    /// 故这里做**文本替换**：把占位符（`%d` / `%@` / `%s`）换成给定文本。
    ///
    /// 注意 `%%`（表示字面百分号）必须**最后**处理，否则 `fmt.coverage_percent`
    /// 的 `覆盖方位 %d%%` 会被 `%d` 替换后的残留影响。这里先替换 `%d`/`%@`，
    /// 再把剩下的 `%%` 还原成 `%`。
    public static func fill(_ key: String, _ value: String) -> String {
        var pattern = catalog.resolve(key, for: current)
        // 占位符按「最长的先」替换，避免 `%@` 被 `%` 规则误伤。
        for ph in ["%@", "%d", "%s", "%ld", "%f"] where pattern.contains(ph) {
            pattern = pattern.replacingOccurrences(of: ph, with: value)
        }
        // 剩余的字面百分号。
        pattern = pattern.replacingOccurrences(of: "%%", with: "%")
        return pattern
    }

    /// 两个占位符的填充（`fmt.min_frames_hint` / `fmt.depth_tier` /
    /// `fmt.atlas_size` / `fmt.detection_found` / `fmt.preset_diff` /
    /// `fmt.rebuild_stage` 等）。
    ///
    /// **按出现顺序**填入 —— 这些格式键在源语言里两个占位符的顺序与语义顺序
    /// 一致（如 `至少需要 %d 帧，当前 %d 帧` → (最少, 当前)）。若将来出现顺序
    /// 与语义不一致的键，必须改用具名占位符而非本方法（届时 G8 闸门会提示
    /// 新增键未被任何视图引用）。
    public static func fill2(_ key: String, _ first: String, _ second: String) -> String {
        var pattern = catalog.resolve(key, for: current)
        // 逐个吃掉占位符：先来先填，保证顺序语义。
        pattern = replaceFirstPlaceholder(in: pattern, with: first)
        pattern = replaceFirstPlaceholder(in: pattern, with: second)
        pattern = pattern.replacingOccurrences(of: "%%", with: "%")
        return pattern
    }

    private static func replaceFirstPlaceholder(in text: String, with value: String) -> String {
        var best: (Range<String.Index>, Int)?
        for ph in ["%@", "%d", "%s", "%ld", "%f"] {
            if let r = text.range(of: ph) {
                let idx = text.distance(from: text.startIndex, to: r.lowerBound)
                if best == nil || idx < best!.1 {
                    best = (r, idx)
                }
            }
        }
        guard let (r, _) = best else { return text }
        return text.replacingCharacters(in: r, with: value)
    }

    /// 类型化重载：`L10n.fill2(.fmtPresetDiff, a, b)`。理由同 `fill(_:_:)`。
    public static func fill2(_ key: L10nKey, _ first: String, _ second: String) -> String {
        fill2(key.rawValue, first, second)
    }

    /// 未能在源语言表中反查到键的原文集合。
    ///
    /// 供「关于」页与单元测试断言为空 —— 这是迁移完整性的度量，避免「以为
    /// 接完了其实漏了一批」。
    public nonisolated(unsafe) static var unresolved: Set<String> = []
}

extension L10nKey {
    /// 源语言原文 → 键 的索引（由 JSON 预建，见 `LocaleCatalog.buildIndex`）。
    public static var sourceIndex: [String: String] = [:]
}

