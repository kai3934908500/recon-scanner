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
//  ── ⚠️ 已知陷阱：ARView 会**抢走** `ARSession.delegate`（缺口 AV）────────
//
//  这是本文件最危险的一行，单独说明。
//
//  Apple 文档（`ARView.session`）原话：
//
//    "RealityKit automatically creates a default session **that the view
//     manages**. If you have an existing or custom session, setting it as the
//     view's session replaces the default session."
//
//  关键在 **"that the view manages"** —— 视图**管理**它持有的会话，
//  而「管理」包含**接管 `delegate`**。`ARSession.delegate` 是**单槽位**的，
//  谁最后赋值谁生效。
//
//  本工程的资源所有权铁律是：**会话与 delegate 都归 `CaptureSession`**
//  （全设备只允许一个活动会话，且采集必须唯一）。故在把会话交给 `ARView`
//  **之后**，必须让 `CaptureSession` **重新认领** delegate —— 否则：
//
//    · `CaptureSession.session(_:didUpdate:)` 不再被调用 → **零帧进内核**；
//    · 但**画面完全正常**（ARView 自己在渲染），于是表现为
//      「黑屏修好了，却一个关键帧都没扫到」—— 比黑屏难查得多。
//
//  修法是**「保存-恢复」**：在赋值会话**之前**记下当前 delegate，
//  赋值**之后**若发现被改写了就还回去（实现见 `mountSession(into:)`）。
//  它不需要知道 delegate 是谁，故不会在「谁拥有会话」上产生第二个答案 ——
//  归属声明的权威始终在 `CaptureSession`（会话的所有者），本文件只做无损中转。
//
//  时机：`makeUIView`（首次挂载）与 `updateUIView`（视图被复用/重挂）**都要**。
//  `updateUIView` 不是冗余 —— SwiftUI 可能在不重建本 struct 的情况下重建
//  底层 `ARView`（如容器尺寸剧变触发重建），那会再抢一次。
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

    /// 挂载会话，并把被 `ARView` 抢走的 `delegate` 还回去（缺口 AV）。
    ///
    /// ── 为什么是「保存-恢复」而不是「传入 `CaptureSession` 再设」────────────
    ///
    /// 三种可选实现的比较（记录取舍理由，避免后来者"简化"掉这一层）：
    ///
    /// | 方案 | 问题 |
    /// |---|---|
    /// | 本文件内 `view.session.delegate = someCaptureSession` | 需要 import `ui/ios` 层的 `CaptureSession` → **UI 层被平台桥接层反向依赖**（架构方向反了）；且让「谁拥有会话」出现第二个答案 |
    /// | 传闭包 `onSessionMounted: () -> Void` 由调用方认领 | 调用方（`ScanView`）的实现是 `@MainActor` 方法，而本 struct 的 `makeUIView` / `updateUIView` 在 Swift 5 模式下**不是** `@MainActor` → 同步调用会触发 actor 隔离诊断，而本 target 开着 `SWIFT_TREAT_WARNINGS_AS_ERRORS`（**本机无 Xcode，无法编译验证**，不能赌） |
    /// | **保存-恢复（本实现）** | 无：零跨层依赖、零 actor 表面、不关心 delegate 是谁 |
    ///
    /// 「保存-恢复」的语义精确：**"把 ARView 抢走的东西还回去"** —— 它不需要
    /// 知道原来的 delegate 是谁，也就不会对「谁拥有会话」产生任何主张。
    /// 归属声明的**权威**仍在 `CaptureSession`（会话的所有者），本文件只做
    /// 无损的中转。这与「资源所有权铁律」不冲突：本文件不**认领**，只**恢复**。
    ///
    /// ⚠️ `session.delegate` 是 `weak`，保存 `previous` 不会产生循环引用，
    /// 也不会延长任何对象寿命（`CaptureSession` 由控制器强持有）。
    private func mountSession(into view: ARView) {
        // ① 先记下 ARKit 会话当前的 delegate —— 即 `CaptureSession` 自己
        //    （`CaptureSession.init` 里设过，见 CameraCapture.swift）。
        let previous = session.delegate
        // ② 赋值会话。★ 就是这一行会让 `ARView` 接管 delegate。
        view.session = session
        // ③ ARView 若改写了 delegate，立刻还回去。
        //    判等后再赋值：已一致时**不重复赋值**（避免无谓的会话状态同步，
        //    且让"是否真的被抢过"这件事可被断点观察）。
        if session.delegate !== previous {
            session.delegate = previous
        }
    }

    public func makeUIView(context: Context) -> ARView {
        // ⚠️ `automaticallyConfigureSession: false` **不可省**：
        //   为 `true` 时 `ARView` 会自行建立并运行一个世界跟踪会话，
        //   与 `CaptureSession` 的那一个直接冲突（同设备只允许一个活动会话，
        //   表现为画面错乱或黑屏）。这行与 `view.session` 的赋值是一个整体，
        //   删掉任一行都会静默退化成「预览与采集各走一路」。
        //
        // ⚠️ 尺寸用 `(x:1, y:1, width:1, height:1)` 而非 `.zero`：
        //   `.zero` 时 Metal 层拿不到 drawable（社区实测
        //   `[CAMetalLayer nextDrawable] returning nil because allocation failed`），
        //   表现为画面区域全黑/无内容。给一个非零初值后由 Auto Layout 撑开，
        //   是社区通用的规避方式（Apple 未承诺 `.zero` 可用）。
        let view = ARView(frame: .init(x: 1, y: 1, width: 1, height: 1),
                          cameraMode: .ar,
                          automaticallyConfigureSession: false)
        // 挂载会话并把 delegate 还回去（缺口 AV，见 mountSession 注释）。
        mountSession(into: view)
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
        // ★ 缺口 AV：**每次**更新都重新挂载一遍（含 delegate 恢复）。
        //   不是冗余 —— 见文件头：底层 `ARView` 可能在不重建本 struct 的情况下
        //   被重建（那会再抢一次 delegate），而 `UIViewRepresentable` 不保证
        //   `makeUIView` 是唯一入口。`mountSession` 内部判等，无重复开销。
        mountSession(into: uiView)
    }
}
