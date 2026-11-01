// SPDX-License-Identifier: BSD-3-Clause
//
// PNG 编码实现（最小子集）：8-bit truecolor、无隔行、filter 0。
//
// 文件结构（PNG 规范 ISO/IEC 15948）：
//   签名 8 字节 → IHDR → IDAT → IEND
// 每块的格式：长度(4, 大端) + 类型(4) + 数据 + CRC32(4, 大端)。
//
// 原始数据流：每行前加一个 filter 字节（0 = None），然后整体做 zlib 压缩。
// 选 filter 0 而不是自适应滤波的理由：纹理图是**已经融合好的 sRGB 图像**，
// 它的相邻像素相关性本来就高，deflate 能直接吃到；而逐行自适应搜索滤波器
// （Paeth/Sub/Up 的最小绝对差启发式）会让编码时间翻 3~5 倍、收益在
// 「4K 图集一次性编码」这个场景下不可感知。宁要可预测的编码时间。

#include "recon/png_encoder.h"

#include <cstring>
#include <vector>

#if RECON_HAVE_ZLIB
#include <zlib.h>
#endif

namespace recon {
namespace {

// ---------------------------------------------------------------------------
// CRC32（PNG 规范附录 D 的多项式 0xEDB88320，反射形式）
// ---------------------------------------------------------------------------
//
// 表在首次使用时构建。用函数内 static 而不是全局对象：避免静态初始化序
// 问题（本文件可能在任何全局构造之前被调用）。
const uint32_t* Crc32Table() {
  static uint32_t table[256];
  static bool built = false;
  if (!built) {
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      table[n] = c;
    }
    built = true;
  }
  return table;
}

// CRC 覆盖「类型 + 数据」（不含长度字段）。刻意不额外提供「只算数据」的
// 重载：PNG 规范里 CRC 的输入永远是 type 起头，分开写两遍反而容易用错。
uint32_t ChunkCrc(const char type[4], const uint8_t* data, size_t len) {
  const uint32_t* table = Crc32Table();
  uint32_t c = 0xFFFFFFFFu;
  for (int i = 0; i < 4; ++i) {
    c = table[(c ^ static_cast<uint8_t>(type[i])) & 0xFFu] ^ (c >> 8);
  }
  for (size_t i = 0; i < len; ++i) {
    c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

void AppendU32BE(std::string* out, uint32_t v) {
  out->push_back(static_cast<char>((v >> 24) & 0xFF));
  out->push_back(static_cast<char>((v >> 16) & 0xFF));
  out->push_back(static_cast<char>((v >> 8) & 0xFF));
  out->push_back(static_cast<char>(v & 0xFF));
}

void AppendChunk(std::string* out, const char type[4], const uint8_t* data,
                 size_t len) {
  AppendU32BE(out, static_cast<uint32_t>(len));
  out->append(type, 4);
  if (len > 0) {
    out->append(reinterpret_cast<const char*>(data), len);
  }
  AppendU32BE(out, ChunkCrc(type, data, len));
}

// ---------------------------------------------------------------------------
// zlib 容器（RFC 1950）：2 字节头 + deflate 流 + 4 字节 Adler-32
// ---------------------------------------------------------------------------
uint32_t Adler32(const uint8_t* data, size_t len) {
  uint32_t a = 1, b = 0;
  // 按 5552 字节分块，避免 uint32 溢出（zlib 的经典做法）。
  const size_t kBlock = 5552;
  size_t i = 0;
  while (i < len) {
    const size_t n = (len - i < kBlock) ? (len - i) : kBlock;
    for (size_t k = 0; k < n; ++k) {
      a += data[i + k];
      b += a;
    }
    a %= 65521u;
    b %= 65521u;
    i += n;
  }
  return (b << 16) | a;
}

// 存储型 deflate：BTYPE=00，每个块最多 65535 字节。
//
// 位布局：每个块以「1 bit BFINAL + 2 bit BTYPE」开始，但因为紧接的
// LEN/NLEN 是字节对齐的，规范要求块头之后补齐到字节边界 —— 对存储型块
// 就是「第一个字节低 3 位是标志、高 5 位丢弃」，后续 LEN/NLEN/数据都
// 字节对齐。
void DeflateStored(const uint8_t* data, size_t len, std::string* out) {
  size_t pos = 0;
  do {
    const size_t n = (len - pos > 65535) ? 65535 : (len - pos);
    const bool last = (pos + n >= len);
    out->push_back(static_cast<char>(last ? 0x01 : 0x00));
    const uint16_t len16 = static_cast<uint16_t>(n);
    const uint16_t nlen16 = static_cast<uint16_t>(~len16);
    out->push_back(static_cast<char>(len16 & 0xFF));
    out->push_back(static_cast<char>((len16 >> 8) & 0xFF));
    out->push_back(static_cast<char>(nlen16 & 0xFF));
    out->push_back(static_cast<char>((nlen16 >> 8) & 0xFF));
    if (n > 0) {
      out->append(reinterpret_cast<const char*>(data + pos), n);
    }
    pos += n;
    // len == 0 时仍需输出一个空块（上面的 do-while 已保证）。
  } while (pos < len);
}

// 把「行滤波后的原始像素流」打成 zlib 流。
bool ZlibWrap(const std::vector<uint8_t>& raw, std::string* out) {
#if RECON_HAVE_ZLIB
  uLongf bound = compressBound(static_cast<uLong>(raw.size()));
  std::string buf;
  buf.resize(bound);
  const int rc = compress2(reinterpret_cast<Bytef*>(&buf[0]), &bound, raw.data(),
                           static_cast<uLong>(raw.size()), Z_BEST_COMPRESSION);
  if (rc == Z_OK) {
    buf.resize(bound);
    *out = std::move(buf);
    return true;
  }
  // zlib 失败（理论上只有内存不足）：退回存储型，不静默丢文件。
#endif
  out->clear();
  out->push_back(static_cast<char>(0x78));  // CMF：deflate, 32K 窗口
  out->push_back(static_cast<char>(0x01));  // FLG：无预设字典、校验和存在
  DeflateStored(raw.data(), raw.size(), out);
  AppendU32BE(out, Adler32(raw.data(), raw.size()));
  return true;
}

}  // namespace

bool PngEncoderHasZlib() {
#if RECON_HAVE_ZLIB
  return true;
#else
  return false;
#endif
}

bool EncodePngRgb8(const uint8_t* rgb, int width, int height,
                   std::string* out_png) {
  if (rgb == nullptr || out_png == nullptr || width <= 0 || height <= 0) {
    return false;
  }

  const size_t row_bytes = static_cast<size_t>(width) * 3;
  // 每行前置 1 字节 filter。
  std::vector<uint8_t> raw;
  raw.resize((row_bytes + 1) * static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    uint8_t* dst = raw.data() + (row_bytes + 1) * static_cast<size_t>(y);
    dst[0] = 0;  // filter None
    std::memcpy(dst + 1, rgb + row_bytes * static_cast<size_t>(y), row_bytes);
  }

  std::string zstream;
  if (!ZlibWrap(raw, &zstream)) return false;

  // ---- IHDR ----
  uint8_t ihdr[13];
  const auto put32 = [](uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
  };
  put32(ihdr + 0, static_cast<uint32_t>(width));
  put32(ihdr + 4, static_cast<uint32_t>(height));
  ihdr[8] = 8;   // bit depth
  ihdr[9] = 2;   // color type：truecolor RGB
  ihdr[10] = 0;  // compression：deflate
  ihdr[11] = 0;  // filter method：adaptive（本编码器只用 filter 0）
  ihdr[12] = 0;  // interlace：none

  out_png->clear();
  out_png->reserve(zstream.size() + 64);
  static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  out_png->append(reinterpret_cast<const char*>(kSig), 8);

  AppendChunk(out_png, "IHDR", ihdr, sizeof(ihdr));
  AppendChunk(out_png, "IDAT",
              reinterpret_cast<const uint8_t*>(zstream.data()), zstream.size());
  AppendChunk(out_png, "IEND", nullptr, 0);
  return true;
}

}  // namespace recon
