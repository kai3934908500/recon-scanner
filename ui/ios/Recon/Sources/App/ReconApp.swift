//
//  ReconApp.swift
//  Recon — App 入口（iOS）
//
//  契约：docs/09_UI规格.md §2（导航结构：栈式，无 Tab 栏 U-NAV-1）、
//       §1.2.1（Liquid Glass 仅用于系统级表面 LG-2）、
//       docs/11_平台权限与集成.md（权限顺序）、docs/13（热与电：场景相位处理）。
//
//  导航结构（docs/09 §2）：
//
//      首页 U-01
//       ├─→ U-02 能力探测
//       ├─→ U-08 参数面板
//       ├─→ U-03 / U-04 扫描
//       │     └─→ U-06 重建进度
//       │           └─→ U-07 结果预览
//       │                 ├─→ U-10 导出
//       │                 ├─→ U-08 参数面板
//       │                 └─→ U-01
//       └─→ U-09 尺度标定
//

import SwiftUI

@main
public struct ReconApp: App {
    /// 全局路由（栈式）。U-NAV-1：禁止底部 Tab 栏，故单一 NavigationStack。
    @StateObject private var router = AppRouter()
    /// 内核编排器（C ABI ↔ 采集 ↔ 状态机的唯一接线点）。
    ///
    /// 它内部持有 `ScanSessionModel`；App 只创建这一个实例，避免「两份 session
    /// 让状态分叉」（同 `L10nBootstrap` 的理由）。视图层订阅 `controller.session`。
    @StateObject private var controller = ReconSessionController()

    /// App 生命周期相位回调 —— 驱动 T-14（切后台置暂停）与回前台的 `clearPaused`。
    @Environment(\.scenePhase) private var scenePhase

    public init() {}

    public var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(router)
                .environmentObject(controller)
                // 保持既有视图对 `ScanSessionModel` 的环境依赖不变：注入编排器
                // **内部的那一份**实例（同一对象，不是副本）。
                .environmentObject(controller.session)
                // 竖屏锁定由 Info.plist 声明（platform/ios/Recon/Info.plist）。
                .onChange(of: scenePhase) { phase in
                    handleScenePhase(phase)
                }
        }
    }

    /// 场景相位处理。
    ///
    /// - `.background`：分发 `kAppBackgrounded`（T-14）——状态仍为 `Capturing`，
    ///   但置 `paused`，相机应停止采帧、释放独占。
    /// - `.active`：清除暂停标志。docs/09 §4.2 **未**为该动作定义独立事件
    ///   （状态本就不变），故直接调用 `clearPaused()` 而不伪造事件。
    ///
    /// 经编排器而非直接对状态机发送：这样切后台时**一并**停掉相机帧投递
    /// （`capture.setPaused(true)`），否则状态机暂停而 ARKit 仍在喷帧
    /// ——那正是 T-14 想避免的耗电/发热。
    private func handleScenePhase(_ phase: ScenePhase) {
        switch phase {
        case .background:
            if controller.session.state == .capturing {
                controller.appDidBackground()
            }
        case .active:
            if controller.session.isPaused {
                controller.appWillForeground()
            }
            // P-01/P-02 的闭合条件：用户可能刚在设置里打开相机权限再切回来，
            // 此时必须重查一次，否则界面会停在「没有相机权限」的陈旧状态
            // （详见 `refreshPermissionStateOnForeground()` 的注释）。
            // 放在 `isPaused` 判断**之外** —— 权限被拒时状态机处于 `Failed`，
            // 不会有暂停标志，两者互不覆盖。
            controller.refreshPermissionStateOnForeground()
        default:
            break
        }
    }
}
