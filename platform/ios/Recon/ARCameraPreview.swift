//
//  ARCameraPreview.swift
//  Recon — 相机实时预览视图（SwiftUI ↔ ARKit 桥接）
//
//  契约：docs/09_UI规格.md §3.3（U-03 预览层）、§3.4（U-04）、§1.5（预览区
//       letterbox 规则）；docs/02_采集与同步.md §5.5 F-1（RGB 与深度同帧同源）。
//
//  ── 这个文件解决的唯一问题（2026-11-02，用户真机反馈）─────────────────────
//
//  真机进扫描页只有**纯黑**。相机其实一直在采（`ARSessionDelegate` 正常回调、
//  帧也一直在往内核投），只是仓内**从来没有任何视图渲染过它**——
//  `ScanView.previewArea` 的注释写的是「此处留出接口，不伪造画面」，而那个
//  接口一直空着。本文件就是把它接上。
//
//  ── 为什么用 RealityKit 的 `ARView`，而不是 ARKit 的 `ARSCNView` ──────────
//
//  三条理由，任一条都足以否决 `ARSCNView`：
//
//   ① **`ARSCNView.session` 是只读的**（`var session: ARSession { get }`）。
//      它**无法**挂载我们自己的 `ARSession` —— 只能用它自己造的那个，并用
//      它自己的 `run(configuration)` 启动。那样就会开出**第二个**会话：一份
//      没有 `.sceneDepth`、没有 `.gravity` 的配置，于是「屏幕上看到的画面」
//      与「实际送进内核的帧」来自两条独立管线（位姿与时间戳都不一致）。
//      这正是 02 §5.5 F-1 明令禁止的形态。
//      `ARView.session` 是**可写**的（`var session: ARSession? { get set }`），
//      配合 `automaticallyConfigureSession: false` 就能挂载采集层那**同一个**
//      会话实例 —— 屏幕上看到的就是正在采的那一路。
//
//   ② **SceneKit 已于 WWDC25 被 Apple 软弃用**（iOS 8.0–26.0 Deprecated，
//      官方迁移文档《Bringing your SceneKit projects to RealityKit》）。
//      而本工程的容器开着 `SWIFT_TREAT_WARNINGS_AS_ERRORS: YES`
//      （project.yml），弃用警告会**直接让 CI 构建失败**。用 `ARSCNView`
//      等于引入一条一旦 CI 升到 Xcode 26 就必然断掉的路径。
//
//   ③ 本产品**不渲染任何虚拟内容**：预览区就是相机画面本身，3D 结果另在
//      U-07 用 `WireframeRenderer` 呈现。RealityKit 在这里只当「相机画面的
//      显示容器」用，其 ECS 与资产格式（USD）都不参与 —— 故迁移零成本。
//
//  ── 不做的事（刻意，各有理由）──────────────────────────────────────────
//
//   · **不 `run()` 任何配置**。会话的生命周期完全归 `CaptureSession` 所有
//     （`start` / `setPaused` / `stop`）。预览视图只**消费**它，绝不驱动它 ——
//     否则「谁在配置会话」就有两个来源（本项目反复踩的漂移形态）。
//   · **不添加任何锚点/实体/手势**。预览区不是可交互的 3D 场景。
//   · **不做画面裁剪**。缩放与 letterbox 由 SwiftUI 侧的 `.aspectRatio` 决定，
//     本视图只负责「把 buffer 铺满给它的 frame」。
//

import SwiftUI
import RealityKit
import ARKit

/// 相机实时预览视图（把采集层的 `ARSession` 画面渲染出来）。
///
/// 用法（`ScanView.previewArea`）：
/// ```swift
/// ARCameraPreview(session: controller.arSession)
///     .aspectRatio(previewAspect, contentMode: .fit)
/// ```
public struct ARCameraPreview: UIViewRepresentable {

    /// **采集层持有的那一个**会话实例（`CaptureSession.arSession`）。
    ///
    /// 必须是同一个实例，理由见文件头 ①。传一个新的 `ARSession()` 会得到黑屏
    /// （没人 `run` 它）或第二个冲突会话。
    public let session: ARSession

    public init(session: ARSession) {
        self.session = session
    }

    public func makeUIView(context: Context) -> ARView {
        // ⚠️ `automaticallyConfigureSession: false` **不可省**：
        //   为 `true` 时 `ARView` 会自行建立并运行一个世界跟踪会话，
        //   与 `CaptureSession` 的那一个直接冲突（同设备只允许一个活动会话，
        //   表现为画面错乱或黑屏）。这行与下一行的 session 赋值是一个整体，
        //   删掉任一行都会静默退化成「预览与采集各走一路」。
        let view = ARView(frame: .zero,
                          cameraMode: .ar,
                          automaticallyConfigureSession: false)
        // 挂载采集层的**同一个**会话（不是新建）。
        view.session = session
        // 极简画面：关掉 RealityKit 的相机后处理（颗粒/运动模糊）。
        // 理由：① §1.1 U-D-4「无浮夸视觉」；② 让预览尽量接近内核实际收到的
        // 那一帧 —— 后处理只影响**显示**（送内核的是 `ARFrame.capturedImage`
        // 原始缓冲，不经 RealityKit 渲染），故这里纯属「所见即所采」的诚实性，
        // 与几何精度无关，不是精度参数。
        view.renderOptions.insert(.disableCameraGrain)
        view.renderOptions.insert(.disableMotionBlur)
        view.isUserInteractionEnabled = false   // 预览不接收手势（见文件头）
        return view
    }

    public func updateUIView(_ uiView: ARView, context: Context) {
        // 会话实例是**稳定**的（`CaptureSession` 持有同一个，不重建），
        // 故正常路径下这里无事可做。
        // 仍保留这段「不一致就纠正」的兜底：若某天会话被替换（例如 App 从
        // 后台回前台重建了采集栈），不纠正就会静默显示**旧会话**的画面 ——
        // 一个界面看起来正常、实际采的是另一路的错误，比黑屏难查得多。
        if uiView.session !== session {
            uiView.session = session
        }
    }
}
