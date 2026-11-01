//
//  PermissionCoordinator.swift
//  Recon — 权限申请时序（iOS）
//
//  契约：docs/11_平台权限与集成.md §1.1（权限清单）、§1.2（用途文案原文）、
//       §1.3（申请时序）、IOS-P-1..IOS-P-4、docs/09_UI规格.md §5.7（P-01..P-04）。
//
//  ── 三条必须照做的规则 ─────────────────────────────────────────────────
//  IOS-P-1：权限只在**用户触发相关功能时**申请，不在启动时申请。
//           相机 → 首页点「开始扫描」后立即申请（U-01 → U-02 之前）。
//           相册 → 仅在 U-10 点「导出到相册」时申请（首次）。
//  IOS-P-2：相机被拒时**不自动反复申请**；提示行内含「去设置」按钮。
//  IOS-P-4：拒绝权限**不改变**管线分支（除相机被拒外）。运动权限在 iOS 侧
//           **不存在被拒场景** —— 见下。
//
//  ── 运动权限为什么没有「被拒」分支 ─────────────────────────────────────
//  §1.2 明确：使用 `CMDeviceMotion` / `CMGyro` / `CMAccelerometer` 时系统
//  **不要求用户授权**；`NSMotionUsageDescription` 只是向审核方与用户说明用途。
//  本产品**不使用** `CMMotionActivityManager` / `CMPedometer`，因此不会出现
//  「运动与健身」授权弹窗。故本协调器对运动权限**不提供** request 接口 ——
//  这不是遗漏，而是规格如此。状态机里的 `kMotionPermissionDenied`（T-07）
//  在 iOS 侧实际不可达（Android 侧可达）。
//

import Foundation
import AVFoundation
import Photos
import UIKit

/// 权限协调器。`@MainActor`：授权回调须回到主线程更新 UI 状态机。
@MainActor
public final class PermissionCoordinator {

    public enum Outcome: Equatable {
        case granted
        case denied
    }

    public init() {}

    // =======================================================================
    // MARK: - 相机（IOS-P-1：用户点「开始扫描」后立即申请）
    // =======================================================================

    /// 申请相机权限。
    ///
    /// - 若已是 `authorized` → 直接返回 `.granted`，**不弹窗**（§1.2 注：
    ///   「已在会话中不重复申请」）。
    /// - 若已 `denied`/`restricted` → 直接返回 `.denied`，**不重复申请**
    ///   （IOS-P-2）；UI 应显示 §5.7 P-01 + P-02「去设置」。
    /// - 仅在 `notDetermined` 时才真正弹系统授权框。
    public func requestCamera() async -> Outcome {
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            return .granted
        case .notDetermined:
            let ok = await AVCaptureDevice.requestAccess(for: .video)
            return ok ? .granted : .denied
        case .denied, .restricted:
            // IOS-P-2：不自动反复申请。
            return .denied
        @unknown default:
            return .denied
        }
    }

    // =======================================================================
    // MARK: - 相册（IOS-P-1：仅在 U-10 点「导出到相册」时申请）
    // =======================================================================

    /// 申请相册**添加**权限。
    ///
    /// 注意用的是 `.addOnly`（对应 `NSPhotoLibraryAddUsageDescription`），
    /// **不是** `.readWrite`（那需要 `NSPhotoLibraryUsageDescription`，
    /// 而 §1.1 明确「否」）。系统弹窗为「添加」而非「读取」。
    public func requestPhotoAdd() async -> Outcome {
        let status = PHPhotoLibrary.authorizationStatus(for: .addOnly)
        switch status {
        case .authorized, .limited:
            return .granted
        case .notDetermined:
            let s = await PHPhotoLibrary.requestAuthorization(for: .addOnly)
            return (s == .authorized || s == .limited) ? .granted : .denied
        case .denied, .restricted:
            return .denied      // IOS-P-2：不反复申请
        @unknown default:
            return .denied
        }
    }

    // =======================================================================
    // MARK: - 运动（无申请接口，见文件头说明）
    // =======================================================================

    /// iOS 侧运动数据**无需授权**（§1.2）。此常量用于在 UI/日志中显式表达
    /// 「运动权限不存在被拒场景」，避免后续维护者误以为漏了申请逻辑。
    public static let motionRequiresAuthorization = false

    // =======================================================================
    // MARK: - 跳转设置（IOS-P-2 / §5.7 P-02）
    // =======================================================================

    /// 跳系统设置页。对应 §5.7 P-02「去设置」按钮。
    public func openAppSettings() {
        guard let url = URL(string: UIApplication.openSettingsURLString) else { return }
        UIApplication.shared.open(url)
    }

    // =======================================================================
    // MARK: - 状态重查（P-01/P-02 这条链的闭合条件）
    // =======================================================================

    /// 当前相机授权是否已生效（不弹窗、不申请，只读）。
    ///
    /// 用途：用户从设置页改完权限回到 App 时重查一次 —— 否则界面会停在
    /// 「没有相机权限」的陈旧状态，一直骗到重启为止（详见
    /// `ReconSessionController.refreshPermissionStateOnForeground()`）。
    ///
    /// `nonisolated`：`AVCaptureDevice.authorizationStatus` 是**线程安全**的
    /// 纯查询，不触碰本类的任何 MainActor 状态，故不引入 actor 跳转；
    /// 这与本项目在 `MonoNumber` 上用过同一手法（见 Components.swift）。
    nonisolated public func cameraAuthorizationGranted() -> Bool {
        AVCaptureDevice.authorizationStatus(for: .video) == .authorized
    }
}
