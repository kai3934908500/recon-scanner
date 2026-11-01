// SPDX-License-Identifier: BSD-3-Clause
//
// PlatformCaptureBridge.mm — 平台无关采集支撑的 Objective-C++ 门面实现。
//
// 直接复用 platform/common/src/{imu_window,depth_convert}.cpp 的 C++ 实现，
// 保证 iOS 与 Android 两端的 IMU 时间窗语义与深度单位换算规则**同源**
// （铁律：同一逻辑写两遍必然漂移）。
//
// 本文件**不含任何判断逻辑** —— 只做类型搬运。任何 `if` 都是漂移的开始，
// 规则请在 platform/common 里改。

#import "PlatformCaptureBridge.h"

#include <cstring>
#include <utility>
#include <vector>

#include "recon_platform/depth_convert.h"
#include "recon_platform/imu_window.h"

using recon::platform::ConvertDepthFloat32MetersToMM16;
using recon::platform::ImuRingBuffer;
using recon::platform::ImuWindow;

// ===========================================================================
// ReconImuWindow
// ===========================================================================

@interface ReconImuWindow () {
    std::vector<recon_imu_sample> _samples;
}
@end

@implementation ReconImuWindow

- (instancetype)initWithWindow:(ImuWindow &&)win {
    self = [super init];
    if (self) {
        // move：避免 100 Hz 下每帧复制整窗（τ_pipeline 预算，docs/02 §3.7）。
        _samples = std::move(win.samples);
    }
    return self;
}

- (NSUInteger)count {
    return static_cast<NSUInteger>(_samples.size());
}

- (const void *)samplesRaw {
    // 空窗必须返回 NULL（C ABI 的 `imu` 可为 NULL，且语义更清晰）。
    return _samples.empty() ? nullptr : static_cast<const void *>(_samples.data());
}

- (int64_t)timestampNsAtIndex:(NSUInteger)index {
    if (index >= _samples.size()) {
        return 0;
    }
    return static_cast<int64_t>(_samples[index].timestamp_ns);
}

@end

// ===========================================================================
// ReconImuRingBuffer
// ===========================================================================

@interface ReconImuRingBuffer () {
    ImuRingBuffer _buffer;
}
@end

@implementation ReconImuRingBuffer

- (instancetype)initWithCapacity:(NSUInteger)capacity {
    self = [super init];
    if (self) {
        _buffer = ImuRingBuffer(static_cast<std::size_t>(capacity));
    }
    return self;
}

- (void)appendTimestampNs:(int64_t)tsNs
                   accelX:(double)ax accelY:(double)ay accelZ:(double)az
                    gyroX:(double)gx gyroY:(double)gy gyroZ:(double)gz {
    recon_imu_sample s;
    std::memset(&s, 0, sizeof(s));
    s.timestamp_ns = tsNs;
    s.accel[0] = ax; s.accel[1] = ay; s.accel[2] = az;
    s.gyro[0] = gx;  s.gyro[1] = gy;  s.gyro[2] = gz;
    _buffer.Append(s);
}

- (ReconImuWindow *)windowBeforeNs:(int64_t)tsNs {
    ImuWindow win = _buffer.WindowBefore(tsNs);
    return [[ReconImuWindow alloc] initWithWindow:std::move(win)];
}

- (void)reset {
    _buffer.Reset();
}

- (NSUInteger)count {
    return static_cast<NSUInteger>(_buffer.Count());
}

- (NSUInteger)droppedByCapacity {
    return static_cast<NSUInteger>(_buffer.DroppedByCapacity());
}

- (NSUInteger)timestampRegressions {
    return static_cast<NSUInteger>(_buffer.TimestampRegressions());
}

@end

// ===========================================================================
// ReconDepthMM
// ===========================================================================

@interface ReconDepthMM ()
@property (nonatomic, readwrite) BOOL ok;
@property (nonatomic, readwrite) uint32_t width;
@property (nonatomic, readwrite) uint32_t height;
@property (nonatomic, readwrite) NSUInteger validPixelCount;
@property (nonatomic, readwrite) NSUInteger truncatedPixelCount;
@property (nonatomic, readwrite, copy) NSData *millimeters;
@end

@implementation ReconDepthMM
@end

// ===========================================================================
// ReconDepthConverter
// ===========================================================================

@implementation ReconDepthConverter

+ (ReconDepthMM *)millimetersFromFloat32Meters:(const void *)src
                                        width:(uint32_t)width
                                       height:(uint32_t)height
                                  strideBytes:(NSUInteger)strideBytes {
    ReconDepthMM *out = [[ReconDepthMM alloc] init];
    out.ok = NO;
    out.millimeters = [NSData data];

    const auto* fsrc = static_cast<const float *>(src);
    const auto r = ConvertDepthFloat32MetersToMM16(
        fsrc, width, height, static_cast<std::size_t>(strideBytes));
    if (!r.ok) {
        return out;
    }

    out.ok = YES;
    out.width = r.frame.width;
    out.height = r.frame.height;
    out.validPixelCount = static_cast<NSUInteger>(r.frame.valid_pixel_count);
    out.truncatedPixelCount = static_cast<NSUInteger>(r.frame.truncated_pixel_count);
    const NSUInteger bytes =
        static_cast<NSUInteger>(r.frame.millimeters.size()) * sizeof(uint16_t);
    out.millimeters = [NSData dataWithBytes:r.frame.millimeters.data()
                                     length:bytes];
    return out;
}

@end
