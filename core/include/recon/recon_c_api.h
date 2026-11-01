/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * recon_c_api.h — 跨平台重建核心的唯一 C ABI 边界。
 *
 * 契约（见 docs/00_架构总纲.md §7、docs/01_接口契约.md）：
 *   - POD-only：跨边界只传 POD 结构体与标量，不传 std:: 容器、不传 C++ 类。
 *   - 所有权明确：所有 const char* 由调用方用 recon_free_string 释放；
 *                  所有返回缓冲区由 recon_free_buffer 释放。
 *   - 无异常跨界：C++ 异常在边界内全部捕获并转为 recon_status。
 *   - 线程安全：除显式标注 *_thread 的函数外，句柄不可跨线程并发调用。
 *   - 版本同步：App 与 core 必须同 RECON_ABI_VERSION。
 *
 * Swift / Kotlin 侧不得直接 include 任何 core/ 内部头文件，只能包含本文件。
 */
#ifndef RECON_RECON_C_API_H_
#define RECON_RECON_C_API_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* 版本与导出                                                                */
/* ------------------------------------------------------------------------- */

#define RECON_ABI_VERSION 1

#if defined(_WIN32)
#  define RECON_API __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#  define RECON_API __attribute__((visibility("default")))
#else
#  define RECON_API
#endif

/* ------------------------------------------------------------------------- */
/* 基础类型                                                                  */
/* ------------------------------------------------------------------------- */

/** 相机内参。像素单位，原点在图像左上角，y 轴向下（OpenCV 约定）。 */
typedef struct recon_camera_intrinsics {
    double fx;          /**< 焦距 x，像素 */
    double fy;          /**< 焦距 y，像素 */
    double cx;          /**< 主点 x，像素 */
    double cy;          /**< 主点 y，像素 */
    double k1;          /**< 径向畸变系数 1 */
    double k2;          /**< 径向畸变系数 2 */
    double k3;          /**< 径向畸变系数 3 */
    double p1;          /**< 切向畸变系数 1 */
    double p2;          /**< 切向畸变系数 2 */
    double width;       /**< 该内参对应的图像宽，像素 */
    double height;      /**< 该内参对应的图像高，像素 */
} recon_camera_intrinsics;

/** 刚体位姿。R 为 3x3 行主序，t 为米（度量尺度）。 */
typedef struct recon_pose {
    double r[9];        /**< 旋转矩阵，行主序：r[0..2] 为第 0 行 */
    double t[3];        /**< 平移向量，单位米 */
    double timestamp_ns;/**< 位姿对应时间戳，单调时钟纳秒 */
} recon_pose;

/** 单个 IMU 采样。 */
typedef struct recon_imu_sample {
    int64_t  timestamp_ns;   /**< 采样时间戳，单调时钟纳秒 */
    double   accel[3];       /**< 加速度计，比力单位 m/s^2（含重力） */
    double   gyro[3];        /**< 陀螺仪，角速度 rad/s */
} recon_imu_sample;

/* ------------------------------------------------------------------------- */
/* 枚举                                                                      */
/* ------------------------------------------------------------------------- */

/** 管线分支。探测结果决定，见 docs/00_架构总纲.md §5。 */
typedef enum recon_pipeline_branch {
    RECON_BRANCH_RGBD = 0,  /**< 有硬件深度：RGBD-TSDF 主线 + MVS 精化 */
    RECON_BRANCH_MONO = 1   /**< 无深度：VIO + SfM + MVS */
} recon_pipeline_branch;

/** 深度等级。决定深度可用性与可信度，见 docs/04_RGBD管线.md §2。 */
typedef enum recon_depth_tier {
    RECON_DEPTH_NONE       = 0,  /**< 无深度 */
    RECON_DEPTH_ALGO       = 1,  /**< 软件运动推断深度，0.5-5m 最准 */
    RECON_DEPTH_TOF        = 2,  /**< ToF 硬件深度 */
    RECON_DEPTH_LIDAR      = 3   /**< LiDAR 硬件深度 */
} recon_depth_tier;

/** 深度来源。 */
typedef enum recon_depth_source {
    RECON_DEPTH_SRC_NONE   = 0,
    RECON_DEPTH_SRC_ALGO   = 1,
    RECON_DEPTH_SRC_TOF    = 2,
    RECON_DEPTH_SRC_LIDAR  = 3
} recon_depth_source;

/** 导出格式。见 docs/06_网格化与后处理.md §3。
 *
 * **不得重排既有取值**：这些整数是已发布的 ABI 契约，三端（核心 / iOS /
 * Android）靠它对号。新增格式只能**追加**到末尾。
 */
typedef enum recon_export_format {
    RECON_EXPORT_GLB      = 0,
    RECON_EXPORT_OBJ_MTL  = 1,
    RECON_EXPORT_PLY      = 2,
    RECON_EXPORT_STL      = 3,  /**< 二进制 STL。**无单位字段**，业界约定毫米 */
    RECON_EXPORT_3MF      = 4   /**< 3MF。OPC/ZIP 容器，`unit="millimeter"` */
} recon_export_format;

/** 重建阶段。进度回调按此上报。 */
typedef enum recon_stage {
    RECON_STAGE_IDLE          = 0,
    RECON_STAGE_PREPROCESS    = 1,  /**< 深度预处理 / 帧解码 */
    RECON_STAGE_VIO           = 2,  /**< 紧耦合 VIO */
    RECON_STAGE_SFM           = 3,  /**< 稀疏 SfM */
    RECON_STAGE_BUNDLE        = 4,  /**< 全局 BA */
    RECON_STAGE_MVS           = 5,  /**< 稠密 MVS */
    RECON_STAGE_RGBD_FUSION   = 6,  /**< TSDF 融合 */
    RECON_STAGE_MESH          = 7,  /**< 泊松网格化 */
    RECON_STAGE_POSTPROC      = 8,  /**< 后处理修复 */
    RECON_STAGE_UV_TEXTURE    = 9,  /**< UV 展开与纹理烘焙 */
    RECON_STAGE_SCALE         = 10, /**< 尺度标定 */
    RECON_STAGE_EXPORT        = 11, /**< 导出 */
    RECON_STAGE_DONE          = 12,
    RECON_STAGE_FAILED        = 13,
    RECON_STAGE_CANCELLED     = 14
} recon_stage;

/* ------------------------------------------------------------------------- */
/* 错误码                                                                    */
/* ------------------------------------------------------------------------- */

/** 通用错误码。分类与完整表见 docs/01_接口契约.md §4。 */
typedef enum recon_status {
    RECON_OK = 0,

    /* 通用 */
    RECON_ERR_INVALID_ARGUMENT   = 1,
    RECON_ERR_OUT_OF_MEMORY      = 2,
    RECON_ERR_NOT_IMPLEMENTED    = 3,
    RECON_ERR_INTERNAL           = 4,
    RECON_ERR_CANCELLED          = 5,
    RECON_ERR_IO                 = 6,
    RECON_ERR_UNSUPPORTED_PLATFORM = 7,
    RECON_ERR_VERSION_MISMATCH   = 8,
    RECON_ERR_NO_CAPABILITY      = 9,

    /* 采集 */
    RECON_ERR_CAPTURE_CAMERA_UNAVAILABLE = 100,
    RECON_ERR_CAPTURE_PERMISSION_DENIED  = 101,
    RECON_ERR_CAPTURE_FRAME_DROPPED      = 102,
    RECON_ERR_CAPTURE_STALLED            = 103,
    RECON_ERR_CAPTURE_INTERRUPTED        = 104,

    /* 同步 */
    RECON_ERR_SYNC_TIME_NONMONOTONIC = 200,
    RECON_ERR_SYNC_CLOCK_DRIFT        = 201,
    RECON_ERR_SYNC_IMU_GAP            = 202,
    RECON_ERR_SYNC_CALIBRATION_MISSING = 203,

    /* VIO */
    RECON_ERR_VIO_TRACKING_LOST   = 300,
    RECON_ERR_VIO_NOT_INITIALIZED = 301,
    RECON_ERR_VIO_SCALE_UNOBSERVABLE = 302,

    /* SfM */
    RECON_ERR_SFM_INSUFFICIENT_MATCHES = 400,
    RECON_ERR_SFM_INITIALIZATION_FAILED = 401,
    RECON_ERR_SFM_BUNDLE_ADJUSTMENT_FAILED = 402,
    RECON_ERR_SFM_INSUFFICIENT_PARALLAX = 403,

    /* MVS */
    RECON_ERR_MVS_NO_DEPTH = 500,
    RECON_ERR_MVS_FILTER_EXHAUSTED = 501,
    RECON_ERR_MVS_GPU_FAILURE = 502,

    /* 深度 / RGBD */
    RECON_ERR_RGBD_NO_DEPTH = 600,
    RECON_ERR_RGBD_DEPTH_MISALIGNED = 601,
    RECON_ERR_RGBD_LOW_CONFIDENCE = 602,

    /* 网格 */
    RECON_ERR_MESH_POISSON_FAILED = 700,
    RECON_ERR_MESH_OUT_OF_MEMORY = 701,
    RECON_ERR_MESH_NON_MANIFOLD_LIMIT = 702,

    /* 后处理 */
    RECON_ERR_POST_HOLE_FILL_FAILED = 800,
    RECON_ERR_POST_UV_FAILED = 801,
    RECON_ERR_POST_TEXTURE_FAILED = 802,

    /* 标定 */
    RECON_ERR_CALIB_REFERENCE_NOT_FOUND = 900,
    RECON_ERR_CALIB_DEGENERATE = 901,
    RECON_ERR_CALIB_NO_REFERENCE = 902,
    RECON_ERR_CALIB_INCONSISTENT = 903,

    /* 导出 */
    RECON_ERR_EXPORT_WRITE_FAILED = 1000,
    RECON_ERR_EXPORT_VALIDATION_FAILED = 1001,
    RECON_ERR_EXPORT_UNSUPPORTED_FORMAT = 1002,

    /* 内存 */
    RECON_ERR_MEMORY_LIMIT_REACHED = 1100,
    RECON_ERR_MEMORY_BLOCK_TOO_LARGE = 1101
} recon_status;

/* ------------------------------------------------------------------------- */
/* 数据结构                                                                  */
/* ------------------------------------------------------------------------- */

/**
 * 帧包。一次采集单元。
 *
 * 所有权：所有指针在调用期间有效，核心内部不持有；调用方在返回后立即复用或释放。
 * 时间戳：rgb/depth/pose/imu 必须位于同一时间轴（单调时钟），见 docs/02_采集与同步.md。
 */
typedef struct recon_frame_packet {
    uint64_t frame_id;                 /**< 单调递增帧号 */
    uint64_t timestamp_ns;             /**< 帧时间戳，单调时钟纳秒 */

    const uint8_t* rgb_data;           /**< BGRA8 像素，可能为 NULL */
    uint32_t       rgb_width;
    uint32_t       rgb_height;
    uint32_t       rgb_stride;         /**< 每行字节数 */

    const uint8_t* depth_data;         /**< uint16 毫米，可能为 NULL */
    uint32_t       depth_width;
    uint32_t       depth_height;
    uint32_t       depth_stride;       /**< 每行字节数 */
    const uint8_t* depth_confidence;   /**< uint8 置信度，可能为 NULL */

    recon_depth_source depth_source;   /**< 深度来源 */
    uint32_t       has_depth;

    const recon_camera_intrinsics* intrinsics; /**< RGB 内参 */
    const recon_pose*  pose;           /**< 平台先验位姿，可为 NULL */
    const recon_imu_sample* imu;       /**< 本帧之前的 IMU 时间窗，可为 NULL */
    uint32_t       imu_count;

    double         exposure_us;        /**< 曝光时间，微秒 */
    double         iso;                /**< 感光度 */
} recon_frame_packet;

/** 运行配置。 */
typedef struct recon_config {
    uint32_t abi_version;              /**< 必须等于 RECON_ABI_VERSION */
    const char* session_dir;           /**< 会话目录，UTF-8 路径 */
    recon_pipeline_branch forced_branch;/**< 0xFFFFFFFF 表示自动探测 */

    int32_t  preset;                   /**< 0=快速 1=标准 2=高精度 */
    uint32_t max_threads;              /**< 0=自动 */

    int32_t  enable_rgbd;              /**< 是否启用 RGBD 分支 */
    int32_t  enable_mvs;               /**< 是否启用 MVS */
    int32_t  enable_texture;           /**< 是否做 UV 烘焙 */

    double   voxel_size_m;             /**< TSDF 体素边长，米 */
    double   poisson_depth;            /**< 泊松八叉树深度 */
    double   mvs_min_depth_m;          /**< MVS 深度下界，米 */
    double   mvs_max_depth_m;          /**< MVS 深度上界，米 */
} recon_config;

/** 导出选项。 */
typedef struct recon_export_options {
    recon_export_format format;
    const char* out_path;              /**< 输出文件路径 */
    int32_t  apply_scale;              /**< 是否应用尺度因子 */
    int32_t  write_binary;             /**< PLY/OBJ 是否二进制输出 */
    int32_t  embed_texture;            /**< GLB 是否内嵌纹理 */
    int32_t  generate_normals;         /**< 是否导出法线 */
    double   target_face_count;        /**< 简化目标面数，<=0 表示不简化 */
} recon_export_options;

/** 进度回调。stage 见 recon_stage，progress 为 [0,1]。 */
typedef void (*recon_progress_cb)(void* user_data,
                                   int32_t stage,
                                   double progress,
                                   const char* message);

/** 状态文本回调。用于 UI 实时显示，极简文字。 */
typedef void (*recon_status_cb)(void* user_data,
                                 int32_t code,
                                 const char* text);

/** 日志回调。level: 0=debug 1=info 2=warn 3=error。 */
typedef void (*recon_log_cb)(void* user_data,
                             int32_t level,
                             const char* message);

/** 采集统计。 */
typedef struct recon_scan_stats {
    uint64_t frames_captured;
    uint64_t frames_accepted;          /**< 通过质量门限 */
    uint64_t keyframes;                /**< 对精度有增益的帧 */
    uint64_t imu_samples;
    double   duration_s;
    double   azimuth_coverage;         /**< 方位覆盖度 [0,1] */
    double   elevation_coverage;       /**< 俯仰覆盖度 [0,1] */
    uint32_t anomaly_blur;
    uint32_t anomaly_fast_motion;
    uint32_t anomaly_low_overlap;
    uint32_t anomaly_specular;
    uint32_t anomaly_low_texture;
    uint32_t anomaly_blind_spot;
} recon_scan_stats;

/** 网格信息。 */
typedef struct recon_mesh_info {
    uint64_t vertex_count;
    uint64_t triangle_count;
    uint64_t texture_count;
    uint32_t non_manifold_edges;
    uint32_t boundary_loops;
    uint32_t hole_count;
    double   hole_area_ratio;
    double   bbox_min[3];
    double   bbox_max[3];
    double   scale_factor;             /**< 应用的尺度因子 */
    double   mean_edge_length_m;       /**< 平均边长，米 */
    uint32_t poisson_depth;
    uint32_t degraded;                 /**< 是否处于降级态 */
} recon_mesh_info;

/** 尺度标定结果。 */
typedef struct recon_scale_result {
    int32_t  ok;
    double   scale;                    /**< 尺度因子 */
    double   relative_sigma;           /**< 相对不确定度 */
    int32_t  reference_type;           /**< 0=身份证 1=银行卡 2=自定义 3=手动 */
    double   reference_width_mm;
    double   reference_height_mm;
    double   measured_width_mm;        /**< 实测尺寸 */
    double   measured_height_mm;
} recon_scale_result;

/** 设备能力探测结果。 */
typedef struct recon_capability {
    recon_pipeline_branch branch;
    recon_depth_tier depth_tier;
    recon_depth_source depth_source;
    int32_t  has_camera;
    int32_t  has_imu;
    int32_t  has_lidar;
    int32_t  has_tof;
    int32_t  supports_gpu;
    int32_t  max_image_width;
    int32_t  max_image_height;
    int32_t  max_fps;
    int32_t  imu_max_rate_hz;
    uint64_t available_memory_bytes;
    uint64_t total_memory_bytes;
} recon_capability;

/**
 * 平台能力**事实**上报载荷（缺口 AC）。
 *
 * 与 `recon_capability` 的分工（**不要混用**）：
 *   · `recon_probe_data` = 平台层**上报的事实**（「这台设备有什么」），
 *     由平台层填、方向是 平台 → core；
 *   · `recon_capability` = core **判定后的结果**（「所以该走哪条分支、
 *     深度层级是什么」），由 core 填、方向是 core → 平台。
 * 后者比前者多出**派生字段**：`branch` / `depth_tier` / `depth_source`。
 *
 * 字段与 `recon::PlatformProbe`（core 内部）一一对应 —— 两处必须同步改，
 * `check_platform_parity.py` 的字段对等判据会守护这一点。
 *
 * ⚠️ 全部使用 `int32_t` 而非 `bool`：POD 跨语言边界上 `bool` 的宽度不保证
 *    （C++ `bool` 为 1 字节，而 Swift `Bool` / Kotlin `Boolean` 经 JNI 传递时
 *    按 1 字节读，某些 ABI 下按字节对齐会产生填充歧义）。本工程既有
 *    `recon_capability` 已全用 `int32_t`，此处沿用同一约定。
 */
typedef struct recon_probe_data {
    int32_t  has_camera;
    int32_t  has_imu;
    int32_t  has_lidar;
    int32_t  has_tof;
    int32_t  has_algo_depth;           /**< 算法/运动推断深度（如 ARCore） */
    int32_t  supports_gpu;
    int32_t  max_image_width;
    int32_t  max_image_height;
    int32_t  max_fps;
    int32_t  imu_max_rate_hz;
    uint64_t total_memory_bytes;
    uint64_t available_memory_bytes;
} recon_probe_data;

/* ------------------------------------------------------------------------- */
/* 生命周期                                                                  */
/* ------------------------------------------------------------------------- */

/** 创建上下文。config 为 NULL 时使用默认配置。 */
RECON_API recon_status recon_create(const recon_config* config,
                                     void** out_context);

/** 销毁上下文。可为 NULL。 */
RECON_API void recon_destroy(void* context);

/** 查询 ABI 版本，用于 App 与 core 一致性校验。 */
RECON_API uint32_t recon_abi_version(void);

/** 释放由核心分配、返回给调用方的字符串。 */
RECON_API void recon_free_string(char* str);

/** 释放由核心分配、返回给调用方的缓冲区。 */
RECON_API void recon_free_buffer(void* buffer, size_t size);

/* ------------------------------------------------------------------------- */
/* 能力探测与参数                                                            */
/* ------------------------------------------------------------------------- */

/**
 * 上报平台能力**事实**（缺口 AC）。
 *
 * ── 为什么需要这个函数（实测根因）────────────────────────────────────────
 *
 * 平台侧原先只能经 `recon::SetPlatformProbeProvider` 注册，而该声明在
 * **内部头 `recon/context.h`**，而平台层的架构铁律是「只允许 include
 * `recon_c_api.h`」（`platform/android/.../native-lib.cpp:6` 明文，G7-b 亦守）。
 * 两条约束叠加的结果是：**双端都注册不了** → 提供者恒为空 → `ProbePlatform()`
 * 恒返回 `kFallback{}`（全零）→ `has_camera=0` + `depth_tier=NONE` +
 * `branch=MONO` → 真机一进扫描就被 `handleCapabilityProbe` 判成 T-03
 * 「相机不可用」→ `Failed`。**装得上、起得来、点不动。**
 *
 * ── 为什么是「数据上报」而不是「函数指针注册」────────────────────────────
 *
 * 候选方案是在 C ABI 上暴露 `recon_set_probe_provider(fn)`（函数指针）。
 * **否决**，三条理由：
 *   ① 跨语言函数指针是 C ABI 上最容易出错的一类边界（调用约定、生命周期、
 *      Swift `@convention(c)` 与 ObjC block、Kotlin JNI 桥各写一遍），
 *      而本工程铁律要求「平台无关逻辑一份实现」—— 函数指针会把这套逻辑
 *      变成**两端各写一份桥**；
 *   ② `Context::ProbeCapability()` 有**一次性缓存**（`capability_probed_`），
 *      探测语义就是「启动时取一次事实」，**不需要**可反复调用的回调；
 *   ③ POD 数据过边界是本工程已建立的成熟模式（`recon_capability` /
 *      `recon_scan_stats` / `recon_mesh_info` 全是 POD），复用同一模式
 *      意味着三端字段对等判据（G10/闸门 7）能**直接覆盖**新入口，不需新机制。
 *
 * ── 语义与约束 ──────────────────────────────────────────────────────────
 *
 * · **全局**（不绑定 context）：能力是设备事实，与某个上下文实例无关；
 *   平台层在 App 启动时调一次即可。绑定 context 会让「未 create 就无法
 *   上报」这种无意义的顺序依赖出现。
 * · **幂等**：可重复调用，后一次覆盖前一次（便于「重新探测」更新事实）。
 * · **未上报时**：核心保持既有行为（`kFallback{}` 全零），**不伪造**能力。
 *   平台层因此**必须**在 `recon_create` 之前或之后尽早调用本函数。
 * · `in` 为 NULL 时返回 `RECON_ERR_INVALID_ARGUMENT`，**不清空**已上报数据
 *   （避免一次误调用把可用状态打回全零）。
 */
RECON_API recon_status recon_set_probe_data(const recon_probe_data* in);

/**
 * 探测设备能力，决定管线分支。
 * 必须在任何采集之前调用一次。
 */
RECON_API recon_status recon_probe_capability(void* context,
                                              recon_capability* out_capability);

/** 设置参数。key 为 ASCII 键名，取值见 docs/08_参数体系.md。 */
RECON_API recon_status recon_set_param(void* context,
                                        const char* key,
                                        int32_t value_int,
                                        double value_double);

/** 读取参数。 */
RECON_API recon_status recon_get_param(void* context,
                                        const char* key,
                                        int32_t* out_value_int,
                                        double* out_value_double);

/** 应用预设。0=快速 1=标准 2=高精度。 */
RECON_API recon_status recon_apply_preset(void* context, int32_t preset);

/** 导出会话参数摘要，用于审计与复现。 */
RECON_API recon_status recon_export_params(void* context,
                                           char** out_json);

/* ------------------------------------------------------------------------- */
/* 采集                                                                      */
/* ------------------------------------------------------------------------- */

/** 开始一次采集会话，重置统计。 */
RECON_API recon_status recon_start_session(void* context,
                                           const char* session_id);

/**
 * 推入一帧。采集期高频调用，核心内部异步处理。
 * 内部队列满时阻塞返回 RECON_ERR_CAPTURE_STALLED，由平台层决定是否丢帧。
 */
RECON_API recon_status recon_push_frame(void* context,
                                         const recon_frame_packet* packet);

/** 结束采集，转入重建准备。 */
RECON_API recon_status recon_finish_capture(void* context);

/** 读取采集统计。 */
RECON_API recon_status recon_get_scan_stats(void* context,
                                             recon_scan_stats* out_stats);

/* ------------------------------------------------------------------------- */
/* 重建                                                                      */
/* ------------------------------------------------------------------------- */

/**
 * 执行重建。阻塞直到完成、失败或被取消。
 * progress_cb / status_cb 可为 NULL。
 */
RECON_API recon_status recon_run_reconstruction(void* context,
                                                recon_progress_cb progress_cb,
                                                recon_status_cb status_cb,
                                                recon_log_cb log_cb,
                                                void* user_data);

/**
 * 尺度标定。使用参照物图像解算尺度因子。
 * reference_type 见 recon_scale_result.reference_type。
 * detected_corners 为 8 个角点坐标（x0,y0,...,x3,y3），顺序为左上、右上、右下、左下。
 */
RECON_API recon_status recon_calibrate_scale(void* context,
                                              const recon_frame_packet* reference_frame,
                                              const double* detected_corners,
                                              int32_t reference_type,
                                              double ref_width_mm,
                                              double ref_height_mm,
                                              recon_scale_result* out_result);

/** 手动设定尺度因子（用户手动输入尺寸微调）。 */
RECON_API recon_status recon_set_manual_scale(void* context,
                                               double scale,
                                               recon_scale_result* out_result);

/** 读取网格信息。重建成功后有效。 */
RECON_API recon_status recon_get_mesh_info(void* context,
                                           recon_mesh_info* out_info);

/* ------------------------------------------------------------------------- */
/* 导出                                                                      */
/* ------------------------------------------------------------------------- */

/** 导出模型。写入后执行校验，校验失败返回 RECON_ERR_EXPORT_VALIDATION_FAILED。 */
RECON_API recon_status recon_export(void* context,
                                    const recon_export_options* options);

/* ------------------------------------------------------------------------- */
/* 控制                                                                      */
/* ------------------------------------------------------------------------- */

/**
 * 请求取消。线程安全，可在任意线程调用。
 * 取消后 recon_run_reconstruction 在当前算子完成后返回 RECON_ERR_CANCELLED。
 */
RECON_API recon_status recon_cancel(void* context);

/** 查询是否已请求取消。 */
RECON_API int32_t recon_is_cancelled(void* context);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* RECON_RECON_C_API_H_ */
