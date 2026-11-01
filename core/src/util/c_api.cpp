// SPDX-License-Identifier: BSD-3-Clause
//
// C ABI 实现层。把 core/include/recon/recon_c_api.h 兑现为可调用接口。
//
// 契约：
//   - 无异常跨界：所有 C++ 异常在此捕获并转为 recon_status。
//   - 所有权明确：返回给调用方的字符串用 recon_free_string 释放。

#include <cstring>
#include <functional>
#include <exception>
#include <memory>
#include <new>
#include <string>

#include "recon/context.h"
#include "recon/exporter.h"
#include "recon/orchestrator.h"
#include "recon/params.h"
#include "recon/png_encoder.h"
#include "recon/recon_c_api.h"
#include "recon/scale_calibration.h"

namespace recon {
namespace {

// 把 C++ 异常转换为错误码。
recon_status Guard(const std::function<recon_status()>& fn) {
  try {
    return fn();
  } catch (const std::bad_alloc&) {
    return RECON_ERR_OUT_OF_MEMORY;
  } catch (const std::exception&) {
    return RECON_ERR_INTERNAL;
  } catch (...) {
    return RECON_ERR_INTERNAL;
  }
}

Context* ToContext(void* handle) { return static_cast<Context*>(handle); }

// 复制 C 字符串到 std::string，NULL 安全。
std::string SafeStr(const char* s) { return s == nullptr ? std::string() : std::string(s); }

// 分配 C 字符串副本，调用方用 recon_free_string 释放。
char* DupCStr(const std::string& s) {
  char* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (out == nullptr) return nullptr;
  std::memcpy(out, s.c_str(), s.size() + 1);
  return out;
}

// 取文件扩展名的小写形式（含点），用于 OBJ 的 mtl / 贴图命名。
std::string PathDir(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return (slash == std::string::npos) ? std::string() : path.substr(0, slash + 1);
}

std::string PathBase(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

// 去掉最后一个扩展名。
std::string PathStem(const std::string& path) {
  const std::string base = PathBase(path);
  const size_t dot = base.find_last_of('.');
  return (dot == std::string::npos || dot == 0) ? base : base.substr(0, dot);
}

}  // namespace
}  // namespace recon

using recon::Context;
using recon::ToContext;
using recon::SafeStr;
using recon::DupCStr;

extern "C" {

uint32_t recon_abi_version(void) { return RECON_ABI_VERSION; }

recon_status recon_create(const recon_config* config, void** out_context) {
  if (out_context == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  *out_context = nullptr;

  if (config != nullptr && config->abi_version != RECON_ABI_VERSION) {
    return RECON_ERR_VERSION_MISMATCH;
  }

  try {
    auto ctx = std::make_unique<Context>(config);
    *out_context = ctx.release();
    return RECON_OK;
  } catch (const std::bad_alloc&) {
    return RECON_ERR_OUT_OF_MEMORY;
  } catch (...) {
    return RECON_ERR_INTERNAL;
  }
}

void recon_destroy(void* context) {
  delete ToContext(context);
}

void recon_free_string(char* str) {
  if (str != nullptr) {
    std::free(str);
  }
}

void recon_free_buffer(void* buffer, size_t size) {
  (void)size;
  if (buffer != nullptr) {
    std::free(buffer);
  }
}

recon_status recon_probe_capability(void* context, recon_capability* out_capability) {
  if (context == nullptr || out_capability == nullptr) {
    return RECON_ERR_INVALID_ARGUMENT;
  }
  Context* ctx = ToContext(context);
  return recon::Guard([&]() {
    ctx->ProbeCapability();
    *out_capability = ctx->capability();
    return RECON_OK;
  });
}

recon_status recon_set_param(void* context, const char* key, int32_t value_int,
                             double value_double) {
  if (context == nullptr || key == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  Context* ctx = ToContext(context);
  return recon::Guard([&]() {
    // 类型判定：整数型参数优先用 value_int，其余用 value_double。
    const recon::ParamSpec* spec = nullptr;
    for (const auto& s : ctx->params().specs()) {
      if (s.key == key) {
        spec = &s;
        break;
      }
    }
    if (spec == nullptr) return RECON_ERR_INVALID_ARGUMENT;

    double v = (spec->type == recon::ParamType::kDouble) ? value_double
                : (spec->type == recon::ParamType::kBool)
                    ? (value_int != 0 ? 1.0 : 0.0)
                    : static_cast<double>(value_int);
    if (!ctx->params().Set(key, v)) return RECON_ERR_INVALID_ARGUMENT;
    return RECON_OK;
  });
}

recon_status recon_get_param(void* context, const char* key,
                             int32_t* out_value_int, double* out_value_double) {
  if (context == nullptr || key == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  Context* ctx = ToContext(context);
  return recon::Guard([&]() {
    double v = 0.0;
    if (!ctx->params().Get(key, &v)) return RECON_ERR_INVALID_ARGUMENT;
    if (out_value_int != nullptr) {
      *out_value_int = static_cast<int32_t>(v < 0 ? v - 0.5 : v + 0.5);
    }
    if (out_value_double != nullptr) *out_value_double = v;
    return RECON_OK;
  });
}

recon_status recon_apply_preset(void* context, int32_t preset) {
  if (context == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  if (preset < 0 || preset > 2) return RECON_ERR_INVALID_ARGUMENT;
  Context* ctx = ToContext(context);
  return recon::Guard([&]() {
    ctx->params().ApplyPreset(static_cast<recon::Preset>(preset));
    return RECON_OK;
  });
}

recon_status recon_export_params(void* context, char** out_json) {
  if (context == nullptr || out_json == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  *out_json = nullptr;
  Context* ctx = ToContext(context);
  return recon::Guard([&]() {
    std::string json = ctx->params().ToJson();
    char* buf = DupCStr(json);
    if (buf == nullptr) return RECON_ERR_OUT_OF_MEMORY;
    *out_json = buf;
    return RECON_OK;
  });
}

recon_status recon_start_session(void* context, const char* session_id) {
  if (context == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  Context* ctx = ToContext(context);
  return recon::Guard([&]() {
    ctx->set_session_id(SafeStr(session_id));
    ctx->set_session_active(true);
    ctx->scan_stats() = recon_scan_stats{};

    // 编排层也需要知道会话开始（它持有帧缓冲与状态机）。
    recon::SessionSpec spec;
    spec.session_id = ctx->session_id();
    spec.session_dir = ctx->session_dir();
    return ctx->orchestrator().startSession(spec);
  });
}

recon_status recon_push_frame(void* context, const recon_frame_packet* packet) {
  if (context == nullptr || packet == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  Context* ctx = ToContext(context);
  return recon::Guard([&]() {
    if (ctx->IsCancelled()) return RECON_ERR_CANCELLED;
    if (!ctx->session_active()) return RECON_ERR_CAPTURE_CAMERA_UNAVAILABLE;

    // 交给编排层做深拷贝 + 边界校验。返回非 OK 时不推进统计 ——
    // 「被拒绝的帧」不能算进 frames_captured，否则 UI 上的帧数会虚高。
    const recon_status st = ctx->orchestrator().onFramePushed(*packet);
    if (st != RECON_OK) return st;

    // 帧计数。质量门限与关键帧判定在 capture 模块内完成，
    // 此处仅做边界校验与计数推进。
    recon_scan_stats& s = ctx->scan_stats();
    s.frames_captured++;
    if (packet->imu != nullptr) {
      s.imu_samples += packet->imu_count;
    }
    return RECON_OK;
  });
}

recon_status recon_finish_capture(void* context) {
  if (context == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  Context* ctx = ToContext(context);
  return recon::Guard([&]() {
    ctx->set_session_active(false);
    return ctx->orchestrator().finishCapture();
  });
}

recon_status recon_get_scan_stats(void* context, recon_scan_stats* out_stats) {
  if (context == nullptr || out_stats == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  Context* ctx = ToContext(context);
  return recon::Guard([&]() {
    *out_stats = ctx->scan_stats();
    return RECON_OK;
  });
}

recon_status recon_run_reconstruction(void* context, recon_progress_cb progress_cb,
                                      recon_status_cb status_cb, recon_log_cb log_cb,
                                      void* user_data) {
  if (context == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  Context* ctx = ToContext(context);
  return recon::Guard([&]() {
    if (ctx->IsCancelled()) return RECON_ERR_CANCELLED;

    // 本次重建的选项来源：Context（构造期固化的 recon_config）。
    // 这里不再现算开关 —— 单阶段跑与整跑必须看同一份（否则会漂移）。
    recon::ReconOptions opt = ctx->options();

    recon::OrchestratorCallbacks cb;
    cb.progress = progress_cb;
    cb.status = status_cb;
    cb.log = log_cb;
    cb.user_data = user_data;

    // 流水线编排见 core/src/orchestrator/orchestrator.cpp（docs/01 §6）。
    // 本调用**同步阻塞**到完成/失败/取消，平台层必须放到后台线程
    // （docs/01 §6.3 的 UI 线程禁令）。
    return ctx->orchestrator().runReconstruction(opt, cb);
  });
}

recon_status recon_calibrate_scale(void* context,
                                    const recon_frame_packet* reference_frame,
                                    const double* detected_corners,
                                    int32_t reference_type, double ref_width_mm,
                                    double ref_height_mm,
                                    recon_scale_result* out_result) {
  if (context == nullptr || reference_frame == nullptr ||
      detected_corners == nullptr || out_result == nullptr) {
    return RECON_ERR_INVALID_ARGUMENT;
  }
  Context* ctx = ToContext(context);
  return recon::Guard([&]() {
    // 参数缺省时用 ParamStore 里的参照物尺寸（银行卡 CR-80 默认）。
    double w_mm = ref_width_mm;
    double h_mm = ref_height_mm;
    if (!(w_mm > 0.0)) {
      double v = 0.0;
      if (ctx->params().Get(recon::keys::kScaleRefWidthMm, &v)) w_mm = v;
    }
    if (!(h_mm > 0.0)) {
      double v = 0.0;
      if (ctx->params().Get(recon::keys::kScaleRefHeightMm, &v)) h_mm = v;
    }

    // 内参：优先取帧包内的内参；缺失则拒绝（单应分解必须有内参）。
    if (reference_frame->intrinsics == nullptr) {
      return RECON_ERR_CALIB_NO_REFERENCE;
    }
    const recon_camera_intrinsics& k = *reference_frame->intrinsics;
    if (!(k.fx > 0.0) || !(k.fy > 0.0)) {
      return RECON_ERR_CALIB_NO_REFERENCE;
    }

    // 图像角点 → 单应分解 → 度量深度 + 一致性（docs/07 §2.4–§2.7）。
    recon::ScaleCalibrationConfig cfg;
    double v_tol = 0.0;
    if (ctx->params().Get(recon::keys::kScaleRefSizeToleranceMm, &v_tol)) {
      cfg.ref_size_tolerance_mm = v_tol;
    }
    const recon::ScaleCalibrationResult r = recon::SolveScaleFromCorners(
        detected_corners, k.fx, k.fy, k.cx, k.cy, w_mm, h_mm, cfg);
    if (!r.ok) {
      return r.status == RECON_OK ? RECON_ERR_CALIB_DEGENERATE : r.status;
    }

    // 尺度因子的最终确定需要「重建几何」，由重建流程在 S8 之后回填
    // （见 orchestrator 的 StageScale）。本调用输出的是**度量深度**与
    // 一致性量，以及调用方传入的参照物信息 —— 尺度因子本身在此只做
    // 一次可用性登记，不臆造数值。
    recon_scale_result& s = ctx->scale();
    s.ok = 1;
    s.scale = 1.0;  // 未与重建几何比对前，恒等尺度（= 不缩放）
    s.relative_sigma = r.relative_sigma;
    s.reference_type = reference_type;
    s.reference_width_mm = w_mm;
    s.reference_height_mm = h_mm;
    s.measured_width_mm = 0.0;
    s.measured_height_mm = 0.0;
    *out_result = s;
    return RECON_OK;
  });
}

recon_status recon_set_manual_scale(void* context, double scale,
                                     recon_scale_result* out_result) {
  if (context == nullptr || out_result == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  Context* ctx = ToContext(context);
  return recon::Guard([&]() {
    if (!(scale > 0.0)) return RECON_ERR_INVALID_ARGUMENT;
    recon_scale_result& r = ctx->scale();
    r.ok = 1;
    r.scale = scale;
    r.reference_type = 3;  // 手动
    if (out_result != nullptr) *out_result = r;
    return RECON_OK;
  });
}

recon_status recon_get_mesh_info(void* context, recon_mesh_info* out_info) {
  if (context == nullptr || out_info == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  Context* ctx = ToContext(context);
  return recon::Guard([&]() {
    *out_info = ctx->mesh_info();
    return RECON_OK;
  });
}

recon_status recon_export(void* context, const recon_export_options* options) {
  if (context == nullptr || options == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  Context* ctx = ToContext(context);
  return recon::Guard([&]() {
    if (options->out_path == nullptr || options->out_path[0] == '\0') {
      return RECON_ERR_INVALID_ARGUMENT;
    }

    recon::ExportFormat fmt = recon::ExportFormat::kGlb;
    switch (options->format) {
      case RECON_EXPORT_GLB:
        fmt = recon::ExportFormat::kGlb;
        break;
      case RECON_EXPORT_OBJ_MTL:
        fmt = recon::ExportFormat::kObjMtl;
        break;
      case RECON_EXPORT_PLY:
        fmt = recon::ExportFormat::kPly;
        break;
      default:
        return RECON_ERR_EXPORT_UNSUPPORTED_FORMAT;
    }

    const recon::Orchestrator& orch = ctx->orchestrator();
    const recon::TriangleMesh& mesh = orch.mesh();
    if (mesh.positions.empty() || mesh.indices.empty()) {
      // 没有可导出的几何：不是「格式不支持」，而是「还没重建」。
      return RECON_ERR_MESH_POISSON_FAILED;
    }

    const std::string out_path = options->out_path;
    const std::string dir = recon::PathDir(out_path);
    const std::string stem = recon::PathStem(out_path);

    recon::ExportOptions eo;
    eo.format = fmt;
    eo.scale_factor = (ctx->mesh_info().scale_factor > 0.0)
                          ? ctx->mesh_info().scale_factor
                          : 1.0;
    eo.apply_scale = options->apply_scale != 0;
    eo.write_binary = options->write_binary != 0;
    eo.embed_texture = options->embed_texture != 0;
    eo.generate_normals = options->generate_normals != 0;

    // 纹理：渲染出的图集是 RGB8，需先编码成 PNG 才能内嵌进 GLB（glTF 只认
    // 已编码的图像字节）。无烘焙结果时不内嵌，UI 侧应显示「无纹理」。
    const recon::BakeResult& bake = orch.bake();
    const bool has_texture = bake.ok && !bake.rgb.empty() &&
                             bake.width > 0 && bake.height > 0;
    if (has_texture) {
      std::string png;
      if (recon::EncodePngRgb8(bake.rgb.data(), bake.width, bake.height, &png)) {
        eo.texture_image = std::move(png);
        eo.texture_file_name = stem + "_texture.png";
      }
    }
    // OBJ 的 mtl 与贴图命名：用「输出文件同目录 + 主干名」，
    // 保证 mtllib 引用与实际落盘文件一致（V13 材质引用完整性）。
    eo.mtl_file_name = stem + ".mtl";

    std::string main_content, secondary_content;
    recon::ValidationResult validation;
    if (!recon::ExportModel(mesh, eo, &main_content, &secondary_content,
                            &validation)) {
      // 区分「校验不过」与「编码失败」：前者是模型的问题（非流形 / 边界环），
      // 后者是内部错误，两者的处置完全不同，不能混成一个错误码。
      return validation.ok ? RECON_ERR_IO : RECON_ERR_EXPORT_VALIDATION_FAILED;
    }

    if (!recon::WriteFileAtomic(out_path, main_content)) {
      return RECON_ERR_EXPORT_WRITE_FAILED;
    }
    // OBJ 需要额外落 mtl 与贴图（GLB 自包含，PLY 无附属文件）。
    if (fmt == recon::ExportFormat::kObjMtl) {
      if (!recon::WriteFileAtomic(dir + eo.mtl_file_name, secondary_content)) {
        return RECON_ERR_EXPORT_WRITE_FAILED;
      }
      if (has_texture && !eo.texture_image.empty()) {
        if (!recon::WriteFileAtomic(dir + eo.texture_file_name,
                                    eo.texture_image)) {
          return RECON_ERR_EXPORT_WRITE_FAILED;
        }
      }
    }
    return RECON_OK;
  });
}

recon_status recon_cancel(void* context) {
  if (context == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  Context* ctx = ToContext(context);
  ctx->cancel_flag().store(true, std::memory_order_relaxed);
  return RECON_OK;
}

int32_t recon_is_cancelled(void* context) {
  if (context == nullptr) return 0;
  return ToContext(context)->IsCancelled() ? 1 : 0;
}

}  // extern "C"
