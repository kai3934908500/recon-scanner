// SPDX-License-Identifier: BSD-3-Clause
//
// 参数系统。全局参数注册表。
//
// 契约（docs/08_参数体系.md）：
//   - 参数键名 snake_case，与 C++ 成员名一一对应。
//   - 三档预设：0=快速 1=标准 2=高精度。
//   - R-PREC-6：所有精度相关阈值必须可被用户覆盖，且覆盖后不发生静默回退。
//     因此合法性校验只做「夹取到合法区间」，绝不回退到默认值。
//
// 本文件是头文件声明，实现见 params.cpp。

#ifndef RECON_CORE_PARAMS_H_
#define RECON_CORE_PARAMS_H_

#include <cstdint>
#include <string>
#include <vector>

namespace recon {

enum class ParamType : uint8_t {
  kInt = 0,
  kDouble = 1,
  kBool = 2,
};

enum class Preset : int32_t {
  kFast = 0,
  kStandard = 1,
  kHighPrecision = 2,
};

// 参数规格。注册表中的静态描述。
struct ParamSpec {
  std::string key;
  ParamType type;
  double default_value;
  double fast_value;
  double standard_value;
  double high_precision_value;
  double min_value;
  double max_value;
  bool user_adjustable;      // 是否在普通 UI 暴露
  bool expert_only;          // 是否需要展开专家区
  const char* affects;       // 影响的精度维度
  const char* note;          // 备注
};

// 参数存储。所有参数以 double 承载，整数与布尔同样如此，避免变体。
class ParamStore {
 public:
  ParamStore();

  // 注册一个参数。重复键返回 false。
  bool Register(const ParamSpec& spec);

  // 按预设重设全部参数。保留用户覆盖项。
  void ApplyPreset(Preset preset);

  // 读取。键不存在返回 false。
  bool Get(const std::string& key, double* out_value) const;

  // 写入。键不存在或类型不符返回 false；越界时夹取到合法区间。
  bool Set(const std::string& key, double value);

  // 重置单个参数到预设值。
  bool ResetToPreset(const std::string& key);

  // 重置全部到预设值。
  void ResetAll(Preset preset);

  // 导出为 JSON，用于审计与复现。
  std::string ToJson() const;

  // 当前生效的预设。
  Preset current_preset() const { return preset_; }

  // 列出全部参数规格。
  const std::vector<ParamSpec>& specs() const { return specs_; }

 private:
  const ParamSpec* Find(const std::string& key) const;
  double PresetValue(const ParamSpec& spec) const;

  std::vector<ParamSpec> specs_;
  std::vector<double> values_;
  std::vector<bool> user_overridden_;
  Preset preset_ = Preset::kStandard;
};

// 键名常量。与 docs/08_参数体系.md 保持一致。
namespace keys {
// 采集类
inline constexpr const char* kCapResolutionWidth = "cap_resolution_width";
inline constexpr const char* kCapFrameRate = "cap_frame_rate";
inline constexpr const char* kCapExposureTimeUs = "cap_exposure_time_us";
inline constexpr const char* kCapIso = "cap_iso";
inline constexpr const char* kCapImuEnabled = "cap_imu_enabled";
inline constexpr const char* kCapQualityGateBlurVol = "cap_quality_gate_blur_vol";
inline constexpr const char* kCapQualityGateCoverage = "cap_quality_gate_coverage";
inline constexpr const char* kCapQualityGateSatRatio = "cap_quality_gate_sat_ratio";
inline constexpr const char* kCapQualityGateTextureDensity = "cap_quality_gate_texture_density";
inline constexpr const char* kCapMaxAngleSpeed = "cap_max_angle_speed";
inline constexpr const char* kCapMaxLinearSpeed = "cap_max_linear_speed";

// 同步类
inline constexpr const char* kSyncMaxDriftPpm = "sync_max_drift_ppm";
inline constexpr const char* kSyncMaxImuGapUs = "sync_max_imu_gap_us";
inline constexpr const char* kSyncImuPreintegrationIntervalUs = "sync_imu_preintegration_interval_us";

// VIO 类
inline constexpr const char* kVioFeatureCount = "vio_feature_count";
inline constexpr const char* kVioMinFeatureDistance = "vio_min_feature_distance";
inline constexpr const char* kVioCornerResponseThreshold = "vio_corner_response_threshold";
inline constexpr const char* kVioKltWindowSize = "vio_klt_window_size";
inline constexpr const char* kVioKltPyramidLevels = "vio_klt_pyramid_levels";
inline constexpr const char* kVioKltMaxIterations = "vio_klt_max_iterations";
inline constexpr const char* kVioKltMinError = "vio_klt_min_error";
inline constexpr const char* kVioMsckfWindowSize = "vio_msckf_window_size";
inline constexpr const char* kVioHuberDelta = "vio_huber_delta";
inline constexpr const char* kVioImuNoiseGyro = "vio_imu_noise_gyro";
inline constexpr const char* kVioImuNoiseAcc = "vio_imu_noise_acc";
inline constexpr const char* kVioImuBiasNoiseGyro = "vio_imu_bias_noise_gyro";
inline constexpr const char* kVioImuBiasNoiseAcc = "vio_imu_bias_noise_acc";
inline constexpr const char* kVioStaticDetectionAccelTol = "vio_static_detection_accel_tol";
inline constexpr const char* kVioStaticDetectionGyroTol = "vio_static_detection_gyro_tol";
inline constexpr const char* kVioStaticDetectionDurationS = "vio_static_detection_duration_s";
inline constexpr const char* kVioPriorRotationSigmaDeg = "vio_prior_rotation_sigma_deg";
inline constexpr const char* kVioPriorTranslationSigmaM = "vio_prior_translation_sigma_m";

// SfM 类
//
// **键名权威来源是 docs/08 §G4（共 20 项）**。此前代码里用的
// `sfm_ratio_test` / `sfm_bundle_max_iterations` / `sfm_loop_closure_*` 等
// 是早期草稿键名，与文档契约漂移。本条统一收敛到 docs/08 G4 的键名，
// 避免「文档一套、代码一套」两处实现各自漂移（项目铁律）。
inline constexpr const char* kSfmFeatureCount = "sfm_feature_count";
inline constexpr const char* kSfmFeatureEdgeThreshold = "sfm_feature_edge_threshold";
inline constexpr const char* kSfmMaxNumNeighbors = "sfm_max_num_neighbors";
inline constexpr const char* kSfmMatchRatioThreshold = "sfm_match_ratio_threshold";
inline constexpr const char* kSfmMinTrackLength = "sfm_min_track_length";
inline constexpr const char* kSfmRansacMaxIterations = "sfm_ransac_max_iterations";
inline constexpr const char* kSfmRansacMinInliers = "sfm_ransac_min_inliers";
inline constexpr const char* kSfmRansacConfidence = "sfm_ransac_confidence";
inline constexpr const char* kSfmRansacMaxErrorPx = "sfm_ransac_max_error_px";
inline constexpr const char* kSfmBaMaxIterations = "sfm_ba_max_iterations";
inline constexpr const char* kSfmBaConvergenceTolerance =
    "sfm_ba_convergence_tolerance";
inline constexpr const char* kSfmBaLossFunction = "sfm_ba_loss_function";
inline constexpr const char* kSfmBaHuberDelta = "sfm_ba_huber_delta";
inline constexpr const char* kSfmBaMinNumImages = "sfm_ba_min_num_images";
inline constexpr const char* kSfmLoopDetectionEnabled = "sfm_loop_detection_enabled";
inline constexpr const char* kSfmLoopMinInliers = "sfm_loop_min_inliers";
inline constexpr const char* kSfmLoopSimilarityScaleTolerance =
    "sfm_loop_similarity_scale_tolerance";
inline constexpr const char* kSfmIncrementalBaEnabled =
    "sfm_incremental_ba_enabled";
inline constexpr const char* kSfmMaxSparsePoints = "sfm_max_sparse_points";
inline constexpr const char* kSfmScalePriorEnabled = "sfm_scale_prior_enabled";

// MVS 类
inline constexpr const char* kMvsPatchSize = "mvs_patch_size";
inline constexpr const char* kMvsPatchIterations = "mvs_patch_iterations";
inline constexpr const char* kMvsPatchRandomSearchRadius = "mvs_patch_random_search_radius";
inline constexpr const char* kMvsNumViews = "mvs_num_views";
inline constexpr const char* kMvsSubpixelIterations = "mvs_subpixel_iterations";
inline constexpr const char* kMvsGeoConsistencyThreshold = "mvs_geo_consistency_threshold";
inline constexpr const char* kMvsPhotoThreshold = "mvs_photo_threshold";
inline constexpr const char* kMvsFusionVoxelSizeM = "mvs_fusion_voxel_size_m";
inline constexpr const char* kMvsImageScale = "mvs_image_scale";
inline constexpr const char* kMvsMinDepthM = "mvs_min_depth_m";
inline constexpr const char* kMvsMaxDepthM = "mvs_max_depth_m";
inline constexpr const char* kMvsGpuTileSize = "mvs_gpu_tile_size";
inline constexpr const char* kMvsPmSearchRadiusRatio = "mvs_pm_search_radius_ratio";
inline constexpr const char* kMvsSubpixelWindowRatio = "mvs_subpixel_window_ratio";
inline constexpr const char* kMvsNumThreads = "mvs_num_threads";

// 深度融合类（G6）
// 由 tools/gen_g6_params.py 从 docs/04 §7.1 生成，勿手改。
// 契约：docs/08 §1 G6（权威定义）、docs/04 §7.1（展开规格）。
inline constexpr const char* kRgbdTsdfVoxelSize = "rgbd_tsdf_voxel_size";
inline constexpr const char* kRgbdTsdfTruncation = "rgbd_tsdf_truncation";
inline constexpr const char* kRgbdVoxelBlockSize = "rgbd_voxel_block_size";
inline constexpr const char* kRgbdMaxBlocksInMemory = "rgbd_max_blocks_in_memory";
inline constexpr const char* kRgbdFusionEnabled = "rgbd_fusion_enabled";
inline constexpr const char* kRgbdDepthPreprocessEnabled = "rgbd_depth_preprocess_enabled";
inline constexpr const char* kRgbdFlyingPixelThreshold = "rgbd_flying_pixel_threshold";
inline constexpr const char* kRgbdBilateralSigmaColor = "rgbd_bilateral_sigma_color";
inline constexpr const char* kRgbdBilateralSigmaSpace = "rgbd_bilateral_sigma_space";
inline constexpr const char* kRgbdAlignToRgbEnabled = "rgbd_align_to_rgb_enabled";
inline constexpr const char* kRgbdConfidenceThreshold = "rgbd_confidence_threshold";
inline constexpr const char* kRgbdPoseGraphOptimization = "rgbd_pose_graph_optimization";
inline constexpr const char* kRgbdPoseGraphIterations = "rgbd_pose_graph_iterations";
inline constexpr const char* kRgbdPoseGraphEdgeThreshold = "rgbd_pose_graph_edge_threshold";
inline constexpr const char* kRgbdLidarRole = "rgbd_lidar_role";
inline constexpr const char* kRgbdMvsFusionWeight = "rgbd_mvs_fusion_weight";
inline constexpr const char* kRgbdTsdfTruncationRatio = "rgbd_tsdf_truncation_ratio";
inline constexpr const char* kRgbdTsdfPriorVoxelSize = "rgbd_tsdf_prior_voxel_size";
inline constexpr const char* kRgbdTsdfMinWeight = "rgbd_tsdf_min_weight";
inline constexpr const char* kRgbdTsdfMinViews = "rgbd_tsdf_min_views";
inline constexpr const char* kRgbdTsdfDispersionMax = "rgbd_tsdf_dispersion_max";
inline constexpr const char* kRgbdTsdfConfidenceRef = "rgbd_tsdf_confidence_ref";
inline constexpr const char* kRgbdTsdfColorViewWeightExp = "rgbd_tsdf_color_view_weight_exp";
inline constexpr const char* kRgbdSigmaRef = "rgbd_sigma_ref";
inline constexpr const char* kRgbdSigmaTof = "rgbd_sigma_tof";
inline constexpr const char* kRgbdSigmaLidar = "rgbd_sigma_lidar";
inline constexpr const char* kRgbdSigmaAlgoRel = "rgbd_sigma_algo_rel";
inline constexpr const char* kRgbdSigmaAlgoAbs = "rgbd_sigma_algo_abs";
inline constexpr const char* kRgbdIncidenceCosMin = "rgbd_incidence_cos_min";
inline constexpr const char* kRgbdWeightCap = "rgbd_weight_cap";
inline constexpr const char* kRgbdConfidenceSource = "rgbd_confidence_source";
inline constexpr const char* kRgbdConfidenceLow = "rgbd_confidence_low";
inline constexpr const char* kRgbdConfidenceHigh = "rgbd_confidence_high";
inline constexpr const char* kRgbdConfidenceFloor = "rgbd_confidence_floor";
inline constexpr const char* kRgbdConfidencePlaneWindow = "rgbd_confidence_plane_window";
inline constexpr const char* kRgbdConfidenceEdgeScale = "rgbd_confidence_edge_scale";
inline constexpr const char* kRgbdFlyingPixelNoiseK = "rgbd_flying_pixel_noise_k";
inline constexpr const char* kRgbdBilateralSupportRatio = "rgbd_bilateral_support_ratio";
inline constexpr const char* kRgbdBilateralEdgeConfidence = "rgbd_bilateral_edge_confidence";
inline constexpr const char* kRgbdBilateralMinBranchMedian = "rgbd_bilateral_min_branch_median";
inline constexpr const char* kRgbdBilateralFgColorMin = "rgbd_bilateral_fg_color_min";
inline constexpr const char* kRgbdAlignDerivedConfScale = "rgbd_align_derived_conf_scale";
inline constexpr const char* kRgbdAlignMaxBaseline = "rgbd_align_max_baseline";
inline constexpr const char* kRgbdDepthMin = "rgbd_depth_min";
inline constexpr const char* kRgbdDepthMax = "rgbd_depth_max";
inline constexpr const char* kRgbdIcpEnabled = "rgbd_icp_enabled";
inline constexpr const char* kRgbdIcpMaxIterations = "rgbd_icp_max_iterations";
inline constexpr const char* kRgbdIcpConvergenceTrans = "rgbd_icp_convergence_trans";
inline constexpr const char* kRgbdIcpConvergenceRot = "rgbd_icp_convergence_rot";
inline constexpr const char* kRgbdIcpPointToPointWeight = "rgbd_icp_point_to_point_weight";
inline constexpr const char* kRgbdIcpPlaneWindow = "rgbd_icp_plane_window";
inline constexpr const char* kRgbdIcpHuberDeltaScale = "rgbd_icp_huber_delta_scale";
inline constexpr const char* kRgbdIcpMinCorrespondences = "rgbd_icp_min_correspondences";
inline constexpr const char* kRgbdIcpMinDepth = "rgbd_icp_min_depth";
inline constexpr const char* kRgbdIcpMaxDepth = "rgbd_icp_max_depth";
inline constexpr const char* kRgbdIcpSampleStride = "rgbd_icp_sample_stride";
inline constexpr const char* kRgbdImuPriorWeight = "rgbd_imu_prior_weight";
inline constexpr const char* kRgbdImuRotationSigmaDeg = "rgbd_imu_rotation_sigma_deg";
inline constexpr const char* kRgbdImuTranslationSigmaM = "rgbd_imu_translation_sigma_m";
inline constexpr const char* kRgbdPlatformPriorWeight = "rgbd_platform_prior_weight";
inline constexpr const char* kRgbdLoopEnabled = "rgbd_loop_enabled";
inline constexpr const char* kRgbdLoopMinInliers = "rgbd_loop_min_inliers";
inline constexpr const char* kRgbdLoopMaxReprojPx = "rgbd_loop_max_reproj_px";
inline constexpr const char* kRgbdLoopScaleConsistencyMax = "rgbd_loop_scale_consistency_max";
inline constexpr const char* kRgbdScaleAnchorEnabled = "rgbd_scale_anchor_enabled";
inline constexpr const char* kRgbdScaleAnchorSigma = "rgbd_scale_anchor_sigma";
inline constexpr const char* kRgbdDepthMvsMedianMax = "rgbd_depth_mvs_median_max";
inline constexpr const char* kRgbdDepthMvsConsistencyMin = "rgbd_depth_mvs_consistency_min";
inline constexpr const char* kRgbdDepthConsistencyWeight = "rgbd_depth_consistency_weight";
inline constexpr const char* kRgbdDepthConsistencySigma = "rgbd_depth_consistency_sigma";
inline constexpr const char* kRgbdHoleFillMvsConfMax = "rgbd_hole_fill_mvs_conf_max";
inline constexpr const char* kRgbdHoleFillTransitionPx = "rgbd_hole_fill_transition_px";
inline constexpr const char* kRgbdHoleFillDepthWeightScale = "rgbd_hole_fill_depth_weight_scale";
inline constexpr const char* kRgbdReflectiveRejectEnabled = "rgbd_reflective_reject_enabled";
inline constexpr const char* kRgbdReflectiveRejectSigma = "rgbd_reflective_reject_sigma";
inline constexpr const char* kRgbdGpuWorkgroupX = "rgbd_gpu_workgroup_x";
inline constexpr const char* kRgbdGpuWorkgroupY = "rgbd_gpu_workgroup_y";
inline constexpr const char* kRgbdGpuVoxelPerThread = "rgbd_gpu_voxel_per_thread";
// 遗留键（不在 §7.1 规格内，但被 TSDF 融合真实消费，保留）。
// 语义与 §7.1 的软降权参数不等价，属硬门槛/旧命名，待后续统一。
inline constexpr const char* kRgbdTsdfMaxDepth = "rgbd_tsdf_max_depth";
inline constexpr const char* kRgbdMinDepthConfidence = "rgbd_min_depth_confidence";
inline constexpr const char* kRgbdBilateralFilterSigma = "rgbd_bilateral_filter_sigma";
inline constexpr const char* kRgbdOutlierRemovalK = "rgbd_outlier_removal_k";

// 网格化类
inline constexpr const char* kMeshPoissonDepth = "mesh_poisson_depth";
inline constexpr const char* kMeshPoissonDepthMin = "mesh_poisson_depth_min";
inline constexpr const char* kMeshPoissonDepthMax = "mesh_poisson_depth_max";
inline constexpr const char* kMeshPoissonScale = "mesh_poisson_scale";
inline constexpr const char* kMeshPoissonSamplesPerNode = "mesh_poisson_samples_per_node";
inline constexpr const char* kMeshPoissonPointWeight = "mesh_poisson_point_weight";
inline constexpr const char* kMeshPoissonDegree = "mesh_poisson_degree";
inline constexpr const char* kMeshPoissonBType = "mesh_poisson_btype";
inline constexpr const char* kMeshPoissonPrune = "mesh_poisson_prune";
inline constexpr const char* kMeshPoissonScaleIsotropic = "mesh_poisson_scale_isotropic";
inline constexpr const char* kMeshPoissonConfidence = "mesh_poisson_confidence";
inline constexpr const char* kMeshPoissonInterpolate = "mesh_poisson_interpolate";
inline constexpr const char* kMeshChunkedEnabled = "mesh_chunked_enabled";
inline constexpr const char* kMeshChunkTargetCells = "mesh_chunk_target_cells";
inline constexpr const char* kMeshTrimConfQuantile = "mesh_trim_conf_quantile";

// 后处理类
inline constexpr const char* kPostOutlierK = "post_outlier_k";
inline constexpr const char* kPostOutlierStdRatio = "post_outlier_std_ratio";
inline constexpr const char* kPostHoleAreaMicroRatio = "post_hole_area_micro_ratio";
inline constexpr const char* kPostHoleAreaSmallRatio = "post_hole_area_small_ratio";
inline constexpr const char* kPostHoleAreaLargeRatio = "post_hole_area_large_ratio";
inline constexpr const char* kPostNormalConsistencyMin = "post_normal_consistency_min";
inline constexpr const char* kPostEdgeLengthVarianceMax = "post_edge_length_variance_max";
inline constexpr const char* kPostMinAspectRatio = "post_min_aspect_ratio";
inline constexpr const char* kPostUvChartCount = "post_uv_chart_count";
inline constexpr const char* kPostUvPadding = "post_uv_padding";
inline constexpr const char* kPostUvIterations = "post_uv_iterations";
inline constexpr const char* kPostTextureAtlasSize = "post_texture_atlas_size";
inline constexpr const char* kPostTextureViewWeight = "post_texture_view_weight";
inline constexpr const char* kPostTextureOcclusionThreshold = "post_texture_occlusion_threshold";
inline constexpr const char* kPostSimplifyTargetFaces = "post_simplify_target_faces";
inline constexpr const char* kPostSimplifyErrorRatio = "post_simplify_error_ratio";

// 尺度类
inline constexpr const char* kScaleReferenceType = "scale_reference_type";
inline constexpr const char* kScaleRefWidthMm = "scale_ref_width_mm";
inline constexpr const char* kScaleRefHeightMm = "scale_ref_height_mm";
inline constexpr const char* kScaleRefSizeToleranceMm = "scale_ref_size_tolerance_mm";
inline constexpr const char* kScaleRefFlatnessTolMm = "scale_ref_flatness_tol_mm";
inline constexpr const char* kScaleImuPriorRelativeSigma = "scale_imu_prior_relative_sigma";
inline constexpr const char* kScaleConsistencyMax = "scale_consistency_max";
inline constexpr const char* kScaleMinIncidenceDeg = "scale_min_incidence_deg";
inline constexpr const char* kScaleRefineHuberPx = "scale_refine_huber_px";
inline constexpr const char* kScaleRefineIterations = "scale_refine_iterations";

// 扫描与关键帧类
inline constexpr const char* kScanMode = "scan_mode";
inline constexpr const char* kScanViewpointDistanceM = "scan_viewpoint_distance_m";
inline constexpr const char* kScanIdealDistanceMinM = "scan_ideal_distance_min_m";
inline constexpr const char* kScanIdealDistanceMaxM = "scan_ideal_distance_max_m";
inline constexpr const char* kScanAzimuthBins = "scan_azimuth_bins";
inline constexpr const char* kScanElevationBins = "scan_elevation_bins";
inline constexpr const char* kScanKfMinRotationDeg = "scan_kf_min_rotation_deg";
inline constexpr const char* kScanKfMinTranslationRatio = "scan_kf_min_translation_ratio";
inline constexpr const char* kScanKfMinQualityScore = "scan_kf_min_quality_score";
inline constexpr const char* kScanKfMinViewIncrementDeg = "scan_kf_min_view_increment_deg";
inline constexpr const char* kScanCoverageTargetAzimuth = "scan_coverage_target_azimuth";
inline constexpr const char* kScanCoverageTargetElevation = "scan_coverage_target_elevation";
inline constexpr const char* kScanBottomStrategy = "scan_bottom_strategy";
inline constexpr const char* kScanStabilizationEnabled = "scan_stabilization_enabled";
inline constexpr const char* kScanReviewMinCoverage = "scan_review_min_coverage";
inline constexpr const char* kScanReviewMaxAnomalies = "scan_review_max_anomalies";

// 性能类
//
// 内存保护白名单（docs/13 §4.2）的承载组。W-1..W-10 全部映射到本组键或
// `rgbd_max_blocks_in_memory`，任何自动降级路径**只允许**写这些键。
inline constexpr const char* kPerfThreadPoolSize = "perf_thread_pool_size";      // W-1
inline constexpr const char* kPerfGpuBatchSize = "perf_gpu_batch_size";          // W-2
inline constexpr const char* kPerfBlockBatchSize = "perf_block_batch_size";      // W-5
inline constexpr const char* kPerfMemoryPoolPreallocMb = "perf_memory_pool_prealloc_mb";  // W-6
inline constexpr const char* kPerfReadAheadFrames = "perf_read_ahead_frames";    // W-10
inline constexpr const char* kPerfMaxConcurrentTasks = "perf_max_concurrent_tasks";  // W-9
inline constexpr const char* kPerfMemoryLimitMb = "perf_memory_limit_mb";        // 内存软上限
inline constexpr const char* kPerfMemoryHighWaterRatio = "perf_memory_high_water_ratio";  // §3.4
inline constexpr const char* kPerfSpillEnabled = "perf_spill_enabled";
inline constexpr const char* kPerfTsdfSpillEnabled = "perf_tsdf_spill_enabled";
inline constexpr const char* kPerfPoissonSpillEnabled = "perf_poisson_spill_enabled";
inline constexpr const char* kPerfMemoryTier = "perf_memory_tier";
inline constexpr const char* kPerfThermalThrottleLevel = "perf_thermal_throttle_level";
}  // namespace keys

// 构建默认参数注册表。
ParamStore BuildDefaultParams();

}  // namespace recon

#endif  // RECON_CORE_PARAMS_H_
