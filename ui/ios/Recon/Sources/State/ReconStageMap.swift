//
//  ReconStageMap.swift
//  Recon — C ABI `recon_stage` → UI §3.6.1 的「14 段」映射（iOS）
//
//  契约：docs/09_UI规格.md §3.6.1（14 段阶段标题取值表）、
//       docs/01_接口契约.md §1（`recon_stage` 枚举）、
//       core/src/orchestrator/orchestrator.cpp 的 `kStages[]`（**实际上报的阶段序**）。
//
//  ── 为什么需要这一层（一个必须如实说明的契约缺口）─────────────────────────
//
//  §3.6.1 规定 UI 显示「阶段 N/14」，但 **C ABI 的 `recon_progress_cb` 只上报
//  `recon_stage` 的枚举值**，而编排层 `kStages[]` 实际只跑 **8 个阶段**：
//
//      PREPROCESS(1) / VIO(2) / MVS(5) / RGBD_FUSION(6)
//      MESH(7) / POSTPROC(8) / UV_TEXTURE(9) / SCALE(10)
//
//  即：**规格是 14 段的细粒度，实现是 8 段的粗粒度。** 二者不同构。
//
//  ── 本层刻意不做的事 ────────────────────────────────────────────────────
//
//  **不插值伪造**。曾考虑过「用 (stage, 全局 progress) 在组内插值，凑出 14 段
//  的连续推进」—— 但那需要知道每个组的 [lo, hi] 边界，而该区间是编排层
//  内部的 `kStages[].lo/hi`，**没有经 C ABI 暴露**。用硬编码的区间去凑，
//  等于把编排层的私有常量**复制成第二份事实来源**（本项目反复吃过这个亏：
//  「同一逻辑写两遍必然漂移」，见 MEMORY.md）。且一旦编排层调整区间权重，
//  这里就会静默错位。
//
//  因此本层的语义是：**C ABI 上报到哪个阶段，就显示该阶段所在组的「入口段号」**。
//  进度条仍按 C ABI 给的全局 progress 连续推进（那是真实数据），
//  只是段号会在组边界跳变 —— **跳变是诚实的，插值是伪造的**。
//
//  ── 组 → 段的对应（14 段中 8 段可显示）─────────────────────────────────
//
//      C ABI 阶段          §3.6.1 段（入口段加粗）
//      PREPROCESS(1)   →   **1** 读取采集数据
//      VIO(2)          →   **2** 特征匹配与几何验证 · 5 全局优化
//      MVS(5)          →   **3** 稠密匹配
//      RGBD_FUSION(6)  →   **4** 深度融合
//      MESH(7)         →   **6** 输入准备 · 7 离群点剔除 · 8 网格裁剪
//      POSTPROC(8)     →   **9** 非流形边修复 · 10 孔洞缝合 · 11 法向统一与规整
//      UV_TEXTURE(9)   →   **12** UV 展开 · 13 纹理烘焙 · 14 纹理校验与简化
//      SCALE(10)       →   （§3.6.1 未列，独立于 14 段）
//
//  **不可达段（6 段）：5 / 7 / 8 / 10 / 11 / 13** —— 它们各自是某个 C ABI 组
//  的**非入口子步骤**，实现未单独上报。要真正显示 14 段，须在 C ABI 层扩展
//  `recon_stage` 或新增「子阶段」上报 —— 那是 core 侧的契约变更，
//  **不是 UI 能单方面解决的**。缺口登记见 `contractGaps`。
//

import Foundation

/// C ABI 阶段号（与 `recon_stage` 逐值对应）。
///
/// 刻意**只列编排层实际上报的 8 个**，外加终态三值。
/// 不列 `RECON_STAGE_SFM(3)` / `RECON_STAGE_BUNDLE(4)` —— 枚举里有、编排层
/// 当前不上报；列了会让「哪些阶段真的会来」变得模糊。
public enum ReconStage: Int32, CaseIterable {
    case idle       = 0
    case preprocess = 1
    case vio        = 2
    case mvs        = 5
    case rgbdFusion = 6
    case mesh       = 7
    case postproc   = 8
    case uvTexture  = 9
    case scale      = 10
    case export     = 11
    case done       = 12
    case failed     = 13
    case cancelled  = 14

    /// 由 C ABI 回传的原始 `int32` 构造。未知值返回 `nil`
    /// —— **不静默归到某个阶段**（那会让「实现新增了阶段」被掩盖）。
    public init?(abi raw: Int32) {
        self.init(rawValue: raw)
    }

    /// 该阶段是否是编排层 `kStages[]` 中真正会被执行的 8 个之一。
    /// （终态三值不属于「执行阶段」。）
    public var isExecutedStage: Bool {
        switch self {
        case .preprocess, .vio, .mvs, .rgbdFusion,
             .mesh, .postproc, .uvTexture, .scale:
            return true
        default:
            return false
        }
    }
}

/// 阶段映射表（唯一事实来源）。
public enum ReconStageMap {

    /// UI §3.6.1 的总段数。
    public static let uiSegmentCount = 14

    /// C ABI 阶段 → UI 段号（1..14）。`nil` 表示该阶段不映射到任何前进段
    /// （`idle` 是起点；`export` 与 `scale` 不在 §3.6.1 的 14 段内；
    /// 终态由 `Outcome` 另行表达）。
    ///
    /// 取值依据：**每组取 §3.6.1 中语义最贴近且为该组首段的那一段**。
    /// 组内的其余段（见文件头的组→段表）当前不可达。
    public static func uiSegment(for stage: ReconStage) -> Int? {
        switch stage {
        case .idle:        return nil
        case .preprocess:  return 1     // 读取采集数据
        case .vio:         return 2     // 特征匹配与几何验证（组入口）
        case .mvs:         return 3     // 稠密匹配
        case .rgbdFusion:  return 4     // 深度融合
        case .mesh:        return 6     // 输入准备（组入口）
        case .postproc:    return 9     // 非流形边修复（组入口）
        case .uvTexture:   return 12    // UV 展开（组入口）
        case .scale:       return nil   // §3.6.1 未列
        case .export:      return nil   // §3.6.1 未列
        case .done:        return 14    // 纹理校验与简化（末段）
        case .failed, .cancelled: return nil
        }
    }

    /// 当前实现**能**显示在「阶段 N/14」里的段号集合（8 段）。
    ///
    /// 恒等于「各 C ABI 组的入口段」∪ {14}。
    /// **与文件头的组→段表逐条对应** —— 改了这里必须同步改那段注释，
    /// 并由 `tools/check_ios_contract.py` 的新判据守住一致性。
    public static let reachableSegments: Set<Int> = [1, 2, 3, 4, 6, 9, 12, 14]

    /// §3.6.1 中**当前不可达**的段号（6 段）。
    public static var unreachableSegments: [Int] {
        (1...uiSegmentCount).filter { !reachableSegments.contains($0) }
    }

    public static func isReachableSegment(_ index: Int) -> Bool {
        reachableSegments.contains(index)
    }

    /// 前进段的**顺序**（升序）。用于把「段号回退」识别为异常。
    public static var orderedReachableSegments: [Int] {
        reachableSegments.sorted()
    }

    /// 契约缺口登记（供闸门与文档引用；**不是**运行期逻辑）。
    ///
    /// 每项：段号 + 所属 C ABI 组 + 缺口原因**代号**。
    ///
    /// ── 为什么这里不写段名、也不写中文原因（真实踩过的坑）────────────────
    ///
    /// 初版在此**手抄**了 6 个段名（`"全局优化"` 等）与 6 条中文原因。两个问题：
    ///
    ///   1. **段名有了第二份来源**：14 段名的唯一来源是 `RebuildStage.name(_:)`
    ///      （`Screens/RebuildView.swift`，其内部经 `L10n.*` 取值）。在这里再抄
    ///      一遍 = 「同一逻辑写两遍必然漂移」（MEMORY.md 铁律）—— 将来改了段名，
    ///      这里会静默过期。
    ///   2. **不该本地化的字符串被写成裸中文**：这些段名既要显示给用户（须走
    ///      `L10n`），又在此被当作数据手抄 → `check_ios_contract.py` 的 G9
    ///      （UI 文案去硬编码）当场拦住 12 段中文。这与第十三轮
    ///      `GlassRole.specNote` 是同一类错误。
    ///
    /// 现在：**段名一律经 `RebuildStage.name(segment)` 运行期派生**（单一来源）；
    /// 本表只保留**非用户可见**的结构化数据（段号 / ABI 组 / 原因代号）。
    /// 原因代号用 ASCII 标识符，供闸门、日志与文档交叉引用，不参与本地化；
    /// 其含义**只记在注释里**（注释会被 G9 剥离），不进入运行期字符串。
    ///
    /// 原因代号：
    ///   `ba_inside_vio`      —— BA 在 VIO/SfM 内完成，未单独上报
    ///   `inside_mesh`        —— 在 MESH 组内完成，未单独上报
    ///   `inside_postproc`    —— 在 POSTPROC 组内完成，未单独上报
    ///   `inside_uv_texture`  —— 在 UV_TEXTURE 组内完成，未单独上报
    ///
    /// 当 C ABI 支持「子阶段上报」后，应逐项删除并把段号移入 `reachableSegments`。
    public static let contractGaps: [(segment: Int, abiGroup: String, reasonCode: String)] = [
        (5,  "VIO(2)",        "ba_inside_vio"),
        (7,  "MESH(7)",       "inside_mesh"),
        (8,  "MESH(7)",       "inside_mesh"),
        (10, "POSTPROC(8)",   "inside_postproc"),
        (11, "POSTPROC(8)",   "inside_postproc"),
        (13, "UV_TEXTURE(9)", "inside_uv_texture"),
    ]

    /// 缺口的**段名**（派生自唯一来源 `RebuildStage`，不在此手抄）。
    public static func gapSegmentName(_ gapIndex: Int) -> String {
        guard contractGaps.indices.contains(gapIndex) else { return "" }
        return RebuildStage.name(contractGaps[gapIndex].segment)
    }

    /// 自检：`contractGaps` 的段号集合必须**恰好**等于 `unreachableSegments`。
    ///
    /// 这是「一份数据 + 一处派生」的守护 —— 两份手抄列表必然漂移
    /// （见 MEMORY.md「第二条事实来源必然漂移——手抄汇总表也不例外」）。
    /// 由 `tools/check_ios_contract.py` 的 G11 在**源码层**校验同一约束，
    /// 此处提供运行期断言，使两端都断得住。
    public static func selfCheck_gapsMatchUnreachable() -> Bool {
        Set(contractGaps.map(\.segment)) == Set(unreachableSegments)
    }
}
