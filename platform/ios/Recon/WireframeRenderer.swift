//
//  WireframeRenderer.swift
//  platform/ios/Recon — 线框渲染器（平台桥接层，非 UI 层）
//
//  契约：docs/09_UI规格.md §3.7 U-R-1..U-R-4。
//
//  ── 为什么放在 platform/ios 而不是 ui/ios ──────────────────────────────
//  docs/09 §1 强制约束：**UI 层只读状态、传参数、显示文字与预览**，
//  「禁止在 UI 层做任何几何/图像计算、任何文件 IO 决策」。
//  线框渲染涉及顶点缓冲布局、投影矩阵、GPU 管线 —— 属**渲染计算**，
//  与 `MetalComputeBackend.mm`（同为平台层）同类，故落在 platform/ios。
//  UI 侧（ResultView）只持有一个 `WireframeRenderView` 并喂入几何。
//
//  ── U-R-1：只做线框 ──────────────────────────────────────────────────
//  管线只绘制 `.line` 图元（三角形三条边），不启用光照/阴影/IBL/纹理。
//
//  ── U-R-3：无自动旋转 / 无惯性 / 无阻尼 ───────────────────────────────
//  本渲染器**不持有任何计时器**，矩阵只在用户手势或外部显式设置时更新。
//  `MTKView` 的 `isPaused = true` —— 不让 MetalKit 的显示循环空转驱动
//  「看起来在动」的错觉，也省电。
//
//  ── 为什么用 MTKView + draw(in:) 而不是 CAMetalLayer + 自绘循环 ────────
//  本项目渲染层此前**从未编译过**（MEMORY.md：GPU 路径从未编译/运行）。
//  MTKView 是系统提供的成熟封装，`makeDefaultLibrary()` + `MTLRenderPipelineState`
//  的用法有官方文档兜底，减少「第一次真编译就炸」的面积。

import Foundation
import MetalKit
import simd

/// 线框渲染视图。UI 层直接使用（SwiftUI 经 `UIViewRepresentable` 包一层）。
public final class WireframeRenderView: MTKView {

    /// 与 `shaders/metal/wireframe.metal` 的 `WireframeUniforms` **逐字段对齐**。
    /// 字段顺序与类型不得单边调整（布局错位 = 静默画错，不报错）。
    private struct Uniforms {
        var mvp: simd_float4x4
        var color: simd_float4
        var pointSize: Float
        var _pad0: Float = 0
        var _pad1: Float = 0
        var _pad2: Float = 0
    }

    // MARK: - GPU 对象

    private var pipeline: MTLRenderPipelineState?
    private var vertexBuffer: MTLBuffer?
    private var indexBuffer: MTLBuffer?
    private var indexCount: Int = 0
    private var uniformBuffer: MTLBuffer?
    private let commandQueue: MTLCommandQueue?

    /// 网格外接球（用于相机取景，由几何真实包围盒导出，非臆测）。
    private var boundingCenter = SIMD3<Float>(0, 0, 0)
    private var boundingRadius: Float = 1.0

    /// 当前旋转（弧度）。手势更新它，本类**不**自行推进（U-R-3）。
    private var yaw: Float = 0
    private var pitch: Float = 0
    /// 双指缩放因子（U-R-3 不禁缩放，只禁自动旋转/惯性）。
    private var zoom: Float = 1.0

    /// 线框颜色（读取设计令牌的 accent 值，与 UI 主题一致）。
    private var lineColor = SIMD4<Float>(0.0, 0.478, 1.0, 1.0)

    // MARK: - 初始化

    public override init(frame: CGRect, device: MTLDevice?) {
        let dev = device ?? MTLCreateSystemDefaultDevice()
        self.commandQueue = dev?.makeCommandQueue()
        super.init(frame: frame, device: dev)
        commonInit()
    }

    public required init(coder: NSCoder) {
        self.commandQueue = MTLCreateSystemDefaultDevice()?.makeCommandQueue()
        super.init(coder: coder)
        commonInit()
    }

    private func commonInit() {
        // U-R-3：不让显示循环空转；只在数据/手势变化时重绘。
        isPaused = true
        enableSetNeedsDisplay = true
        // U-R-1：线框在纯黑底上最清晰（bg.preview 两套主题均为纯黑）。
        clearColor = MTLClearColorMake(0, 0, 0, 1)
        colorPixelFormat = .bgra8Unorm
        depthStencilPixelFormat = .depth32Float
        delegate = self

        guard let dev = device else {
            // 无 Metal 设备（模拟器/极旧机型）：不静默留黑，由 UI 侧据
            // `isRenderable == false` 显示如实说明。
            return
        }
        buildPipeline(dev)
    }

    /// 建渲染管线。着色器取自 `default.metallib`（包根，G15/P-4 守护）。
    private func buildPipeline(_ dev: MTLDevice) {
        guard let lib = dev.makeDefaultLibrary() else {
            NSLog("recon: 找不到 default.metallib —— 线框渲染不可用（见 G15 落位判据）")
            return
        }
        guard let vfn = lib.makeFunction(name: "recon_wireframe_vertex"),
              let ffn = lib.makeFunction(name: "recon_wireframe_fragment") else {
            NSLog("recon: default.metallib 缺少 recon_wireframe_* 函数"
                  + " —— shaders/metal/wireframe.metal 是否漏进 build phase（P12）")
            return
        }
        let desc = MTLRenderPipelineDescriptor()
        desc.vertexFunction = vfn
        desc.fragmentFunction = ffn
        desc.colorAttachments[0].pixelFormat = colorPixelFormat
        desc.depthAttachmentPixelFormat = depthPixelFormat
        do {
            pipeline = try dev.makeRenderPipelineState(descriptor: desc)
        } catch {
            NSLog("recon: 线框管线创建失败：\(error)")
        }
        uniformBuffer = dev.makeBuffer(length: MemoryLayout<Uniforms>.stride,
                                       options: .storageModeShared)
    }

    /// 是否可以渲染（UI 据此决定显示线框还是如实说明）。
    public var isRenderable: Bool { pipeline != nil && uniformBuffer != nil }

    // MARK: - 几何装载

    /// 装载预览网格。调用方（UI）负责在 Core 侧取得几何 —— 本类不读任何文件。
    ///
    /// - Parameter positions: Float 数组，XYZ 交错（`recon_preview_mesh.positions`）
    /// - Parameter indices:   UInt32 数组，每 3 个一三角面（`.indices`）
    @discardableResult
    public func load(positions: [Float], indices: [UInt32]) -> Bool {
        guard let dev = device, !positions.isEmpty, !indices.isEmpty else { return false }

        let pBytes = positions.count * MemoryLayout<Float>.size
        let iBytes = indices.count * MemoryLayout<UInt32>.size
        guard let vb = dev.makeBuffer(bytes: positions, length: pBytes,
                                      options: .storageModeShared),
              let ib = dev.makeBuffer(bytes: indices, length: iBytes,
                                      options: .storageModeShared) else {
            NSLog("recon: 线框顶点/索引缓冲分配失败（positionBytes=\(pBytes), indexBytes=\(iBytes)）")
            return false
        }
        vertexBuffer = vb
        indexBuffer = ib
        indexCount = indices.count

        // 从真实几何算外接球（取轴向包围盒的中心与半径）—— 用于相机取景，
        // 保证任意尺度的模型都能完整入画（尺度未知时不会画出画面外）。
        var lo = SIMD3<Float>(repeating: .greatestFiniteMagnitude)
        var hi = SIMD3<Float>(repeating: -.greatestFiniteMagnitude)
        var i = 0
        while i + 2 < positions.count {
            let p = SIMD3<Float>(positions[i], positions[i + 1], positions[i + 2])
            lo = simd_min(lo, p)
            hi = simd_max(hi, p)
            i += 3
        }
        boundingCenter = (lo + hi) * 0.5
        let ext = hi - lo
        // 半径 = 包围盒对角线的一半，至少 1e-4 以免除零（退化几何）。
        boundingRadius = max(simd_length(ext) * 0.5, 1e-4)
        resetView()
        setNeedsDisplay()
        return true
    }

    /// 重置视角（U-R-3：直接赋值，无插值动画）。
    public func resetView() {
        yaw = 0.6
        pitch = 0.5
        zoom = 1.0
    }

    /// 由手势驱动旋转。**不写惯性、不写阻尼**（U-R-3）。
    public func applyRotation(deltaX: Float, deltaY: Float) {
        yaw += deltaX * 0.01
        pitch += deltaY * 0.01
        // 俯仰限位，避免翻转导致视觉失向。
        let limit: Float = .pi / 2 - 0.01
        pitch = min(max(pitch, -limit), limit)
        setNeedsDisplay()
    }

    /// 由手势驱动缩放。
    public func applyZoom(scale: Float) {
        zoom = min(max(zoom * scale, 0.2), 5.0)
        setNeedsDisplay()
    }

    // MARK: - 矩阵

    /// 构造 MVP（U-R-3：**无时间项**）。
    private func makeMVP(aspect: Float) -> simd_float4x4 {
        // 相机距离按外接球半径自适应，保证模型完整入画。
        let distance = boundingRadius * 3.0 / zoom
        let eye = SIMD3<Float>(0, 0, distance)
        let view = simd_float4x4.lookAt(eye: eye, center: .zero,
                                       up: SIMD3<Float>(0, 1, 0))
        let proj = simd_float4x4.perspective(fovy: 45 * .pi / 180,
                                            aspect: aspect, near: 0.01,
                                            far: distance * 4)
        let rot = simd_float4x4.rotationX(pitch) * simd_float4x4.rotationY(yaw)
        // 先把模型中心平移到原点，再旋转、再视图/投影。
        let toOrigin = simd_float4x4.translation(-boundingCenter)
        return proj * view * rot * toOrigin
    }
}

// MARK: - MTKViewDelegate

extension WireframeRenderView: MTKViewDelegate {

    public func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        // 尺寸变化只影响投影矩阵，下一帧重算即可。
        setNeedsDisplay()
    }

    public func draw(in view: MTKView) {
        guard let pipeline,
              let uniformBuffer,
              let vb = vertexBuffer,
              let ib = indexBuffer,
              indexCount > 0,
              let cmd = commandQueue?.makeCommandBuffer(),
              let rpd = currentRenderPassDescriptor,
              let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else {
            return
        }

        let aspect = Float(max(drawableSize.width, 1) / max(drawableSize.height, 1))
        var u = Uniforms(mvp: makeMVP(aspect: aspect),
                         color: lineColor,
                         pointSize: 1)
        memcpy(uniformBuffer.contents(), &u, MemoryLayout<Uniforms>.stride)

        enc.setRenderPipelineState(pipeline)
        enc.setVertexBuffer(vb, offset: 0, index: 0)
        enc.setVertexBuffer(uniformBuffer, offset: 0, index: 1)
        enc.setFragmentBuffer(uniformBuffer, offset: 0, index: 1)
        // U-R-1：只画线框。三角网格的线框 = 每条边画一次。
        enc.drawIndexedPrimitives(type: .line,
                                  indexCount: indexCount,
                                  indexType: .uint32,
                                  indexBuffer: ib,
                                  indexBufferOffset: 0)
        enc.endEncoding()
        cmd.commit()
    }
}

// MARK: - 4x4 辅助

private extension simd_float4x4 {

    static func translation(_ t: SIMD3<Float>) -> simd_float4x4 {
        var m = matrix_identity_float4x4
        m.columns.3 = SIMD4<Float>(t.x, t.y, t.z, 1)
        return m
    }

    static func rotationX(_ a: Float) -> simd_float4x4 {
        let c = cos(a), s = sin(a)
        return simd_float4x4(columns: (
            SIMD4<Float>(1, 0, 0, 0),
            SIMD4<Float>(0, c, s, 0),
            SIMD4<Float>(0, -s, c, 0),
            SIMD4<Float>(0, 0, 0, 1)))
    }

    static func rotationY(_ a: Float) -> simd_float4x4 {
        let c = cos(a), s = sin(a)
        return simd_float4x4(columns: (
            SIMD4<Float>(c, 0, -s, 0),
            SIMD4<Float>(0, 1, 0, 0),
            SIMD4<Float>(s, 0, c, 0),
            SIMD4<Float>(0, 0, 0, 1)))
    }

    static func lookAt(eye: SIMD3<Float>, center: SIMD3<Float>,
                       up: SIMD3<Float>) -> simd_float4x4 {
        let f = simd_normalize(center - eye)
        let s = simd_normalize(simd_cross(f, up))
        let u = simd_cross(s, f)
        return simd_float4x4(columns: (
            SIMD4<Float>(s.x, u.x, -f.x, 0),
            SIMD4<Float>(s.y, u.y, -f.y, 0),
            SIMD4<Float>(s.z, u.z, -f.z, 0),
            SIMD4<Float>(-simd_dot(s, eye), -simd_dot(u, eye),
                         simd_dot(f, eye), 1)))
    }

    /// 右手系透视投影（深度映射到 [0,1]，与 Metal 的 NDC 一致）。
    static func perspective(fovy: Float, aspect: Float,
                            near: Float, far: Float) -> simd_float4x4 {
        let y = 1 / tan(fovy * 0.5)
        let x = y / aspect
        let z = far / (near - far)
        return simd_float4x4(columns: (
            SIMD4<Float>(x, 0, 0, 0),
            SIMD4<Float>(0, y, 0, 0),
            SIMD4<Float>(0, 0, z, -1),
            SIMD4<Float>(0, 0, z * near, 0)))
    }
}
