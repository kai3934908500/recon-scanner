# xatlas（UV 展开，MIT）

## 来源

| 项 | 值 |
|---|---|
| 仓库 | https://github.com/jpcy/xatlas |
| 分支 | `master` |
| **commit** | `f700c7790aaa030e794b52ba7791a05c085faf0c` |
| commit 日期 | 2022-07-25（上游此后无更新，项目已稳定） |
| 下载日期 | 2026-10-30 |
| 下载 URL | `https://codeload.github.com/jpcy/xatlas/zip/refs/heads/master` |
| 归档大小 | 7,125,026 B（1093 个条目） |
| 下载耗时 | 2 min 56 s（≈ 40 KB/s） |
| 许可 | **MIT**（Copyright 2018-2020 Jonathan Young） |

> **国内网络说明**：本次下载**未使用 VPN**。实测结果（2026-10-30）：
>
> | 地址 | 结果 |
> |---|---|
> | `github.com/jpcy/xatlas`（网页） | HTTP 200，7.9 s |
> | **`codeload.github.com`（归档下载）** | **HTTP 200，可完整下载，≈ 40 KB/s** |
> | `raw.githubusercontent.com` | 不通（连接被拒） |
> | `ghproxy.net` / `ghfast.top` / `hub.gitmirror.com` | 不通 |
> | `gitclone.com` | HTTP 502 |
> | Gitee 镜像（API 搜索） | **无 xatlas 镜像** |
>
> 结论：**只有 `codeload.github.com` 直连可用**，慢但能完整下载。

## 只提取了核心 3 个文件

上游归档里包含 examples、test、premake 二进制，以及
`source/thirdparty/bgfx/3rdparty/dxsdk/`（含 **DirectX SDK 的 EULA**，
**不是 MIT**）。整包引入会污染许可扫描，因此只提取：

```
xatlas/
  LICENSE       1075 B
  xatlas.cpp   332735 B
  xatlas.h       9904 B
  xatlas_c.h     6618 B   ← C API，便于 iOS/Android 原生侧调用
```

合计约 **350 KB**，**零外部依赖**，仅需 C++11。

## 验证结果（2026-10-30）

| 项 | 结果 |
|---|---|
| 单独编译（MinGW g++ `-std=c++11 -O2`） | **通过**，6.1 s，零警告 |
| 端到端展开（立方体 12 三角形） | **通过**：6 个 chart、8 顶点 → 24 个（接缝分裂）、索引 36 不变 |
| 输出坐标口径 | **像素坐标**（图集 1111×749），需除以 `width`/`height` 归一化 |
| 与自研回退路径对比 | 立方体两侧均为 **6 个 chart / 24 个 UV 顶点**，行为一致 |

## 与自研回退路径的关系

`docs/06_网格化与后处理.md` §2.8 规定：xatlas 是**主路径**，平面/立方体投影是
**失败回退**。当前 `core/src/mesh/uv_unwrap.cpp` 实现的是回退路径
（见 §2.5.1 (23)）。

接入 xatlas 后应改为：
1. 默认走 xatlas（它做 chart 合并 + LSCM/ABF 参数化，接缝更少、拉伸更小）；
2. xatlas 超时（> 60 s）或失败 → 回退到 `UnwrapMesh`；
3. 两者都失败 → 报 `W_MESH_UV_FALLBACK_CUBE`。

## 升级方式

若需更新：重新下载归档并用同样的 4 个文件覆盖，然后**更新本文件的 commit 与
日期**。不要整包解压进 `third_party/`（会带入 DirectX SDK EULA 等非 MIT 内容）。
