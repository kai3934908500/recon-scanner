// SPDX-License-Identifier: BSD-3-Clause
//
// PlatformCaptureBridge.h — 平台无关采集支撑的 Objective-C++ 门面（给 Swift 用）。
//
// 规则本体在 platform/common/（**跨端共享**）：
//   · IMU 环形缓冲 + 「本帧之前」时间窗切片 → platform/common/src/imu_window.cpp
//   · 深度缓冲单位换算 Float32 米 → uint16 毫米 → platform/common/src/depth_convert.cpp
//
// Swift 无法直接消费 C++，故用一层 Objective-C++ 薄封装暴露。这是 Android 侧
// JNI 桥接（native-lib.cpp / platform-bridge.cpp）的 iOS **对等物** —— 两端功能
// 对等由 tools/check_platform_parity.py 校验。
//
// ── 为什么必须走这一层，而不能在 Swift 里另写一份 ──────────────────────────
//
// 铁律：**同一逻辑写两遍必然漂移**（见 MEMORY.md）。IMU 时间窗的边界语义
// （`<=` 含等于）与深度的取整/钳位规则，错一位都会**静默**改变喂给内核的数据。
// 若 Swift 与 Kotlin 各写一份，任何一侧漂移都无法在编译期发现。
//
// 本层**只做类型搬运**：把 Swift 的标量/缓冲转成 C++ 调用，把 C++ 结果转回
// ObjC 类型。**不含任何判断逻辑** —— 一旦这里长出 `if`，就是漂移的开始。
//
// ── 与 docs/12 §4.2 的关系 ────────────────────────────────────────────────
//
// docs/12 §4.2 规定的混编入口是 `Recon-Bridging-Header.h`（顶层函数声明可见性）
// 与 `module.modulemap`（类型可见性）。本头由 bridging header `#import` 引入 ——
// 与既有的 `CaptureStateBridge.h` 完全同构。

#import <Foundation/Foundation.h>

#include <stdint.h>

NS_ASSUME_NONNULL_BEGIN

/// IMU 时间窗（由 platform/common 的 `ImuRingBuffer::WindowBefore` 产出）。
///
/// 生命周期：由 `ReconImuRingBuffer` 的 `windowBeforeNs:` 返回，持有 C++ 侧
/// `ImuWindow` 的**所有值**（vector 已 move 进本对象），与缓冲本体解耦 ——
/// 因此调用方可以安全地在取出后继续 append 新样本。
@interface ReconImuWindow : NSObject

/// 窗口内样本数。
@property (nonatomic, readonly) NSUInteger count;

/// 指向**连续 `recon_imu_sample` 数组**的首地址（升序）。
///
/// 可直接投递给 C ABI 的 `recon_push_frame`（其 `imu` 参数即
/// `const recon_imu_sample*`），**无需逐样本拷贝** —— 这对 100 Hz 下的
/// `τ_pipeline` 预算（docs/02 §3.7）是必要的。`count == 0` 时为 NULL。
///
/// 生命周期与本对象一致；调用方**不得**在对象释放后使用该指针。
@property (nonatomic, readonly, nullable) const void* samplesRaw;

/// 第 `index` 个样本的时间戳（纳秒）。仅用于诊断（如窗口跨度）。
- (int64_t)timestampNsAtIndex:(NSUInteger)index;

@end

/// IMU 采样环形缓冲（线程安全；规则本体见 platform/common/src/imu_window.cpp）。
@interface ReconImuRingBuffer : NSObject

/// @param capacity 环形容量（样本数）。0 会被规则本体提升为 1。
- (instancetype)initWithCapacity:(NSUInteger)capacity;

/// 追加一个采样。`accel` 必须是**比力（含重力）**m/s² —— 见 recon_c_api.h。
- (void)appendTimestampNs:(int64_t)tsNs
                   accelX:(double)ax accelY:(double)ay accelZ:(double)az
                    gyroX:(double)gx gyroY:(double)gy gyroZ:(double)gz;

/// 取「`tsNs` 之前（含等于）」的窗口，并把已消费样本移出缓冲。
- (ReconImuWindow *)windowBeforeNs:(int64_t)tsNs;

/// 清空缓冲并归零诊断计数。
- (void)reset;

/// 当前缓冲样本数。
@property (nonatomic, readonly) NSUInteger count;
/// 因容量上限丢弃的样本累计数（诊断用）。
@property (nonatomic, readonly) NSUInteger droppedByCapacity;
/// 时间戳倒序次数（诊断用；docs/02 §3.8 F-10）。
@property (nonatomic, readonly) NSUInteger timestampRegressions;

@end

/// 深度单位换算结果（平台无关数学见 platform/common/src/depth_convert.cpp）。
@interface ReconDepthMM : NSObject

/// 换算是否成功（入参非法时为 NO，**调用方必须处理**）。
@property (nonatomic, readonly) BOOL ok;
@property (nonatomic, readonly) uint32_t width;
@property (nonatomic, readonly) uint32_t height;
/// 有效像素数（深度非零，含触顶像素）。
@property (nonatomic, readonly) NSUInteger validPixelCount;
/// 被上限夹取的像素数（是 validPixelCount 的子集）。
@property (nonatomic, readonly) NSUInteger truncatedPixelCount;
/// 行优先**紧密排列**的 uint16 毫米。字节长度 == width × height × 2。
@property (nonatomic, readonly, copy) NSData *millimeters;

@end

/// 深度缓冲换算门面。
@interface ReconDepthConverter : NSObject

/// 把「单平面 Float32、单位米、沿光轴 z」的深度缓冲换算为 uint16 毫米。
///
/// @param src          源缓冲首地址（`kCVPixelFormatType_DepthFloat32`）。
/// @param width        宽度（像素）。
/// @param height       高度（像素）。
/// @param strideBytes  源缓冲每行**字节数**（可能带行填充）。
/// @return 换算结果；`ok == NO` 表示入参非法。
+ (ReconDepthMM *)millimetersFromFloat32Meters:(const void *)src
                                        width:(uint32_t)width
                                       height:(uint32_t)height
                                  strideBytes:(NSUInteger)strideBytes;

@end

NS_ASSUME_NONNULL_END
