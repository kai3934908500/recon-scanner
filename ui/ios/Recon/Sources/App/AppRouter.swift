//
//  AppRouter.swift
//  Recon — 栈式导航路由（iOS）
//
//  契约：docs/09_UI规格.md §2「界面清单」（U-01..U-10 与 `route` 列）、
//       §2「硬约束」（U-NAV-1..U-NAV-4）、§5.6（返回/取消必须先弹纯文字确认）。
//
//  U-NAV-1：底部 Tab 栏禁止 —— 本路由刻意不提供 tab 维度。
//  U-NAV-3：U-03 / U-04 / U-06 上的返回手势必须先弹纯文字确认。该约束不靠
//           「记得拦」落实，而是由 `requiresConfirmOnPop` 显式声明，配合
//           ScanView 的拦截实现。
//

import SwiftUI

/// 界面路由（与 docs/09 §2 的 `route` 列一一对应）。
public enum AppRoute: Hashable {
    /// U-01 首页 / 项目列表（根，不入栈）
    case home
    /// U-02 能力探测结果页
    case capability
    /// U-03 扫描界面（自由环绕）
    case scanFree
    /// U-04 定点引导扫描界面（默认）
    case scanGuided
    /// 自检清单界面（`Reviewing` 态）。
    ///
    /// ⚠️ docs/09 §2 的界面清单**未列本页**（缺口 AF，与已修的缺口 AD
    ///    「`Previewing` 元素表漏登记」同构）。其存在性由四处规格共同要求：
    ///    ① §4.1 状态图画它为节点（`│ │ Reviewing │ │ 自检清单 │`）；
    ///    ② §4.2 有五条迁移落在它身上（T-11 进入 / T-16 severe 禁用 /
    ///       T-17 确认重建 / T-18 继续扫描 / T-19 ✕ 取消）；
    ///    ③ docs/03 §7.1.2 给出 CK-1..CK-7 完整呈现规则；
    ///    ④ §5.6 Q-05 给出确认框文案。
    ///    缺了本路由 → `Capturing → Reviewing` 之后的四条出边**没有承载者**，
    ///    状态机彻底卡死、`confirmReconstruct()`（重建唯一入口）零调用。
    ///    判定为「规格表缺一行」（补登记），非「两份契约冲突」（不擅自裁决）。
    case reviewing(sessionID: UUID)
    /// U-06 重建进度界面
    case rebuild(sessionID: UUID)
    /// U-07 重建结果预览
    case result(sessionID: UUID)
    /// U-08 参数面板
    case params
    /// U-09 尺度标定界面
    case calibration
    /// U-10 导出界面
    case export(sessionID: UUID)

    /// 该路由是否要求「返回前必须弹 §5.6 的纯文字确认」（U-NAV-3）。
    public var requiresConfirmOnPop: Bool {
        switch self {
        // `.reviewing` 归入本组：此时采集数据**尚未落盘**（T-19 ✕ 走 §5.6 Q-02
        // 的删帧确认），与 U-03/U-04 同属「返回即可能丢数据」的一类。
        case .scanFree, .scanGuided, .reviewing, .rebuild:
            return true
        case .home, .capability, .result, .params, .calibration, .export:
            // U-NAV-4：U-07 与 U-01 之间允许直接返回（会话已落盘）。
            return false
        }
    }
}

/// 栈式路由。`@MainActor` —— 导航状态只在主线程变更。
@MainActor
public final class AppRouter: ObservableObject {

    /// 导航栈。根为 `.home`（U-01）。
    @Published public var path: [AppRoute] = []

    public init() {}

    // --- 推入 -------------------------------------------------------------

    /// 按当前扫描模式推入对应扫描界面（U-03 / U-04）。
    public func pushScan(guided: Bool) {
        path.append(guided ? .scanGuided : .scanFree)
    }

    public func push(_ route: AppRoute) {
        path.append(route)
    }

    /// 回到首页（U-01）并清空栈。用于 T-25「回到首页」。
    public func popToHome() {
        path.removeAll()
    }

    /// 出栈一层。
    public func pop() {
        if !path.isEmpty { path.removeLast() }
    }

    /// 出栈到指定路由（该路由本身保留）。
    public func popTo(_ route: AppRoute) {
        guard let idx = path.lastIndex(of: route) else { return }
        path.removeSubrange(path.index(after: idx)...)
    }

    /// 替换栈顶（如 U-06 → U-07，避免用户返回到正在进行的重建页）。
    public func replaceTop(with route: AppRoute) {
        if path.isEmpty { path = [route] } else { path[path.count - 1] = route }
    }
}
