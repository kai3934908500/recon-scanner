// SPDX-License-Identifier: BSD-3-Clause
//
// 桌面验证工具。
//
// 用途：
//   1. 在开发机上跑黄金数据集回归（不可能每次都真机测试）
//   2. 打印参数注册表，供核对 docs/08_参数体系.md
//   3. 导出模型并跑校验
//
// 见 docs/12_构建与打包.md §5。

#include <cstdio>
#include <cstring>
#include <string>

#include "recon/context.h"
#include "recon/exporter.h"
#include "recon/mesh_repair.h"
#include "recon/params.h"
#include "recon/recon_c_api.h"

namespace {

int PrintUsage() {
  std::printf(
      "recon_cli — 重建核心验证工具\n"
      "\n"
      "用法:\n"
      "  recon_cli params                 打印参数注册表\n"
      "  recon_cli params-json            打印参数注册表 JSON（供 UI 接线验证）\n"
      "  recon_cli capability             打印设备能力探测结果\n"
      "  recon_cli export <in.obj> <out>   校验并导出模型\n"
      "\n"
      "退出码: 0=成功 1=校验失败 2=用法错误\n");
  return 2;
}

// 简易 OBJ 读取：仅支持 v / vn / vt / f。
recon::TriangleMesh LoadObj(const std::string& path) {
  recon::TriangleMesh mesh;
  FILE* fh = std::fopen(path.c_str(), "rb");
  if (fh == nullptr) return mesh;

  char line[1024];
  while (std::fgets(line, sizeof(line), fh) != nullptr) {
    if (line[0] == 'v' && line[1] == ' ') {
      double x = 0, y = 0, z = 0;
      if (std::sscanf(line + 2, "%lf %lf %lf", &x, &y, &z) == 3) {
        mesh.positions.emplace_back(x, y, z);
      }
    } else if (line[0] == 'f' && line[1] == ' ') {
      int a = 0, b = 0, c = 0;
      if (std::sscanf(line + 2, "%d %d %d", &a, &b, &c) == 3) {
        // OBJ 索引从 1 开始，且可能带 v/vt/vn 后缀。
        mesh.indices.push_back(static_cast<uint32_t>(a > 0 ? a - 1 : 0));
        mesh.indices.push_back(static_cast<uint32_t>(b > 0 ? b - 1 : 0));
        mesh.indices.push_back(static_cast<uint32_t>(c > 0 ? c - 1 : 0));
      }
    }
  }
  std::fclose(fh);
  return mesh;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return PrintUsage();
  }

  const std::string cmd = argv[1];

  if (cmd == "params") {
    recon::ParamStore params = recon::BuildDefaultParams();
    std::printf("参数总数: %zu\n", params.specs().size());
    std::printf("%-40s %-8s %-12s %-12s %-12s %-12s %-8s\n", "键名", "类型",
                "默认", "快速", "标准", "高精度", "可调");
    for (const auto& s : params.specs()) {
      const char* type = s.type == recon::ParamType::kInt ? "int"
                        : s.type == recon::ParamType::kBool ? "bool" : "double";
      const char* adjustable = s.user_adjustable ? "是" : (s.expert_only ? "专家" : "否");
      std::printf("%-40s %-8s %-12.6g %-12.6g %-12.6g %-12.6g %-8s\n",
                  s.key.c_str(), type, s.default_value, s.fast_value,
                  s.standard_value, s.high_precision_value, adjustable);
    }
    return 0;
  }

  if (cmd == "params-json") {
    // 导出注册表 JSON（与 C ABI `recon_export_params` 同一份 `ParamStore::ToJson`）。
    // 用途：让 iOS 参数面板的接线结果可被**离线验证** ——
    // 解析器对本输出做 JSON 合法性检查，避免「Swift 解析失败 → 静默显示占位」。
    recon::ParamStore params = recon::BuildDefaultParams();
    std::printf("%s\n", params.ToJson().c_str());
    return 0;
  }

  if (cmd == "capability") {
    void* ctx = nullptr;
    if (recon_create(nullptr, &ctx) != RECON_OK) {
      std::printf("创建上下文失败\n");
      return 1;
    }
    recon_capability cap{};
    recon_probe_capability(ctx, &cap);
    const char* branch = cap.branch == RECON_BRANCH_RGBD ? "BRANCH_RGBD" : "BRANCH_MONO";
    const char* tier = cap.depth_tier == RECON_DEPTH_LIDAR   ? "LIDAR"
                       : cap.depth_tier == RECON_DEPTH_TOF  ? "TOF"
                       : cap.depth_tier == RECON_DEPTH_ALGO ? "ALGO"
                                                           : "NONE";
    std::printf("管线分支: %s\n", branch);
    std::printf("深度等级: %s\n", tier);
    std::printf("相机: %d  IMU: %d  GPU: %d\n", cap.has_camera, cap.has_imu,
                cap.supports_gpu);
    std::printf("最大分辨率: %dx%d @ %d fps\n", cap.max_image_width,
                cap.max_image_height, cap.max_fps);
    std::printf("IMU 最高采样率: %d Hz\n", cap.imu_max_rate_hz);
    recon_destroy(ctx);
    return 0;
  }

  if (cmd == "export") {
    if (argc < 4) {
      return PrintUsage();
    }
    const std::string in_path = argv[2];
    const std::string out_path = argv[3];

    recon::TriangleMesh mesh = LoadObj(in_path);
    if (mesh.positions.empty()) {
      std::printf("无法读取输入模型: %s\n", in_path.c_str());
      return 1;
    }

    recon::RecomputeVertexNormals(&mesh);
    recon::RemoveDegenerateTriangles(&mesh);
    recon::RemoveIsolatedVertices(&mesh);

    recon::MeshStats stats = recon::AnalyzeMesh(mesh);
    std::printf("输入: 顶点 %llu  三角形 %llu\n",
                static_cast<unsigned long long>(stats.vertex_count),
                static_cast<unsigned long long>(stats.triangle_count));
    std::printf("拓扑: 非流形边 %u  边界环 %u  退化 %u\n",
                stats.non_manifold_edges, stats.boundary_loops,
                stats.degenerate_triangles);

    recon::ExportOptions options;
    options.format = recon::ExportFormat::kGlb;
    options.scale_factor = 1.0;

    std::string main_data, secondary;
    recon::ValidationResult validation;
    if (!recon::ExportModel(mesh, options, &main_data, &secondary, &validation)) {
      std::printf("导出失败，校验错误:\n");
      for (const auto& e : validation.errors) {
        std::printf("  - %s\n", e.c_str());
      }
      return 1;
    }

    if (!recon::WriteFileAtomic(out_path, main_data)) {
      std::printf("写入失败: %s\n", out_path.c_str());
      return 1;
    }
    std::printf("导出成功: %s (%zu 字节)\n", out_path.c_str(), main_data.size());
    return 0;
  }

  return PrintUsage();
}
