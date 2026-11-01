//
//  ScanSessionModel.swift
//  Recon — UI 层的采集会话状态（iOS）
//
//  契约：docs/09_UI规格.md §4.1（UI 状态）、§4.2（迁移表 T-01..T-30）、
//       §4.3（异常态处理路径）、§3.3（扫描界面的可交互规则）。
//
//  ── 铁律遵守方式 ────────────────────────────────────────────────────────
//
//  迁移**规则本体**不写在这里。规则只有一份实现：platform/common/src/
//  capture_state.cpp（C++17，两端共享，已由 tests/unit/test_capture_state.cpp
//  覆盖 T-01..T-30 全量 + 非法迁移 + 终态无出边）。本文件只是它的 Swift 消费者
//  —— 通过 Objective-C++ 桥（CaptureStateBridge）调用同一份规则。
//
//  「同一逻辑写两遍必然漂移」：因此本文件**不得**出现任何 if 分支复刻迁移表。
//  它只做三件事：① 转发事件；② 记录本次迁移命中的规则编号（用于日志/调试）；
//  ③ 按 docs/09 §3.3 的界面规则派生只读的展示态。
//

import Foundation
import SwiftUI

/// 采集会话的 UI 状态与派生展示量。
///
/// `@MainActor`：状态机桥声明为「仅在主线程使用」，故整类固定在主 actor 上，
/// 编译期即排除跨线程误用。
@MainActor
public final class ScanSessionModel: ObservableObject {

    /// 当前 UI 状态（镜像 C++ `CaptureState`）。
    @Published public private(set) var state: ReconCaptureState = .idle
    /// 最近一次迁移命中的规则编号（`T-01`..`T-30` / `—`）。仅用于日志与自检。
    @Published public private(set) var lastRule: String = "—"
    /// 最近一次事件是否被接受（`false` 表示该 (state,event) 组合在 §4.2 中不存在）。
    @Published public private(set) var lastAccepted: Bool = true
    /// 采集暂停变体（T-14 置位，`clearPaused` 清除）。
    @Published public private(set) var isPaused: Bool = false

    /// 已采集帧数（用于 §3.3「已采集 N 帧」与「完成扫描」的禁用规则）。
    @Published public var capturedFrames: Int = 0
    /// 覆盖方位百分比（0...1）。来源：02 §4.7 视角盲区检测的方位直方图。
    @Published public var coverageFraction: Double = 0
    /// 「完成扫描」所需最小帧数（`cap_min_frames_before_finish`，默认 12）。
    @Published public var minFramesBeforeFinish: Int = 12

    private let machine = ReconCaptureStateMachine.shared()

    public init() {
        syncFromMachine()
    }

    // =======================================================================
    // MARK: - 事件转发（唯一入口）
    // =======================================================================

    /// 把事件交给共享状态机，并同步本地镜像。
    ///
    /// 副作用（启动相机、写 manifest、调 `recon_*`）**不在这里**执行 —— 与 C++
    /// 侧的分工一致：状态机只回答「合法吗 / 去哪」，执行由调用方按新状态驱动。
    ///
    /// - Parameter failureStage: 若非 `nil`，在转发前先记录失败阶段，供 T-24
    ///   「按失败阶段重试」正确路由。
    @discardableResult
    public func send(_ event: ReconCaptureEvent,
                     failureStage: ReconFailureStage? = nil) -> ReconCaptureState {
        if let failureStage {
            machine.setFailureStage(failureStage)
        }
        // ⚠️ ObjC 选择子 `- (ReconCaptureState)applyEvent:(ReconCaptureEvent)event;`
        //    导入 Swift 后被 **API 改写**为 `apply(_:)` —— 因为方法基名 `applyEvent`
        //    的 `Event` 后缀与首参类型 `ReconCaptureEvent` 重复，Swift 会去掉它。
        //    写 `machine.applyEvent(event)` 会报
        //    "'applyEvent' has been renamed to 'apply(_:)'"（CI 首次真编暴露）。
        let next = machine.apply(event)
        syncFromMachine(nextState: next)
        return next
    }

    /// App 回到前台时清除暂停标志（docs/09 §4.2 未为该动作定义独立事件，
    /// 因为状态本就不变）。
    public func clearPaused() {
        machine.clearPaused()
        syncFromMachine()
    }

    private func syncFromMachine(nextState: ReconCaptureState? = nil) {
        state = nextState ?? machine.currentState()
        isPaused = machine.isPaused()
    }

    // =======================================================================
    // MARK: - 派生展示态（docs/09 §3.3）
    // =======================================================================

    /// 状态名（与 C++ `StateName` 语义一致，供日志与调试）。
    public var stateName: String {
        switch state {
        case .idle:                 return "Idle"
        case .capabilityProbe:      return "CapabilityProbe"
        case .requestingPermission: return "RequestingPermission"
        case .previewing:           return "Previewing"
        case .capturing:            return "Capturing"
        case .reviewing:            return "Reviewing"
        case .processing:           return "Processing"
        case .completed:            return "Completed"
        case .failed:               return "Failed"
        case .cancelled:            return "Cancelled"
        @unknown default:           return "Unknown"
        }
    }

    /// 当前失败阶段对应的**重试恢复路径**（docs/09 §4.2 T-24 的正文）。
    ///
    /// 仅作 UI 提示用途；实际迁移仍由状态机裁决。
    public var retryTarget: String? {
        guard state == .failed else { return nil }
        switch machine.failureStage() {
        case .camera:         return L10n.t(.retryToCamera)
        case .reconstruction: return L10n.t(.retryToRebuild)
        case .calibration:    return L10n.t(.retryToCalibration)
        case .none:           return nil
        @unknown default:     return nil
        }
    }

    /// §3.3「完成扫描」的禁用规则：
    /// 关键帧 < `cap_min_frames_before_finish` 时禁用，并在下方补一行警告
    /// `至少需要 12 帧，当前 8 帧`。
    public var canFinishScan: Bool {
        capturedFrames >= minFramesBeforeFinish
    }

    /// 上述警告文案；未触发时为 `nil`（U-D-7：不隐藏失败/降级）。
    public var finishScanHint: String? {
        guard !canFinishScan else { return nil }
        return L10n.fill2(.fmtMinFramesHint,
                          String(minFramesBeforeFinish), String(capturedFrames))
    }

    /// 采集暂停时按钮文案变「继续」（§3.3「暂停」行）。
    // 暂停/继续用同一域的成对键（歧义原文「继续」有 3 个键，必须精确指定）。
    public var pauseButtonTitle: String { isPaused ? L10n.t(.scanResume) : L10n.t(.scanPause) }

    /// 是否处于终态（终态无出边，除规格指明的离开动作）。
    public var isTerminal: Bool {
        state == .completed || state == .failed || state == .cancelled
    }

    // =======================================================================
    // MARK: - 文案格式化（§1.4「数字」：等宽 + 千分位；§3.3 文案）
    // =======================================================================

    /// `已采集 128 帧` 中的帧数段（千分位，§1.4）。
    nonisolated public static func frameCountText(_ n: Int) -> String {
        MonoNumber.group(n)
    }

    /// `覆盖方位 63%` 中的百分比段。
    ///
    /// 算法来源（§3.3 末）：百分比 = 已覆盖方位 bin 数 / 36（10° 一格），
    /// 由 02 §4.7 的方位直方图给出。此处只做「比例 → 取整百分数」的呈现。
    nonisolated public static func percentText(_ fraction: Double) -> String {
        let clamped = min(max(fraction, 0), 1)
        return "\(Int((clamped * 100).rounded()))%"
    }
}
