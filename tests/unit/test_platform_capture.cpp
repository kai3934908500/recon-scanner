// SPDX-License-Identifier: BSD-3-Clause
//
// 平台无关采集支撑的单元测试（docs/14 §5.1 平台适配层）。
//
// 覆盖两块**已从 iOS Swift 下沉到 platform/common** 的逻辑：
//   ① IMU 采样环形缓冲 + 「本帧之前」时间窗切片（imu_window.h）
//   ② 深度缓冲单位换算 Float32 米 → uint16 毫米（depth_convert.h）
//
// 为什么必须在桌面测：这两条规则**双端共享**（铁律：同一逻辑写两遍必然漂移），
// 且其边界写法（`<=` / `<`、取整方式、钳位阈值）错一位就**静默**改变喂给内核
// 的数据 —— 属最难查的一类。放桌面单测即把「只能真机发现」变成「可静态发现」。
//
// 每条断言都对应一个**具体的错误实现**（若把实现改回错误写法，本测试必失败）。

#include "recon_platform/depth_convert.h"
#include "recon_platform/imu_window.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using recon::platform::ConvertDepthFloat32MetersToMM16;
using recon::platform::ImuRingBuffer;
using recon::platform::kDepthMaxMM;

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::printf("  [FAIL] %s\n", what);
    ++g_failures;
  }
}

recon_imu_sample MakeSample(int64_t ts_ns, double ax, double ay, double az) {
  recon_imu_sample s;
  std::memset(&s, 0, sizeof(s));
  s.timestamp_ns = ts_ns;
  s.accel[0] = ax; s.accel[1] = ay; s.accel[2] = az;
  s.gyro[0] = 0.0; s.gyro[1] = 0.0; s.gyro[2] = 0.0;
  return s;
}

// ------------------------------------------------------------------------- //
// ① IMU 时间窗切片
// ------------------------------------------------------------------------- //

void TestImuWindowBasic() {
  std::printf("[TEST] IMU 时间窗：基本切片与消费语义\n");
  ImuRingBuffer buf;
  // 100 Hz：ts = 0, 10ms, 20ms, 30ms, 40ms
  for (int i = 0; i < 5; ++i) {
    buf.Append(MakeSample(i * 10'000'000LL, 0.1 * i, 0.0, 9.81));
  }
  Check(buf.Count() == 5, "追加 5 条后计数为 5");

  // 帧 ts = 25ms → 窗口应含 0/10/20ms 三条（**含等于语义**的边界在 20ms 未触达，
  // 另用 TestImuWindowInclusiveBoundary 单独锁）。
  const auto w = buf.WindowBefore(25'000'000LL);
  Check(w.count() == 3, "帧 ts=25ms 切出 3 条（0/10/20ms）");
  Check(w.samples.size() == 3 && w.samples[0].accel[0] == 0.0 &&
            w.samples[2].accel[0] == 0.2,
        "窗口内样本按时间升序，且为首 3 条");
  // 消费后移出：若不移出，下一帧会重复预积分已算区间（双重计数）。
  Check(buf.Count() == 2, "切窗后缓冲只剩 2 条（30/40ms）");

  const auto w2 = buf.WindowBefore(45'000'000LL);
  Check(w2.count() == 2, "第二次切窗取到剩余 2 条");
  Check(buf.Count() == 0, "全部消费后缓冲为空");
}

void TestImuWindowInclusiveBoundary() {
  std::printf("[TEST] IMU 时间窗：边界必须为「含等于」\n");
  ImuRingBuffer buf;
  buf.Append(MakeSample(1'000'000LL, 1.0, 0, 0));   // 1ms
  buf.Append(MakeSample(2'000'000LL, 2.0, 0, 0));   // 2ms
  buf.Append(MakeSample(3'000'000LL, 3.0, 0, 0));   // 3ms

  // 帧 ts 恰等于 2ms：C ABI 明文「本帧之前的 IMU 时间窗」，2ms 的样本**属于**窗口。
  // 若实现写成 `storage[i].timestamp < ts`（严格小于），此处会只切出 1 条 →
  // 本断言失败。这正是「边界错一位静默改数据」的守门判据。
  const auto w = buf.WindowBefore(2'000'000LL);
  Check(w.count() == 2, "帧 ts 恰等于样本 ts 时，该样本**含**在本窗（2 条而非 1 条）");
  Check(w.samples.size() == 2 && w.samples[1].accel[0] == 2.0,
        "含等于的样本是 ts=2ms 那条");
  Check(buf.Count() == 1, "只剩 1 条未消费");
}

void TestImuWindowEmptyAndNoFutureSamples() {
  std::printf("[TEST] IMU 时间窗：空窗与不含未来样本\n");
  ImuRingBuffer buf;
  Check(buf.WindowBefore(1000).count() == 0, "空缓冲切出空窗");

  buf.Append(MakeSample(5'000'000LL, 1.0, 0, 0));
  // 帧 ts 早于唯一样本 → 不得把「未来的」样本切进来（否则预积分用了尚未发生的量）。
  const auto w = buf.WindowBefore(1'000'000LL);
  Check(w.count() == 0, "帧早于所有样本时，窗口为空（绝不取未来样本）");
  Check(buf.Count() == 1, "未来样本仍留在缓冲中未被消费");
}

void TestImuWindowOutOfOrderSortsNotDrops() {
  std::printf("[TEST] IMU 时间窗：乱序输入升序输出且不丢样本\n");
  ImuRingBuffer buf;
  // 平台队列抖动造成轻微乱序（docs/02 §3.8 F-10 须检出的异常之一）。
  buf.Append(MakeSample(3'000'000LL, 3.0, 0, 0));
  buf.Append(MakeSample(1'000'000LL, 1.0, 0, 0));
  buf.Append(MakeSample(2'000'000LL, 2.0, 0, 0));
  Check(buf.TimestampRegressions() == 1, "检出 1 次时间戳倒序");

  const auto w = buf.WindowBefore(10'000'000LL);
  // 铁律「不静默降级」：乱序只排序、**不丢**。
  Check(w.count() == 3, "乱序 3 条全部保留（不丢样本）");
  Check(w.samples.size() == 3 && w.samples[0].accel[0] == 1.0 &&
            w.samples[1].accel[0] == 2.0 && w.samples[2].accel[0] == 3.0,
        "输出已按时间升序");
  Check(w.span_ns() == 2'000'000LL, "跨度 = 3ms - 1ms = 2ms");
}

void TestImuWindowCapacityDrop() {
  std::printf("[TEST] IMU 时间窗：容量上限丢最旧并计数\n");
  ImuRingBuffer buf(3);  // 容量 3
  for (int i = 0; i < 5; ++i) {
    buf.Append(MakeSample(i * 1'000'000LL, static_cast<double>(i), 0, 0));
  }
  Check(buf.Count() == 3, "容量 3，追加 5 条后只保留 3 条");
  Check(buf.DroppedByCapacity() == 2, "丢弃计数 = 2");
  const auto w = buf.WindowBefore(10'000'000LL);
  // 丢的是**最旧**的（0/1ms），保留 2/3/4ms —— 若误丢最新，本断言失败。
  Check(w.count() == 3 && w.samples[0].accel[0] == 2.0,
        "丢的是最旧样本，窗内首条为 accel=2.0");
}

void TestImuWindowCapacityZeroDefensive() {
  std::printf("[TEST] IMU 时间窗：容量 0 的防御\n");
  ImuRingBuffer buf(0);  // 应被提升为 1
  Check(buf.capacity() == 1, "容量 0 被提升为 1（防除零/越界）");
  buf.Append(MakeSample(1'000'000LL, 1.0, 0, 0));
  buf.Append(MakeSample(2'000'000LL, 2.0, 0, 0));
  Check(buf.Count() == 1, "容量 1 时始终只保留最新 1 条");
  Check(buf.DroppedByCapacity() == 1, "丢弃计数 = 1");
}

void TestImuWindowReset() {
  std::printf("[TEST] IMU 时间窗：重置清空并归零诊断计数\n");
  ImuRingBuffer buf(2);
  buf.Append(MakeSample(2'000'000LL, 1.0, 0, 0));
  buf.Append(MakeSample(1'000'000LL, 2.0, 0, 0));  // 触发倒序
  buf.Append(MakeSample(3'000'000LL, 3.0, 0, 0));  // 触发容量丢弃
  Check(buf.TimestampRegressions() >= 1, "重置前有倒序计数");
  buf.Reset();
  Check(buf.Count() == 0, "重置后缓冲为空");
  Check(buf.DroppedByCapacity() == 0, "重置后丢弃计数归零");
  Check(buf.TimestampRegressions() == 0, "重置后倒序计数归零");
}

// ------------------------------------------------------------------------- //
// ② 深度单位换算
// ------------------------------------------------------------------------- //

/// 便捷：把一组 float 米值转成毫米。
std::vector<std::uint16_t> Convert(const std::vector<float>& meters,
                                   std::uint32_t w, std::uint32_t h,
                                   std::size_t stride_bytes = 0) {
  if (stride_bytes == 0) stride_bytes = static_cast<std::size_t>(w) * sizeof(float);
  const auto r = ConvertDepthFloat32MetersToMM16(meters.data(), w, h, stride_bytes);
  return r.ok ? r.frame.millimeters : std::vector<std::uint16_t>{};
}

void TestDepthBasicConversion() {
  std::printf("[TEST] 深度换算：米 → 毫米四舍五入\n");
  // 0.5 m → 500 mm；0.3504 m → 350.4 → 350（四舍五入）；
  // 0.3506 m → 350.6 → 351（**向零截断会得 350** → 本判据专门锁取整方式）。
  std::vector<float> src = {0.5f, 0.3504f, 0.3506f, 1.0f};
  const auto mm = Convert(src, 4, 1);
  Check(mm.size() == 4, "输出长度 = 4");
  Check(mm[0] == 500, "0.5 m → 500 mm");
  Check(mm[1] == 350, "0.3504 m → 350 mm");
  Check(mm[2] == 351, "0.3506 m → 351 mm（四舍五入，非向零截断）");
  Check(mm[3] == 1000, "1.0 m → 1000 mm");
}

void TestDepthInvalidPixels() {
  std::printf("[TEST] 深度换算：NaN/Inf/负/零 → 0\n");
  const float nan_v = std::nanf("");
  const float inf_v = std::numeric_limits<float>::infinity();
  std::vector<float> src = {nan_v, inf_v, -inf_v, -1.0f, 0.0f, 2.5f};
  const auto r = ConvertDepthFloat32MetersToMM16(src.data(), 6, 1,
                                                6 * sizeof(float));
  Check(r.ok, "参数合法时 ok == true");
  Check(r.frame.millimeters[0] == 0, "NaN → 0");
  Check(r.frame.millimeters[1] == 0, "+Inf → 0");
  Check(r.frame.millimeters[2] == 0, "-Inf → 0");
  Check(r.frame.millimeters[3] == 0, "负值 → 0");
  Check(r.frame.millimeters[4] == 0, "0 → 0");
  Check(r.frame.millimeters[5] == 2500, "2.5 m → 2500 mm");
  // 有效像素只算最后那个（其余均为无效）。
  Check(r.frame.valid_pixel_count == 1, "有效像素数 = 1");
  Check(r.frame.truncated_pixel_count == 0, "无截断");
}

void TestDepthClampAtMax() {
  std::printf("[TEST] 深度换算：上限钳位与截断计数\n");
  // 65.535 m 恰好触顶；100 m 远超。
  std::vector<float> src = {65.535f, 100.0f, 0.9f};
  const auto r = ConvertDepthFloat32MetersToMM16(src.data(), 3, 1,
                                                3 * sizeof(float));
  Check(r.frame.millimeters[0] == kDepthMaxMM, "65.535 m → 钳到 65535");
  Check(r.frame.millimeters[1] == kDepthMaxMM, "100 m → 钳到 65535");
  Check(r.frame.millimeters[2] == 900, "0.9 m → 900 mm（不受钳位影响）");
  // 触顶像素**计入有效**（其深度 65535 非零，与 core `valid_input` 口径一致）；
  // 同时单独记为 truncated。二者是「是否为零」与「是否触顶」两个维度，
  // **不是互补关系**。若实现把触顶者算作无效，本判据失败。
  Check(r.frame.truncated_pixel_count == 2, "截断计数 = 2");
  Check(r.frame.valid_pixel_count == 3, "有效计数 = 3（含 2 个触顶，触顶仍非零）");
}

void TestDepthRowPadding() {
  std::printf("[TEST] 深度换算：容忍行填充（按 stride 取址）\n");
  // 2×2，每行 2 像素（8 字节）但行尾多 4 字节填充 → stride = 12。
  // 若实现按 `width*4` 紧排取址，第二行会读到填充字节 → 值全错。
  std::vector<std::uint8_t> raw(12 * 2, 0);
  auto put = [&](std::size_t byte_off, float v) {
    std::memcpy(raw.data() + byte_off, &v, sizeof(float));
  };
  put(0, 0.1f); put(4, 0.2f);      // 行 0
  put(12, 0.3f); put(16, 0.4f);    // 行 1（跳过 4 字节填充）
  // 填充区填成垃圾，确保一旦被读到就会被发现。
  put(8, 9.9f); put(20, 9.9f);

  const auto r = ConvertDepthFloat32MetersToMM16(
      reinterpret_cast<const float*>(raw.data()), 2, 2, 12);
  Check(r.ok, "带填充的 stride 合法");
  Check(r.frame.width == 2 && r.frame.height == 2, "尺寸 = 2×2");
  Check(r.frame.millimeters.size() == 4, "输出紧密排列 4 个像素");
  Check(r.frame.millimeters[0] == 100 && r.frame.millimeters[1] == 200 &&
            r.frame.millimeters[2] == 300 && r.frame.millimeters[3] == 400,
        "按 stride 正确跳过行填充（未被 9.9 污染）");
}

void TestDepthIllegalArgs() {
  std::printf("[TEST] 深度换算：非法入参拒绝（不猜测）\n");
  std::vector<float> src(4, 0.5f);
  Check(!ConvertDepthFloat32MetersToMM16(nullptr, 2, 2, 8).ok,
        "源指针为空 → ok == false");
  Check(!ConvertDepthFloat32MetersToMM16(src.data(), 0, 2, 8).ok,
        "width 为 0 → ok == false");
  Check(!ConvertDepthFloat32MetersToMM16(src.data(), 2, 0, 8).ok,
        "height 为 0 → ok == false");
  // stride 小于 width*4：必须拒绝，不得「尽力而为」读越界。
  Check(!ConvertDepthFloat32MetersToMM16(src.data(), 2, 2, 7).ok,
        "stride 不足一行 → ok == false（不越界读）");
}

void TestDepthValidRatio() {
  std::printf("[TEST] 深度换算：有效像素占比\n");
  std::vector<float> src = {0.0f, 0.0f, 1.0f, 1.0f};  // 2 有效 / 4
  const auto r = ConvertDepthFloat32MetersToMM16(src.data(), 2, 2, 8);
  Check(r.ok, "ok");
  Check(std::fabs(r.frame.valid_ratio() - 0.5) < 1e-12, "valid_ratio = 0.5");
}

}  // namespace

int main() {
  std::printf("=== 平台无关采集支撑单元测试（imu_window / depth_convert）===\n");
  TestImuWindowBasic();
  TestImuWindowInclusiveBoundary();
  TestImuWindowEmptyAndNoFutureSamples();
  TestImuWindowOutOfOrderSortsNotDrops();
  TestImuWindowCapacityDrop();
  TestImuWindowCapacityZeroDefensive();
  TestImuWindowReset();
  TestDepthBasicConversion();
  TestDepthInvalidPixels();
  TestDepthClampAtMax();
  TestDepthRowPadding();
  TestDepthIllegalArgs();
  TestDepthValidRatio();

  if (g_failures == 0) {
    std::printf("全部通过。\n");
    return 0;
  }
  std::printf("失败 %d 项。\n", g_failures);
  return 1;
}
