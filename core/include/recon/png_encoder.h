// SPDX-License-Identifier: BSD-3-Clause
//
// PNG 编码（自实现）。契约：docs/06_网格化与后处理.md §3（纹理落盘）。
//
// 为什么自实现而不引入 libpng / stb_image_write：
//   · libpng 随 OpenCV vendor（许可面已登记），但它不是本项目的硬依赖 ——
//     desktop 构建里 OpenCV 可能根本不存在，而纹理导出是**核心功能**，
//     不能因为「恰好没装 OpenCV」就静默丢纹理。
//   · stb_image_write 是单头 MIT，但会额外引入一个第三方快照与它的
//     license 文本，而本项目对 third_party 的许可面管理是逐条登记的。
//   · PNG 对本场景只需一种最小子集：8-bit truecolor（color type 2）、
//     无隔行、filter 0。这个子集的编码器不到 150 行，完全可控。
//
// 压缩：**zlib 可用时用 zlib**（PoissonRecon vendor 的 ZLIB 1.3.1，zlib
// 许可，已在 docs/10 登记）；不可用时退到**存储型 deflate**（BTYPE=00，
// 不压缩但格式完全合法）。后者体积约等于原始像素 + 每 64 KB 5 字节开销，
// 是「格式正确、体积大」的降级 —— 与「为了体积而改精度参数」不同，
// 这里只影响纹理文件的磁盘占用（R-PREC-4 允许）。

#ifndef RECON_CORE_PNG_ENCODER_H_
#define RECON_CORE_PNG_ENCODER_H_

#include <cstdint>
#include <string>

namespace recon {

/**
 * 把紧凑 RGB8 像素编码成 PNG 字节流。
 *
 * @param rgb      长度必须 ≥ width × height × 3（行主序，无 padding）
 * @param width    宽（像素），必须 > 0
 * @param height   高（像素），必须 > 0
 * @param out_png  输出 PNG 字节
 * @return 成功返回 true；参数非法或内存不足返回 false
 */
bool EncodePngRgb8(const uint8_t* rgb, int width, int height,
                   std::string* out_png);

// 当前编码器是否使用 zlib 压缩。false 表示走存储型 deflate
// （文件合法但体积大）。用于日志与诊断。
bool PngEncoderHasZlib();

}  // namespace recon

#endif  // RECON_CORE_PNG_ENCODER_H_
