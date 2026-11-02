# 一加平板 3 Pro（OnePlus Pad 3 Pro）设备档案

面向本仓库 Android 原生移植（NativeActivity + Vulkan）的目标设备说明。每条结论都标注 **已核实** / **未核实** 并给出出处；未核实的数字不要当成事实写进代码默认值。

- 设备：一加平板 3 Pro（国行，2026-05-15 开售）
- SoC：骁龙 8 Elite Gen 5（Snapdragon 8 Elite Gen 5，SM8850）
- GPU：Adreno 840
- 内存：16 GB（16GB+512GB 为顶配 SKU）
- 系统：Android 16 / ColorOS

---

## 一、硬件规格

### 1.1 屏幕

| 项目 | 值 | 状态 | 来源 |
|---|---|---|---|
| 尺寸 | 13.2 英寸 | 已核实 | [手机中国/腾讯新闻](https://so.html5.qq.com/page/real/search_news?docid=70000021_3766a0679e988652) |
| **分辨率** | **3392 × 2400**（3.4K 级） | **已核实** | [手机中国](https://so.html5.qq.com/page/real/search_news?docid=70000021_3766a0679e988652)、[IT之家](https://www.ithome.com/0/845/595.htm) |
| 像素密度 | 315 ppi | 已核实 | 同上（与 13.2″/3392×2400 自洽：√(3392²+2400²)/13.2 ≈ 314.8） |
| 面板类型 | LCD（TCL 华星 LTPS 背板） | 已核实（LCD）/ 已核实（LTPS，同面板兄弟机型） | [IT之家](https://www.ithome.com/0/845/595.htm) |
| 宽高比 | 7:5 | 已核实 | [IT之家](https://www.ithome.com/0/845/595.htm) |
| 最高刷新率 | 144 Hz（官方描述为"最高 144Hz 动态刷新率"） | 已核实 | [手机中国](https://so.html5.qq.com/page/real/search_news?docid=70000021_3766a0679e988652) |
| 刷新率可变范围 | 30–144 Hz | 已核实（同面板 OPPO Pad 4 Pro 标称 30~144Hz） | [IT之家](https://www.ithome.com/0/845/595.htm) |
| 峰值亮度 | 1000 nits（HBM） | 已核实 | [腾讯新闻](https://so.html5.qq.com/page/real/search_news?docid=70000021_6036a3d5a1742352) |
| 对比度 | 1500:1 | 已核实 | 同上 |
| 屏占比 | 89.4% | 已核实 | 同上 |
| 调光 | 全亮度 DC 调光、莱茵智能护眼 5.0 | 已核实 | [手机中国](https://so.html5.qq.com/page/real/search_news?docid=70000021_3766a0679e988652) |

**系统设置里实际提供哪几档（60/90/120/144）**：**未核实**。面板本身支持 30–144Hz 连续可变，但 ColorOS 的"屏幕刷新率"菜单具体给几档、以及"默认/最高"两档如何映射，需要真机 `adb shell dumpsys display` 确认。**不要把这个列表写死进代码**，运行时用 `Display.getSupportedModes()` 枚举（见第三节）。

**可用的横向分辨率（landscape）**：面板原生就是横向 3392×2400（ppi 与 13.2″ 对角线自洽，说明这是原生方向，不是旋转出来的）。因此：

- 全屏 immersive 下可用绘制区 = **3392 × 2400**（7:5 ≈ 1.413:1）
- 减去系统栏后约 **3392 × 2280** 左右（**未核实**，取决于 ColorOS 手势条高度）
- 16:9 内容在此屏上为 3392 × 1908；本仓库的 Hor+ 做法会把 1080 行的虚拟屏横向撑到 3392/2400 比例，即约 1526×1080 的虚拟宽度——见 `docs/ANDROID.md`

> 常见误传值 3200×2136 / 3456×2400 均**不成立**。3392×2400 由三家独立来源交叉确认，且与 315 ppi / 13.2″ 几何自洽。

### 1.2 SoC 与内存

| 项目 | 值 | 状态 | 来源 |
|---|---|---|---|
| 型号 | 骁龙 8 Elite Gen 5（第五代骁龙 8 至尊版），SM8850-AC | 已核实 | [Qualcomm 官方](https://www.qualcomm.com/smartphones/products/8-series/snapdragon-8-elite-gen-5)、[Wikipedia SoC 列表](https://en.wikipedia.org/wiki/List_of_Qualcomm_Snapdragon_systems_on_chips) |
| 制程 | 3 nm（TSMC N3P），126.2 mm² | 已核实 | [Wikipedia SoC 列表](https://en.wikipedia.org/wiki/List_of_Qualcomm_Snapdragon_systems_on_chips) |
| CPU 架构 | 第三代 Qualcomm Oryon（全大核，无小核） | 已核实 | [Qualcomm 官方](https://www.qualcomm.com/smartphones/products/8-series/snapdragon-8-elite-gen-5) |
| CPU 簇布局 | **2× 4.61 GHz Prime + 6× 3.63 GHz Performance**（2+6，全大核） | **已核实** | [Wikipedia SoC 列表](https://en.wikipedia.org/wiki/List_of_Qualcomm_Snapdragon_systems_on_chips) |
| CPU 最高主频 | 4.61 GHz（SM8850-AC）；Galaxy 版 SM8850-1-AD 为 4.74 GHz | 已核实 | 同上；Qualcomm 页写 4.74 GHz 是顶配版 |
| 变体 | SM8850-5-AC（2+5 核）、SM8850-1-AD（Galaxy）、SM8850-1-AB（V Series，GPU 缩水） | 已核实 | 同上 |
| 本机实际绑定频率 | 4.61 GHz 还是 4.74 GHz | **未核实** | 需真机 `cat /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq` |
| 内存类型 | LPDDR5X，四通道 16-bit（总 64-bit），5300 MHz | **已核实** | [Wikipedia SoC 列表](https://en.wikipedia.org/wiki/List_of_Qualcomm_Snapdragon_systems_on_chips) |
| **内存带宽** | **84.8 GB/s** | **已核实** | 同上 |
| 本机内存 | 16 GB | 已核实 | [IT之家](https://so.html5.qq.com/page/real/search_news?docid=70000021_56569fee2b001952) |
| 存储 | 512 GB（顶配） | 已核实 | 同上 |
| 电池 / 充电 | 13380 mAh / 67W 有线 | 已核实 | [腾讯新闻](https://so.html5.qq.com/page/real/search_news?docid=70000021_6036a3d5a1742352) |
| 机身 | 672 g / 5.94 mm | 已核实 | 同上 |
| 散热 | 石墨片 + VC 均热板，总面积 40760 mm² | 已核实 | [手机中国](https://so.html5.qq.com/page/real/search_news?docid=70000021_3766a0679e988652) |
| 安兔兔综合跑分 | 447 万 | 已核实（厂商宣称） | 同上 |

> 平板形态 + 40760 mm² 均热板是本机的关键优势：同样的 SoC 在手机上持续功耗通常被压到 5–6 W，平板能长期维持更高档位。这直接决定了第四节的分辨率档位可以比手机激进。

### 1.3 GPU

| 项目 | 值 | 状态 | 来源 |
|---|---|---|---|
| 型号 | Adreno 840 | 已核实 | [vulkan.gpuinfo.org 实测报告](https://vulkan.gpuinfo.org/displayreport.php?id=52917) |
| **实际暴露的 Vulkan 版本** | **1.4.351**（驱动 512.891.11）；另有报告 1.4.295 / 512.842.41 | **已核实** | [report 52917](https://vulkan.gpuinfo.org/displayreport.php?id=52917)、[report 50885](https://vulkan.gpuinfo.org/displayreport.php?id=50885) |
| Qualcomm 标称 API | Vulkan 1.3、OpenCL 3.0 FP、OpenGL ES 3.2（**已过时**，见下） | 已核实（确为其官网原文） | [Qualcomm 官方](https://www.qualcomm.com/smartphones/products/8-series/snapdragon-8-elite-gen-5) |
| 核心频率 | **1200 MHz**（SM8850-AC）；Galaxy 版 SM8850-1-AD 为 1300 MHz | **已核实** | [Wikipedia SoC 列表](https://en.wikipedia.org/wiki/List_of_Qualcomm_Snapdragon_systems_on_chips) |
| **FP32 算力** | **3686.4 GFLOPS = 3.69 TFLOPS**（@1200 MHz）；Galaxy 版 3993.6 GFLOPS | **已核实** | 同上 |
| FP32 lane 数 | 1536 lanes（推算：3686.4 ÷ 1.2 GHz ÷ 2） | 推算，非官方 | 基于上述官方数字的算术 |
| 相对 Adreno 830 | **非加宽，是同宽演进**（830 为 1200 MHz / 3686.4 GFLOPS，lane 数相同）；官方称 GPU 性能 +23%、光追 +25% | 已核实（性能宣称）/ 推算是推理 | [Qualcomm 官方](https://www.qualcomm.com/smartphones/products/8-series/snapdragon-8-elite-gen-5)、[Wikipedia](https://en.wikipedia.org/wiki/List_of_Qualcomm_Snapdragon_systems_on_chips) |
| 缩水版（勿用作基准） | SM8850-1-AB "V Series"：8 CU，1200 MHz，2457.6 GFLOPS（1024 lanes） | 已核实 | [Wikipedia SoC 列表](https://en.wikipedia.org/wiki/List_of_Qualcomm_Snapdragon_systems_on_chips) |
| 扩展数量 | 207（report 52917）/ 183（report 50885） | 已核实 | [vulkan.gpuinfo.org](https://vulkan.gpuinfo.org/displayreport.php?id=52917) |
| 光追 | 硬件光追，+25% vs 8 Elite；驱动暴露 `VK_KHR_ray_query` / `ray_tracing_pipeline` / `ray_tracing_position_fetch` | 已核实 | [report 52917](https://vulkan.gpuinfo.org/displayreport.php?id=52917) |

> **重要（两处别踩坑）**：
> 1. Qualcomm 产品页写 "Vulkan 1.3"，但实际驱动上报 `apiVersion = 1.4.351`。**以运行时 `vkEnumerateInstanceVersion` 为准**，不要按 1.3 写死。
> 2. Adreno 840 相对 830 **不是加宽**（都是 1536 lanes @1200 MHz），提升来自架构与频率。所以"840 比 830 多一堆 ALU"的假设不成立，着色器优化策略应与 830 一致。

### 1.4 系统

| 项目 | 值 | 状态 | 来源 |
|---|---|---|---|
| Android 版本 | 16 | 已核实（vulkan.gpuinfo 同 GPU 报告均为 Android 16.0） | [vulkan.gpuinfo.org](https://vulkan.gpuinfo.org/displayreport.php?id=52884) |
| 定制系统 | ColorOS（一加） | 已核实 | [IT之家](https://so.html5.qq.com/page/real/search_news?docid=70000021_56569fee2b001952) |
| API level | 36（Android 16） | 已核实 | 由 Android 16 推出 |

---

## 二、Adreno 840 的 Vulkan 能力与可用的高通特性

以下扩展列表来自 **真实 Adreno 840 设备上报的 Vulkan 硬件数据库记录**（Xiaomi 25102PCBEG，驱动 512.891.11，Vulkan 1.4.351，Android 16），不是推测：<https://vulkan.gpuinfo.org/displayreport.php?id=52917>

### 2.1 与本移植直接相关的扩展（全部已核实存在）

| 扩展 | 用途 | 对本项目的价值 |
|---|---|---|
| `VK_GOOGLE_display_timing` | 查询/指定 present 时刻 | **高**。做帧节奏（frame pacing）的标准手段，比死等 vsync 稳 |
| `VK_KHR_incremental_present` | 只提交脏区域 | 中。UI 型画面有用，全屏 3D 收益小 |
| `VK_EXT_swapchain_maintenance1` | `presentMode` 运行时可改、present fence | 中。可在 FIFO/MAILBOX 间热切换 |
| `VK_EXT_surface_maintenance1` | surface 缩放/裁剪 | 低 |
| `VK_KHR_fragment_shading_rate` | 可变着色率（VRS） | **高**。降低边缘/远景像素着色开销，典型 10–25% GPU 节省 |
| `VK_EXT_fragment_density_map` / `_map2` / `_map_offset` | 片元密度图 | **高**。可按区域降采样，等价于"局部降分辨率"，比全局降分辨率观感好得多 |
| `VK_QCOM_fragment_density_map_offset` | 高通版 FDM 偏移 | 高（同上，Adreno 优化版） |
| `VK_QCOM_render_pass_store_ops` | 免 store 的 attachment | 中。省带宽 |
| `VK_QCOM_render_pass_transform` | render pass 预旋转 | **高**。平板上可省掉一次全屏旋转 blit |
| `VK_QCOM_multiview_per_view_viewports` / `_render_areas` | 分视图视口/渲染区 | 中。VR/分屏；单屏无用 |
| `VK_QCOM_tile_shading` + `VK_QCOM_tile_properties` + `VK_QCOM_tile_memory_heap` | 显式 tile 内存与 per-tile dispatch | 中高。Adreno 是 TBDR，正确用可省大量带宽 |
| `VK_EXT_subpass_merge_feedback` | 提示驱动合并 subpass | 中。省带宽 |
| `VK_EXT_hdr_metadata` + `VK_EXT_swapchain_colorspace` | HDR 元数据/色彩空间 | 低（本机 LCD，HDR 意义有限） |
| `VK_EXT_astc_decode_mode` + `VK_EXT_texture_compression_astc_hdr` | ASTC 解码模式 | 中。ASTC 是 Adreno 首选纹理格式 |
| `VK_EXT_image_compression_control` / `_swapchain` | 固定压缩（AFBC） | 中高。省带宽、省电 |
| `VK_KHR_synchronization2`、`VK_KHR_dynamic_rendering`、`VK_KHR_dynamic_rendering_local_read` | 现代同步/渲染 | 高。减少 barrier 与 render pass 管理开销 |
| `VK_EXT_descriptor_buffer`、`VK_EXT_graphics_pipeline_library`、`VK_KHR_pipeline_binary` | 降低 CPU 开销 / 管线缓存 | **高**。模拟器 CPU 瓶颈明显，管线编译卡顿可用 pipeline_binary 缓解 |
| `VK_EXT_mesh_shader`、`VK_KHR_cooperative_matrix` | mesh shader / 矩阵加速 | 低（P5R 用不上） |
| `VK_EXT_memory_decompression` | 显存内解压 | 中。可替代 CPU 解压纹理 |
| `VK_EXT_load_store_op_none`、`VK_KHR_load_store_op_none` | 免 load/store | 中。省带宽 |
| `VK_EXT_frame_boundary` | 帧边界标记 | 低 |

**确认不存在的扩展（别写进代码）**：

| 扩展 | 状态 |
|---|---|
| `VK_EXT_fragment_shading_rate` | ❌ **不存在**，只有 KHR 版 |
| `VK_KHR_maintenance8` | ❌ **不存在**（有 5/6/7，无 8） |

> 意外发现：驱动里还有 `VK_ARM_data_graph`、`VK_ARM_tensors`、`VK_NV_optical_flow`、`VK_QCOM_data_graph_model` —— AI/图计算扩展已经进了移动驱动，与游戏无关但说明这是个很新的驱动栈。

**纹理压缩（已核实）**：ASTC LDR **全支持**（14 种 block size × UNORM/SRGB），另有 `textureCompressionETC2 = true`、`textureCompressionBC = true`。

**驱动上限（已核实，来自同一份报告）**：

| 限制 | 值 |
|---|---|
| `maxMemoryAllocationSize` | 1 GiB（1073741824） |
| `maxStorageBufferRange` | 2 GiB − 1（2147483647） |
| `maxImageDimension2D` | 16384 |
| `maxViewportDimensions` | 16384 × 16384 |
| `maxFramebufferWidth/Height` | 16384 |
| `maxComputeWorkGroupInvocations` | 1024 |
| `maxComputeSharedMemorySize` | 32768 |
| `maxPushConstantsSize` | 256 |
| `maxUniformBufferRange` | 65536 |
| `maxBoundDescriptorSets` | 7 |
| `maxVertexInputAttributes` / `Bindings` | 32 / 32 |
| `maxColorAttachments` | 8 |
| `maxComputeSharedMemorySize` | **32768**（32 KB，Adreno 的经典硬约束） |
| `subgroupSize` | **64**（min = max = 64，固定） |
| `maxFragmentDensityTexelSize` | 256 × 256 |
| `maxSamplerAllocationCount` | 65536 |
| `timestampPeriod` | 52.0833 ns |
| `maxMemoryAllocationCount` | 4294967295 |

> **两个必须注意的 Adreno 约束**：
> 1. `subgroupSize` 固定 **64**（不是 32）。任何假设 subgroup 为 32 的 compute shader（大量从 PC 移植的代码）会算错，必须用 `gl_SubgroupSize` / `subgroupElect`，不能硬编码 32。
> 2. `maxComputeSharedMemorySize` 只有 **32 KB**，远低于桌面 GPU。移植 compute pass 时要按 32 KB 重新划分 workgroup。

### 2.2 公开 API 可用 vs 需要厂商 SDK

| 特性 | 第三方 App 能否用 | 说明 |
|---|---|---|
| 上表所有 `VK_*` 扩展 | ✅ **可以** | 都是 Vulkan 标准/注册扩展，`vkEnumerateDeviceExtensionProperties` 拿到即可用，无需 SDK |
| 可变着色率 / fragment density map | ✅ **可以** | 走 Vulkan 扩展，公开 |
| AFBC 固定压缩（`VK_EXT_image_compression_control`） | ✅ **可以** | 公开扩展 |
| `PerformanceHintManager`（ADPF） | ✅ **可以** | 公开 Android API（API 31+），本仓库已用（`perf_profile.h` 的 `adpf`）。详见 2.4 |
| `PowerManager.getThermalStatus` | ✅ **可以** | 公开 Android API（**API 29**，不是 31）。详见 2.4 |
| **Snapdragon Game Super Resolution (GSR)** | ✅ **可以，但需自行集成 shader** | GSR **不是驱动扩展**，是 Qualcomm Snapdragon Studios 提供的**空间单通道上采样 shader/库**，链接进自己的 render pass 即可，无需 vendor API、无需权限。**反证**：197 个扩展里没有任何 `VK_*GSR*` 名字。注意：Qualcomm 的 SGTK/GSR 官方页 404，以下信息来自二手中文报道，**具体许可条款未核实** |
| **Adreno Frame Motion Engine 2.x (AFME)** | ❌ **不可以** | 帧插值由 OEM + 游戏白名单联合开启（如 Galaxy S25 出厂用 AFME 2.0 把 60 插到 120）。无任何公开 API，第三方 App 无法打开 |
| **Snapdragon Game Performance Framework (SGPF)** | ❌ 基本不可以 | 系统/OEM 级框架。只有那些**已被暴露成 Vulkan 扩展**的能力（tile shading、FDM、mesh shader）才够得着；SGTK / Snapdragon Profiler 是**工具**，不是运行时 API |
| **Adreno Control Panel** | ❌ 非 App 接口 | OEM 集成的 UI 组件（一加/OPPO 有内置），不是 SDK |
| Adreno GPU 驱动可更新 | ⚠️ 取决于 OEM | 机制见 2.3；一加/ColorOS 是否注册 `ro.gfx.driver.0` **未核实** |

> 结论：**本移植只应依赖 Vulkan 扩展 + 公开 Android API**。AFME/SGPF 这类"超帧"是 OEM 侧卖点（官方宣传"超画 3.4K + 原生 144 帧"），App 拿不到，别在代码里规划。GSR 倒是可以自己做——但本质是"写一个上采样 shader"，与我们自己做的上采样没有 API 层面的区别。

### 2.3 可更新 GPU 驱动：确切机制（已核实 AOSP 源码）

**系统属性**（`GraphicsEnvironment.java`，由 OEM 在 build 时写入，普通 App 无法改）：

```java
"ro.gfx.driver.0"            // production 驱动包名
"ro.gfx.driver.1"            // prerelease 驱动包名
"ro.gfx.driver_build_time"   // 驱动构建时间
```

**按 App 强制指定驱动不是 `setprop`，而是写 `Settings.Global`**（全部 `@hide`，前缀是 `updatable_driver_`，不是 `game_driver_`）。以下键名与语义**逐字取自 AOSP `Settings.java`**：

| 键 | 语义 |
|---|---|
| `updatable_driver_all_apps` | `0`=默认，`1`=所有 App 用 production 驱动，`2`=所有 App 用 prerelease 驱动，`3`=所有 App 用系统驱动 |
| `updatable_driver_production_opt_in_apps` | 逗号分隔包名，指定用 production 驱动 |
| `updatable_driver_prerelease_opt_in_apps` | 逗号分隔包名，指定用 prerelease 驱动 |
| `updatable_driver_production_opt_out_apps` | 主动退出 |
| `updatable_driver_production_allowlist` | 允许使用 production 驱动的包（`"*"` 表示全部） |
| `updatable_driver_production_denylist` / `..._denylists` | 禁用列表（按驱动版本） |
| `updatable_driver_sphal_libraries` | 可更新驱动允许加载的 sphal 库 |

正确命令：

```bash
# 只让本 App 用可更新驱动
adb shell settings put global updatable_driver_production_opt_in_apps io.github.p5rrecomp
# 或让所有 App 都用
adb shell settings put global updatable_driver_all_apps 1
# 查看当前状态
adb shell getprop ro.gfx.driver.0
adb shell settings get global updatable_driver_all_apps
```

开发者选项里的 "Game driver preferences"（游戏驱动偏好）UI 只是这些键的前端。

> ⚠️ 常见错误说法：`adb shell setprop debug.gfx.driver.<pkg> <driver>` —— **这个属性不存在**；`game_driver_opt_in_apps` 也不对，真实前缀是 `updatable_driver_`。

### 2.4 公开的 GPU 性能 / 温度 API（已核实 API level）

**PerformanceHintManager（ADPF，API 31+）**——Vulkan 游戏循环最有价值的公开性能 API：

| API | API level |
|---|---|
| `PerformanceHintManager.createHintSession(int[] tids, long initialTargetWorkDurationNanos)` | **31** |
| `PerformanceHintManager.getPreferredUpdateRateNanos()` | 31 |
| `Session.updateTargetWorkDuration(long)` | **31** |
| `Session.reportActualWorkDuration(long)` | **31** |
| `Session.setThreads(int[])` / `close()` | 31 |
| `Session.reportActualWorkDuration(WorkDuration)` | **35** |
| `Session.setPreferPowerEfficiency(boolean)` | **35** |

语义：把渲染线程的 tid 注册进去，给出**每帧目标工作时长（ns）**，每帧调 `reportActualWorkDuration()` 上报真实耗时（统一用 `SystemClock.uptimeNanos()` 计时），系统据此调整 CPU 核心调度与频率。`createHintSession` 在不支持的设备上返回 `null`，**必须判空**。

**温度（API 29+）**：

| API | API level |
|---|---|
| `PowerManager.getCurrentThermalStatus()` | **29** |
| `PowerManager.addThermalStatusListener(Executor, OnThermalStatusChangedListener)` | **29** |
| `PowerManager.getThermalHeadroom(int forecastSeconds)` | 29 |
| `PowerManager.addThermalHeadroomListener(...)` / `getThermalHeadroomThresholds()` | 30 |

`THERMAL_STATUS_*` = `NONE`0 / `LIGHT`1 / `MODERATE`2 / `SEVERE`3 / `CRITICAL`4 / `EMERGENCY`5 / `SHUTDOWN`6。

`getThermalHeadroom()` 返回 **0.0–1.0 的余量比例**（1.0 = 未降频），比离散的 status 更适合驱动动态分辨率/片元密度缩放——在系统真正降频**之前**就主动降档，比被动掉帧观感好得多。本机 13380 mAh 大电池 + 40760 mm² 均热板，热余量比手机宽裕，但 13.2″ 大屏本身也是热源。


---

## 三、Android 刷新率与帧调度 API 的正确用法

### 3.1 API 对照表（签名与 API level 均已核实，来源为 AOSP 头文件源码）

| API | API level | 签名 | 备注 |
|---|---|---|---|
| `ANativeWindow_setFrameRate` | **30** | `int32_t ANativeWindow_setFrameRate(ANativeWindow*, float frameRate, int8_t compatibility)` | 等价于 strategy = `ONLY_IF_SEAMLESS` |
| `ANativeWindow_setFrameRateWithChangeStrategy` | **31** | `int32_t ANativeWindow_setFrameRateWithChangeStrategy(ANativeWindow*, float frameRate, int8_t compatibility, int8_t changeFrameRateStrategy)` | 能强制非无缝切换 |
| `ANativeWindow_clearFrameRate` | 31 | `void/int32_t ANativeWindow_clearFrameRate(ANativeWindow*)` | 等价于 frameRate = 0 |
| `ANativeWindow_tryAllocateBuffers` | 30 | `void ANativeWindow_tryAllocateBuffers(ANativeWindow*)` | 预分配 buffer，减少首帧卡顿 |
| `Surface.setFrameRate(float, int)` | 30 | Java | 二参版 |
| `Surface.setFrameRate(float, int, int)` | 31 | Java | 三参版（带 strategy） |
| `Surface.clearFrameRate()` | 31 | Java | |
| `Surface.setFrameRate(FrameRateParams)` | 36（flagged） | Java | 新 API，`@FlaggedApi(FLAG_ARR_SETFRAMERATE_API)`，暂不要依赖 |
| `AChoreographer_registerRefreshRateCallback` | 31 | C | 监听刷新率变化 |
| `WindowManager.LayoutParams.preferredDisplayModeId` | 23 | Java | 直接指定 `Display.Mode`；**优先级最高**，会覆盖 `preferredRefreshRate` |
| `WindowManager.LayoutParams.preferredRefreshRate` | 21 | Java | 见下方 3.1.1 的重要语义变化 |
| `WindowManager.LayoutParams.preferredMinDisplayRefreshRate` / `preferredMaxDisplayRefreshRate` | — | Java | **`@hide` + `@TestApi`，普通 App 不可用**（已核实源码），不要用 |
| `Display.getSupportedModes()` / `Display.Mode` | 23 | Java | 枚举真实档位，唯一可靠来源 |

### 3.1.1 `preferredRefreshRate` 的语义在 API 34 变了（已核实 AOSP 源码注释）

AOSP `WindowManager.LayoutParams` 的原文注释：

> Before API 34, this must be one of the supported refresh rates obtained for the display(s) the window is on. The selected refresh rate will be applied to the display's default mode.
> Starting API 34, this value is not limited to the supported refresh rates ... Any refresh rate can be provided as the preferred window refresh rate. The OS will select the refresh rate that best matches the `preferredRefreshRate`.
> Setting this value is the equivalent of calling `Surface#setFrameRate(preferred_frame_rate, FRAME_RATE_COMPATIBILITY_DEFAULT, CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS)`.
> This should be used **in favor of** `LayoutParams#preferredDisplayModeId` for applications that want to specify the refresh rate, but do not want to specify a preference for any other displayMode properties (e.g., resolution).
> **This value is ignored if `preferredDisplayModeId` is set.**

来源：[`WindowManager.java`（AOSP main）](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/core/java/android/view/WindowManager.java)

推论（对本机 API 36 直接适用）：

1. 在 Android 16 上，**`preferredRefreshRate = 144f` 本身就能表达意图**，不需要再去枚举 `Display.Mode`。
2. 但它等价于 `CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS`——**从 60Hz 跳到 144Hz 可能被静默拒绝**。
3. 所以推荐顺序是：**先设 `preferredRefreshRate`（简单、无需枚举），再调 `Surface.setFrameRate(..., CHANGE_FRAME_RATE_ALWAYS)` 兜底强制**。
4. 只有当你还需要锁定分辨率（比如强制原生 3392×2400）时，才用 `preferredDisplayModeId`；一旦设了它，`preferredRefreshRate` 会被忽略。
5. `preferredMinDisplayRefreshRate` / `preferredMaxDisplayRefreshRate` 是 `@hide` + `@TestApi`，**普通 App 拿不到**，别写进代码。



```c
// ANativeWindow_FrameRateCompatibility
ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT      = 0  // 游戏内容用这个
ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE = 1  // 视频（固定帧率源）用这个
ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_GTE          = 2  // UI/滚动，"大于等于"

// ANativeWindow_ChangeFrameRateStrategy（API 31+）
ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS = 0  // 默认：只在能无缝切换时切
ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS           = 1  // 允许非无缝（可能闪一下）
```

> ⚠️ **最容易写错的一点**：很多教程（包括一些 AI 生成的内容）会说游戏要用 `FIXED_SOURCE`。**错的**。AOSP 头文件原文写得很清楚：`DEFAULT` 的注释是 "This value should be used when displaying game content"，`FIXED_SOURCE` 的注释是 "This value should be used for video content"。游戏用 `DEFAULT`。

> ⚠️ **第二容易写错**：`setFrameRate` 默认 strategy 是 `ONLY_IF_SEAMLESS`。从 60Hz 切到 144Hz 在很多面板上是**非无缝**的，会被静默拒绝。想真正拿到 144Hz，必须用 API 31 的三参版并传 `ALWAYS`。

### 3.2 推荐实现（C / ANativeWindow）

放在窗口创建之后、swapchain 重建之后各调一次。

```c
#include <android/native_window.h>
#include <android/api-level.h>

// 返回 0 成功，负值失败（-EINVAL 表示参数非法）
static int nx_request_display_rate(ANativeWindow* window, float hz)
{
    if (!window || hz <= 0.0f) return -1;

    // API 31+：能指定"允许非无缝切换"，这是从 60Hz 拿到 144Hz 的关键。
    if (__builtin_available(android 31, *)) {
        return ANativeWindow_setFrameRateWithChangeStrategy(
                window,
                hz,
                ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT,   // 游戏内容
                ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS);          // 允许非无缝
    }
    // API 30：只有两参版，strategy 被固定为 ONLY_IF_SEAMLESS，
    // 60 -> 144 这类切换可能被拒绝，属于预期行为。
    if (__builtin_available(android 30, *)) {
        return ANativeWindow_setFrameRate(window, hz,
                ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
    }
    return -1; // API 29 及以下：无此 API，靠系统自己挑
}

static void nx_clear_display_rate(ANativeWindow* window)
{
    if (!window) return;
    if (__builtin_available(android 31, *)) {
        ANativeWindow_clearFrameRate(window);
    }
    // API 30 及以下：无法清除，保持上次的投票
}
```

`__builtin_available` 需要 NDK r23+ 且开启 `-D__ANDROID_API__` 与运行时检查；若不想用 Clang 扩展，等价写法是运行时读 `android_get_device_api_level()`：

```c
#include <android/api-level.h>
int api = android_get_device_api_level();
if (api >= 31) { /* 三参版 */ }
else if (api >= 30) { /* 两参版 */ }
```

**minSdk 门控建议**：本机是 Android 16（API 36），但仓库若要保持宽兼容，`minSdk` 设 30 即可覆盖两参版；把三参版放在 `api >= 31` 分支。**不要**为了 144Hz 把 minSdk 提到 31——两参版在 30 上仍能正确表达意图，只是可能被"无缝"限制拒绝。

### 3.3 Java 侧（选 mode + 投票，两条腿走路）

Native 的 `setFrameRate` 只是"投票"，**不改变实际显示模式**。要真正把面板切到 144Hz，还需要在 Java 侧用 `preferredDisplayModeId` 指定 `Display.Mode`。

```java
import android.view.Display;
import android.view.Surface;
import android.view.WindowManager;

/**
 * 推荐路线（API 34+，本机 API 36 适用）：
 *   1) preferredRefreshRate 表达意图（简单，不需要枚举 mode）
 *   2) Surface.setFrameRate(..., ALWAYS) 强制生效（preferredRefreshRate 内部用的是
 *      ONLY_IF_SEAMLESS，60->144 可能被拒）
 *   3) 需要同时锁定分辨率时，才改用 preferredDisplayModeId（它会覆盖 preferredRefreshRate）
 */
void lockRefresh(WindowManager wm, Window window, Surface surface, float wantHz) {
    // --- 1) 窗口层投票：API 34+ 可传任意 Hz，系统挑最接近的 ---
    WindowManager.LayoutParams lp = window.getAttributes();
    lp.preferredDisplayModeId = 0;      // 先清掉，否则 preferredRefreshRate 被忽略
    lp.preferredRefreshRate = wantHz;   // 144f
    window.setAttributes(lp);

    // --- 2) Surface 层投票：用 ALWAYS 强制非无缝切换 ---
    if (android.os.Build.VERSION.SDK_INT >= 31) {
        surface.setFrameRate(wantHz,
                Surface.FRAME_RATE_COMPATIBILITY_DEFAULT,   // 游戏用 DEFAULT，不是 FIXED_SOURCE
                Surface.CHANGE_FRAME_RATE_ALWAYS);          // 允许非无缝，否则可能被拒
    } else if (android.os.Build.VERSION.SDK_INT >= 30) {
        surface.setFrameRate(wantHz, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
    }
    // API 30 以下：无此 API，只能靠系统
}

/**
 * 需要同时锁定原生分辨率时用这条（可选）。
 * 注意：设了 preferredDisplayModeId 后 preferredRefreshRate 会被忽略。
 */
void lockMode(Window window, Display display, float wantHz) {
    Display.Mode best = null;
    float bestDelta = Float.MAX_VALUE;
    for (Display.Mode m : display.getSupportedModes()) {
        if (m.getPhysicalWidth() != 3392 || m.getPhysicalHeight() != 2400) continue; // 只要原生分辨率
        float d = Math.abs(m.getRefreshRate() - wantHz);
        if (d < bestDelta) { bestDelta = d; best = m; }
    }
    if (best == null || bestDelta > 1.0f) return;   // 容差 1Hz（面板常报 143.99）
    WindowManager.LayoutParams lp = window.getAttributes();
    lp.preferredDisplayModeId = best.getModeId();
    window.setAttributes(lp);
}

void releaseRefresh(Surface surface) {
    if (android.os.Build.VERSION.SDK_INT >= 31) surface.clearFrameRate();
}
```

**为什么"两条腿"**：

1. `preferredRefreshRate`（API 34+）已经把"任意 Hz → 系统挑最近档位"这件事做了，**这是最简单可靠的一条**。
2. 但它内部等价于 `ONLY_IF_SEAMLESS`，60→144 这类大跨度切换在部分面板上非无缝，会被拒。所以必须再用 `Surface.setFrameRate(..., ALWAYS)` 强制一次。
3. `preferredDisplayModeId` 是"重型"手段：能锁死具体 mode（含分辨率），但会**覆盖** `preferredRefreshRate`，且要求 modeId 必须真实存在于 `getSupportedModes()`。
4. ColorOS 的"智能刷新率"可能仍会干预；此时 App 侧手段已用尽，需要用户把系统刷新率设成"高/144"。

### 3.4 运行时监听刷新率变化

```c
// 刷新率可能被系统改掉（省电策略、多窗口）。注册回调而不是假设它不变。
#include <android/choreographer.h>
static void on_refresh_rate(AChoreographer* c, int32_t rate_ns, void* data) {
    // rate_ns 是周期（纳秒）；144Hz ≈ 6.94 ms
    double hz = rate_ns > 0 ? 1e9 / (double)rate_ns : 0.0;
    // 用它校准 present pacing 的间隔
}
// API 31+：
AChoreographer* c = AChoreographer_getInstance();
AChoreographer_registerRefreshRateCallback(c, on_refresh_rate, NULL);
```

### 3.5 adb 侧的手段（调试/兜底，不是产品方案）

```bash
# 强制锁定最高/最低刷新率（标准 AOSP Settings.Global 键，ColorOS 上是否生效需实测）
adb shell settings put system peak_refresh_rate 144.0
adb shell settings put system min_refresh_rate  144.0
# 恢复自动
adb shell settings put system peak_refresh_rate 0
adb shell settings put system min_refresh_rate  0

# 看当前实际模式与所有可用档位
adb shell dumpsys display | grep -iE "refreshRate|modeId|mSupportedModes"

# 看 SurfaceFlinger 实际合成的刷新率
adb shell dumpsys SurfaceFlinger | grep -iE "refresh|fps"
```

`peak_refresh_rate` / `min_refresh_rate` 是 AOSP `Settings.Global` 的标准键，社区长期用于强制高刷（三星 S20 时代即以此法把 120Hz 锁到 96Hz，见 [IT之家报道](https://www.ithome.com/0/845/595.htm) 同源的社区做法；GitHub 上亦有成脚本化的整理，如 [melody_android](https://github.com/ionuttbara/melody_android)）。

**但 ColorOS 是否尊重这两个键未核实**——国产 ROM 常自行接管刷新率策略，可能忽略或覆盖。必须真机实测，不能作为产品依赖。

---

## 四、针对本设备的推荐配置

配置键名对齐仓库现有的 `runtime/src/perf_profile.h`（`perf_mode` / `fps_mode` / `refresh_hz` / `render_height` / `present_mode` / `adpf`）。

| # | 配置项 | 推荐值 | 预期效果 |
|---|---|---|---|
| 1 | `perf_mode` | `quality` 起步，逐级试 `ultra` | 平板 40760 mm² 均热板可支撑比手机高一档的持续功耗；先从 quality 稳住 60fps 再上探 |
| 2 | `fps_mode` / `refresh_hz` | `fps_mode=60` + `refresh_hz=144` | **关键**：帧率与刷新率解耦。面板 144Hz 降低 present 延迟与抖动，游戏逻辑仍锁 60，GPU 负载不增加。这比 `fps_mode=144` 划算得多 |
| 3 | 渲染分辨率档位 | `render_height=1440`（0.6×）为首选；1080 为保底；1600 为画质档 | P5R 原生 1080p 资产。1440 行上采样到 2400 观感接近原生，GPU 负载约为原生 3392×2400 的 36%。全原生 2400 行在 Adreno 840 上跑 60fps 有风险 |
| 4 | `present_mode` | `fifo`（先），确认稳定后试 `mailbox` | FIFO 最稳、功耗最低；MAILBOX 降延迟但可能撕裂/增功耗。**不要用 `immediate`**（Adreno 上易撕裂且耗电） |
| 5 | `adpf=1` | 开启 | ADPF 让系统按实际帧时长调 CPU 频率，比固定高性能模式省电、且减少热降频抖动 |
| 6 | GPU 驱动 | 先用系统自带驱动（512.891.11 系） | 第三方 Turnip 在 Adreno 8xx 上成熟度不如原厂；原厂驱动已暴露 207 个扩展，功能不缺。若遇驱动 bug 再考虑 Turnip |
| 7 | 性能模式 | 系统"性能模式"开启，**不要**开"均衡/省电" | 省电模式会把面板压回 60Hz 并限制 GPU 频率，直接废掉第 2 条 |
| 8 | 散热 | 摘掉保护壳；避免边充边玩 | 67W 快充发热叠加 SoC 发热会提前触发降频；平板被动散热依赖背面，壳是主要热阻 |
| 9 | 开发者选项 | 关闭"强制进行 GPU 渲染"、关闭"不保留活动"、关闭"强制 4x MSAA"；开启"停用 HW 叠加层"**仅用于排查** | 强制 GPU 渲染会让所有 UI 走 GPU，干扰测量；4x MSAA 会成倍增加带宽 |
| 10 | 刷新率锁定 | App 内走 3.3 的两条腿；调试期用 `adb shell settings put system min_refresh_rate 144.0` 兜底 | 单靠 App 投票在 ColorOS 智能刷新率下可能不生效 |

### 4.1 分辨率档位换算（供 `render_height` 参考）

原生 3392×2400（7:5）。按高度缩放并保持 7:5：

| `render_height` | 实际渲染尺寸 | 占原生像素比 | 适用 |
|---|---|---|---|
| 1080 | 1526 × 1080 | 20% | 保底 60fps，画质明显软 |
| 1440 | 2035 × 1440 | 36% | **推荐默认** |
| 1600 | 2261 × 1600 | 44% | 画质档 |
| 1800 | 2544 × 1800 | 56% | 上探，注意温度 |
| 2400 | 3392 × 2400 | 100% | 原生，风险高 |

> 注意仓库 `docs/ANDROID.md` 记录的 `render_scale` 默认值会"never above 1080 lines"，即默认上限 1080 行。本机应显式设 `render_height` 覆盖，或打开 `allow_over_1080`。

### 4.2 帧节奏（frame pacing）建议

- 用 `VK_GOOGLE_display_timing`（已核实支持）拿每个 vsync 的 `actualPresentTime`，据此调整下一帧的 CPU 起跑时刻，而不是用 `sleep` 硬等。
- 面板 144Hz 时，60fps 目标的可用 vsync 间隔是 2.4 个刷新周期——**不是整数**，所以不能简单"每 N 个 vsync 出一帧"。用 `refresh_hz=144` + `fps=60` 时，应做 3/2 交替（2,2,3 周期）或直接让 `present_mode=fifo` 交给驱动。
- 若追求绝对稳，建议 `refresh_hz=120` + `fps=60`（整数 2 倍关系），代价是放弃 144Hz 的低延迟。

---

## 五、参考链接

**设备与屏幕**

1. [一加平板 3 Pro 开售：骁龙 8 Elite Gen 5 + 144Hz 高刷屏（手机中国 / 腾讯新闻）](https://so.html5.qq.com/page/real/search_news?docid=70000021_3766a0679e988652) — 3392×2400、13.2″、315ppi、144Hz、1000nits、40760mm² 散热
2. [144Hz 高刷屏！一加新平板：13380mAh 大电池（腾讯新闻）](https://so.html5.qq.com/page/real/search_news?docid=70000021_6036a3d5a1742352) — 3392×2400、1000nits HBM、1500:1、89.4% 屏占比、672g/5.94mm
3. [一加平板 3 Pro 来了！3.4K 屏 + 骁龙 8 至尊版（IT之家）](https://so.html5.qq.com/page/real/search_news?docid=70000021_17869fea62187652)
4. [消息称一加平板 3 Pro 配置可参考 OPPO Pad 5 Pro（IT之家）](https://so.html5.qq.com/page/real/search_news?docid=70000021_56569fee2b001952) — 13.2″ 3.4K 144Hz LCD、13380mAh、67W
5. [全球首款骁龙 8 至尊版平板 OPPO Pad 4 Pro 开售（IT之家）](https://www.ithome.com/0/845/595.htm) — 同面板：3392×2400、315ppi、144Hz、7:5、TCL 华星 LTPS、30~144Hz

**SoC / GPU**

6. [Qualcomm Snapdragon 8 Elite Gen 5 官方产品页](https://www.qualcomm.com/smartphones/products/8-series/snapdragon-8-elite-gen-5) — Oryon 4.74GHz、Vulkan 1.3 / OpenCL 3.0 FP / GLES 3.2、4.6GHz 版本与 7 核版本脚注
7. [vulkan.gpuinfo.org — Adreno (TM) 840 设备报告（id=52917）](https://vulkan.gpuinfo.org/displayreport.php?id=52917) — Vulkan 1.4.351、驱动 512.891.11、207 个扩展、全部 limits
8. [vulkan.gpuinfo.org — Adreno (TM) 840 另一份报告（id=50885）](https://vulkan.gpuinfo.org/displayreport.php?id=50885) — Vulkan 1.4.295、驱动 512.842.41、183 个扩展
9. [vulkan.gpuinfo.org — Android 设备列表](https://vulkan.gpuinfo.org/listdevices.php?platform=android) — 用于交叉核对 Adreno 8xx 系列
10. [Wikipedia — List of Qualcomm Snapdragon systems on chips](https://en.wikipedia.org/wiki/List_of_Qualcomm_Snapdragon_systems_on_chips) — SM8850-AC/1-AD/1-AB 分档、Adreno 840 @1200MHz / 3686.4 GFLOPS、Oryon 2×4.61+6×3.63 GHz、LPDDR5X 5300MHz / 84.8 GB/s

**Android API（AOSP 源码，权威）**

11. [`native_window.h`（AOSP main）](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/main/libs/nativewindow/include/android/native_window.h) — `ANativeWindow_setFrameRate`(API 30)、`setFrameRateWithChangeStrategy`(API 31)、`ANativeWindow_FrameRateCompatibility`、`ANativeWindow_ChangeFrameRateStrategy`、`ANativeWindow_clearFrameRate`
12. [`Surface.java`（AOSP main）](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/core/java/android/view/Surface.java) — `FRAME_RATE_COMPATIBILITY_*` = 0/1/2、`CHANGE_FRAME_RATE_*` = 0/1、`setFrameRate`、`clearFrameRate`、`FrameRateParams`(flagged)
13. [`WindowManager.java`（AOSP main）](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/core/java/android/view/WindowManager.java) — `preferredRefreshRate` 在 API 34 的语义变化、`preferredDisplayModeId` 覆盖关系、`preferredMin/MaxDisplayRefreshRate` 为 `@hide`+`@TestApi`
14. [`Settings.java`（AOSP main）](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/core/java/android/provider/Settings.java) — `updatable_driver_*` 全部键名与语义（`all_apps` = 0/1/2/3）
15. [`GraphicsEnvironment.java`（AOSP main）](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/core/java/android/os/GraphicsEnvironment.java) — `ro.gfx.driver.0` / `ro.gfx.driver.1` / `ro.gfx.driver_build_time`
16. [`PerformanceHintManager`](https://developer.android.com/reference/android/os/PerformanceHintManager) — `createHintSession` / `updateTargetWorkDuration`（API 31）
17. [`PerformanceHintManager.Session`](https://developer.android.com/reference/android/os/PerformanceHintManager.Session) — `reportActualWorkDuration`、`setPreferPowerEfficiency`（API 35）
18. [`PowerManager`](https://developer.android.com/reference/android/os/PowerManager) — `getCurrentThermalStatus` / `addThermalStatusListener`（API 29）、`getThermalHeadroom`

**仓库内部**

19. [`docs/ANDROID.md`](ANDROID.md) — 现有 `render_scale` / `resolution` 行为与 1080 行上限
20. [`runtime/src/perf_profile.h`](../runtime/src/perf_profile.h) — `perf_mode` / `fps_mode` / `refresh_hz` / `render_height` / `present_mode` / `adpf` 的实际语义

---

## 六、未核实清单（不要当事实用）

| 项 | 状态 | 如何核实 |
|---|---|---|
| ColorOS「屏幕刷新率」菜单的具体档位（60/90/120/144）与"默认 vs 最高"映射 | **未核实** | 真机设置页截图；`adb shell dumpsys display` |
| 本机 SoC 是 4.61GHz 还是 4.74GHz 版本 | **未核实** | `adb shell cat /sys/devices/system/cpu/cpu7/cpufreq/cpuinfo_max_freq` |
| Adreno 840 的 ALU 数 | **未核实**（1536 lanes 是从 3686.4 GFLOPS ÷ 1200 MHz ÷ 2 推算的，非官方公布） | 需 Qualcomm 文档或 microbenchmark |
| "Adreno HPM"（独立 GPU 显存池）是否存在于 840 | **未核实** | 仅见于下一代产品的二手中文报道；无 Qualcomm 一手来源 |
| 减掉系统栏后的确切可用高度 | **未核实** | 真机 `adb shell wm size` / `dumpsys window` |
| `peak_refresh_rate` / `min_refresh_rate` 在 ColorOS 上是否生效 | **未核实** | 真机实测 |
| GSR / AFME / SGPF 的第三方可用性与许可 | **未核实** | Snapdragon Game Toolkit 文档（需登录 Qualcomm ID） |
| 一加/ColorOS 是否支持 Play 商店可更新 GPU 驱动（`ro.gfx.driver.0`） | **未核实** | `adb shell getprop ro.gfx.driver.0` |
| 144Hz 下的实际持续功耗与降频拐点 | **未核实** | 需真机功耗仪 + 长跑测试 |
| Turnip（第三方开源驱动）在 Adreno 840 上的可用性 | **未核实** | 社区反馈；Adreno 840 较新，支持可能滞后 |
