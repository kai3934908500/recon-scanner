// SPDX-License-Identifier: BSD-3-Clause
//
// native-lib.cpp — Android JNI 桥接实现。
//
// 契约：
//   · core/include/recon/recon_c_api.h —— 唯一可用头文件（不得 include core/ 内部头）。
//   · docs/11_平台权限与集成.md §3.7 —— 平台 → 核心层的字段对接契约。
//
// 本文件是 Kotlin NativeBridge 的 native 侧。**只**做：
//   JNI 类型转换 ⟷ recon_* 调用 ⟷ 返回值映射。
// 不包含任何算法逻辑（算法全部在 core/ 内，铁律：逻辑单一来源）。

#include <jni.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "recon/recon_c_api.h"

namespace {

// JNI 侧字段布局约定（与 NativeBridge.kt 的 CoreTypes 对应）：
// 能力探测返回 IntArray：[branch, depthTier, depthSource, hasCamera, hasImu,
//                          hasLidar, hasTof, supportsGpu, maxW, maxH, maxFps,
//                          imuMaxRateHz]
enum CapabilityIndex {
  kCapBranch = 0, kCapDepthTier, kCapDepthSource, kCapHasCamera, kCapHasImu,
  kCapHasLidar, kCapHasTof, kCapSupportsGpu, kCapMaxW, kCapMaxH, kCapMaxFps,
  kCapImuRateHz, kCapCount
};

// 网格信息返回 DoubleArray 布局。
enum MeshInfoIndex {
  kMeshVerts = 0, kMeshTris, kMeshTexCount, kMeshNonManifold, kMeshBoundaryLoops,
  kMeshHoleCount, kMeshHoleAreaRatio, kMeshBboxMinX, kMeshBboxMinY, kMeshBboxMinZ,
  kMeshBboxMaxX, kMeshBboxMaxY, kMeshBboxMaxZ, kMeshScale, kMeshMeanEdgeLen,
  kMeshPoissonDepth, kMeshDegraded, kMeshCount
};

// 采集统计返回 DoubleArray 布局（与 iOS ReconScanStats 逐项对应）。
// 注意：uint64 计数在 double 下可精确表示到 2^53 —— 帧数远小于该界，安全。
enum StatsIndex {
  kStatsFramesCaptured = 0, kStatsFramesAccepted, kStatsKeyframes, kStatsImuSamples,
  kStatsDurationS, kStatsAzimuthCoverage, kStatsElevationCoverage,
  kStatsAnomalyBlur, kStatsAnomalyFastMotion, kStatsAnomalyLowOverlap,
  kStatsAnomalySpecular, kStatsAnomalyLowTexture, kStatsAnomalyBlindSpot,
  kStatsCount
};

// 参数读取返回 DoubleArray 布局：[int 值, double 值]。
enum ParamValueIndex { kParamInt = 0, kParamDouble, kParamCount };

// 尺度标定结果返回 DoubleArray 布局（与 iOS ReconScaleResult 逐项对应）。
enum ScaleResultIndex {
  kScaleOk = 0, kScaleScale, kScaleRelSigma, kScaleRefType,
  kScaleRefWidthMm, kScaleRefHeightMm, kScaleMeasuredWidthMm, kScaleMeasuredHeightMm,
  kScaleCount
};

// 把 recon_scale_result 打进 DoubleArray。
jdoubleArray ScaleResultToArray(JNIEnv* env, const recon_scale_result& r) {
  jdoubleArray out = env->NewDoubleArray(kScaleCount);
  if (out == nullptr) return nullptr;
  const jdouble vals[kScaleCount] = {
      static_cast<jdouble>(r.ok), r.scale, r.relative_sigma,
      static_cast<jdouble>(r.reference_type),
      r.reference_width_mm, r.reference_height_mm,
      r.measured_width_mm, r.measured_height_mm};
  env->SetDoubleArrayRegion(out, 0, kScaleCount, vals);
  return out;
}

// 把 jbyteArray 拷进 std::vector<uint8_t>。返回 false 表示 OOM/异常。
bool CopyByteArray(JNIEnv* env, jbyteArray src, std::vector<uint8_t>* out) {
  if (src == nullptr) return true;  // 允许空
  const jsize n = env->GetArrayLength(src);
  if (n < 0) return false;
  out->resize(static_cast<size_t>(n));
  if (n > 0) {
    env->GetByteArrayRegion(src, 0, n, reinterpret_cast<jbyte*>(out->data()));
    if (env->ExceptionCheck()) return false;
  }
  return true;
}

bool CopyShortArray(JNIEnv* env, jshortArray src, std::vector<uint16_t>* out) {
  if (src == nullptr) return true;
  const jsize n = env->GetArrayLength(src);
  if (n < 0) return false;
  out->resize(static_cast<size_t>(n));
  if (n > 0) {
    env->GetShortArrayRegion(src, 0, n, reinterpret_cast<jshort*>(out->data()));
    if (env->ExceptionCheck()) return false;
  }
  return true;
}

// 重建进度回调的 C 侧 trampoline：把进度转回 Kotlin 接口。
struct ProgressTrampoline {
  JNIEnv* env;
  jobject callback;
  jmethodID method;
};

void OnProgress(void* user_data, int32_t stage, double progress, const char* message) {
  auto* ctx = static_cast<ProgressTrampoline*>(user_data);
  if (ctx == nullptr || ctx->callback == nullptr) return;
  // 注意：这里运行在 native 重建线程。Kotlin 侧实现需自行切主线程更新 UI。
  jstring jmsg = ctx->env->NewStringUTF(message != nullptr ? message : "");
  ctx->env->CallVoidMethod(ctx->callback, ctx->method,
                           static_cast<jint>(stage),
                           static_cast<jdouble>(progress),
                           jmsg);
  if (jmsg != nullptr) ctx->env->DeleteLocalRef(jmsg);
  // 若回调抛异常，清掉以免影响 native 重建循环（异常在边界内吞掉）。
  if (ctx->env->ExceptionCheck()) ctx->env->ExceptionClear();
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_com_recon_scan_NativeBridge_abiVersion(JNIEnv*, jobject) {
  return static_cast<jint>(recon_abi_version());
}

JNIEXPORT jlong JNICALL
Java_com_recon_scan_NativeBridge_create(JNIEnv* env, jobject, jstring session_dir,
                                        jint preset, jint max_threads) {
  const char* dir = env->GetStringUTFChars(session_dir, nullptr);
  if (dir == nullptr) return 0;

  recon_config cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.abi_version = RECON_ABI_VERSION;
  cfg.session_dir = dir;
  cfg.forced_branch = 0xFFFFFFFFu;  // 自动探测
  cfg.preset = preset;
  cfg.max_threads = (max_threads > 0) ? static_cast<uint32_t>(max_threads) : 0u;
  cfg.enable_rgbd = 1;
  cfg.enable_mvs = 1;
  cfg.enable_texture = 1;

  void* context = nullptr;
  const recon_status st = recon_create(&cfg, &context);
  env->ReleaseStringUTFChars(session_dir, dir);
  if (st != RECON_OK) return 0;
  return reinterpret_cast<jlong>(context);
}

JNIEXPORT void JNICALL
Java_com_recon_scan_NativeBridge_destroy(JNIEnv*, jobject, jlong context) {
  if (context != 0) recon_destroy(reinterpret_cast<void*>(context));
}

JNIEXPORT jint JNICALL
Java_com_recon_scan_NativeBridge_setProbeData(
    JNIEnv*, jobject,
    jboolean has_camera, jboolean has_imu, jboolean has_lidar, jboolean has_tof,
    jboolean has_algo_depth, jboolean supports_gpu,
    jint max_w, jint max_h, jint max_fps, jint imu_max_rate_hz,
    jlong total_memory_bytes, jlong available_memory_bytes) {
  // 与 iOS 侧 `ReconProbeData.cValue` 逐字段对应（缺口 AC）。
  // 这里**没有**数组往返：C ABI 收的是 `const recon_probe_data*`，JNI 侧
  // 只需把 Kotlin 的基本类型填进一个栈上 POD 即可 —— 比能力探测的
  // `IntArray` 更直接（后者是 core → 平台的 out 参数，必须在堆上分配）。
  recon_probe_data d;
  std::memset(&d, 0, sizeof(d));
  // jboolean 在 JNI 里已保证是 0/1（JNI_FALSE=0 / JNI_TRUE=1），但显式写
  // `? 1 : 0` 能在阅读时立刻看出「这是 int32_t 字段，不是 bool」。
  d.has_camera = (has_camera == JNI_TRUE) ? 1 : 0;
  d.has_imu = (has_imu == JNI_TRUE) ? 1 : 0;
  d.has_lidar = (has_lidar == JNI_TRUE) ? 1 : 0;
  d.has_tof = (has_tof == JNI_TRUE) ? 1 : 0;
  d.has_algo_depth = (has_algo_depth == JNI_TRUE) ? 1 : 0;
  d.supports_gpu = (supports_gpu == JNI_TRUE) ? 1 : 0;
  // 负值是无效事实，钳到 0（与 c_api.cpp 的钳位规则一致；此处提前钳是为了
  // 让「Kotlin 传了负数」这件事在端上有迹可循，而不是被 c_api 静默吃掉）。
  d.max_image_width = (max_w > 0) ? max_w : 0;
  d.max_image_height = (max_h > 0) ? max_h : 0;
  d.max_fps = (max_fps > 0) ? max_fps : 0;
  d.imu_max_rate_hz = (imu_max_rate_hz > 0) ? imu_max_rate_hz : 0;
  // jlong 是**有符号** 64 位，而字段是 uint64_t。负值（不应出现，但 ABI 上
  // 可达）直接归零 —— 一个负的内存量会让下游的「够不够」比较得出反直觉结果。
  d.total_memory_bytes =
      (total_memory_bytes > 0) ? static_cast<uint64_t>(total_memory_bytes) : 0u;
  d.available_memory_bytes =
      (available_memory_bytes > 0) ? static_cast<uint64_t>(available_memory_bytes) : 0u;
  return static_cast<jint>(recon_set_probe_data(&d));
}

JNIEXPORT jintArray JNICALL
Java_com_recon_scan_NativeBridge_probeCapability(JNIEnv* env, jobject, jlong context) {
  if (context == 0) return nullptr;
  recon_capability cap;
  std::memset(&cap, 0, sizeof(cap));
  if (recon_probe_capability(reinterpret_cast<void*>(context), &cap) != RECON_OK) {
    return nullptr;
  }
  jintArray out = env->NewIntArray(kCapCount);
  if (out == nullptr) return nullptr;
  const jint vals[kCapCount] = {
      static_cast<jint>(cap.branch),
      static_cast<jint>(cap.depth_tier),
      static_cast<jint>(cap.depth_source),
      cap.has_camera, cap.has_imu, cap.has_lidar, cap.has_tof, cap.supports_gpu,
      cap.max_image_width, cap.max_image_height, cap.max_fps, cap.imu_max_rate_hz};
  env->SetIntArrayRegion(out, 0, kCapCount, vals);
  return out;
}

JNIEXPORT jint JNICALL
Java_com_recon_scan_NativeBridge_setParam(JNIEnv* env, jobject, jlong context,
                                          jstring key, jint value_int, jdouble value_double) {
  if (context == 0) return RECON_ERR_INVALID_ARGUMENT;
  const char* k = env->GetStringUTFChars(key, nullptr);
  if (k == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  const recon_status st = recon_set_param(reinterpret_cast<void*>(context), k, value_int, value_double);
  env->ReleaseStringUTFChars(key, k);
  return static_cast<jint>(st);
}

JNIEXPORT jint JNICALL
Java_com_recon_scan_NativeBridge_applyPreset(JNIEnv*, jobject, jlong context, jint preset) {
  if (context == 0) return RECON_ERR_INVALID_ARGUMENT;
  return static_cast<jint>(recon_apply_preset(reinterpret_cast<void*>(context), preset));
}

JNIEXPORT jdoubleArray JNICALL
Java_com_recon_scan_NativeBridge_getParam(JNIEnv* env, jobject, jlong context, jstring key) {
  if (context == 0) return nullptr;
  const char* k = env->GetStringUTFChars(key, nullptr);
  if (k == nullptr) return nullptr;
  int32_t v_int = 0;
  double v_double = 0.0;
  const recon_status st =
      recon_get_param(reinterpret_cast<void*>(context), k, &v_int, &v_double);
  env->ReleaseStringUTFChars(key, k);
  if (st != RECON_OK) return nullptr;
  jdoubleArray out = env->NewDoubleArray(kParamCount);
  if (out == nullptr) return nullptr;
  const jdouble vals[kParamCount] = {static_cast<jdouble>(v_int), v_double};
  env->SetDoubleArrayRegion(out, 0, kParamCount, vals);
  return out;
}

JNIEXPORT jstring JNICALL
Java_com_recon_scan_NativeBridge_exportParams(JNIEnv* env, jobject, jlong context) {
  if (context == 0) return nullptr;
  char* json = nullptr;
  const recon_status st = recon_export_params(reinterpret_cast<void*>(context), &json);
  if (st != RECON_OK || json == nullptr) return nullptr;
  jstring out = env->NewStringUTF(json);
  // 所有权在 core：必须交还，否则每调用一次泄漏一段 JSON。
  recon_free_string(json);
  return out;
}

JNIEXPORT jint JNICALL
Java_com_recon_scan_NativeBridge_startSession(JNIEnv* env, jobject, jlong context, jstring session_id) {
  if (context == 0) return RECON_ERR_INVALID_ARGUMENT;
  const char* sid = env->GetStringUTFChars(session_id, nullptr);
  if (sid == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  const recon_status st = recon_start_session(reinterpret_cast<void*>(context), sid);
  env->ReleaseStringUTFChars(session_id, sid);
  return static_cast<jint>(st);
}

JNIEXPORT jint JNICALL
Java_com_recon_scan_NativeBridge_pushFrame(
    JNIEnv* env, jobject, jlong context, jlong frame_id, jlong timestamp_ns,
    jbyteArray rgb, jint rgb_w, jint rgb_h, jint rgb_stride,
    jshortArray depth, jint depth_w, jint depth_h, jbyteArray depth_confidence,
    jint depth_source,
    jdouble fx, jdouble fy, jdouble cx, jdouble cy,
    jdouble k1, jdouble k2, jdouble k3, jdouble p1, jdouble p2,
    jdoubleArray pose_r, jdoubleArray pose_t, jlong pose_ts_ns,
    jdoubleArray imu_interleaved, jint imu_count,
    jdouble exposure_us, jdouble iso) {
  if (context == 0) return RECON_ERR_INVALID_ARGUMENT;

  std::vector<uint8_t> rgb_buf, conf_buf;
  std::vector<uint16_t> depth_buf;
  if (!CopyByteArray(env, rgb, &rgb_buf)) return RECON_ERR_OUT_OF_MEMORY;
  if (!CopyShortArray(env, depth, &depth_buf)) return RECON_ERR_OUT_OF_MEMORY;
  if (!CopyByteArray(env, depth_confidence, &conf_buf)) return RECON_ERR_OUT_OF_MEMORY;

  recon_camera_intrinsics intr;
  std::memset(&intr, 0, sizeof(intr));
  intr.fx = fx; intr.fy = fy; intr.cx = cx; intr.cy = cy;
  intr.k1 = k1; intr.k2 = k2; intr.k3 = k3; intr.p1 = p1; intr.p2 = p2;
  intr.width = rgb_w; intr.height = rgb_h;

  recon_pose pose;
  std::memset(&pose, 0, sizeof(pose));
  bool has_pose = false;
  if (pose_r != nullptr && pose_t != nullptr) {
    if (env->GetArrayLength(pose_r) >= 9 && env->GetArrayLength(pose_t) >= 3) {
      env->GetDoubleArrayRegion(pose_r, 0, 9, pose.r);
      env->GetDoubleArrayRegion(pose_t, 0, 3, pose.t);
      pose.timestamp_ns = static_cast<double>(pose_ts_ns);
      has_pose = true;
    }
  }

  // IMU：交错布局 [ts_ns, ax, ay, az, gx, gy, gz] × imu_count
  std::vector<recon_imu_sample> imu_buf;
  if (imu_interleaved != nullptr && imu_count > 0) {
    const jsize total = env->GetArrayLength(imu_interleaved);
    if (total >= imu_count * 7) {
      std::vector<double> raw(static_cast<size_t>(total));
      env->GetDoubleArrayRegion(imu_interleaved, 0, total, raw.data());
      imu_buf.resize(static_cast<size_t>(imu_count));
      for (int i = 0; i < imu_count; ++i) {
        const double* p = &raw[static_cast<size_t>(i) * 7];
        imu_buf[i].timestamp_ns = static_cast<int64_t>(p[0]);
        imu_buf[i].accel[0] = p[1]; imu_buf[i].accel[1] = p[2]; imu_buf[i].accel[2] = p[3];
        imu_buf[i].gyro[0] = p[4];  imu_buf[i].gyro[1] = p[5];  imu_buf[i].gyro[2] = p[6];
      }
    }
  }

  recon_frame_packet pkt;
  std::memset(&pkt, 0, sizeof(pkt));
  pkt.frame_id = static_cast<uint64_t>(frame_id);
  pkt.timestamp_ns = static_cast<uint64_t>(timestamp_ns);
  pkt.rgb_data = rgb_buf.empty() ? nullptr : rgb_buf.data();
  pkt.rgb_width = static_cast<uint32_t>(rgb_w);
  pkt.rgb_height = static_cast<uint32_t>(rgb_h);
  pkt.rgb_stride = static_cast<uint32_t>(rgb_stride);
  pkt.depth_data = depth_buf.empty() ? nullptr : reinterpret_cast<const uint8_t*>(depth_buf.data());
  pkt.depth_width = static_cast<uint32_t>(depth_w);
  pkt.depth_height = static_cast<uint32_t>(depth_h);
  pkt.depth_stride = static_cast<uint32_t>(depth_w * 2);  // uint16
  pkt.depth_confidence = conf_buf.empty() ? nullptr : conf_buf.data();
  pkt.depth_source = static_cast<recon_depth_source>(depth_source);
  pkt.has_depth = depth_buf.empty() ? 0u : 1u;
  pkt.intrinsics = &intr;
  pkt.pose = has_pose ? &pose : nullptr;
  pkt.imu = imu_buf.empty() ? nullptr : imu_buf.data();
  pkt.imu_count = static_cast<uint32_t>(imu_buf.size());
  pkt.exposure_us = exposure_us;
  pkt.iso = iso;

  return static_cast<jint>(recon_push_frame(reinterpret_cast<void*>(context), &pkt));
}

JNIEXPORT jint JNICALL
Java_com_recon_scan_NativeBridge_finishCapture(JNIEnv*, jobject, jlong context) {
  if (context == 0) return RECON_ERR_INVALID_ARGUMENT;
  return static_cast<jint>(recon_finish_capture(reinterpret_cast<void*>(context)));
}

JNIEXPORT jdoubleArray JNICALL
Java_com_recon_scan_NativeBridge_getScanStats(JNIEnv* env, jobject, jlong context) {
  if (context == 0) return nullptr;
  recon_scan_stats s;
  std::memset(&s, 0, sizeof(s));
  if (recon_get_scan_stats(reinterpret_cast<void*>(context), &s) != RECON_OK) return nullptr;
  jdoubleArray out = env->NewDoubleArray(kStatsCount);
  if (out == nullptr) return nullptr;
  const jdouble vals[kStatsCount] = {
      static_cast<jdouble>(s.frames_captured),
      static_cast<jdouble>(s.frames_accepted),
      static_cast<jdouble>(s.keyframes),
      static_cast<jdouble>(s.imu_samples),
      s.duration_s,
      s.azimuth_coverage,
      s.elevation_coverage,
      static_cast<jdouble>(s.anomaly_blur),
      static_cast<jdouble>(s.anomaly_fast_motion),
      static_cast<jdouble>(s.anomaly_low_overlap),
      static_cast<jdouble>(s.anomaly_specular),
      static_cast<jdouble>(s.anomaly_low_texture),
      static_cast<jdouble>(s.anomaly_blind_spot)};
  env->SetDoubleArrayRegion(out, 0, kStatsCount, vals);
  return out;
}

JNIEXPORT jint JNICALL
Java_com_recon_scan_NativeBridge_runReconstruction(JNIEnv* env, jobject, jlong context,
                                                   jobject progress_callback) {
  if (context == 0) return RECON_ERR_INVALID_ARGUMENT;

  ProgressTrampoline tramp{nullptr, nullptr, nullptr};
  recon_progress_cb cb = nullptr;
  void* user = nullptr;
  if (progress_callback != nullptr) {
    jclass cls = env->GetObjectClass(progress_callback);
    jmethodID mid = env->GetMethodID(cls, "onProgress", "(IDLjava/lang/String;)V");
    if (mid != nullptr) {
      tramp.env = env;
      tramp.callback = progress_callback;
      tramp.method = mid;
      cb = &OnProgress;
      user = &tramp;
    }
    env->DeleteLocalRef(cls);
  }

  return static_cast<jint>(
      recon_run_reconstruction(reinterpret_cast<void*>(context), cb, nullptr, nullptr, user));
}

JNIEXPORT jdoubleArray JNICALL
Java_com_recon_scan_NativeBridge_getMeshInfo(JNIEnv* env, jobject, jlong context) {
  if (context == 0) return nullptr;
  recon_mesh_info info;
  std::memset(&info, 0, sizeof(info));
  if (recon_get_mesh_info(reinterpret_cast<void*>(context), &info) != RECON_OK) return nullptr;
  jdoubleArray out = env->NewDoubleArray(kMeshCount);
  if (out == nullptr) return nullptr;
  const jdouble vals[kMeshCount] = {
      static_cast<jdouble>(info.vertex_count),
      static_cast<jdouble>(info.triangle_count),
      static_cast<jdouble>(info.texture_count),
      static_cast<jdouble>(info.non_manifold_edges),
      static_cast<jdouble>(info.boundary_loops),
      static_cast<jdouble>(info.hole_count),
      info.hole_area_ratio,
      info.bbox_min[0], info.bbox_min[1], info.bbox_min[2],
      info.bbox_max[0], info.bbox_max[1], info.bbox_max[2],
      info.scale_factor, info.mean_edge_length_m,
      static_cast<jdouble>(info.poisson_depth),
      static_cast<jdouble>(info.degraded)};
  env->SetDoubleArrayRegion(out, 0, kMeshCount, vals);
  return out;
}

JNIEXPORT jdoubleArray JNICALL
Java_com_recon_scan_NativeBridge_calibrateScale(
    JNIEnv* env, jobject, jlong context,
    jbyteArray rgb, jint rgb_w, jint rgb_h, jint rgb_stride,
    jshortArray depth, jint depth_w, jint depth_h,
    jdouble fx, jdouble fy, jdouble cx, jdouble cy,
    jdoubleArray pose_r, jdoubleArray pose_t, jlong pose_ts_ns,
    jdoubleArray detected_corners, jint reference_type,
    jdouble ref_width_mm, jdouble ref_height_mm) {
  if (context == 0) return nullptr;

  // 构造参照帧包（与 pushFrame 同一打包方式，复用同一批布局约定）。
  std::vector<uint8_t> rgb_buf;
  std::vector<uint16_t> depth_buf;
  if (!CopyByteArray(env, rgb, &rgb_buf)) return nullptr;
  if (!CopyShortArray(env, depth, &depth_buf)) return nullptr;

  recon_camera_intrinsics intr;
  std::memset(&intr, 0, sizeof(intr));
  intr.fx = fx; intr.fy = fy; intr.cx = cx; intr.cy = cy;
  intr.width = rgb_w; intr.height = rgb_h;

  recon_pose pose;
  std::memset(&pose, 0, sizeof(pose));
  bool has_pose = false;
  if (pose_r != nullptr && pose_t != nullptr) {
    if (env->GetArrayLength(pose_r) >= 9 && env->GetArrayLength(pose_t) >= 3) {
      env->GetDoubleArrayRegion(pose_r, 0, 9, pose.r);
      env->GetDoubleArrayRegion(pose_t, 0, 3, pose.t);
      pose.timestamp_ns = static_cast<double>(pose_ts_ns);
      has_pose = true;
    }
  }

  recon_frame_packet pkt;
  std::memset(&pkt, 0, sizeof(pkt));
  pkt.rgb_data = rgb_buf.empty() ? nullptr : rgb_buf.data();
  pkt.rgb_width = static_cast<uint32_t>(rgb_w);
  pkt.rgb_height = static_cast<uint32_t>(rgb_h);
  pkt.rgb_stride = static_cast<uint32_t>(rgb_stride);
  pkt.depth_data = depth_buf.empty() ? nullptr
                                     : reinterpret_cast<const uint8_t*>(depth_buf.data());
  pkt.depth_width = static_cast<uint32_t>(depth_w);
  pkt.depth_height = static_cast<uint32_t>(depth_h);
  pkt.depth_stride = static_cast<uint32_t>(depth_w * 2);
  pkt.has_depth = depth_buf.empty() ? 0u : 1u;
  pkt.intrinsics = &intr;
  pkt.pose = has_pose ? &pose : nullptr;

  // 角点：必须是 8 个（x0,y0,...,x3,y3）。
  std::vector<double> corners;
  if (detected_corners != nullptr) {
    const jsize n = env->GetArrayLength(detected_corners);
    if (n >= 8) {
      corners.resize(8);
      env->GetDoubleArrayRegion(detected_corners, 0, 8, corners.data());
    }
  }
  if (corners.empty()) return nullptr;

  recon_scale_result res;
  std::memset(&res, 0, sizeof(res));
  const recon_status st = recon_calibrate_scale(
      reinterpret_cast<void*>(context), &pkt, corners.data(),
      reference_type, ref_width_mm, ref_height_mm, &res);
  if (st != RECON_OK) return nullptr;
  return ScaleResultToArray(env, res);
}

JNIEXPORT jdoubleArray JNICALL
Java_com_recon_scan_NativeBridge_setManualScale(JNIEnv* env, jobject, jlong context,
                                               jdouble scale) {
  if (context == 0) return nullptr;
  recon_scale_result res;
  std::memset(&res, 0, sizeof(res));
  if (recon_set_manual_scale(reinterpret_cast<void*>(context), scale, &res) != RECON_OK) {
    return nullptr;
  }
  return ScaleResultToArray(env, res);
}

JNIEXPORT jint JNICALL
Java_com_recon_scan_NativeBridge_export(JNIEnv* env, jobject, jlong context, jint format,
                                        jstring out_path, jboolean apply_scale, jboolean embed_texture) {
  if (context == 0) return RECON_ERR_INVALID_ARGUMENT;
  const char* path = env->GetStringUTFChars(out_path, nullptr);
  if (path == nullptr) return RECON_ERR_INVALID_ARGUMENT;
  recon_export_options opt;
  std::memset(&opt, 0, sizeof(opt));
  opt.format = static_cast<recon_export_format>(format);
  opt.out_path = path;
  opt.apply_scale = apply_scale ? 1 : 0;
  opt.embed_texture = embed_texture ? 1 : 0;
  opt.generate_normals = (format == RECON_EXPORT_OBJ_MTL) ? 1 : 0;
  const recon_status st = recon_export(reinterpret_cast<void*>(context), &opt);
  env->ReleaseStringUTFChars(out_path, path);
  return static_cast<jint>(st);
}

JNIEXPORT jint JNICALL
Java_com_recon_scan_NativeBridge_cancel(JNIEnv*, jobject, jlong context) {
  if (context == 0) return RECON_ERR_INVALID_ARGUMENT;
  return static_cast<jint>(recon_cancel(reinterpret_cast<void*>(context)));
}

// 预览网格（docs/09 §3.7 U-R-1..U-R-4）。**必须与 iOS 侧对等** ——
// 否则平台对等闸门（check_platform_parity.py 闸门 7）会报
// 「iOS 调用了 Android 未调用的 core 函数」。
//
// 返回布局（jobjectArray，共 4 项）：
//   [0] float[]  positions（XYZ 交错，长度 = 3*vertex_count）
//   [1] int[]    indices  （每 3 个一三角面，长度 = 3*triangle_count）
//   [2] double[] meta     （[0]=source_triangle_count, [1]=decimation_step）
//   [3] null 占位（保持数组定长，便于 Kotlin 侧按固定下标取用）
//
// 返回 null 表示「尚未重建 / 分配失败」—— 与 iOS 的 `previewMesh()` 返回 nil 对等。
//
// 所有权：`recon_get_preview_mesh` 返回的两块缓冲由 core 分配，本层拷贝进
// Java 数组后**立即**用 `recon_free_buffer` 释放（两个缓冲各自独立释放）。
JNIEXPORT jobjectArray JNICALL
Java_com_recon_scan_NativeBridge_getPreviewMesh(
    JNIEnv* env, jobject, jlong context, jint max_triangles) {
  if (context == 0) return nullptr;
  recon_preview_mesh mesh;
  std::memset(&mesh, 0, sizeof(mesh));
  if (recon_get_preview_mesh(reinterpret_cast<void*>(context),
                             static_cast<uint32_t>(max_triangles), &mesh) != RECON_OK) {
    return nullptr;
  }
  // 从此刻起任何提前返回都必须释放缓冲 —— 用哨兵统一收尾，避免漏释放。
  jobjectArray out = nullptr;
  do {
    if (mesh.positions == nullptr || mesh.indices == nullptr ||
        mesh.vertex_count == 0 || mesh.triangle_count == 0) {
      break;  // 半成品：不交给 UI（不静默降级）
    }
    const jsize pos_len = static_cast<jsize>(mesh.vertex_count) * 3;
    const jsize idx_len = static_cast<jsize>(mesh.triangle_count) * 3;

    jfloatArray pos = env->NewFloatArray(pos_len);
    if (pos == nullptr) break;
    env->SetFloatArrayRegion(pos, 0, pos_len, mesh.positions);

    jintArray idx = env->NewIntArray(idx_len);
    if (idx == nullptr) break;
    // uint32 indices：本项目的顶点数不可能达到 int32 溢出量级（预览还会抽稀），
    // 故转 int 安全；此处显式转换而非 memcpy，避免端序/宽度假设。
    std::vector<jint> tmp(static_cast<size_t>(idx_len));
    for (jsize k = 0; k < idx_len; ++k) {
      tmp[static_cast<size_t>(k)] = static_cast<jint>(mesh.indices[k]);
    }
    env->SetIntArrayRegion(idx, 0, idx_len, tmp.data());

    jdoubleArray meta = env->NewDoubleArray(2);
    if (meta == nullptr) break;
    const jdouble meta_vals[2] = {
        static_cast<jdouble>(mesh.source_triangle_count),
        static_cast<jdouble>(mesh.decimation_step)};
    env->SetDoubleArrayRegion(meta, 0, 2, meta_vals);

    jclass obj_cls = env->FindClass("java/lang/Object");
    if (obj_cls == nullptr) break;
    out = env->NewObjectArray(4, obj_cls, nullptr);
    if (out == nullptr) break;
    env->SetObjectArrayElement(out, 0, pos);
    env->SetObjectArrayElement(out, 1, idx);
    env->SetObjectArrayElement(out, 2, meta);
    // [3] 留 null。
  } while (false);

  if (mesh.positions != nullptr) {
    recon_free_buffer(mesh.positions, static_cast<size_t>(mesh.vertex_count) * 3 * sizeof(float));
  }
  if (mesh.indices != nullptr) {
    recon_free_buffer(mesh.indices, static_cast<size_t>(mesh.triangle_count) * 3 * sizeof(uint32_t));
  }
  return out;
}

JNIEXPORT jint JNICALL
Java_com_recon_scan_NativeBridge_isCancelled(JNIEnv*, jobject, jlong context) {
  if (context == 0) return 0;
  return static_cast<jint>(recon_is_cancelled(reinterpret_cast<void*>(context)));
}

}  // extern "C"
