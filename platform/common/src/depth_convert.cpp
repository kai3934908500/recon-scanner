// SPDX-License-Identifier: BSD-3-Clause
//
// depth_convert.cpp — 深度缓冲单位换算（Float32 米 → uint16 毫米）。
//
// 规则表与设计理由见同目录 depth_convert.h。

#include "recon_platform/depth_convert.h"

#include <cmath>
#include <cstring>

namespace recon {
namespace platform {

DepthConvertResult ConvertDepthFloat32MetersToMM16(
    const float* src, std::uint32_t width, std::uint32_t height,
    std::size_t src_stride_bytes) {
  DepthConvertResult out;
  if (src == nullptr || width == 0 || height == 0) {
    return out;  // ok == false
  }
  // 每像素 4 字节（Float32）。stride 必须至少容纳一整行。
  const std::size_t bytes_per_pixel = sizeof(float);
  const std::size_t min_stride = static_cast<std::size_t>(width) * bytes_per_pixel;
  if (src_stride_bytes < min_stride) {
    return out;  // ok == false：行填充声明不合法，拒绝而非猜测。
  }

  DepthFrameMM frame;
  frame.width = width;
  frame.height = height;
  frame.millimeters.assign(static_cast<std::size_t>(width) * height, 0);

  const auto* base = reinterpret_cast<const std::uint8_t*>(src);
  const double max_mm = static_cast<double>(kDepthMaxMM);

  for (std::uint32_t y = 0; y < height; ++y) {
    const std::uint8_t* row = base + static_cast<std::size_t>(y) * src_stride_bytes;
    const std::size_t dst_base = static_cast<std::size_t>(y) * width;
    for (std::uint32_t x = 0; x < width; ++x) {
      float m = 0.0f;
      // 按字节偏移取值才能同时容忍「有/无行填充」两种情况，且避免
      // 未对齐指针的 float* 解引用（UB）。memcpy 会被编译器优化成单次 load。
      std::memcpy(&m, row + static_cast<std::size_t>(x) * bytes_per_pixel,
                  sizeof(float));

      // std::isfinite 同时排除 NaN 与 ±Inf。不要写成 `m != m`：在 fast-math
      // 类优化下该比较可被折叠，留下 NaN 泄漏到整数转换。
      if (!std::isfinite(m) || m <= 0.0f) {
        continue;  // 保持 0（已 assign 初始化为 0）。
      }
      const double mm = static_cast<double>(m) * 1000.0;
      if (mm >= max_mm) {
        frame.millimeters[dst_base + x] = kDepthMaxMM;
        ++frame.truncated_pixel_count;
      } else {
        // +0.5 后取整 = 四舍五入（见头文件「为什么取整用四舍五入」）。
        frame.millimeters[dst_base + x] =
            static_cast<std::uint16_t>(mm + 0.5);
      }
      ++frame.valid_pixel_count;
    }
  }

  out.ok = true;
  out.frame = std::move(frame);
  return out;
}

}  // namespace platform
}  // namespace recon
