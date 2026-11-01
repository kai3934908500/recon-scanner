// SPDX-License-Identifier: BSD-3-Clause
//
// MetalComputeBackend.mm — iOS YUV→BGRA8 转换实现（GPU 主路径 + CPU 回退）。
//
// 契约与设计说明见同目录 MetalComputeBackend.h 的文件头。此处只记**实现**要点。
//
// ── 三条必须写对的细节（每条都曾是本项目「静默失效」的形态）───────────────
//
// ① **平面缓冲必须用 `GetBaseAddressOfPlane`，不能用 `GetBaseAddress`**。
//    `ARFrame.capturedImage` 是 `420YpCbCr8BiPlanarFullRange` 双平面缓冲，
//    `CVPixelBufferGetBaseAddress` 对它返回 **NULL**。旧实现
//    （`ReconSessionController.pushFrameToCore`）正是用了后者 + `guard else { return }`
//    → **静默丢弃 100% 的帧**，`pushedFrames` 恒 0 而 UI 显示「采集中」
//    （docs/14 §5.1.3 缺口 J）。本文件**绝不**调用 `GetBaseAddress`
//    处理 `capturedImage` —— 该纪律由 `check_ios_contract.py` 的 G14-a 静态守护
//    （判据落在**调用点**上，且实参名必须在 `PLANAR_BUFFER_HINTS` 清单内）。
//
// ② **两个平面的 stride 可能带行填充，且互不相同**。
//    `GetBytesPerRowOfPlane(buf, 0)` 是 Y 的行字节数，`(buf, 1)` 是 CbCr 的。
//    Y 常见 1080p → 1920（无填充），但**不可假设**（某些设备会 align 到 64/128）。
//    内核按显式 stride 取像素，故两处都要传对；传错会让从第 2 行起**整体斜移**。
//
// ③ **色彩矩阵必须按 attachment 选，不能写死 BT.601**。
//    ARKit 在 1080p 上给的是 `kCVImageBufferYCbCrMatrix_ITU_R_709_2`。
//    若写死 601，红色会偏（709 的 kr=0.2126 vs 601 的 0.299），**不报错**。
//    故此处读 `CVPixelBufferGetAttachment(buf, kCVImageBufferYCbCrMatrix, …)`；
//    读不到时**不猜** —— 用 601 并在 `fallbackReason` 里记「矩阵未知，按 601」，
//    让上层能看到这个假设（不静默）。
//
// ── GPU 路径为什么要走 CVMetalTextureCache 而不是自己建纹理 ──────────────
// `CVMetalTextureCacheCreateTextureFromImage` 让 CoreVideo 直接为 `CVPixelBuffer`
// 建一个 **IOSurface 背衬的 MTLTexture 视图**（零拷贝）。若自己
// `newBufferWithBytes` + `texture2D` 上传，就要每帧多拷 3 MB（1080p YUV420
// ≈ 3.1 MB）——那正好把 GPU 转换省下的时间又还回去了。
//
// ── 为什么输出用 `MTLStorageModeShared` 而不是 Private ────────────────────
// 输出需要被 CPU 读回（`recon_push_frame` 是 C ABI，吃的是 CPU 内存）。
// `Shared` 在 Apple Silicon / iOS 上即统一内存，读回无需 blit
// （`getBytes` 直接可用）。若用 `Private` 就必须再加一次 blit 到 `Shared`
// 缓冲 —— 那是纯粹的浪费。
//
// ── ⚠️ 未经编译 ───────────────────────────────────────────────────────────
// 本机 Windows / 无 Xcode。以下代码**从未被编译过**，也**从未运行过**。
// 语言层正确性只有人工复核与静态判据；GPU 路径的性能（docs/11 的 < 0.4 ms）
// 是**目标值而非实测值**。

#import "MetalComputeBackend.h"

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <cstdint>
#include <cstring>
#include <string>   // 显式的自身依赖：本文件用 std::string（MatrixFromAttachment 的
                    // reason 出参）。此前只靠 <vector> → <stdexcept> → <string> 的
                    // **传递包含**拿到，属未声明依赖 —— 换 SDK 或裁剪包含链即会
                    // 报 "no type named 'string' in namespace 'std'"。故显式写出。
#include <vector>

#include "recon_platform/yuv_convert.h"

// GPU 算法的**唯一事实来源**：参数块布局与 workgroup 尺寸都从这里取，
// **不在本文件重抄一份**（否则就是第二事实来源，必然漂移）。
// 该头是纯 C++（只依赖 <stdint.h>），可被 ObjC++ 直接 `#include`。
//
// 路径靠 `shaders/common` 头搜索路径（见 tools/gen_ios_project.py 的
// HEADER_SEARCH_PATHS）；**不要**写成相对路径 `../../shaders/...` ——
// 那是「数 `..` 的层数」，容易算错，且对 CMake 与 Xcode 两个消费者
// 意味着两种不同的相对基准。
#include "gpu_algo.h"

using recon::platform::BgraFrame;
using recon::platform::ColorMatrix;
using recon::platform::ConvertYuv420BiPlanarToBgra8;
using recon::platform::ConvertYuv420PlanarToBgra8;
using recon::platform::MakeYuvToBgraParams;
using recon::platform::QuantRange;
using recon::platform::YuvToBgraParams;
using recon::platform::YuvConvertResult;

// ===========================================================================
// ReconBgraFrame 私有可写属性
// ===========================================================================
//
// ⚠️ 必须在**全局作用域**声明（ObjC 的 `@interface` 不能出现在 C++ 匿名
//    命名空间内），且必须在 `MakeResult` **之前** —— 这样 `MakeResult` 里对
//    只读属性写值用的是**编译期可查的属性赋值**，而不是 KVC 字符串。
//
//    这里刻意**不用** `setValue:forKey:`：KVC 的 key 是字符串，拼错或属性
//    改名时编译器**完全不报错**，只在运行期抛 `NSUnknownKeyException`
//    （或在 `-Os` 下被优化成未定义行为）。属性赋值则编译期即校验。
@interface ReconBgraFrame ()
@property (nonatomic, readwrite) BOOL ok;
@property (nonatomic, readwrite) uint32_t width;
@property (nonatomic, readwrite) uint32_t height;
@property (nonatomic, readwrite, copy) NSData *data;
@property (nonatomic, readwrite) BOOL usedGPU;
@property (nonatomic, readwrite, copy) NSString *fallbackReason;
@property (nonatomic, readwrite) NSUInteger clampedChannelCount;
@end

// ===========================================================================
// 内部工具
// ===========================================================================
namespace {

// 注：这里**没有** `HasMetalDevice()` 这类"看起来该有"的函数 —— 设备检查
// 已经包含在 `MetalProbe()` 的探测里（它会同时确认设备、metallib 与 kernel
// 三者齐备，比只查设备更严格）。多写一个只查设备的函数反而会诱导后来者
// 用它做"GPU 可用"的判断，从而漏掉 metallib 缺失这一类真实失败。

// 由像素格式判 full / limited range。
//
//   `...FullRange` 后缀 → full（iOS 的 ARKit 路径就是它）。
//   其余（`420YpCbCr8BiPlanarVideoRange`）→ limited。
//
// 这是**像素格式自带的**信息，比读 attachment 更可靠（attachment 在部分
// 设备/配置下缺失）。两者都拿不到时才退到"假定 full"—— iOS ARKit 的实际
// 情形，且带 fallbackReason 记录。
QuantRange RangeFromFormat(OSType fmt, bool *known) {
    *known = true;
    if (fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        return QuantRange::kFull;
    }
    if (fmt == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
        return QuantRange::kLimited;
    }
    *known = false;
    return QuantRange::kFull;   // 保守默认（iOS ARKit 实为 full）
}

// 由 `kCVImageBufferYCbCrMatrix` attachment 选 BT.601 / BT.709。
// `known == false` 表示附件缺失或取值不认识 → 用 601 并记原因。
ColorMatrix MatrixFromAttachment(CVPixelBufferRef buf, bool *known,
                                 std::string *reason) {
    CFTypeRef raw = CVBufferGetAttachment(buf, kCVImageBufferYCbCrMatrix, nullptr);
    *known = true;
    if (raw == nullptr) {
        *known = false;
        if (reason) *reason = "YCbCrMatrix 附件缺失，按 BT.601（**这是一个假设**）";
        return ColorMatrix::kBt601;
    }
    // 附件是 CFString。用桥接比较避免手写 CFString 字面量比较。
    NSString *s = (__bridge NSString *)raw;
    if ([s isEqualToString:(__bridge NSString *)kCVImageBufferYCbCrMatrix_ITU_R_709_2]) {
        return ColorMatrix::kBt709;
    }
    if ([s isEqualToString:(__bridge NSString *)kCVImageBufferYCbCrMatrix_ITU_R_601_4]) {
        return ColorMatrix::kBt601;
    }
    *known = false;
    if (reason) {
        *reason = [NSString stringWithFormat:
            @"未知的 YCbCrMatrix（%@），按 BT.601", s].UTF8String;
    }
    return ColorMatrix::kBt601;
}

// 把 `BgraFrame` 转成 ObjC 结果对象。
//
// 用**属性赋值**（见上方类扩展的说明），不用 KVC —— 后者拼错 key 编译期不报。
ReconBgraFrame *MakeResult(const BgraFrame &f, BOOL usedGPU, NSString *reason) {
    ReconBgraFrame *r = [[ReconBgraFrame alloc] init];
    r.ok = YES;
    r.width = f.width;
    r.height = f.height;
    r.usedGPU = usedGPU;
    r.fallbackReason = reason ?: @"";
    r.clampedChannelCount = f.clamped_channel_count;
    r.data = [NSData dataWithBytes:f.data.data() length:f.data.size()];
    return r;
}

// ---------------------------------------------------------------------------
// 一次性探测：Metal 设备 + default.metallib + kernel 函数
// ---------------------------------------------------------------------------
//
// ⚠️ 本段曾写成 ObjC 方法 `+ (struct ReconMetalProbe)metalProbe`，返回一个
//    **从未定义**的结构体 —— `ReconMetalProbe` 是**不完整类型**，不能作返回
//    类型，在 macOS 上必然编译失败。本机无 Xcode，是靠人工复核发现的。
//    改为文件作用域静态量 + 裸函数：`dispatch_once` 的 block 捕获静态存储期
//    变量是合法的（block 不持有它，直接访问）。
//
// 为什么必须缓存：`newDefaultLibrary` 与 `newFunctionWithName:` 都是相对昂贵
// 的操作（要解析/链接 metallib），而这是**每帧**都会走到的路径。探测结果
// 只取决于 bundle 里的产物，运行期不会变化。
struct ReconMetalProbe {
    BOOL available = NO;
    NSString *reason = @"";
};

ReconMetalProbe &MetalProbe() {
    static ReconMetalProbe probe;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (dev == nil) {
            probe.reason = @"无 Metal 设备（模拟器或不支持 Metal 的环境）";
            return;
        }
        // MT-2：必须用 `newDefaultLibrary`（**不带** bundle 参数），
        // 让系统在 App bundle 里找 `default.metallib`；失败再退回显式 bundle
        // 查询（便于报错时把真实原因带回去）。
        id<MTLLibrary> lib = [dev newDefaultLibrary];
        if (lib == nil) {
            lib = [dev newDefaultLibraryWithBundle:[NSBundle mainBundle] error:nil];
        }
        if (lib == nil) {
            probe.reason = @"找不到 default.metallib —— 需先运行 "
                           @"tools/build_metal_shaders.sh（见 docs/12 §4.4）";
            return;
        }
        if ([lib newFunctionWithName:@"recon_yuv_to_bgra"] == nil) {
            probe.reason = @"default.metallib 里没有 recon_yuv_to_bgra —— "
                           @"着色器未随本次构建更新";
            return;
        }
        probe.available = YES;
        probe.reason = @"";
    });
    return probe;
}

// ---------------------------------------------------------------------------
// 一次性构建 compute pipeline（**不在每帧重建**）
// ---------------------------------------------------------------------------
//
// ⚠️ 这一条直接决定 docs/11「1080p 单帧 < 0.4 ms」是否可能成立：
//    `newComputePipelineStateWithFunction:` 要把 MSL 编译/链接成 GPU 指令，
//    单次开销在**毫秒级**。若写在每帧路径上，转换耗时会被它主导，
//    「< 0.4 ms」就成了空话（且随帧数线性放大）。
//    故与探测一样用 `dispatch_once` 缓存。
//
// 失败时 `pipe == nil` 且 `*reason` 说明原因 —— 调用方转 CPU 回退并上报
// （不静默）。
id<MTLComputePipelineState> CachedPipeline(NSString **reason) {
    static id<MTLComputePipelineState> pipe = nil;
    static NSString *failReason = @"";
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        ReconMetalProbe &probe = MetalProbe();
        if (!probe.available) { failReason = probe.reason; return; }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLLibrary> lib = [dev newDefaultLibrary]
                          ?: [dev newDefaultLibraryWithBundle:[NSBundle mainBundle]
                                                         error:nil];
        id<MTLFunction> fn = [lib newFunctionWithName:@"recon_yuv_to_bgra"];
        if (fn == nil) { failReason = @"kernel recon_yuv_to_bgra 缺失"; return; }
        NSError *err = nil;
        pipe = [dev newComputePipelineStateWithFunction:fn error:&err];
        if (pipe == nil) {
            failReason = [NSString stringWithFormat:@"建 compute pipeline 失败：%@",
                          err.localizedDescription ?: @"未知"];
        }
    });
    if (reason) *reason = failReason;
    return pipe;
}

// ---------------------------------------------------------------------------
// 一次性创建 command queue（同上：队列创建是每帧路径上的浪费）
// ---------------------------------------------------------------------------
id<MTLCommandQueue> CachedQueue(NSString **reason) {
    static id<MTLCommandQueue> queue = nil;
    static NSString *failReason = @"";
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (dev == nil) { failReason = @"无 Metal 设备"; return; }
        queue = [dev newCommandQueue];
        if (queue == nil) { failReason = @"建 command queue 失败"; }
    });
    if (reason) *reason = failReason;
    return queue;
}

}  // namespace

// ===========================================================================
// ReconBgraFrame
// ===========================================================================
// 可写属性在**文件头部**的类扩展里声明（`readonly` 对外、可写对内）。
// 这里只放 `@implementation`。
@implementation ReconBgraFrame
@end

// ===========================================================================
// ReconYuvConverter
// ===========================================================================
@implementation ReconYuvConverter

// ---------------------------------------------------------------------------
// GPU 可用性（惰性、一次性探测，结果缓存）
// ---------------------------------------------------------------------------
//
// 为什么缓存：`makeDefaultLibrary()` 与 `newFunctionWithName:` 都是相对昂贵的
// 操作，而本方法是每帧路径上被查的。用 `dispatch_once` 保证只探一次，
// 且线程安全（采集在 ARKit 队列、结果读取可能在其它的队列）。
+ (BOOL)metalAvailable {
    return MetalProbe().available;
}

+ (NSString *)metalUnavailableReason {
    return MetalProbe().reason ?: @"";
}

// ---------------------------------------------------------------------------
// 入口：ARKit capturedImage
// ---------------------------------------------------------------------------
+ (nullable ReconBgraFrame *)bgra8FromCapturedImage:(CVPixelBufferRef)capturedImage {
    if (capturedImage == nullptr) return nil;

    // ① 平面数必须是 2（Y + CbCr）。否则不是本函数能处理的 YUV 双平面
    //    —— **显式返回 nil**，由调用方决定（不猜格式、不静默继续）。
    size_t planes = CVPixelBufferGetPlaneCount(capturedImage);
    if (planes != 2) {
        // 单平面但恰好是 BGRA 的缓冲（模拟器 / 某些配置）→ 交给兼容路径。
        OSType fmt = CVPixelBufferGetPixelFormatType(capturedImage);
        if (fmt == kCVPixelFormatType_32BGRA) {
            return [self bgra8FromBgraPixelBuffer:capturedImage];
        }
        return nil;
    }

    const uint32_t width = (uint32_t)CVPixelBufferGetWidth(capturedImage);
    const uint32_t height = (uint32_t)CVPixelBufferGetHeight(capturedImage);
    if (width == 0 || height == 0) return nil;

    // ② 锁缓冲（`.readOnly`；GPU 路径下 CoreVideo 会保证 IOSurface 不被
    //    回收）。注意 beta 侧 buffer 也要锁 —— 与 MSL 侧一致。
    if (CVPixelBufferLockBaseAddress(capturedImage, kCVPixelBufferLock_ReadOnly)
        != kCVReturnSuccess) {
        return nil;
    }

    // ③ 取平面地址与 stride。**严禁** GetBaseAddress（缺口 J 的根因）。
    const uint8_t *y_base =
        (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(capturedImage, 0);
    const uint8_t *c_base =
        (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(capturedImage, 1);
    const size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(capturedImage, 0);
    const size_t c_stride = CVPixelBufferGetBytesPerRowOfPlane(capturedImage, 1);

    if (y_base == nullptr || c_base == nullptr) {
        CVPixelBufferUnlockBaseAddress(capturedImage, kCVPixelBufferLock_ReadOnly);
        return nil;
    }

    // ④ 色彩矩阵与 range。
    std::string reasonStr;
    bool matrixKnown = true;
    const ColorMatrix matrix =
        MatrixFromAttachment(capturedImage, &matrixKnown, &reasonStr);
    bool rangeKnown = true;
    const QuantRange range =
        RangeFromFormat(CVPixelBufferGetPixelFormatType(capturedImage), &rangeKnown);

    // ⑤ 系数由**唯一实现**算出（GPU 参数块与 CPU 路径共用同一份）。
    const YuvToBgraParams params = MakeYuvToBgraParams(matrix, range);

    // ⑥ 先试 GPU。**失败不静默**：转 CPU 并在结果里带上原因。
    BgraFrame frame;
    BOOL usedGPU = NO;
    NSString *fallback = reasonStr.empty() ? @"" :
        [NSString stringWithUTF8String:reasonStr.c_str()];

    if (params.valid && [self metalAvailable]) {
        NSString *gpuReason = nil;
        if ([self tryGPUConvert:capturedImage
                          width:width
                         height:height
                       yStride:y_stride
                       cStride:c_stride
                         params:params
                          out:&frame
                       reason:&gpuReason]) {
            usedGPU = YES;
        } else if (gpuReason.length > 0) {
            fallback = gpuReason;
        }
    } else if (!params.valid) {
        fallback = @"系数派生失败（矩阵/range 元数据非法）";
    } else if (fallback.length == 0) {
        fallback = [self metalUnavailableReason];
    }

    // ⑦ CPU 回退。**交错平面**（NV12/NV21，iOS 是 NV12 序）→ 用双平面版本。
    if (!usedGPU) {
        // 判 Cb 是否在前：iOS 的 `420YpCbCr8BiPlanar*` 固定为 CbCr（NV12 序）。
        const bool cb_first = true;
        const YuvConvertResult r = ConvertYuv420BiPlanarToBgra8(
            y_base, y_stride, c_base, c_stride, width, height, cb_first, params);
        if (!r.ok) {
            CVPixelBufferUnlockBaseAddress(capturedImage,
                                           kCVPixelBufferLock_ReadOnly);
            return nil;
        }
        frame = r.frame;
    }

    CVPixelBufferUnlockBaseAddress(capturedImage, kCVPixelBufferLock_ReadOnly);

    return MakeResult(frame, usedGPU, fallback);
}

// ---------------------------------------------------------------------------
// GPU 路径
// ---------------------------------------------------------------------------
//
// 返回 YES 时 `*out` 已填好；返回 NO 时 `*reason` 说明为何回退。
//
// ⚠️ 本方法**从未运行过**。设计取舍见文件头。
+ (BOOL)tryGPUConvert:(CVPixelBufferRef)buf
                width:(uint32_t)width
               height:(uint32_t)height
              yStride:(size_t)yStride
              cStride:(size_t)cStride
               params:(const YuvToBgraParams &)params
                  out:(BgraFrame *)out
               reason:(NSString **)reason {
    // ①②③④⑤⑥ 全部走**一次性缓存**：设备 / 库 / 函数 / pipeline / queue。
    // 每帧重建任一项都会把「< 0.4 ms」的预算吃掉（见 CachedPipeline 注释）。
    NSString *why = nil;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (dev == nil) { *reason = @"无 Metal 设备"; return NO; }

    id<MTLComputePipelineState> pipe = CachedPipeline(&why);
    if (pipe == nil) { *reason = why ?: @"compute pipeline 不可用"; return NO; }

    id<MTLCommandQueue> queue = CachedQueue(&why);
    if (queue == nil) { *reason = why ?: @"command queue 不可用"; return NO; }

    // ── 用 CVMetalTextureCache 为两个平面建**零拷贝**纹理视图 ──────────────
    CVMetalTextureCacheRef cache = nullptr;
    if (CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, dev, nullptr, &cache)
        != kCVReturnSuccess || cache == nullptr) {
        *reason = @"建 CVMetalTextureCache 失败";
        return NO;
    }

    // Y 平面：r8Unorm（planar format 0）
    CVMetalTextureRef texY = nullptr;
    CVReturn cr = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, cache, buf, nullptr,
        MTLPixelFormatR8Unorm, (size_t)width, (size_t)height, 0, &texY);
    if (cr != kCVReturnSuccess || texY == nullptr) {
        CFRelease(cache);
        *reason = @"为 Y 平面建纹理失败";
        return NO;
    }
    // CbCr 平面：rg8Unorm（planar format 1），半分辨率。
    // 尺寸必须与内核的 `(x>>1, y>>1)` 索引一致 → 向上取整（奇数边时多一列）。
    const size_t cw = ((size_t)width + 1) / 2;
    const size_t ch = ((size_t)height + 1) / 2;
    CVMetalTextureRef texC = nullptr;
    cr = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, cache, buf, nullptr,
        MTLPixelFormatRG8Unorm, cw, ch, 1, &texC);
    if (cr != kCVReturnSuccess || texC == nullptr) {
        CFRelease(texY);
        CFRelease(cache);
        *reason = @"为 CbCr 平面建纹理失败";
        return NO;
    }

    // ── 输出纹理（Shared → 可直接 getBytes 读回）───────────────────────────
    //
    // ⚠️⚠️ 像素格式**必须是 `RGBA8Unorm`，不能是 `BGRA8Unorm`** —— 这是本
    //      函数最容易写错、且**不报任何错**的一处（会静默把 R 与 B 互换）：
    //
    //   MSL 的 `texture2d<float, access::write>` 是**归一化 float 通道**视图，
    //   它没有"字节序"概念 —— `write(float4(x,y,z,w))` 按**分量序号**写入
    //   `.r,.g,.b,.a` 通道：
    //       · 格式 RGBA8Unorm → 内存字节 [x,y,z,w]
    //       · 格式 BGRA8Unorm → 内存字节 [z,y,x,w]（第 0 字节装的是 `.b`）
    //
    //   kernel（`shaders/metal/yuv.metal`）写的是 `float4(rgb.z, rgb.y, rgb.x, 1)`
    //   —— 即 x=B, y=G, z=R。它**已经**手工做过一次 B/R 交换，目的是让
    //   **内存**得到 B,G,R,A。
    //     · 配 RGBA8Unorm：内存 = [x,y,z,w] = [B,G,R,1] ✓ 正确
    //     · 配 BGRA8Unorm：内存 = [z,y,x,w] = [R,G,B,1] ✗ R/B 互换
    //
    //   而 Vulkan 侧 `imageStore(out_bgra, gid, vec4(rgb.z, rgb.y, rgb.x, 1.0))`
    //   配 `layout(rgba8, ...) image2D` —— 也是"手工换序 + RGBA8 存储"，
    //   与这里**同构**。两端必须同时是 RGBA8，否则跨端行为不一致。
    //   （灰度化用 BT.601 的 (77,150,29)，R/B 互换 → 所有颜色都偏且无任何报错。）
    MTLTextureDescriptor *desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:(NSUInteger)width
                                                          height:(NSUInteger)height
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> texOut = [dev newTextureWithDescriptor:desc];
    if (texOut == nil) {
        CFRelease(texC); CFRelease(texY); CFRelease(cache);
        *reason = @"建输出纹理失败";
        return NO;
    }

    // ── 参数块：**直接用 common 的结构体**，不在此重抄一份 ──────────────────
    //
    // 曾经这里手抄了一个 `ReconYuvToBgraGpuParamsRaw`（20 字段）——
    // 那是**第二事实来源**：`shaders/common/gpu_algo.h` 的
    // `ReconYuvToBgraGpuParams` 才是权威定义（由 tools/shader_parity.py 的
    // P4/P5 与两端着色器逐字段比对）。手抄一份必然漂移，且漂移后**没有任何
    // 闸门在看 host 侧这一份**。现改为直接 `#include` 该头并复用其类型。
    ReconYuvToBgraGpuParams p;
    std::memset(&p, 0, sizeof(p));
    p.y_width = (int32_t)width;
    p.y_height = (int32_t)height;
    p.cbcr_width = (int32_t)cw;
    p.cbcr_height = (int32_t)ch;
    p.y_stride_bytes = (int32_t)yStride;
    p.cbcr_stride_bytes = (int32_t)cStride;
    p.out_width = (int32_t)width;
    p.out_height = (int32_t)height;
    p.out_stride_bytes = (int32_t)(width * 4u);
    p.chroma_pixel_stride = 2;     // iOS 为交错 CbCr；非 2 时 kernel 拒绝产出
    p.cb_index_in_pair = 0;        // NV12：Cb 在偶数偏移
    p.y_bias = params.y_bias;
    p.y_gain = params.y_gain;
    p.c_bias = params.c_bias;
    p.c_gain = params.c_gain;
    p.r_from_cr = params.r_from_cr;
    p.b_from_cb = params.b_from_cb;
    p.g_from_cr = params.g_from_cr;
    p.g_from_cb = params.g_from_cb;

    // ── 编码并 dispatch ────────────────────────────────────────────────────
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:pipe];
    [enc setTexture:CVMetalTextureGetTexture(texY) atIndex:0];
    [enc setTexture:CVMetalTextureGetTexture(texC) atIndex:1];
    [enc setTexture:texOut atIndex:2];
    [enc setBytes:&p length:sizeof(p) atIndex:0];
    // `dispatchThreadgroups`（**不是** `dispatchThreads`）：
    //   · `dispatchThreads:threadsPerThreadgroup:` 依赖
    //     `supportsFamily(MTLGPUFamilyApple4)` 及以上的 non-uniform threadgroup
    //     支持。部署目标是 iOS 15.0（docs/12 §4.6），**含 A10/A9 设备**
    //     （iPhone 7 / 6s），那些设备上该 API 不可用。
    //   · 故改为向上取整的 threadgroup 分派，由 kernel 内的
    //     `gid >= out_width` 检查处理尾部 —— 这与 Vulkan 侧
    //     `vkCmdDispatch(ceil(w/8), ceil(h/8))` 的语义**完全一致**，
    //     两端因此是同一件事。
    //   workgroup 尺寸取自 common（MT-4 的唯一例外，但值仍不手抄）。
    const NSUInteger tgx = (NSUInteger)((width  + RECON_GPU_WORKGROUP_X - 1)
                                        / RECON_GPU_WORKGROUP_X);
    const NSUInteger tgy = (NSUInteger)((height + RECON_GPU_WORKGROUP_Y - 1)
                                        / RECON_GPU_WORKGROUP_Y);
    [enc dispatchThreadgroups:MTLSizeMake(tgx, tgy, 1)
        threadsPerThreadgroup:MTLSizeMake(RECON_GPU_WORKGROUP_X,
                                          RECON_GPU_WORKGROUP_Y,
                                          RECON_GPU_WORKGROUP_Z)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];

    BOOL ok = (cb.status == MTLCommandBufferStatusCompleted);
    if (ok) {
        // 从 Shared 纹理读回**紧密排列**的字节（内核按 out_width×4 写，
        // 故直接用 `getBytes` 的默认 stride == 紧排即可）。
        BgraFrame f;
        f.width = width;
        f.height = height;
        f.data.resize((size_t)width * height * 4u);
        [texOut getBytes:f.data.data()
              bytesPerRow:(NSUInteger)(width * 4u)
               fromRegion:MTLRegionMake2D(0, 0, width, height)
              mipmapLevel:0];
        *out = std::move(f);
    } else {
        *reason = [NSString stringWithFormat:@"GPU 命令缓冲未完成（status=%ld）",
                   (long)cb.status];
    }

    // CoreVideo 类型（`CVMetalTextureRef` / `CVMetalTextureCacheRef`）在
    // CoreFoundation 层与 `CFTypeRef` toll-free 桥接，故按 CF 的 Create 规则
    // 用 `CFRelease` 配对释放（这也是 Apple 官方 CVMetalTextureCache 示例的
    // 写法）。**不要**改成某个"看起来更贴切"的名字 —— 该 API 不存在，
    // 写了就是编译错误（本项目无 Xcode，只能靠人工复核发现，故在此注明）。
    CFRelease(texC);
    CFRelease(texY);
    CFRelease(cache);
    return ok;
}

// ---------------------------------------------------------------------------
// 兼容路径：单平面 BGRA8 → 紧密排列
// ---------------------------------------------------------------------------
+ (nullable ReconBgraFrame *)bgra8FromBgraPixelBuffer:(CVPixelBufferRef)pixelBuffer {
    if (pixelBuffer == nullptr) return nil;
    if (CVPixelBufferGetPixelFormatType(pixelBuffer) != kCVPixelFormatType_32BGRA) {
        return nil;
    }
    // ── 为什么这里也用 `GetBaseAddressOfPlane(buf, 0)` 而不是 `GetBaseAddress` ──
    //
    // 语义上，单平面缓冲用 `GetBaseAddress` 是**正确**的。但本项目对
    // `CVPixelBuffer` 的取址 API 坚持**一条纪律**：
    //
    //     任何 `CVPixelBufferRef` 的基址访问都走 `GetBaseAddressOfPlane(buf, 0)`。
    //
    // 理由有三，任一条都足以成立：
    //   ① **闸门要判据可判**：G14-a 的判据是「对可能是平面格式的缓冲用
    //      `GetBaseAddress`」。若这里存在一个"合法特例"，就需要在闸门里加
    //      豁免机制（或把实参改个不被清单匹配的名字）—— 而豁免机制一旦存在，
    //      真正的违规就能借它蒙混过关。**消灭特例比维护豁免清单可靠**。
    //   ② **单平面缓冲的 plane 0 就是全部数据**，`GetBaseAddressOfPlane(buf, 0)`
    //      与 `GetBaseAddress(buf)` 返回同一地址，**零语义损失**。
    //   ③ 一条规则不需要记忆分支：改代码的人不必先判断"这个缓冲是不是平面的"。
    //
    // 故 `GetBaseAddress` 在本仓库是**完全不出现**的（由 G14-a 守护）。
    if (CVPixelBufferGetPlaneCount(pixelBuffer) != 1) return nil;
    if (CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly)
        != kCVReturnSuccess) {
        return nil;
    }
    const uint32_t w = (uint32_t)CVPixelBufferGetWidth(pixelBuffer);
    const uint32_t h = (uint32_t)CVPixelBufferGetHeight(pixelBuffer);
    const uint8_t *base =
        (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0);
    const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
    if (base == nullptr || w == 0 || h == 0) {
        CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
        return nil;
    }

    BgraFrame f;
    f.width = w;
    f.height = h;
    f.data.resize((size_t)w * h * 4u);
    // 按行拷贝并把 stride 规整为紧密排列（内核按 width×4 紧排消费）。
    for (uint32_t y = 0; y < h; ++y) {
        std::memcpy(f.data.data() + (size_t)y * w * 4u,
                    base + (size_t)y * stride, (size_t)w * 4u);
    }
    CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

    return MakeResult(f, NO, @"输入已是 BGRA8（单平面），未做色彩转换");
}

@end
