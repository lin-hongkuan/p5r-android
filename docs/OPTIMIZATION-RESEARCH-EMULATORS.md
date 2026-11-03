# 开源 Switch 模拟器优化实践研究：分辨率缩放、帧调度与 Adreno 专属优化

> 目标读者：`p5r-android` 的 runtime / NVN 层开发者。
> 目标设备：OnePlus Pad 3 Pro，Snapdragon 8 Elite Gen 5，Adreno 840，3.4K 144Hz，16GB RAM。
> 项目目标：稳定 "2K" 高分辨率 + 90fps。
>
> **方法说明。** 本文只做研究，不改任何源码。所有来自网络的论断都附 markdown 链接；凡是我没有真正读到源码 / 文档的地方，都标注 **未核实**。
>
> **关于各模拟器源码的可得性（重要：官方仓库全部已下架，本文用的是镜像/分支）。**
>
> | 目标 | 官方仓库状态 | 本文实际读的仓库（已核实存在） |
> |---|---|---|
> | **yuzu** | `yuzu-emu/yuzu` 404；`yuzu-mirror/yuzu` 返回 **451 Repository access blocked** | **[Nailington/Yuzu](https://github.com/Nailington/Yuzu)**，分支 `master`，GPL-3.0，自述 "Most updated source of Yuzu available on Internet Archive via Wayback Machine (should be March 02, 2024)" |
> | **Sudachi** | `Sudachi-Emu/sudachi` 404 | **[p-yukusai/sudachi-emu](https://github.com/p-yukusai/sudachi-emu)**，`master`，GPL-3.0，`settings_enums.h` 与 yuzu 逐字相同 |
> | **Ryujinx** | `Ryujinx/Ryujinx` 404；`Ryujinx-Mirror/Ryujinx` **451 blocked**；`Ryubing/Ryujinx` 404 | **[alula/Ryujinx](https://github.com/alula/Ryujinx)**，分支 **`qlaunch`**（注意不是 master），MIT，完整 Ryujinx 树 |
> | **Skyline** | 仓库在，但分支 `ftx1` 的 tree 只有 **1 个 blob（README.md）** | **无可读代码** → 未核实 |
> | **Strato** | `strato-emu/strato` `master` 正常 | 保留 Skyline 布局于 `app/src/main/cpp/skyline/` |
> | **Eden** | `eden-emu/eden` 404，未找到可用 GitHub fork | 未覆盖 |
> | **Citron** | `citron-neo/emulator` `main` 正常 | yuzu 的 GPL-3.0 分支，文件头仍保留 `SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project` |
>
> Skyline 的 `README.md` 原文：*"Skyline's source code has been removed from all owner-controlled branches and tags at the request of authorized agents of Nintendo Co., Ltd."* → **Skyline 零代码证据**。
>
> 关键结论：**yuzu 与 Ryujinx 现在都有可读源码**，本文因此能给出两家的直接对比（1.10 节），而不是只靠同源推断。Citron/Sudachi 与 yuzu 同源，用于交叉确认。

---

## 一、分辨率缩放（各模拟器做法 + 可借鉴点）

### 1.1 Citron / yuzu 系：`ResolutionScalingInfo` + 整数 `up_scale` / `down_shift`

yuzu 系的分辨率缩放不是"一个 float 乘数"，而是**一对整数**：`up_scale`（分子）与 `down_shift`（分母的 log2）。核心结构体在 `src/common/settings.h`：

```cpp
struct ResolutionScalingInfo {
    u32 up_scale{1};
    u32 down_shift{0};
    f32 up_factor{1.0f};
    f32 down_factor{1.0f};
    bool active{};
    bool downscale{};

    s32 ScaleUp(s32 value) const {
        if (value == 0) { return 0; }
        return std::max((value * static_cast<s32>(up_scale)) >> static_cast<s32>(down_shift), 1);
    }
    u32 ScaleUp(u32 value) const { /* 同上，无符号版 */ }
};
```

来源：[github.com/citron-neo/emulator/blob/main/src/common/settings.h](https://github.com/citron-neo/emulator/blob/main/src/common/settings.h)

**为什么用整数而不是 float**：`(v * up) >> shift` 是**可逆且确定**的，同一个 guest 像素坐标在 viewport / scissor / copy region / clear rect / texelFetch 五处缩放的结果必然一致；float 乘除会在边界像素上产生 1px 缝隙（这正是本项目 `docs/nvn/android-perf/PLAN.md` 5.1 担心的 "1-px seams where a texelFetch misses the rescale mask"）。**这是本项目最该抄的一点。**

档位在 `src/common/settings.cpp` 的 `TranslateResolutionInfo()`：

| `ResolutionSetup` | `up_scale` | `down_shift` | 实际倍率 | 备注 |
|---|---|---|---|---|
| `Res1_4X` | 1 | 2 | 0.25x | `downscale = true` |
| `Res1_2X` | 1 | 1 | 0.5x | `downscale = true` |
| `Res3_4X` | 3 | 2 | 0.75x | `downscale = true` |
| `Res1X` | 1 | 0 | 1.0x | |
| `Res5_4X` | 5 | 2 | 1.25x | |
| `Res3_2X` | 3 | 1 | 1.5x | |
| `Res7_4X` | 7 | 2 | 1.75x | |
| `Res2X` | 2 | 0 | 2.0x | |
| `Res3X` … `Res8X` | 3…8 | 0 | 3x…8x | |

收口：

```cpp
info.up_factor   = static_cast<f32>(info.up_scale) / (1U << info.down_shift);
info.down_factor = static_cast<f32>(1U << info.down_shift) / info.up_scale;
info.active      = info.up_scale != 1 || info.down_shift != 0;
```

枚举在 `src/common/settings_enums.h`：
`ENUM(ResolutionSetup, Res1_4X, Res1_2X, Res3_4X, Res1X, Res3_2X, Res2X, Res3X, Res4X, Res5X, Res6X, Res7X, Res8X, Res5_4X, Res7_4X);`

**关键：yuzu 支持降分辨率（0.25x / 0.5x / 0.75x），不只是升。** 对本项目（从 720p 上探 2K，也可能为保 90fps 降到 0.75x）这是双向杠杆。

### 1.2 哪些 image 允许被缩放：`ImageInfo::rescaleable`

yuzu 系**不是无差别缩放所有纹理**，而是在 `ImageInfo` 构造时决定 `rescaleable`。`src/video_core/texture_cache/image_info.cpp`：

```cpp
constexpr u32 RescaleHeightThreshold   = 288;
constexpr u32 DownscaleHeightThreshold = 512;
```

- 默认 `rescaleable = false;`
- 2D 纹理：`rescaleable = !config.IsPitchLinear();`（**pitch-linear 不缩放**——stride 由 guest 给定，缩放会破坏语义）
- 2D array：`rescaleable = true;`
- Cubemap / CubeArray / 3D / 1D / 1DBuffer：**保持 false**
- Render target：`rescaleable = block.depth == 0; rescaleable &= size.height > RescaleHeightThreshold;`
- Zeta（depth）：`rescaleable = block.depth == 0;`
- Fermi2D / DMA image：`rescaleable = block.depth == 0 && size.height > RescaleHeightThreshold;`

统一收口：

```cpp
rescaleable &= (block.depth == 0) && resources.levels == 1;      // 只有单 mip
rescaleable &= size.height > RescaleHeightThreshold ||
               GetFormatType(format) != SurfaceType::ColorTexture;
downscaleable = size.height > DownscaleHeightThreshold;          // 降分辨率要求更高
```

来源：[github.com/citron-neo/emulator/blob/main/src/video_core/texture_cache/image_info.cpp](https://github.com/citron-neo/emulator/blob/main/src/video_core/texture_cache/image_info.cpp)

**可借鉴的三条规则**：
1. **高度 ≤ 288 的 image 不升分辨率**。小目标（bloom downsample 链、LUT、shadow map 小级）升上去只吃带宽不出画质，且常被 shader 用 `texelFetch` 按固定像素坐标采样，缩放后语义会错。
2. **只有单 mip 的 image 能缩放**（`resources.levels == 1`）。多 mip 链每级都要重算尺寸，且 mip 生成用固定 kernel，缩放后 kernel 半径不匹配。**对本项目尤其重要**：`docs/nvn/TILING.md` 提到 P5R 的 BNTX 纹理有 12 级 mip 链，必须原样保留。
3. **`downscaleable` 门槛（512）比 `rescaleable`（288）高**。降分辨率更危险——0.5x 时 288px 只剩 144px，`texelFetch` 整数坐标会塌陷。**本项目若要加 0.75x 档，必须照抄这种"双阈值"，不能只用一个。**

### 1.3 纹理 vs 渲染目标的区分：`ImageCanRescale` + 别名闭包

一个 guest 内存块可能同时是"渲染目标"和"被采样纹理"（P5R 到处都是：3D 场景渲染到 960x540，然后被 composite pass 采样）。只在 RT 路径放大，采样侧就读到未缩放内容。

yuzu 的解法是**沿别名（alias）关系做闭包**。`src/video_core/texture_cache/texture_cache.h`：

```cpp
template <class P>
bool TextureCache<P>::ImageCanRescale(ImageBase& image) {
    if (!image.info.rescaleable) { return false; }
    if (Settings::values.resolution_info.downscale && !image.info.downscaleable) { return false; }
    if (True(image.flags & (ImageFlagBits::Rescaled | ImageFlagBits::CheckingRescalable))) { return true; }
    if (True(image.flags & ImageFlagBits::IsRescalable)) { return true; }
    image.flags |= ImageFlagBits::CheckingRescalable;
    for (const auto& alias : image.aliased_images) {          // ← 递归检查所有别名
        Image& other_image = slot_images[alias.id];
        if (!ImageCanRescale(other_image)) {
            image.flags &= ~ImageFlagBits::CheckingRescalable;
            return false;
        }
    }
    image.flags &= ~ImageFlagBits::CheckingRescalable;
    image.flags |= ImageFlagBits::IsRescalable;
    return true;
}
```

`CheckingRescalable` 是**环检测**（防别名成环无限递归）。

来源：[github.com/citron-neo/emulator/blob/main/src/video_core/texture_cache/texture_cache.h](https://github.com/citron-neo/emulator/blob/main/src/video_core/texture_cache/texture_cache.h)

**对本项目的直接含义**：`runtime/src/nvn/nvn_image.cpp` 有 `StorageKey` + `g_live_images`（注释原文 "Live textures over the same storage share one image"），同一套"别名"概念。要做缩放，必须让 `StorageKey` 带上"是否已缩放"位，否则同一 storage 的两个 view 会一个缩放一个不缩放 → 画面撕裂。

### 1.4 什么时候缩放：`RescaleRenderTargets()` 的滞后启发式

yuzu 不是"设置一改就立刻缩放"，而是**每次绑定 RT 时重新判定**，用 `scale_rating` 做两帧确认：

```cpp
const auto check_rescale = [&](ImageViewId view_id, ImageId& id_save) {
    ...
    can_rescale &= ImageCanRescale(image);
    any_rescaled |= True(image.flags & ImageFlagBits::Rescaled) ||
                    GetFormatType(image.info.format) != SurfaceType::ColorTexture;
    scale_rating = std::max<u32>(scale_rating, image.scale_tick <= frame_tick
                                                   ? image.scale_rating + 1U
                                                   : image.scale_rating);
};
...
if (can_rescale) {
    rescaled = any_rescaled || scale_rating >= 2;     // ← 两帧确认
    if (rescaled) { /* 对每个 RT 调 ScaleUp */ scale_rating = 2; }
} else {
    rescaled = false;
    /* 对每个 RT 调 ScaleDown */ scale_rating = 1;
}
```

`scale_rating >= 2`：**同一个 RT 连续两帧都可缩放，才真的放大**。理由：`vkCreateImage` 昂贵（Adreno 上还有 UBWC 元数据分配），不能因一帧临时绑定就重建。**直接对应本项目 `docs/nvn/android-perf/PLAN.md` G1 的"Adreno 上 image 重建代价"。**

还有一条**降级逃生门**：只要有一个 RT 的 `ImageCanRescale` 为 false（比如该帧把 pitch-linear 纹理绑成 RT），**整组 RT 一起 `ScaleDown`**，不做"部分缩放"——混着缩放会让 viewport 尺寸和 shader 假设打架。

### 1.5 viewport / scissor 与 shader 尺寸查询

缩放后 guest 的 `surface_clip` 还是原始尺寸，必须换算：

```cpp
u32 up_scale = 1, down_shift = 0;
if (is_rescaling) {
    up_scale  = Settings::values.resolution_info.up_scale;
    down_shift = Settings::values.resolution_info.down_shift;
}
render_targets.size = Extent2D{
    (maxwell3d->regs.surface_clip.width  * up_scale) >> down_shift,
    (maxwell3d->regs.surface_clip.height * up_scale) >> down_shift,
};
render_targets.is_rescaled = is_rescaling;
```

并在状态翻转时置脏位：

```cpp
const bool rescaled = RescaleRenderTargets();
if (is_rescaling != rescaled) {
    flags[Dirty::RescaleViewports] = true;
    flags[Dirty::RescaleScissors]  = true;
    is_rescaling = rescaled;
}
```

**本项目已有对应物**：`runtime/src/nvn/nvn_shaders.cpp:126` 的 `support_buffer_set_viewport()`：

```cpp
void support_buffer_set_viewport(SupportBuffer& sb, float width, float height, bool transform_disabled, float scale) {
    sb.render_scale[0][0] = scale;  // the render target's (NX_HUD_1080: 1.5); gl_FragCoord.xy is divided by it
    sb.viewport_inverse[0] = width  != 0 ? scale * 2.0f / width  : 0.0f;
    sb.viewport_inverse[1] = height != 0 ? scale * 2.0f / height : 0.0f;
```

而 `runtime/src/nvn/nvn_shaders.h:190`：
`float render_scale[73][4];    // 0x0c0 x only: [0] render target, [1..64] textures, [65..72] images`
以及 `nvn_shaders.h:189` 的 `int32_t frag_scale_count[4];`。

**结论：本项目的 support buffer 已经完整实现了 Ryujinx/yuzu 的 render-scale 契约**（73 个 scale 槽 + `frag_scale_count` + `nvn_shaders.h:196-197` 的 `offsetof` 静态断言）。**shader 侧基础设施已就位**，缺的只是 image 侧真的去缩放、以及往这些槽里填非 1.0 的值。这是本项目最被低估的资产——`docs/nvn/ANDROID-PORT.md` 5.1 说 "Going below 720p would need the translator's render-scale support-buffer fields (TRANSLATOR.md:278). Not planned."，但字段其实已经写好了。

### 1.6 拷贝 / blit 路径的缩放

`nvn_copy.cpp` 对应 yuzu 的 `TextureCache::BlitImage`。yuzu 对拷贝两端分别缩放 region：

```cpp
if (ImageCanRescale(src_image)) { ScaleUp(src_image); }
if (ImageCanRescale(dst_image)) { ScaleUp(dst_image); }
region.start.x = resolution.ScaleUp(region.start.x);
region.start.y = resolution.ScaleUp(region.start.y);
region.end.x   = resolution.ScaleUp(region.end.x);
region.end.y   = resolution.ScaleUp(region.end.y);
```

且**两端缩放状态必须一致**，否则退回 `ScaleDown`。本项目 `runtime/src/nvn/nvn_copy.cpp:134` 的注释已区分"两边同尺寸 → 普通 copy；只有一边 → 需要 blit 缩放"，但那是 `NX_HUD_1080` 的 1.5x 场景。要支持任意倍率，判断要改成"两端的 scale 是否相等"。

### 1.7 升采样 / 锐化：`ScalingFilter` 枚举 + 独立 present pass

yuzu 系把"内部渲染分辨率"和"最终上采样"完全解耦：

`src/common/settings_enums.h`：
```
ENUM(ScalingFilter, NearestNeighbor, Bilinear, Bicubic, Gaussian, ScaleForce, ScaleFx, Lanczos, Fsr, Fsr2, CRTEasyMode, CRTRoyale, Cas, MaxEnum);
```

每个 filter 是独立的 `WindowAdaptPass` 子类（`src/video_core/renderer_vulkan/present/filters.h`）：
```cpp
std::unique_ptr<WindowAdaptPass> MakeNearestNeighbor(const Device&, VkFormat);
std::unique_ptr<WindowAdaptPass> MakeBilinear(const Device&, VkFormat);
std::unique_ptr<WindowAdaptPass> MakeBicubic(const Device&, VkFormat);
std::unique_ptr<WindowAdaptPass> MakeLanczos(const Device&, VkFormat);
std::unique_ptr<WindowAdaptPass> MakeGaussian(const Device&, VkFormat);
std::unique_ptr<WindowAdaptPass> MakeScaleForce(const Device&, VkFormat);
std::unique_ptr<WindowAdaptPass> MakeScaleFx(const Device&, VkFormat);
std::unique_ptr<WindowAdaptPass> MakeCRT(const Device&, VkFormat);
```

**移动端关键细节**在 `src/video_core/renderer_vulkan/vk_blit_screen.cpp` 的 `SetWindowAdaptPass()`：

```cpp
// OPTIMIZED FOR LOW GPU ACCURACY - use Bilinear instead of heavy filters for performance
const auto effective_filter =
    Settings::IsGPULevelLow() && scaling_filter != Settings::ScalingFilter::NearestNeighbor &&
            scaling_filter != Settings::ScalingFilter::Bilinear
        ? Settings::ScalingFilter::Bilinear
        : scaling_filter;
```

即**低 GPU 档位下强制把 FSR / Lanczos / Bicubic 降级成 Bilinear**，只保留 Nearest 和 Bilinear。明确的"上采样不是免费"表态。

另外 `FSR`/`Fsr2`/`Cas` 在 switch 里和 `Bilinear` 走同一个 default 分支（`MakeBilinear`）——说明 **Citron 当前版本的 FSR 选项并没有真正接到 FSR shader 上**（与 yuzu 原版不同，可能是 Citron 裁掉）。**本项目 `runtime/src/nvn/nvn_present.cpp` 反而更完整**：自带 `NX_FSR=1` 的 EASU + RCAS 两段式实现（`fsr_on()` 在 `nvn_present.cpp:139`，`fsr_sharpness()` 支持 `NX_FSR_SHARP`，`fsr_inter()` 分配 `R16G16B16A16_SFLOAT` 中间 image）。

### 1.8 内存账：缩放后要重算 size

yuzu 在 `texture_cache.h` 里对缩放 image 重算字节数：

```cpp
const u64 scale_up   = static_cast<u64>(Settings::values.resolution_info.up_scale *
                                        Settings::values.resolution_info.up_scale);
const u64 down_shift = static_cast<u64>(Settings::values.resolution_info.down_shift +
                                        Settings::values.resolution_info.down_shift);
const u64 tentative_size = (image_size_bytes * scale_up) >> down_shift;
```

即**面积按 `up_scale² >> down_shift²` 缩放**，并计入 `total_used_memory`。

**对本项目（16GB）**：P5R 720p handheld 下 GPU 侧约 1.0-1.2GB（`docs/nvn/android-perf/MEASUREMENTS.md`：`pipe1` Graphics 1,207MB）。上到 2K（约 2560x1440，面积是 1280x720 的 4 倍）后，**若所有 RT 都放大，RT 内存约 4 倍**。但按 yuzu 规则（只放大单 mip、高度 > 288 的 RT），实际只放大 3D 场景链和窗口纹理，UI/纹理不动 → 估算增量 1.5-2.5GB。16GB 可承受，但需在 `runtime/src/memguard_android.cpp` 加预算检查。

### 1.9 可借鉴点小结（→ 本项目）

| yuzu 做法 | 本项目现状 | 可借鉴 |
|---|---|---|
| `up_scale`/`down_shift` 整数倍率 | `platform_android.cpp:323 window_size()` 只有 `target_h` / `render_scale`（float，且 `scale = min(scale, 1080.0f/sh)` 硬封顶 1080） | 换整数对，解掉 1080 封顶 |
| `rescaleable` 三条件（非 pitch-linear、单 mip、block.depth==0） | 无 | 新增判定 |
| 双阈值（288 升 / 512 降） | 无 | 新增 |
| 别名闭包 `ImageCanRescale` | `nvn_image.cpp` 有 `StorageKey`/`g_live_images` 但无缩放概念 | 在 `StorageKey` 加 scale 位 |
| 两帧确认 `scale_rating >= 2` | 无 | 新增，避免重建风暴 |
| 整组 RT 一起降级 | 无 | 新增 |
| support buffer 的 73 个 `render_scale` 槽 | **已实现**（`nvn_shaders.h:190`） | 只需填值 |
| `ScalingFilter` 枚举 + 独立 present pass | `nvn_present.cpp` 有 Bilinear + FSR1 | 补 Nearest / FSR 档位选择 |
| 低 GPU 档强制 Bilinear | 无 | 新增 |

---

### 1.10 yuzu vs Ryujinx：两种真实设计的正面对比（**本节最有价值**）

两家都真正实现了内部渲染分辨率缩放，但设计完全不同。本项目要在 2K 上做缩放，**必须先选一种模型**。

| 维度 | yuzu / Citron | Ryujinx |
|---|---|---|
| 倍率表示 | **有理数对** `up_scale` / `down_shift` | **单个 float** `GraphicsConfig.ResScale`（`public static float ResScale = 1f;`，无倍率枚举） |
| 能否降分辨率 | **能**（0.5x / 0.75x，靠 `downscale` + `downscaleable`） | **不能**（无 <1x 模式） |
| 宿主 image | **双份**：`original_image` + `scaled_image`，翻转 `current_image` | 双份：`GetScaledHostTexture()` + `ReplaceStorage()`，并**重建所有 view** |
| 缩放实现 | `vk_texture_cache.cpp` `Image::ScaleUp(bool ignore)` | `Texture.cs` `SetScale(float scale)` |
| 资格判定 | `ImageInfo::rescaleable`（**格式/结构导向**）：非 pitch-linear、`block.depth==0`、`levels==1`、`height>288` 或非 ColorTexture | `TextureCache.IsUpscaleCompatible()` + `UpscaleSafeMode()`（**内容猜测导向**）：仅 2D/2DArray/2DMS 且非压缩；`Levels>3`、`<8px`、近似正方形、16:9 但 `<360p`、`W==H*H` 全部拉黑 |
| 状态机 | `ImageFlagBits::Rescaled / CheckingRescalable / IsRescalable` + `scale_rating` / `scale_tick` | `enum TextureScaleMode { Eligible=0, Scaled=1, Blacklisted=2, Undesired=3 }` |
| 迟滞 | `rescaled = any_rescaled \|\| scale_rating >= 2;`（两帧确认） | `Eligible → Scaled` 迁移 + `StateUpdater.CommitBindings()` 的 `do { … } while (scaleMismatch);` 重绑循环 |
| "既采样又渲染"的解法 | 写 storage image 时降级：`FillImageViews<has_blacklists>` 里 `ScaleDown(image); image.scale_rating = 0;`，由 `vk_graphics_pipeline.cpp` 的 `add_image(desc, desc.is_written)` 提供黑名单位 | CPU 写入时降级 + 传播：`ScaledSetWeight = 10` / `ScaledSetThreshold = 30` 打分，`BlacklistScale()`；`TextureManager.UpdateRenderTargetScale()` 里 `mismatch \|= blacklisted && hasUpscaled;` 且 `if (blacklisted) foreach (…) color?.BlacklistScale();` |
| shader 侧契约 | **push constant 位掩码**：`NUM_TEXTURE_SCALING_WORDS = 4`、`NUM_IMAGE_SCALING_WORDS = 2`，加 `down_factor` 和 4-float `render_area`；SPIR-V 侧 `EmitIsTextureScaled` / `EmitIsImageScaled` / `EmitResolutionDownFactor` / `EmitRenderArea` | **support buffer 数组**：`RenderScaleMaxCount = 1 + 64 + 8`（`[0]` fragment output，`[1..64]` textures，`[65..72]` images），`frag_scale_count` 单独一个 s32 |
| 负 scale 技巧 | 无 | **有**：`UpdateScale()` 里 `result = -scale;`，注释 "enable interpolation using gl_FragCoord. (helps 'invent' new integer values between scaled pixels)" |
| blit 过滤 | `BlitScale`：`// Depth and integer formats must use NEAREST filter for blits.` → `is_bilinear = is_color && !IsPixelFormatInteger(info.format)` | 由 GAL 后端决定 |
| FSR 位置 | **不在** window pass（`vk_blit_screen.cpp` 把 `ScalingFilter::Fsr` 路由到 `MakeBilinear`），而在 `present/layer.cpp` 的 `CreateFSR()`：EASU+RCAS 进两张 `R16G16B16A16_SFLOAT` | `FsrScalingFilter`（`FsrScaling.spv` + `FsrSharpening.spv`），在 `Window.Present()` 里**替代** `BlitColor` |
| 锐化参数 | `fsr_sharpening_slider` 0–200，用时 `/100.0f` | `MathF.Max(0.01f, level)` |

**三家共同的铁律（可直接作为本项目的验收清单）**：
1. **只有两种设计**：要么"资格过滤 + 双份分配"（yuzu、Ryujinx），要么"完全不缩放、宿主 extent 断言等于 guest extent"（Skyline/Strato）。**没有"无差别放大所有 attachment"这第三种设计。**
2. **"既采样又渲染"靠降级与传播解决，从不靠双驻留。** 两家都会把不可缩放的 attachment 拖累到**整组**一起回 1x——因为一个 framebuffer 只有一个 extent。
3. **迟滞是必须的**，因为判定是"每帧一次且粘性"的。
4. **shader 侧一致性用显式"该绑定是否被缩放"的位掩码/数组，而不是改变 `textureSize` 的返回值。**
5. **blit 过滤按格式决定，不由用户选择**：深度/整数格式必须 NEAREST。

**Strato / Skyline 的决定性负面证据（重要，避免我们走错路）**：
Strato（Skyline 的直接延续，同样的 `skyline::gpu` 命名空间与 MPL-2.0 头）**完全没有实现渲染分辨率缩放**。`app/src/main/cpp/skyline/gpu/texture/texture.cpp` 的 `Texture::SynchronizeHostImpl()` 里：

```cpp
if (guest->dimensions != dimensions)
    throw exception("Guest and host dimensions being different is not supported currently");
```

即**宿主 `VkImage` 的 extent 被强制要求等于 guest extent**——这正是 yuzu/Ryujinx 刻意打破的那条不变量。我核对过该文件全文：`resolutionScale` / `ResolutionScale` **一次都没出现**。它的 `RenderPassUsage { None, Sampled, RenderTarget }`（`texture.h`）只用于 render pass 兼容性，不是缩放资格；`common/settings.h` 里也没有任何分辨率条目。

来源：
- [github.com/Nailington/Yuzu/blob/master/src/common/settings.h](https://github.com/Nailington/Yuzu/blob/master/src/common/settings.h)
- [github.com/Nailington/Yuzu/blob/master/src/common/settings.cpp](https://github.com/Nailington/Yuzu/blob/master/src/common/settings.cpp)
- [github.com/Nailington/Yuzu/blob/master/src/video_core/texture_cache/image_info.cpp](https://github.com/Nailington/Yuzu/blob/master/src/video_core/texture_cache/image_info.cpp)
- [github.com/Nailington/Yuzu/blob/master/src/video_core/texture_cache/texture_cache.h](https://github.com/Nailington/Yuzu/blob/master/src/video_core/texture_cache/texture_cache.h)
- [github.com/Nailington/Yuzu/blob/master/src/video_core/texture_cache/image_base.h](https://github.com/Nailington/Yuzu/blob/master/src/video_core/texture_cache/image_base.h)
- [github.com/Nailington/Yuzu/blob/master/src/video_core/renderer_vulkan/vk_texture_cache.cpp](https://github.com/Nailington/Yuzu/blob/master/src/video_core/renderer_vulkan/vk_texture_cache.cpp)
- [github.com/Nailington/Yuzu/blob/master/src/video_core/renderer_vulkan/vk_rasterizer.cpp](https://github.com/Nailington/Yuzu/blob/master/src/video_core/renderer_vulkan/vk_rasterizer.cpp)
- [github.com/Nailington/Yuzu/blob/master/src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp](https://github.com/Nailington/Yuzu/blob/master/src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp)
- [github.com/Nailington/Yuzu/blob/master/src/video_core/renderer_vulkan/pipeline_helper.h](https://github.com/Nailington/Yuzu/blob/master/src/video_core/renderer_vulkan/pipeline_helper.h)
- [github.com/Nailington/Yuzu/blob/master/src/shader_recompiler/backend/spirv/emit_spirv.h](https://github.com/Nailington/Yuzu/blob/master/src/shader_recompiler/backend/spirv/emit_spirv.h)
- [github.com/Nailington/Yuzu/blob/master/src/video_core/renderer_vulkan/present/fsr.cpp](https://github.com/Nailington/Yuzu/blob/master/src/video_core/renderer_vulkan/present/fsr.cpp)
- [github.com/Nailington/Yuzu/blob/master/src/video_core/renderer_vulkan/present/layer.cpp](https://github.com/Nailington/Yuzu/blob/master/src/video_core/renderer_vulkan/present/layer.cpp)
- [github.com/alula/Ryujinx/blob/qlaunch/src/Ryujinx.Graphics.Gpu/Image/TextureScaleMode.cs](https://github.com/alula/Ryujinx/blob/qlaunch/src/Ryujinx.Graphics.Gpu/Image/TextureScaleMode.cs)
- [github.com/alula/Ryujinx/blob/qlaunch/src/Ryujinx.Graphics.Gpu/Image/Texture.cs](https://github.com/alula/Ryujinx/blob/qlaunch/src/Ryujinx.Graphics.Gpu/Image/Texture.cs)
- [github.com/alula/Ryujinx/blob/qlaunch/src/Ryujinx.Graphics.Gpu/Image/TextureCache.cs](https://github.com/alula/Ryujinx/blob/qlaunch/src/Ryujinx.Graphics.Gpu/Image/TextureCache.cs)
- [github.com/alula/Ryujinx/blob/qlaunch/src/Ryujinx.Graphics.Gpu/Image/TextureManager.cs](https://github.com/alula/Ryujinx/blob/qlaunch/src/Ryujinx.Graphics.Gpu/Image/TextureManager.cs)
- [github.com/alula/Ryujinx/blob/qlaunch/src/Ryujinx.Graphics.Gpu/Image/TextureBindingsManager.cs](https://github.com/alula/Ryujinx/blob/qlaunch/src/Ryujinx.Graphics.Gpu/Image/TextureBindingsManager.cs)
- [github.com/alula/Ryujinx/blob/qlaunch/src/Ryujinx.Graphics.Shader/SupportBuffer.cs](https://github.com/alula/Ryujinx/blob/qlaunch/src/Ryujinx.Graphics.Shader/SupportBuffer.cs)
- [github.com/alula/Ryujinx/blob/qlaunch/src/Ryujinx.Graphics.Vulkan/Window.cs](https://github.com/alula/Ryujinx/blob/qlaunch/src/Ryujinx.Graphics.Vulkan/Window.cs)
- [github.com/strato-emu/strato/blob/master/app/src/main/cpp/skyline/gpu/texture/texture.cpp](https://github.com/strato-emu/strato/blob/master/app/src/main/cpp/skyline/gpu/texture/texture.cpp)

**对本项目的取舍建议**：本项目是**原生重编译（recomp）+ NVN 直通**，不是 guest GPU 指令模拟，因此**没有 yuzu/Ryujinx 那种"每帧重新发现 image"的问题**——P5R 的 render target 在 `nvn_image.cpp` 里是按 guest `NVNtexture` 稳定标识的。所以：
- **该抄 yuzu 的"有理数对"**（1.1），因为要支持 0.75x 降档且要避免 1px 缝。
- **不该抄 Ryujinx 的内容启发式**（`Levels>3`、`W==H*H` 这些是在猜"这到底是不是 render texture"）——本项目**已经知道**哪些是 render target（`nvn_draw.cpp` 的 pass 目标）。
- **必须抄两家的"整组一起降级"**（铁律 2），因为 Adreno 的 framebuffer 也只有一个 extent。

---

## 二、帧率与帧调度（present mode / setFrameRate / ADPF / 解锁帧率的坑）

### 2.1 Present mode：MAILBOX 的由来，以及 90fps 时它会怎么坏

`runtime/src/nvn/nvn_vk.cpp:437` 起：

```cpp
ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
...
auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
if (!std::getenv("NX_FIFO")) {
    if (has(VK_PRESENT_MODE_MAILBOX_KHR))
        ci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;   // L445
    else if (has(VK_PRESENT_MODE_IMMEDIATE_KHR))
        ci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
}
```

注释给的理由（`nvn_vk.cpp:433-436`）：

> "The game paces itself on its vsync event (hle_vi.cpp, 60 Hz): a FIFO present also waited for the display's own refresh, two clocks drifting against each other (stutter near 60 fps, docs/nvn/DEBUG.md #33). MAILBOX never blocks and shows the newest frame at each refresh"

**这个理由在 60Hz 面板 + 60fps 内容下成立，但在 144Hz 面板 + 90fps 内容下会变成新问题**：
- MAILBOX 永远显示"最新一帧"，面板每 6.94ms（144Hz）取一次。
- 90fps 内容每 11.11ms 产生一帧。144 与 90 的最小公倍数是 720 → **每 8 帧（88.9ms）一个完整周期**，某些帧被显示 1 次（6.94ms）、某些 2 次（13.9ms）。
- 结果：**judder 周期约 89ms，肉眼可见的"呼吸"**。不是本项目的 bug，是 MAILBOX + 非整数倍刷新率的固有行为。

本项目自己的文档已预见（`docs/nvn/android-perf/PLAN.md` P2）：

> "The game's vsync event is a host timer, frames are paced by a CPU sleep, and presents are MAILBOX into a 5-image swapchain on a panel that may run at 120 Hz. Phase is free-running against the display, so 60 fps content can show 1-3 refreshes per frame (judder) and drift."

**P2 处方（Android only）**：① `ANativeWindow_setFrameRate`；② FIFO + `minImageCount` 3；③ 用 `AChoreographer_postVsyncCallback` 驱动 `hle_vi` 的 vsync；④ 保留 `pace_present` 作上限。

**对本项目 90fps 目标的修正**：目标面板 144Hz，90fps 内容。144/90 = 1.6，**不是整数**。要稳定 90fps 只有两条路：
- **(a) 让面板切到 90Hz**：`ANativeWindow_setFrameRate(w, 90.0f, ...)`。这样 FIFO 就是 11.11ms 一拍，完美均匀。**首选**。
- **(b) 保持 144Hz + MAILBOX**，接受 89ms 周期 judder。**不符合"稳定 90fps"验收标准。**

所以 **P2 的目标值必须参数化为 90 而非写死 60**。当前 `platform_android.cpp:528 request_frame_rate()` 默认值硬编码：

```cpp
const char* e = getenv("NX_WINDOW_HZ");
const float hz = e ? float(std::atof(e)) : 60.0f;
```

其注释说明它是为"90Hz 面板上的 60fps 不匀"而加：

> "A 90 Hz panel (Lenovo Tab M11) showed a 30 fps frame for 3 refreshes or, when a present landed next to a refresh, 2 or 4: uneven motion in films and cutscenes."

**说明本项目已经踩过"非整数倍刷新率"的坑（90Hz 面板 + 60fps 内容）。现在要在 144Hz 面板 + 90fps 内容上再踩一次。**

### 2.2 `ANativeWindow_setFrameRate` 的确切语义

AOSP 头文件 `frameworks/native/libs/nativewindow/include/android/native_window.h`（读自 `android.googlesource.com` `main` 分支）：

```c
enum ANativeWindow_FrameRateCompatibility {
    ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT = 0,
    ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE = 1,
    ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_GTE = 2
};

int32_t ANativeWindow_setFrameRate(ANativeWindow* window, float frameRate, int8_t compatibility)
        __INTRODUCED_IN(30);

enum ANativeWindow_ChangeFrameRateStrategy {
    ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS = 0,
    ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS = 1
};
```

来源：[android.googlesource.com/platform/frameworks/native/+/refs/heads/main/libs/nativewindow/include/android/native_window.h](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/main/libs/nativewindow/include/android/native_window.h)

**关键（游戏内容必须用 `DEFAULT`，不是 `FIXED_SOURCE`）**：
- `DEFAULT = 0` 文档原文："There are no inherent restrictions on the frame rate of this window. When the system selects a frame rate other than what the app requested, the app will be able to run at the system frame rate without requiring pull down. **This value should be used when displaying game content.**"
- `FIXED_SOURCE = 1` 文档原文："This window is being used to display content with an inherently fixed frame rate, e.g. a video... **This value should be used for video content.**"

**本项目当前用的是 `1`（FIXED_SOURCE）**，`platform_android.cpp:541`：

```cpp
int32_t r = set_always ? set_always(w, hz, 1 /* COMPATIBILITY_FIXED_SOURCE */, 1 /* CHANGE_FRAME_RATE_ALWAYS */)
                       : set(w, hz, 1);
```

**这是应该改的 bug**：对游戏内容传 `FIXED_SOURCE` 会告诉系统"我会用 pulldown 适配非匹配刷新率"，调度器因此**降低**把面板切到请求刷新率的优先级。游戏应该传 `0`。

官方文档还明确这个 API 只是**请求**（[developer.android.com/media/optimize/performance/frame-rate](https://developer.android.com/media/optimize/performance/frame-rate) ）：

> "Important: The scheduler takes a number of factors into consideration when deciding on the display refresh rate, so there is no guarantee that your app will get the frame rate you request with `setFrameRate()`."

以及：

> "To see if a call to `setFrameRate()` results in a change to the display refresh rate, register for display change notifications by calling `DisplayManager.registerDisplayListener()` or `AChoreographer_registerRefreshRateCallback()`."

**本项目当前完全不验证请求是否生效**——只 log 了返回值 `r`，没有注册 `AChoreographer_registerRefreshRateCallback`。在 OnePlus Pad 3 Pro 上（144Hz，可能有自适应刷新率策略）**很可能请求被静默忽略**，而我们会以为它在 90Hz 上跑。**这是必须补的可观测性。**

官方另两条建议：传精确帧率（"best to pass in the exact frame rate rather than rounding to an integer"，即 `90.0f`）；surface 保持可见但不再提交帧时（暂停菜单）应 `setFrameRate(0)` 清除。

### 2.3 Swappy（Android Frame Pacing library）

官方 Vulkan 集成文档（[developer.android.com/games/sdk/frame-pacing/vulkan/add-functions](https://developer.android.com/games/sdk/frame-pacing/vulkan/add-functions) ）的调用序列：

```c
uint64_t refreshDuration;  // in nanoseconds
// Example: 60 FPS sets "refreshDuration" to 16,666,666.
SwappyVk_initAndGetRefreshCycleDuration(env, jactivity, physicalDevice, device, swapchain, &refreshDuration);

// 30 FPS 示例
SwappyVk_setSwapIntervalNS(device, swapchain, SWAPPY_SWAP_30FPS);

SwappyVk_setWindow(...);   // 显示面变化时必须重新设 ANativeWindow

// 每帧
SwappyVk_queuePresent(...);  // 内部调用 vkQueuePresentKHR

// 销毁
SwappyVk_destroySwapchain(device, swapchain);
vkDestroySwapchainKHR(device, swapchain, &allocator);
```

自适应行为可调：
```c
void SwappyVk_setAutoSwapInterval(bool enabled);
void SwappyVk_setMaxAutoSwapIntervalNS(uint64_t max_swap_ns);
void SwappyVk_setAutoPipelineMode(bool enabled);
```

官方提醒：

> "Note: To achieve proper frame pacing, the Android Frame Pacing API can insert Vulkan commands into the presentation queue."

**对本项目的适配难度（评估）**：Swappy 要求**它自己拥有 swapchain 和 present**（`SwappyVk_queuePresent` 内部 `vkQueuePresentKHR`）。但本项目是 `vk_bridge.cpp` + `vk_swapchain.cpp` 做了一层**虚拟 swapchain**，present 路径在 `nvn_vk.cpp:745 gpu_present()`，且 `gpu_present` 跑在 worker 线程上（`nvn_queue.cpp` 的 `worker_run([...] { gpu_present(...) })`）。**插入 Swappy 需要把它放进 `gpu_present` 替换 `p_vkQueuePresentKHR`，且 Swappy 可能要求与 swapchain 创建同线程**——这与"present 在 worker 线程"冲突。**未核实**：Swappy 是否要求 `setWindow`/`setSwapIntervalNS` 与 swapchain 创建同线程（文档未明说）。

**更简单的替代**：本项目已有 `pace_present`（`nvn_queue.cpp:594`）做 CPU sleep 限速，加上 2.1 的 FIFO + `setFrameRate(90)` + Choreographer 驱动 vsync，就能达到 Swappy 的主要收益（均匀间隔），**不需要引入 gamesdk 依赖**。`PLAN.md` P2 也是这个判断：

> "The alternative is the Android Frame Pacing library (Swappy, Apache-2.0), which does the same with `VK_GOOGLE_display_timing`."

### 2.4 ADPF（`APerformanceHint`）：已实现，但三个明显缺口

`runtime/src/perf_hint.cpp` 已用 `dlsym` 动态加载：

```cpp
x.get_manager = reinterpret_cast<GetManager>(dlsym(lib, "APerformanceHint_getManager"));
x.create      = reinterpret_cast<CreateSession>(dlsym(lib, "APerformanceHint_createSession"));
x.report      = reinterpret_cast<ReportActual>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
x.close       = reinterpret_cast<CloseSession>(dlsym(lib, "APerformanceHint_closeSession"));
```

NDK 官方头文件（[developer.android.com/ndk/reference/group/a-performance-hint](https://developer.android.com/ndk/reference/group/a-performance-hint) ）确认的完整 API 面：

| 函数 | 签名 | 用途 |
|---|---|---|
| `APerformanceHint_getManager` | `APerformanceHintManager*()` | 取 manager |
| `APerformanceHint_createSession` | `(manager, const int32_t* threadIds, size_t size, int64_t initialTargetWorkDurationNanos)` | 建 session |
| `APerformanceHint_createSessionUsingConfig` | `(manager, ASessionCreationConfig*, APerformanceHintSession** out)` | 新式建 session |
| `APerformanceHint_reportActualWorkDuration` | `(session, int64_t)` | 上报实际耗时 |
| `APerformanceHint_reportActualWorkDuration2` | `(session, AWorkDuration*)` | CPU/GPU 分开上报 |
| `APerformanceHint_updateTargetWorkDuration` | `(session, int64_t)` | **改目标帧时长** |
| `APerformanceHint_setThreads` | `(session, const pid_t*, size_t)` | **增删线程而不重建 session** |
| `APerformanceHint_setNativeSurfaces` | `(session, ANativeWindow**, size_t, ASurfaceControl**, size_t)` | **关联 surface** |
| `APerformanceHint_notifyWorkloadIncrease` | `(session, bool cpu, bool gpu, const char*)` | **预告负载上升** |
| `APerformanceHint_notifyWorkloadSpike` | `(session, bool cpu, bool gpu, const char*)` | **预告一次性尖峰** |
| `APerformanceHint_notifyWorkloadReset` | `(session, bool cpu, bool gpu, const char*)` | 负载特征重置 |
| `APerformanceHint_setPreferPowerEfficiency` | `(session, bool)` | 省电优先 |
| `APerformanceHint_getMaxGraphicsPipelineThreadsCount` | `(manager) -> int` | 图形管线线程上限 |
| `APerformanceHint_isFeatureSupported` | `(APerformanceHintFeature) -> bool` | 特性探测 |

**三个缺口**：

1. **目标时长写死 16.67ms / 33.3ms**（`perf_hint.cpp:113-116`）：
   ```cpp
   const char* cap = std::getenv("NX_FPS_CAP");
   int64_t target = cap && std::atoi(cap) > 0 && std::atoi(cap) <= 30 ? 33'333'333 : 16'666'667;
   ```
   **90fps 需要 11'111'111ns**，当前代码产生不出这个值（`cap_fps()` 在 `games/p5r/game.cpp:116` 也只有 30/60 两档）。**这是"稳定 90fps"的第一阻塞项。**

2. **session 重建代价**：`perf_hint_frame()` 在 `g_dirty` 时**关闭并重建**：
   ```cpp
   if (g_dirty && !g_tids.empty()) {
       if (g_session) a.close(g_session);
       g_session = a.create(a.get_manager(), g_tids.data(), g_tids.size(), target);
       g_dirty = false;
   }
   ```
   NDK 提供了 `APerformanceHint_setThreads()` 正是为了避免这个。`PLAN.md` R2 已指出：
   > "add threads via `APerformanceHint_setThreads` (API 34) instead of closing and recreating the session"

3. **没有 `setNativeSurfaces` / `notifyWorkloadSpike`**。后者对 P5R 特别有价值：**场景切换的 pipeline 编译尖峰**（`MEASUREMENTS.md`："Adreno compiles a new pipeline in ~15-20 ms (149 pipelines took 3.1 s in one window of the first run)"）正是它的设计场景——提前告诉系统"接下来几帧很重"，让 DVFS 提前升频。

### 2.5 "游戏逻辑与帧率绑定"的坑——本项目已有完整解，且比 yuzu 系更硬

这是本项目与 yuzu 系最大的架构差异，也是 90fps 目标最危险的地方。

**yuzu 系的做法**：Switch 游戏自己按 `nvnWindowSetPresentInterval` 决定节奏，模拟器基本不管。yuzu 有 `VSyncMode` 枚举（`ENUM(VSyncMode, Immediate, Mailbox, Fifo, FifoRelaxed);`）但**没有"把 30fps 游戏推到 90fps"的能力**——它不做游戏侧逻辑适配。

**本项目的做法**：`games/p5r/game.cpp` 深入引擎内部。注释（`game.cpp:60-84`）：

> "The game's target frame rate is `[[main+0x2209f50]+0x84]`: 30 on the Switch, read through main+f73730 all over the game and set by main+f73740(fps)... Every frame the game's main thread (main+fd4140) sets the engine's frame interval to 60 / it through a virtual setter (main+1048ee0: vsyncs per frame, 1..4, stored at obj+0x30), which the present (main+1025c9c) hands to nvnWindowSetPresentInterval."

最关键的警告（`game.cpp:67-73`）：

> "**Most of the game steps 1 / target per frame, so under the target it runs slow**: at ~10 fps the ATLUS logos took 35 s instead of 15... On the Adreno 610 (~8-13 fps in the 3D title and the casino's cutscenes) Joker's fall to the title menu and the police corner played at 30-45 % speed while their music and voices kept real time: the sound 'ahead'."

以及（`game.cpp:75-79`）：

> "A target that follows the device's frame rate (paced_target) keeps real time, but the game is made for steps of 1/30 and 1/60 s: with targets of 19-21 the casino battle where Violet joins never reached its command menu (shadows idle, buttons dead; no thread blocked, the battle's logic waited for good), twice."

**这是"游戏逻辑绑定帧率"的完整证据链**：P5R 每帧步进 `1/target` 秒，非标准 target 值（19、20、21）会让战斗逻辑死锁。当前解法是 `paced_target()`（`game.cpp:156`）：

```cpp
int paced_target(uint64_t period, int used) {
    const int cap = cap_fps(), lo = min_fps();
    ...
    if (g_field_scene && !g_scene) {  // only the cap, a half or a third of it: the nearest step
        const int a = cap, b = cap / 2, c = cap / 3;
        const double s = step * cap;  // in steps of the cap
        return s < 1.5 ? a : s < 2.5 ? b : c;
    }
    return std::clamp(int(1.0 / step + 0.5), lo, cap);
}
```

即**只在 cap、cap/2、cap/3 三档之间切换**，因为"steps of 1, 2 or 3 thirtieths"是引擎能接受的整数步长。

**对 90fps 的推论（关键，且当前代码不支持）**：
- cap=90 时 `cap/2 = 45`、`cap/3 = 30`。**45 和 30 都不是 P5R 的原生步长**（原生 30/60）。
- `game.cpp:116` 的 `cap_fps()` 是 `env_int("NX_FPS_CAP", 30) >= 60 ? 60 : 30` —— **只有 30 和 60**。要支持 90 必须改。
- 而 `game.cpp:354` 的 `const int interval = fps ? std::clamp(60 / fps, 1, 4) : 60 / cap_fps();` **在 fps=90 时 `60/90 = 0`，被 `clamp(0, 1, 4)` 拉到 1**。即 90fps 下 present interval 恒为 1，**游戏认为自己在跑 60fps**，逻辑步长 1/60 而实际帧率 90 → **游戏以 1.5 倍速运行**。

**这是"解锁 90fps"的头号正确性风险，且当前代码必然触发。**

本项目 `docs/nvn/studies/fps-unlock-1710.md` 已用 Wonder 验证过同一类问题：

> "**Wonder cannot simply be unlocked above 60.** Its logic advances by the present interval it asked for (1 or 2 sixtieths per frame), not by delta time. Presenting at 120 Hz with interval 1 would run the game at 2x speed (inferred, see 3)."
> "`NX_DISPLAY_HZ=120` ... **Experimental: game logic runs fast above 60.** Below 60 it is wrong the same way"

`hle_vi.cpp` 的 `NX_DISPLAY_HZ` 注释：

> "Experimental: Wonder's logic steps one 1/60 s unit per present interval, so above 60 the game runs fast."

**结论：`hle_vi.cpp` 的 `display_period_ns()` 与 `game.cpp` 的 `cap_fps()` 必须成对修改，且必须验证 P5R 在 target=90 时的行为。** 基于现有证据，唯一安全的 90fps 路径是**保持游戏逻辑在 60fps（target=60, interval=1）而渲染/呈现跑 90fps**——这需要帧生成或"每 2 帧渲染 3 帧"插值，代价高。次优路径：**先在 60fps 上把 2K 分辨率做实，再评估 90fps。**

### 2.6 帧调度可观测性：已有，但目标值不匹配

`runtime/src/frame_stats.cpp:228 frame_presented()` 已输出 present 间隔直方图（`frame_stats.cpp:238`）：

```
"present intervals over 600: <14 ms %llu, 14-19 %llu, 19-25 %llu, 25-31 %llu, 31-36 %llu, 36-50 %llu, ..."
```

**问题：分桶按 60fps/30fps 设计（14/19/25/31/36/50ms）。90fps 周期 11.11ms 全部落进 "<14 ms" 桶，直方图失去分辨率。** 必须改成 90fps 友好的桶（8/10/11.5/13/16/22/33ms）或改成"相对目标周期的百分比偏差"。

`frame_stats.cpp:121 thermal_headroom()` 已接 `AThermal_acquireManager` / `AThermal_getThermalHeadroom` —— 好基础。OnePlus Pad 3 Pro 在 2K+90fps 下必然受热限制，需要这个数据定"能撑多久"。

### 2.7 可借鉴点小结（→ 本项目）

| 项 | 本项目现状 | 应做 |
|---|---|---|
| present mode | MAILBOX（`nvn_vk.cpp:445`） | 90fps 时改 FIFO + `setFrameRate(90)` |
| `setFrameRate` compatibility | `1` = FIXED_SOURCE（`platform_android.cpp:541`） | 改 `0` = DEFAULT（游戏内容） |
| `setFrameRate` 目标值 | 硬编码 60（`platform_android.cpp:535`） | 参数化，支持 90 |
| 刷新率验证 | 无 | 加 `AChoreographer_registerRefreshRateCallback` |
| vsync 源 | 自由运行 timer（`hle_vi.cpp` 的 `vsync_thread`） | `AChoreographer_postVsyncCallback` |
| ADPF 目标 | 写死 16.67/33.3ms（`perf_hint.cpp:113`） | 支持 11.11ms |
| ADPF 线程管理 | close + create（`perf_hint.cpp:110-117`） | `APerformanceHint_setThreads` |
| ADPF 负载预告 | 无 | `notifyWorkloadSpike`（场景加载） |
| 帧率上限枚举 | 30/60 两档（`game.cpp:116`） | 加 90 并修正 interval 换算 |
| present 直方图 | 60fps 桶（`frame_stats.cpp:238`） | 90fps 桶 |

---

## 三、Adreno / 高通专属优化

### 3.1 UBWC（Universal Bandwidth Compression）——最高优先级

**机制**：Adreno 对 framebuffer 做无损压缩，压缩后读写带宽可降数倍。但**某些 image 创建标志会让压缩静默关闭**，Vulkan 不报错。

本项目已踩过并修了一半。`runtime/src/nvn/nvn_image.cpp:189-209`：

```cpp
// With ubwc_flags (Android): only where a view can differ, and with the list of those formats, as Adreno keeps
// its framebuffer compression (UBWC) on an image whose views are listed; MUTABLE_FORMAT alone turns it off
// (docs/nvn/android-perf/PHASE4.md G1a). In every run the only reinterpretation was RGBA8 viewed as sRGB.
VkFormat listed[2] = {};
VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
img->mutable_format = aspect == 0 && !img->alpha_from_r;
if (img->mutable_format && ubwc_flags()) {          // L200
    auto pair = [&](VkFormat a, VkFormat b) { ... };
    if (pair(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB) ||
        pair(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB)) {
        list.viewFormatCount = 2, list.pViewFormats = listed;
        ci.pNext = &list;
    } else {
        img->mutable_format = compressed;  // block formats: no UBWC to keep
    }
}
if (img->mutable_format) ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
```

`ubwc_flags()` 定义在 `runtime/src/nvn/nvn_caps.cpp:107`（`NX_UBWC_FLAGS`，Android 默认开）。

storage 用法（`nvn_image.cpp:222-230`）：

```cpp
// NX_IMAGE_STORAGE=0: no colour image can be a storage image. STORAGE usage turns Adreno's framebuffer
// compression (UBWC) off for the image, and some games never bind one (P5R: "storage image 0" in the pool
// peaks; games/p5r/game.cpp makes it its default). A storage view of such an image gets the dummy (logged).
```

`games/p5r/game.cpp` 确实把它设成默认（`setenv("NX_IMAGE_STORAGE", "0", 0);`）。

**已量到的收益**（`games/p5r/OPTIMIZACION.md` 的 ABAB 表格，Adreno 610）：

| 改动 | GPU 时间 | fps | 结论 |
|---|---|---|---|
| `NX_IMAGE_STORAGE=0` | 81.6/83.0 → 78.5/79.4 ms | 7.27/7.07 → 7.53/7.40（**+3.5%**） | **P5R 默认开启** |

**还能做的**（`PLAN.md` G1 剩余部分）：
- `STORAGE` 只在 NVN texture flags 真带 image-usage 位时才加（当前是"格式允许就加，除非全局关掉"）；
- `VK_IMAGE_TILING_LINEAR` 一律不用（本项目已是 `VK_IMAGE_TILING_OPTIMAL`，`nvn_image.cpp:216`，OK）；
- **attachment 用 `COLOR_ATTACHMENT_OPTIMAL` 而非 `GENERAL`**。本项目当前所有 attachment 是 `GENERAL`（`PLAN.md` 0.4：「attachments in `GENERAL`」）。**未核实**：Qualcomm 文档说 `MUTABLE_FORMAT` 和 storage usage 影响 UBWC，但 `GENERAL` layout 是否影响 UBWC 我没找到明确来源。

Qualcomm 官方最佳实践页（`PLAN.md` 引用）：
- [docs.qualcomm.com/doc/80-78185-2/topic/mobile_best_practices.html](https://docs.qualcomm.com/doc/80-78185-2/topic/mobile_best_practices.html)
- [docs.qualcomm.com/doc/80-78185-2/topic/gmem_loads.html](https://docs.qualcomm.com/doc/80-78185-2/topic/gmem_loads.html)

**⭐ 如何*验证* UBWC 到底有没有生效（本项目最需要的一条）。**
上面这些"哪些用法会关掉 UBWC"的说法，绝大多数来自二手资料，Qualcomm 的 Adreno GPU Developer Guide 本身抓不到（会 302 到 `qualcomm.com`，或返回空壳页）。但 Vulkan 提供了一个**规范内、可直接查询**的办法：

`VK_EXT_image_compression_control`（[refpage](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_image_compression_control.html)）——创建 image 后调 `vkGetImageSubresourceLayout2EXT`，读 `VkImageCompressionPropertiesEXT`，它会报告**实际应用的压缩方案与压缩率**。

**建议本项目加一个 `NX_UBWC_AUDIT=1` 开关**：在 `nvn_image.cpp` 创建每个 image 之后查一次，按格式/usage/flag 组合聚合统计，启动时打印一张表——"哪些 image 真的拿到了压缩、哪些没有"。这比反复猜 `GENERAL` / `MUTABLE_FORMAT` / `STORAGE` 哪个是元凶要可靠得多，而且**不需要 AGI**（AGI 在本项目的 Samsung 设备上曾经被 blocked，见 `docs/nvn/ANDROID.md`）。
**未核实**：Adreno 840 的驱动是否暴露 `VK_EXT_image_compression_control`（Adreno 740 的 Phase 0 扩展列表里没有它，但这不代表 840 也没有）。

**顺带一个免费的 UBWC 线索**：Mesa 把 UBWC 当作 **image 创建期的属性**，而不是运行期状态——这从 `TU_DEBUG=noubwc` 是个调试开关就能看出来。也就是说**一旦 image 用错 flag 创建出来，后面任何 layout 转换都救不回压缩**。所以本项目应该把 `NX_UBWC_AUDIT` 放在**创建点**，而不是帧末。

### 3.2 GMEM（tile memory）load/store——TBR 的核心

Adreno 是 tile-based：每个 render pass 的 attachment 在 GMEM（on-chip）里渲染，pass 开始 load、结束 store 到 system memory。**每次 load/store 都是一次全屏带宽操作。**

**规则**：
1. **`loadOp = DONT_CARE`** 当整个 attachment 会被覆盖（`nvn_clears.cpp:188-195` 已把 pending clear 转成 `loadOp = CLEAR`，对的）。
2. **`storeOp = DONT_CARE`** 当 pass 之后没人读这个 attachment（depth/stencil 尤其）。
3. **不要在 pass 中间 `vkCmdClearAttachments`**——强制一次额外 GMEM 操作。`nvn_clears.cpp` 注释已明确：「with some channels masked off, is never a loadOp; it runs in the pass, with vkCmdClearAttachments on the scissor」。
4. **减少 pass 数量**。实际数据（`PLAN.md` 0.4）：「629 draws and ~70 passes ... 53 ended by a clear of other targets, 15 by a draw, 3 by dispatches, 12 clear-only」。**70 个 pass 里 12 个是纯 clear**——TBDR 上每个都是一次 store+load。

**当前 storeOp 状态**（`nvn_renderpass.cpp:56`、`:71`）：

```cpp
a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;                                      // L56: colour，无条件
a.storeOp = k[9] & kNoStore ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;  // L71: depth，有条件
```

（`kNoStore` 在 `nvn_renderpass.cpp:26`：`constexpr uint32_t kNoStore = 0x100;`）

即**colour 永远 STORE**，depth 有条件 DONT_CARE。`PLAN.md` G5 提议用 per-submit lookahead 把"之后没人读的 depth"也设 DONT_CARE，估 `−0.3 to −1 ms`。

**扩展**：`VK_QCOM_render_pass_store_ops`（Phase 0 在 Adreno 740 上**已检测到存在**，`MEASUREMENTS.md`：「Extensions present: ... `VK_QCOM_render_pass_store_ops`, `VK_EXT_load_store_op_none`」）。它允许把 store 变成 no-op（`VK_ATTACHMENT_STORE_OP_NONE_QCOM`），比 `DONT_CARE` 更强。**已从 Khronos refpage 确认其要点**：内容"may not be written to memory, but prior contents are preserved"；若 render area 内确实产生了内容，那些内容变为 undefined。`STORE` "may force an implementation to… flush data to memory or to a higher level cache"，而 `NONE_QCOM` 可跳过这次 flush。**它已被 `VK_KHR_load_store_op_none` / Vulkan 1.3 的 `VK_ATTACHMENT_STORE_OP_NONE` 取代** → 本项目应优先用后者（跨厂商），QCOM 版当 fallback。
来源：[VK_QCOM_render_pass_store_ops refpage](https://docs.vulkan.org/refpages/latest/refpages/source/VK_QCOM_render_pass_store_ops.html)

**另外两条 Khronos 指南给出的硬性理由（比经验更权威，值得写进代码注释）**：
- `VK_ATTACHMENT_LOAD_OP_CLEAR` "is significantly faster than using explicit clear commands like `vkCmdClearAttachments`, as it allows the hardware to initialize tile memory directly without any external memory traffic." → **本项目的 `nvn_clears.cpp` 把 pending clear 变成 `loadOp = CLEAR` 正是官方推荐做法**，✅。
- MSAA resolve：Qualcomm 在 `VK_QCOM_rotated_copy_commands` 的 spec 里直接写 `vkCmdResolveImage2` "is very slow and extremely bandwidth intensive on Qualcomm's GPU architecture"，应改用 `pResolveAttachments`。**本项目目前 1x 用不到；将来若加 MSAA 必须走 `pResolveAttachments`。**

来源：[Khronos Vulkan Guide — TBR Best Practices](https://docs.vulkan.org/guide/latest/tile_based_rendering_best_practices.html)

### 3.3 Render pass 合并与 barrier 精细化

当前每个 pass 结束都有一次全屏障（`PLAN.md` 0.4）：

> "each pass ends with an ALL_GRAPHICS → ALL_COMMANDS memory barrier (#71), and each command buffer begins with ALL → ALL"

`PLAN.md` G2 分析：

> "Today: ALL_GRAPHICS → ALL_COMMANDS with MEMORY_READ|WRITE after each of ~70 passes. The next pass's vertex and binning work then waits for the previous pass's last fragment, so Adreno cannot overlap binning of pass N+1 with rendering of pass N."

处方：
> "Arm's guidance: ALL_GRAPHICS → FRAGMENT_SHADER (plus EARLY/LATE_FRAGMENT_TESTS and COLOR_ATTACHMENT_OUTPUT for attachment loads, and TRANSFER/COMPUTE for those users) between passes. Avoid backward fragment → vertex dependencies."
> "Change: use that dst mask, and add VERTEX_SHADER only when a draw's *vertex* shader samples a rendered image (reflection knows which stages read which bindings)."
> "Expected: −0.5 to −2 ms GPU"

参考：Arm 的 Vulkan 同步文章（`PLAN.md` 引用）
[developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/vulkan-synchronization-how-pipeline-barriers-map-to-mali-gpu-hardware](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/vulkan-synchronization-how-pipeline-barriers-map-to-mali-gpu-hardware)

（虽是 Mali，但 TBDR 的 binning/rendering 两阶段结构相同。）

### 3.4 原生格式：R11G11B10F 与 D24S8

当前把两者都做了 widening（`docs/nvn/ANDROID-PORT.md` 3.2 的 workaround 表）：

| Workaround | Mac 原因 | Android 建议 |
|---|---|---|
| D24/D24S8 → D32F（`nvn_format.cpp:8,40`） | M1 无 D24 | **Adreno 和 Mali 支持 `D24_UNORM_S8_UINT`：格式属性允许时就用它** |
| R11G11B10F → RGBA16F（`nvn_format.cpp:46`, `nvn_upload.cpp:22`, `nvn_pipeline.cpp:178,347,543`） | Metal 在 Maxwell 写 0 的地方写 NaN/Inf | **试原生 `B10G11R11_UFLOAT`（内存和带宽减半）** |

`PLAN.md` G3 估：
> "−0.5 to −1.5 ms GPU (half the bandwidth of the HDR chain; 4 instead of 5 B/px of depth), and 30-60 MB of GPU memory."

**对 2K 目标的意义**：2K 下像素数约为 720p 的 4 倍，**带宽是最先撞墙的资源**。R11G11B10F → RGBA16F 是 4B/px → 8B/px，**HDR 链带宽翻倍**；D24S8 → D32S8 是 4B/px → 5B/px。这两项在 720p 下是 "nice to have"，在 2K 下是**必需**。

### 3.5 `VK_KHR_push_descriptor` / dynamic uniform offsets

当前每 draw 分配 4 个 descriptor set（`ANDROID-PORT.md` 3.3）：

> "Every draw allocates 4 sets (`nvn_sets.cpp`; pool of 65,536 UBO and 16,384 SSBO in `nvn_vk.cpp:340`)."
> "**Per-draw descriptor cost.** Allocating 4 sets for each of 594 draws per frame (PERF-PLAN) is costly on mobile drivers. Use `VK_KHR_push_descriptor` (Adreno 6xx+, recent Mali) for sets 0-1, and cache set 2."

实测进展（`MEASUREMENTS.md`）：`sets1`（set cache）→ sets reused 36%；`dyn1`（dynamic uniform offsets）→ **sets reused 82%**，Phase 2 notes：

> "**Dynamic uniform offsets** (`ProgramLayout::dynamic`, nvn_pipeline.cpp) work on Adreno."

但 `VK_KHR_push_descriptor` **还没接**。Phase 0 已确认 Adreno 740 暴露了它：
> "Extensions present: `VK_KHR_push_descriptor`, `VK_KHR_descriptor_update_template`, `VK_EXT_robustness2`, `VK_EXT_extended_dynamic_state{,2}`, `VK_GOOGLE_display_timing`, `VK_QCOM_render_pass_store_ops`, `VK_EXT_load_store_op_none`."

缺失项（重要）：
> "Missing: `VK_EXT_graphics_pipeline_library`, `VK_EXT_descriptor_buffer`, `VK_EXT_extended_dynamic_state3`, `VK_KHR_present_wait`."

**`VK_KHR_present_wait` 缺失**影响帧调度：没有它就没法精确知道"上一帧何时真正上屏"，只能靠 FIFO 背压或 Swappy 的 display timing。**未核实**：Adreno 840（8 Elite Gen 5）的新驱动是否补上。

Qualcomm 的具体建议（`PLAN.md` R3 引用）：
> "Qualcomm's guide prefers UBOs over push constants on Adreno; this plan keeps UBOs."

即 **Adreno 上 UBO 优于 push constant**。`VK_KHR_push_descriptor` 值得做，但**不要**把 UBO 改成 push constant。

### 3.6 Pipeline cache 与预创建

已有磁盘 pipeline cache（`runtime/src/nvn/nvn_pipeline_cache.cpp`），路径 `<data root>/pipelines_<vendor>_<device>.bin`，校验 vendor/device/driver build。实测：

> "Adreno compiles a new pipeline in ~15-20 ms (149 pipelines took 3.1 s in one window of the first run), which makes the on-disk pipeline cache (DEBUG-2 #73) essential: warm, 0.5 ms each."
> —— `docs/nvn/android-perf/MEASUREMENTS.md`

`PLAN.md` P1 提议进一步**持久化 key 列表**（不只 driver blob）：

> "persist the key list (PipelineKey with shader hashes instead of pointers, plus the layout) next to the cache blob; at boot and at each scene load, create every known pipeline on a background thread into the same map; draws find them ready."

`VK_EXT_graphics_pipeline_library` 能缩短首次编译，但**Adreno 740 的专有驱动没有这个扩展**（Phase 0 确认 missing），**Turnip 有**（**未核实**：Turnip 扩展支持列表我没读到）。

**对 2K 的影响**：分辨率变化会让部分 pipeline 的 render pass / viewport 相关 state 变化，**可能触发重建**。若 yuzu 式的 `scale_rating >= 2` 两帧确认不实现，用户切分辨率时会出现几十秒的编译风暴。

### 3.7 `adrenotools` 与自定义驱动（已接）

已集成 libadrenotools（`runtime/src/platform_android.cpp:40-41`、`403-425`）：

```cpp
#if defined(NX_CUSTOM_DRIVER)
#include <adrenotools/driver.h>
...
void* h = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, libdir.c_str(), ...);
```

`include/adrenotools/driver.h` 明确的**硬约束**（读自原文件）：

> "IMPORTANT: The app MUST be packaged w/ useLegacyPackaging = true. The reason is that we install the hook in hookLibDir; but this folder won't be set correctly when useLegacyPackaging == false because Android expects to read the *.so directly from the apk"
> "hookLibDir: IMPORTANT: This path MUST point to whatever `getApplicationInfo().nativeLibraryDir` (Kotlin code) returns. Failing to do so will result in this function returning a valid ptr but then the hook will fail and either the original driver is loaded as fallback, or vkEnumeratePhysicalDevices returns 0 devices."
> "customDriverDir: IMPORTANT: This path MUST NOT be on sdcard/storage. i.e. use ANativeActivity::internalDataPath ... Otherwise you will get permission denied because dlopen() can't be done on *.so libraries that any random user or application can manipulate"

`include/adrenotools/priv.h` 的 feature flags：
```c
enum {
    ADRENOTOOLS_DRIVER_CUSTOM = 1 << 0,
    ADRENOTOOLS_DRIVER_FILE_REDIRECT = 1 << 1,
    ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT = 1 << 2,
};
```

README：
> "Currently supports loading custom GPU drivers such as turnip, enabling BCn textures and redirecting file operations to allow accessing shader dumps and modification of the driver config file without root."
> "Support: Android 9+, Arm64"

来源：[github.com/bylaws/libadrenotools](https://github.com/bylaws/libadrenotools)

**注意点**：README 明确说 libadrenotools **不是给终端用户装驱动的工具**（"This library is not for installing into Android and is not for end users"）。本项目 Launcher 有 "Import GPU driver" UI（`Launcher.java` 的 `driverDir()`），这是把开发者工具暴露给用户。

**关于 Turnip 与 UBWC 的更正（重要）**：我先前怀疑 Turnip 不支持 UBWC，**这是错的**。Mesa freedreno 文档里 `TU_DEBUG` 的调试选项包含 **`noubwc`**（与 `sysmem`、`gmem`、`nobin`、`forcebin`、`nolrz` 并列，用于"narrow down the hang cause"）——一个用来*关闭* UBWC 的调试开关，只有在 UBWC 默认开启时才存在（[docs.mesa3d.org/drivers/freedreno.html](https://docs.mesa3d.org/drivers/freedreno.html)）。所以 **Turnip 实现 UBWC**。Turnip 真正的代价是：**没有厂商扩展**（无 `VK_QCOM_*`、无 `VK_GOOGLE_display_timing` 保证、无 Adreno 专属快路径），以及整体性能通常低于专有 blob（未核实具体数字，没找到权威基准）。

**Turnip 的另一条实用限制**：它按 workload 自动在 SYSMEM 与 GMEM 之间调优（`TU_AUTOTUNE_ALGO` = `bandwidth`（默认）/ `profiled` / `profiled_imm` / `prefer_sysmem` / `prefer_gmem`），且文档明确"small RPs always use SYSMEM mode as they generally don't benefit from GMEM rendering due to the overhead of tiling"。**对本项目的含义：不要把一帧切成很多个很小的 render pass**——Turnip 会退回 sysmem，整个 tiling 收益就没了。这条与 3.2 的"减少 pass 数量"是同一个方向，但理由更硬。

**Turnip 的 LRZ 脆弱点**（同样适用于专有驱动的最佳实践）：Mesa 文档说明 "LRZ buffer could be formed only in one direction, changing depth comparison directions without disabling LRZ would lead to a malformed LRZ buffer"；a650 之前方向靠 CPU 跟踪，"it's impossible to reuse LRZ between render passes"；a7xx 才有双向 LRZ（[docs.mesa3d.org/drivers/freedreno/hw/lrz.html](https://docs.mesa3d.org/drivers/freedreno/hw/lrz.html)）。**实践规则：一个 render pass 内保持单一、一致的 depth `VkCompareOp`。**

### 3.8 KGSL turbo（已实现，默认关闭）

`runtime/src/perf_hint.cpp:74 gpu_turbo()`：

```cpp
// NX_GPU_TURBO=1 (an experiment, off by default): the Adreno kernel driver's power control off, so the GPU runs at its
// highest clock (thermal limits still apply). The idea is libadrenotools' adrenotools_set_turbo (BSD-2-Clause); this
// is written from the kernel's msm_kgsl UAPI: IOCTL_KGSL_SETPROPERTY (type 0x09, nr 0x32) with KGSL_PROP_PWRCTRL (0xE)
// and the value 0. ...
// The setting outlives the process (the GPU stayed at its top clock with the game closed), so every start sets it:
// off with NX_GPU_TURBO=1, back on (the driver's normal clock control) otherwise.
void gpu_turbo() {
    ...
    int fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    ...
    unsigned int enable = turbo ? 0 : 1;
    Property p{0xE, &enable, sizeof enable};
    int r = ioctl(fd, _IOW(0x09, 0x32, Property), &p);
```

**`KGSL_PROP_PWRCTRL = 0xE`，`IOCTL_KGSL_SETPROPERTY = _IOW(0x09, 0x32, ...)`，值 0 = 关闭 driver 电源控制（锁最高频）。** 实现直接对着内核 UAPI 写，不依赖 adrenotools。

**与 libadrenotools 的对照（已核实源码，`src/driver.cpp`）**——本项目的实现与上游语义一致，但上游有两个细节值得抄：

```c
void adrenotools_set_turbo(bool turbo) {
    uint32_t enable{turbo ? 0U : 1U};          // ← turbo 写 0，不是 1
    kgsl_device_getproperty prop{ .type = KGSL_PROP_PWRCTRL,
                                  .value = (void*)&enable, .sizebytes = sizeof(enable) };
    int kgslFd{open("/dev/kgsl-3d0", O_RDWR)};
    if (kgslFd < 0) return;                    // ← 静默失败，void 返回
    ioctl(kgslFd, IOCTL_KGSL_SETPROPERTY, &prop);
    close(kgslFd);                             // ← 每次调用都开/关 fd
}
```

- **`turbo` 写 `0`**（该属性是"power-control **enable**"，0 即"不限制"）。本项目的 `enable = turbo ? 0 : 1` 与此一致，✅。
- **返回 `void`、无错误上报、`/dev/kgsl-3d0` 打不开就静默 no-op**。本项目有 `log(...)`，比上游好。
- **上游每次调用都 `close(fd)`**；本项目在 turbo 模式下**故意不 close**（"The descriptor stays open: the driver ties the setting to it"）。这是本项目的**有意偏离**，注释也写明了理由。**未核实**：设置到底是否绑定到 fd 的生命周期——两种说法都无法从 UAPI 头文件直接证实。**建议：按"绑定 fd"实现（保守），但在 `gpu_turbo()` 里加一条注释说明这是经验观察而非文档保证。**

**权限现实**：`/dev/kgsl-3d0` 在量产机上是 root/vendor-only，普通 app 会拿到 `EACCES`——这正是上游把它做成 `void` 返回的原因。**所以 `NX_GPU_TURBO` 在普通用户设备上很可能根本无效**，这也进一步支持 4.4 ㉑ 的"降级为纯诊断"结论。

**更现代的替代**：`VK_QCOM_queue_perf_hint`（[提案](https://docs.vulkan.org/features/latest/features/proposals/VK_QCOM_queue_perf_hint.html)）用 `vkQueueSetPerfHintQCOM(queue, &VkPerfHintInfoQCOM{type, scale})` 取代 ioctl hack，`VkPerfHintTypeQCOM ∈ {DEFAULT, FREQUENCY_MIN, FREQUENCY_MAX, FREQUENCY_SCALED}`。hint 是归一化的（不是绝对频率）、在 queue 生命周期内持续、并在多个活跃 queue 之间排序（`FREQUENCY_MAX` 最高）。**这是"强制最高频"的官方正路**，且不需要 root。**未核实**：Adreno 840 的驱动是否暴露该扩展。

**重要警告（代码注释已记录）**：
> "The setting outlives the process (the GPU stayed at its top clock with the game closed)"

即设置**会泄漏到进程之外**，影响整机。本项目每次启动都显式设置（开或关），对的。但 `if (!turbo) close(fd);` —— **turbo 模式下 fd 保持打开**（"The descriptor stays open: the driver ties the setting to it"），进程退出时内核关闭 fd，设置可能不会自动恢复。**未核实**：进程退出后 KGSL 是否恢复 PWRCTRL。

**判断**：`NX_GPU_TURBO` 应**只作诊断工具**，不作默认。OnePlus Pad 3 Pro 的 Adreno 840 频率调度已相当激进，强制锁最高频只会更快撞热墙。正确做法是 **ADPF 让系统自己判断**（2.4）。

### 3.9 VRS / fragment density map

`VK_EXT_fragment_density_map`（及 `VK_EXT_fragment_density_map2`）允许按屏幕区域指定不同 fragment 密度，即"中心全分辨率、边缘半分辨率"。

**对 P5R 的适用性评估（我的判断，非引用）**：P5R 是**UI 密集**的回合制 RPG——对话框、菜单、战斗指令面板占据大量屏幕面积且需要**清晰**。VRS 会让边缘文字模糊。**不建议**用于 2K 目标。

**更合适的场景**：3D 场景（非 UI pass）的边缘。但本项目当前**没有 pass 分类**能力（`nvn_draw.cpp` 有 `g_hud_target`，但那是 `NX_HUD_1080` 的倍率逻辑，不是密度图）。

`VK_QCOM_fragment_density_map_offset` 是 Qualcomm 变体，**未核实**其可用性与语义。

- [registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_fragment_density_map.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_fragment_density_map.html)
- [registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_fragment_density_map2.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_fragment_density_map2.html)

**结论：优先级最低。** 先把 UBWC + GMEM + 原生格式做完。

### 3.10 其它 Qualcomm 扩展清单

| 扩展 | 作用 | 对本项目 | 来源 |
|---|---|---|---|
| `VK_QCOM_render_pass_store_ops` | 允许 `STORE_OP_NONE`，比 `DONT_CARE` 更强 | **值得做**（G5 加强版），Adreno 740 已确认存在 | [registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_render_pass_store_ops.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_render_pass_store_ops.html) |
| `VK_QCOM_render_pass_transform` | driver 在 pass 内做旋转，省一次全屏拷贝 | **值得评估**：本项目有 `preRotate` swapchain + present pass，可能省一次 blit | [registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_render_pass_transform.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_render_pass_transform.html) |
| `VK_QCOM_rotated_copy_commands` | `vkCmdBlitImage` 带旋转 | 同上，适用 `nvn_copy.cpp` | [registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_rotated_copy_commands.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_rotated_copy_commands.html) |
| `VK_QCOM_tile_shading` | 手动控制 tile 调度 | **未核实**，太新 | [registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_tile_shading.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_tile_shading.html) |
| `VK_QCOM_multiview_per_view_viewports` | 每 view 独立 viewport | P5R 无 multiview，不需要 | [registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_multiview_per_view_viewports.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_multiview_per_view_viewports.html) |
| `VK_QCOM_image_processing` | 硬件图像处理（box/gaussian） | **可评估**：P5R bloom 链是多次 downsample | [registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_image_processing.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_image_processing.html) |
| `VK_QCOM_ycbcr_degamma` | YCbCr 反 gamma | P5R 视频播放（`hle_movie.cpp`）可能相关 | [registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_ycbcr_degamma.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_ycbcr_degamma.html) |
| `VK_EXT_attachment_feedback_loop_layout` | pass 内采样自己的 attachment | **值得做**：`PLAN.md` G4 提到用它替代 GENERAL | [docs.vulkan.org/features/latest/features/proposals/VK_EXT_attachment_feedback_loop_layout.html](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_attachment_feedback_loop_layout.html) |
| `VK_EXT_load_store_op_none` | `LOAD_OP_NONE` / `STORE_OP_NONE`（跨厂商） | Adreno 740 已确认存在；比 QCOM 版通用 | [registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_load_store_op_none.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_load_store_op_none.html) |

### 3.11 可借鉴点小结（→ 本项目）

| 项 | 优先级 | 依据 |
|---|---|---|
| UBWC：storage usage 只在需要时加 | **P0**（已做一半） | 实测 +3.5% fps（Adreno 610） |
| 原生 R11G11B10F / D24S8 | **P0**（2K 必需） | `PLAN.md` G3 估 −0.5~−1.5ms + 省 30-60MB |
| `storeOp = DONT_CARE` 用于死 depth | **P1** | `PLAN.md` G5 估 −0.3~−1ms |
| `VK_EXT_load_store_op_none` / `VK_QCOM_render_pass_store_ops` | **P1** | Adreno 740 已确认支持 |
| 精细化 inter-pass barrier（不用 ALL_COMMANDS） | **P1** | `PLAN.md` G2 估 −0.5~−2ms |
| `VK_KHR_push_descriptor` | **P2** | Adreno 740 已确认支持；但保持 UBO 不用 push constant |
| Pipeline 预创建（持久化 key 列表） | **P2** | 15-20ms/个 × 149 个 = 3.1s |
| `VK_EXT_attachment_feedback_loop_layout` 替代 GENERAL | **P2** | `PLAN.md` G4 |
| VRS / fragment density map | **P3（不建议）** | UI 密集，会糊字 |
| KGSL turbo | **仅诊断** | 会泄漏到进程外 |

---

## 四、本项目的具体可落地点

> 每条：**改动文件 → 改动内容 → 预期收益 / 风险**。所有路径均已在本仓库核对存在。行号基于 HEAD `5e05e70`。
>
> **⚠️ 实现状态提示（写作期间仓库有并发改动，落地前必须以工作树为准重新核对）。**
> 我核对到的当前工作树状态：
> - **已落地**：`perf_profile.{h,cpp}` + `perf_thermal.{h,cpp}` 新增；`hud_quarters()` 增加 `NX_HUD_RES=1440` 档（`nvn_image.cpp:99-110`，返回 8，即 2x = 2560x1440）；`display_period_ns()` 改为读 `nx::perf::profile()` 的 `vsync_hz`/`fps`（`hle_vi.cpp:26`）；**⑪ 已落地**——`perf_hint.cpp:70-74` 新增 `perf_target_ns()`，返回 `1e9 / pf.fps`，不再是写死的 16.67/33.3ms。
> - **仍未修**：**⑦ 的 compatibility 标志仍是 `1`（FIXED_SOURCE）**——`platform_android.cpp:478` 和 `:483` 两处都是 `api.set_always(w, hz, 1, 1)`，注释 `:477` 也仍写 "COMPATIBILITY_FIXED_SOURCE"。**这是本文最高性价比的单点修复，建议优先处理。**
> - 另：`platform_android.cpp` 的 `request_frame_rate()` 已重构成 `api.set_always(...)` 形式（原 L528-543），行号已变。
>
> 因此下文的**行号**与"当前无此能力"的判断可能已被部分推翻；但**分析与风险判断**（尤其 ⑭ 的 90fps 1.5 倍速 bug、以及 ⑦ 的语义错误）仍然成立。

### 4.1 分辨率与缩放（支撑 "2K" 目标）

#### ① 解除 1080p 硬封顶，改为整数倍率对

- **改动文件**：`runtime/src/platform_android.cpp`（`window_size()`，L323 起）
- **改动内容**：当前逻辑
  ```cpp
  float scale = rs.empty() ? default_render_scale() : std::strtof(rs.c_str(), nullptr);
  scale = std::clamp(scale, 0.25f, 1.0f);
  scale = std::min(scale, 1080.0f / float(sh));   // ← 硬封顶
  ```
  改为 yuzu 式的 `up_scale` / `down_shift` 整数对，并新增 "2K" 档（`2560x1440`）。`parse_target_h()` 已有 `%ux%u` 解析，可直接接受 `2560x1440`，但被 `1080.0f / sh` 挡掉。同时 `RESOLUTION` 选项（`Launcher.java` 的 `options.add(new Option("RESOLUTION", ...))`）要加 "1440p"/"2K" 项。
- **预期收益**：这是 2K 目标的**唯一入口**。当前代码在数学上不可能渲染超过 1080p。
- **风险**：`default_render_scale()`（L363）对 Adreno 7xx/8xx 返回 1.0，`scale = min(1.0, 1080/sh)` 在 3.4K 屏（`sh` ≈ 1600+）上得到 ~0.67 → 实际渲染约 1080p。要上 2K 必须**同时**改 `default_render_scale()` 的 tier 表。

#### ② 把 `NX_HUD_1080` 的倍率机制泛化为任意倍率

- **改动文件**：`runtime/src/nvn/nvn_vk.h`（`hud_quarters()`/`hud_up()`/`hud_up_end()`/`hud_scale()`，L49-53）、`runtime/src/nvn/nvn_image.cpp`（`hud_quarters()` 实现，L99-110；`hud_1080_texture()`）
- **改动内容**：`hud_quarters()` 当前返回 8（1440p）/ 6（1080p）/ 5（900p），只支持 2x / 1.5x / 1.25x 三个离散值（`hud_scale() = quarters / 4.0f`）。改为通用 `up_scale`/`down_shift` 对，`hud_up(v) = (v * up) >> shift`。
- **预期收益**：`NX_HUD_1080` 已**验证过"只放大 UI/HUD 目标、3D 保持原尺寸"这条路可行**（`ANDROID-PORT.md` 记录 Adreno 610 上 "Resolución 1080p en el título 3D: 12,6 → 10,15 fps (−19%)"）。泛化后可把 UI 放到 2K 而 3D 留在 1080p——"2K 观感 + 可接受 GPU 成本"的最佳折中。
- **风险**：`nvn_draw.cpp` 的 `const float k = hud_scale();` 和 `nvn_sets.cpp` 的 `const float scale = g_hud_target ? hud_scale() : 1.0f;` 都要跟着改；`nvn_mask_clear.cpp`、`nvn_copy.cpp`、`nvn_clears.cpp` 里所有 `hud_up*` 调用点需审计。

#### ③ 在 support buffer 里真正填非 1.0 的 `render_scale`

- **改动文件**：`runtime/src/nvn/nvn_shaders.cpp`（`support_buffer_set_viewport()`，L126；L127 写 `sb.render_scale[0][0]`）、`runtime/src/nvn/nvn_shaders.h`（`render_scale[73][4]`，L190；`frag_scale_count`，L189）
- **改动内容**：基础设施**已完整**——73 个 scale 槽（`[0]` render target、`[1..64]` textures、`[65..72]` images）+ `frag_scale_count` + `offsetof` 静态断言（`nvn_shaders.h:196-197`）。当前只写 `render_scale[0][0]`，其余保持 1.0。要做的是：当一个 image 被缩放时，把它在 support buffer 里的槽填成实际倍率，并更新 `frag_scale_count`。
- **预期收益**：**① 和 ② 的必要前提**。没有它，缩放的 image 在 shader 里 `textureSize()` / `texelFetch()` 会读到错的尺寸，画面直接错。基础设施零成本，只是没人填。
- **风险**：`render_scale` 是 `float[73][4]`，**只有 x 分量被用**（注释 "x only"）。填错槽位会导致难以定位的画质问题。`nvn_spirv_patch.cpp` 里有 SPIR-V 补丁逻辑读这些槽，需同步审计。

#### ④ 引入 image 级 `rescaleable` 判定（含双阈值）

- **改动文件**：`runtime/src/nvn/nvn_image.cpp`（image 创建路径，`StorageKey`/`g_live_images` 附近）
- **改动内容**：照抄 yuzu 的 `ImageInfo` 判定：`rescaleable &= !pitch_linear && block.depth == 0 && levels == 1`；`rescaleable &= height > 288 || format 非 ColorTexture`；`downscaleable = height > 512`。
- **预期收益**：避免把小目标（bloom downsample 链、shadow map 小级、LUT）放大——这些放大只吃带宽不出画质，且会破坏 `texelFetch` 的整数语义。
- **风险**：P5R 的 BNTX 纹理有 12 级 mip 链（`TILING.md`），`levels == 1` 会把它们全部排除在缩放外——**这是对的**，但要确认没有关键的大目标恰好是多 mip。

#### ⑤ 别名闭包：缩放决策要沿 storage 传递

- **改动文件**：`runtime/src/nvn/nvn_image.cpp`（`StorageKey` 结构体；`g_live_images`）
- **改动内容**：`StorageKey` 当前是 `{memory, format, width, height, depth, levels, target, samples, swizzle}`。加一个 `scaled` 位，或在 `GpuImage` 上加 `rescaled` 标志（`nvn_vk.h` 的 `GpuImage` 已有 `uw`/`hud`/`storage`/`mutable_format` 等布尔位，加一个是自然扩展）。缩放判定时沿 `g_live_images` 做闭包（带环检测）。
- **预期收益**：避免"同一 storage 的两个 view 一个缩放一个不缩放"导致的画面撕裂。这是 yuzu `ImageCanRescale` 的核心目的。
- **风险**：`g_live_images` 是 `std::map<StorageKey, std::weak_ptr<GpuImage>>`，闭包遍历要在 GPU 锁下做（`nvn_vk.h` 开头："Everything here runs under the GPU lock"），注意锁序。

#### ⑥ 缩放状态的两帧确认 + 整组降级

- **改动文件**：`runtime/src/nvn/nvn_image.cpp` 或新的 `nvn_scale.cpp`
- **改动内容**：加 `scale_rating` / `scale_tick` 字段（`GpuImage` 里），照抄 `RescaleRenderTargets()` 的 `scale_rating >= 2` 逻辑；任一 RT 不可缩放时整组 `ScaleDown`。
- **预期收益**：避免 Adreno 上 `vkCreateImage` 重建风暴。本项目已实测 Adreno 上 image 重建昂贵（UBWC 元数据）。
- **风险**：两帧延迟意味着切换分辨率后画面会在 2 帧内不一致——只在**运行时动态改分辨率**时可见，静态设置不受影响。

### 4.2 帧率与帧调度（支撑 "90fps" 目标）

#### ⑦ `setFrameRate` 的 compatibility 改成 DEFAULT，目标值参数化

- **改动文件**：`runtime/src/platform_android.cpp`（`request_frame_rate()`，L528 起）
- **改动内容**：
  - L541 的 `1 /* COMPATIBILITY_FIXED_SOURCE */` → `0 /* ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT */`。AOSP 头文件明确写 "This value should be used when displaying game content"（`FIXED_SOURCE` 是给视频的）。
  - L535 的 `const float hz = e ? float(std::atof(e)) : 60.0f;` 默认值改为跟随实际目标帧率（90 或 60），而不是硬编码 60。
- **预期收益**：**明确的 bug 修复**。传 `FIXED_SOURCE` 会让系统调度器认为"应用会用 pulldown 适配"，从而**降低**把面板切到请求刷新率的优先级——正好与想要的相反。
- **风险**：极低。`DEFAULT` 是游戏内容的推荐值。

#### ⑧ 验证刷新率请求是否生效（可观测性）

- **改动文件**：`runtime/src/platform_android.cpp`（`request_frame_rate()` 之后）、`runtime/src/frame_stats.cpp`
- **改动内容**：用 `AChoreographer_registerRefreshRateCallback` 注册刷新率变化回调，把实际面板刷新率 log 出来。Android 官方文档明确要求这样才能知道请求是否被采纳（[developer.android.com/media/optimize/performance/frame-rate](https://developer.android.com/media/optimize/performance/frame-rate) ）。
- **预期收益**：OnePlus Pad 3 Pro 的 144Hz 面板可能有自适应刷新率策略，**请求 90Hz 很可能被静默忽略**。没有这个回调，我们会一直在错误假设下调优。
- **风险**：低。只是观测。

#### ⑨ 90fps 时改用 FIFO present mode

- **改动文件**：`runtime/src/nvn/nvn_vk.cpp`（present mode 选择，L437 起）
- **改动内容**：当前无条件优先 MAILBOX（除非 `NX_FIFO`）。改为：当目标帧率是面板刷新率的整数分数（90 在 90Hz 模式下是 1/1）时用 FIFO；只有非整数比时才用 MAILBOX。或更简单：**目标 ≥ 60fps 时一律 FIFO + `setFrameRate`**（`PLAN.md` P2 方案）。
- **预期收益**：FIFO 的背压给出均匀的 11.11ms 间隔。MAILBOX 在 144Hz + 90fps 下会有 89ms 周期 judder（见 2.1）。
- **风险**：FIFO 会把迟到一帧变成 22.2ms。在 90fps 这种紧张预算下，一次超时就是可见卡顿。需配合 ⑩。

#### ⑩ 用 Choreographer 驱动 `hle_vi` 的 vsync，而不是自由运行 timer

- **改动文件**：`runtime/src/hle_vi.cpp`（`vsync_thread()`；`display_period_ns()`，L26）
- **改动内容**：当前 `vsync_thread()` 是 `next += display_period_ns(); sleep_ns(...)` 的自由运行 timer，与面板刷新**完全独立**（两个时钟漂移）。改为 `AChoreographer_postVsyncCallback`（API 33+；更早用 `postFrameCallback64`）驱动。
- **预期收益**：游戏时钟和面板时钟合一，消除漂移。`PLAN.md` P2 估计 "present intervals 16.7 ± 1 ms in the Phase 0 histogram"。
- **风险**：中。vsync 是游戏主循环的节拍源，改回调驱动后**回调延迟**会影响游戏感知的帧时长。`game.cpp` 的 `paced_target()` 依赖 `fw[0x118]` 的精确 tick 值，需重新标定。`NX_DISPLAY_HZ` 这个开关会失去意义。

#### ⑪ ADPF 目标时长支持 90fps

- **改动文件**：`runtime/src/perf_hint.cpp`（`perf_hint_frame()`，L107 起，特别 L113-116）
- **改动内容**：
  ```cpp
  const char* cap = std::getenv("NX_FPS_CAP");
  int64_t target = cap && std::atoi(cap) > 0 && std::atoi(cap) <= 30 ? 33'333'333 : 16'666'667;
  ```
  改为从实际目标帧率算：`target = 1e9 / fps`。90fps → `11'111'111`。
- **预期收益**：**90fps 目标的必要条件**。ADPF 的 target 告诉系统"一帧应该用多久"，写死 16.67ms 会让系统按 60fps 调 DVFS，90fps 下永远调不够。
- **风险**：低。但若 target 设成 11.11ms 而实际达不到，ADPF 会**持续推高频率**直到热墙，可能导致比 60fps 更差的长期表现。建议配合 `AThermal_getThermalHeadroom`（`frame_stats.cpp:121` 已有）做闭环。

#### ⑫ ADPF 用 `setThreads` 而非重建 session

- **改动文件**：`runtime/src/perf_hint.cpp`（`perf_hint_thread()`、`perf_hint_frame()`，L107 起）
- **改动内容**：当前 `g_dirty` 时 `a.close(g_session); a.create(...)` 重建。改为动态加载 `APerformanceHint_setThreads`，有 `g_dirty` 时调它。
- **预期收益**：消除场景切换（新线程加入）时的 session 重建抖动。NDK 文档明确 `setThreads` 就是为此设计（[developer.android.com/ndk/reference/group/a-performance-hint](https://developer.android.com/ndk/reference/group/a-performance-hint) ）。
- **风险**：低。`setThreads` 是 API 34+，需 `dlsym` 探测（现有代码已是 dlsym 模式）。

#### ⑬ 用 `notifyWorkloadSpike` 预告 pipeline 编译尖峰

- **改动文件**：`runtime/src/perf_hint.cpp`（新增函数）、`runtime/src/nvn/nvn_pipeline_cache.cpp`（新 pipeline 创建处）
- **改动内容**：在场景加载、以及 `vkCreateGraphicsPipelines` 调用前，调 `APerformanceHint_notifyWorkloadSpike(session, /*cpu=*/true, /*gpu=*/true, "pipeline-compile")`。
- **预期收益**：实测 Adreno 上 "149 pipelines took 3.1 s in one window of the first run"。提前预告能让 DVFS 在尖峰**开始前**升频。可能把 3.1s 缩短 20-40%（**未核实**，只是 DVFS 响应常识）。
- **风险**：低。但 `notifyWorkloadSpike` 是较新 API（NDK 文档标注 API 36），需 `dlsym` 探测。

#### ⑭ 修正 90fps 下的 present interval 换算（**正确性 bug**）

- **改动文件**：`games/p5r/game.cpp`（`cap_fps()` L116；interval 换算 L354；`paced_target()` L156）
- **改动内容**：
  - `cap_fps()`：`env_int("NX_FPS_CAP", 30) >= 60 ? 60 : 30` → 支持 90。
  - L354：`const int interval = fps ? std::clamp(60 / fps, 1, 4) : 60 / cap_fps();` —— **`fps=90` 时 `60/90 = 0`，被 clamp 到 1**。游戏认为 interval=1 即 60fps，但实际跑 90fps → **游戏以 1.5 倍速运行**。必须改成显式映射（或引入"渲染 90 / 逻辑 60"双时钟）。
  - `paced_target()`：`cap/2` 和 `cap/3` 在 cap=90 时是 45 和 30。45 不是 P5R 原生步长（原生 30/60）。
- **预期收益**：**90fps 路径上的头号正确性风险**。不修就是 1.5 倍速。
- **风险**：**极高**。本项目注释（`game.cpp:67-79`）记录了两次因 target 非标准值导致战斗逻辑死锁的事故。`fps-unlock-1710.md` 也用 Wonder 验证了同一类问题。**建议：90fps 不在第一阶段做。先做 2K@60，把"渲染 90 / 逻辑 60"框架设计清楚再动。**

#### ⑮ present 间隔直方图改成 90fps 友好

- **改动文件**：`runtime/src/frame_stats.cpp`（`frame_presented()`，L228；直方图在 L238）
- **改动内容**：L238 的桶是 `<14, 14-19, 19-25, 25-31, 31-36, 36-50` ms（60/30fps 视角）。90fps 周期 11.11ms 全落进 `<14`。改为按**相对目标周期的偏差**分桶（±5%/±10%/±20%/±50%），或加 90fps 专用桶（8/10/11.5/13/16/22/33）。
- **预期收益**：没有这个，90fps 的调优**完全瞎飞**——直方图看不出任何东西。
- **风险**：无。纯观测。但会改变现有日志格式，`tools/` 下的解析脚本要跟着改。

### 4.3 Adreno 专项（支撑 2K 的带宽预算）

#### ⑯ 原生 R11G11B10F（`B10G11R11_UFLOAT_PACK32`）

- **改动文件**：`runtime/src/nvn/nvn_format.cpp`（L46 附近的 widening）、`runtime/src/nvn/nvn_upload.cpp`（L22）、`runtime/src/nvn/nvn_pipeline.cpp`（L178、L347、L543）、`runtime/src/nvn/nvn_caps.cpp`
- **改动内容**：在设备报告 `B10G11R11_UFLOAT_PACK32` 支持 COLOR_ATTACHMENT + BLEND 时用它，保留现有 fragment output clamp（`ufloat_colors`，防负值/NaN）。
- **预期收益**：HDR 链带宽**减半**（8B/px → 4B/px）。`PLAN.md` G3 估 `−0.5 to −1.5 ms GPU` + 省 30-60MB。**在 2K 下这个收益按面积放大 4 倍**。
- **风险**：bloom/blur 的 NaN 回归。`ANDROID-PORT.md` 3.2 已记录 Mac 上 Metal 的 NaN/Inf 问题。需要 `NX_R11G11B10_EMULATE=1` 作回退开关，并做 A/B。

#### ⑰ 原生 D24S8（`D24_UNORM_S8_UINT`）

- **改动文件**：`runtime/src/nvn/nvn_format.cpp`（L8、L40）、`runtime/src/nvn/nvn_raster.cpp`（`NX_EQUAL_SLACK` / depth bias，L123-141）
- **改动内容**：Adreno 支持 `D24_UNORM_S8_UINT`（M1 不支持，这是当初 widening 的原因）。在 `vkGetPhysicalDeviceImageFormatProperties` 允许时用它。
- **预期收益**：depth 带宽 5B/px → 4B/px（约 −20%），且**depth precision 变成精确 24 位**——这会消除 `nvn_raster.cpp:135` 注释里 "yuzu rescales them when it emulates D24 the same way" 那个 workaround 的需求。
- **风险**：`NX_EQUAL_SLACK` 需重新调（UNORM depth 和 float depth 的 bias 单位不同）。`PLAN.md` G3 明确提到："Re-tune `NX_EQUAL_SLACK`: depth-bias units differ between UNORM and float depth." `DEBUG-2.md #60` 的 EQUAL depth slack 是已知敏感区域。

#### ⑱ 接入 `VK_EXT_load_store_op_none` / `VK_QCOM_render_pass_store_ops`

- **改动文件**：`runtime/src/nvn/nvn_vk.cpp`（设备创建，`dexts` 列表，L193-240）、`runtime/src/nvn/nvn_renderpass.cpp`（`storeOp`，L56、L71；`kNoStore` 在 L26）
- **改动内容**：在 `dexts` 里探测并启用这两个扩展；在 `nvn_renderpass.cpp` 里，当 pass 的 attachment 之后不被读时用 `VK_ATTACHMENT_STORE_OP_NONE`（比 `DONT_CARE` 更强）。
- **预期收益**：Adreno 740 已确认支持。每个"死 store"省一次全屏 GMEM→system memory 写入。本项目有 70 个 pass/帧，12 个是 clear-only。
- **风险**：中。`STORE_OP_NONE` 之后读该 attachment 是 UB。需要 `PLAN.md` G5 那样的 per-submit lookahead 来证明"之后没人读"。

#### ⑲ 精细化 inter-pass barrier

- **改动文件**：`runtime/src/nvn/nvn_draw.cpp`（`pass_end_only`，pass 结束处）
- **改动内容**：当前每 pass 结束是 `ALL_GRAPHICS → ALL_COMMANDS | MEMORY_READ|WRITE`。改为 `ALL_GRAPHICS → FRAGMENT_SHADER | EARLY_FRAGMENT_TESTS | LATE_FRAGMENT_TESTS | COLOR_ATTACHMENT_OUTPUT`（+ TRANSFER/COMPUTE 按需），只在**顶点着色器真的采样了被渲染的 image** 时加 `VERTEX_SHADER`。
- **预期收益**：`PLAN.md` G2 估 `−0.5 to −2 ms GPU`。允许 Adreno 把 pass N+1 的 binning 与 pass N 的 rendering 重叠。
- **风险**：`DEBUG-2.md #71` 记录过 NVIDIA 上的 tile 类 bug。保留 `NX_PASS_BARRIER_FULL=1` 作回退开关。

#### ⑳ `VK_KHR_push_descriptor` 接上（set 0/1）

- **改动文件**：`runtime/src/nvn/nvn_sets.cpp`（`build_sets`/`bind_sets`）、`runtime/src/nvn/nvn_vk.cpp`（`dexts` 探测）、`runtime/src/nvn/nvn_pipeline.cpp`（`ProgramLayout`）
- **改动内容**：set 0/1 用 push descriptor（配合 `VK_KHR_descriptor_update_template` 减少 `vkCmdPushDescriptorSet` 的 CPU 成本），set 2 保持缓存（已做，`dyn1` 实测 82% 复用）。
- **预期收益**：省掉每 draw 2 次 `vkAllocateDescriptorSets` + `vkUpdateDescriptorSets`。profile 显示 `build_sets/bind_sets` 占 replay 的 3.4%（`PLAN.md` 0.3）。R3 估 `−1 to −2 ms`。
- **风险**：中。Adreno 740 上已确认存在，但**push descriptor 的写入是即时的**，如果 set 内容在同一 draw 内多次变化会有性能问题。另 `PLAN.md` 提醒："Qualcomm's guide prefers UBOs over push constants on Adreno" —— **push descriptor 里放的仍应是 UBO，不是 push constant**。

### 4.4 其它

#### ㉑ `NX_GPU_TURBO` 降级为纯诊断，并从 Launcher 移除

- **改动文件**：`runtime/src/perf_hint.cpp`（`gpu_turbo()`，L74）、`games/p5r/java/io/github/p5rrecomp/Launcher.java`（如有对应 UI 项）
- **改动内容**：保留实现但**不要**在 Launcher 暴露。注释已警告 "The setting outlives the process (the GPU stayed at its top clock with the game closed)"——会污染整机。
- **预期收益**：避免用户误开导致热失控。正确做法是 ADPF（⑪⑫⑬）。
- **风险**：无（只是不给入口）。

#### ㉒ 2K 下的内存预算检查

- **改动文件**：`runtime/src/memguard_android.cpp`、`runtime/src/nvn/nvn_image.cpp`（缩放后重算 size）
- **改动内容**：照 yuzu 的 `GetScaledImageSizeBytes`（面积按 `up_scale² >> down_shift²`）把缩放后的 image 计入 `total_used_memory`；在 `memguard_android.cpp` 里对 2K 档设置更高预算上限（16GB 设备可用 ~6-8GB）。
- **预期收益**：避免 2K 下 OOM 被 LMK 杀掉。实测 720p 下 "app total PSS 3.29 GB, Graphics 0.97 GB"，2K 下 Graphics 可能到 2.5-4GB。
- **风险**：低。但 `MEMORY.md` 记录过 runtime 自己的 guard 只报 0.5-1.2GB（"it does not see most of it"），说明现有预算计算本身有缺口，需先修。

---

## 五、参考链接

### 模拟器源码（实际读到的）

- Citron `ResolutionScalingInfo`：[github.com/citron-neo/emulator/blob/main/src/common/settings.h](https://github.com/citron-neo/emulator/blob/main/src/common/settings.h)
- Citron `TranslateResolutionInfo` / `UpdateRescalingInfo`：[github.com/citron-neo/emulator/blob/main/src/common/settings.cpp](https://github.com/citron-neo/emulator/blob/main/src/common/settings.cpp)
- Citron `ResolutionSetup` / `ScalingFilter` / `VSyncMode` 枚举：[github.com/citron-neo/emulator/blob/main/src/common/settings_enums.h](https://github.com/citron-neo/emulator/blob/main/src/common/settings_enums.h)
- Citron `ImageInfo` / `rescaleable` / `RescaleHeightThreshold`：[github.com/citron-neo/emulator/blob/main/src/video_core/texture_cache/image_info.cpp](https://github.com/citron-neo/emulator/blob/main/src/video_core/texture_cache/image_info.cpp)
- Citron `ImageCanRescale` / `RescaleRenderTargets` / `ScaleUp` / `ScaleDown`：[github.com/citron-neo/emulator/blob/main/src/video_core/texture_cache/texture_cache.h](https://github.com/citron-neo/emulator/blob/main/src/video_core/texture_cache/texture_cache.h)
- Citron `ImageFlagBits`（`Rescaled` / `CheckingRescalable` / `IsRescalable`）：[github.com/citron-neo/emulator/blob/main/src/video_core/texture_cache/image_base.h](https://github.com/citron-neo/emulator/blob/main/src/video_core/texture_cache/image_base.h)
- Citron `BlitScreen::SetWindowAdaptPass`（低 GPU 档降级 Bilinear）：[github.com/citron-neo/emulator/blob/main/src/video_core/renderer_vulkan/vk_blit_screen.cpp](https://github.com/citron-neo/emulator/blob/main/src/video_core/renderer_vulkan/vk_blit_screen.cpp)
- Citron `filters.h`（`MakeBilinear` / FSR 等）：[github.com/citron-neo/emulator/blob/main/src/video_core/renderer_vulkan/present/filters.h](https://github.com/citron-neo/emulator/blob/main/src/video_core/renderer_vulkan/present/filters.h)
- Citron 仓库根：[github.com/citron-neo/emulator](https://github.com/citron-neo/emulator)
- Skyline README（源码已删除的说明）：[github.com/skyline-emu/skyline](https://github.com/skyline-emu/skyline)
- Strato 仓库：[github.com/strato-emu/strato](https://github.com/strato-emu/strato)

### libadrenotools

- 仓库与 README（"Android 9+ / Arm64"、用途边界）：[github.com/bylaws/libadrenotools](https://github.com/bylaws/libadrenotools)
- `include/adrenotools/driver.h`（`adrenotools_open_libvulkan` 的硬约束）：[github.com/bylaws/libadrenotools/blob/master/include/adrenotools/driver.h](https://github.com/bylaws/libadrenotools/blob/master/include/adrenotools/driver.h)
- `include/adrenotools/priv.h`（`ADRENOTOOLS_DRIVER_CUSTOM` / `FILE_REDIRECT` / `GPU_MAPPING_IMPORT`）：[github.com/bylaws/libadrenotools/blob/master/include/adrenotools/priv.h](https://github.com/bylaws/libadrenotools/blob/master/include/adrenotools/priv.h)
- AdrenoToolsTest 示例工程：[github.com/darksylinc/AdrenoToolsTest](https://github.com/darksylinc/AdrenoToolsTest)

### Android 平台 API

- 帧率 API 总览（`setFrameRate` 只是请求、需注册刷新率回调、传精确帧率）：[developer.android.com/media/optimize/performance/frame-rate](https://developer.android.com/media/optimize/performance/frame-rate)
- `ANativeWindow_setFrameRate` / `ANativeWindow_FrameRateCompatibility` / `ANativeWindow_ChangeFrameRateStrategy` 头文件：[android.googlesource.com/platform/frameworks/native/+/refs/heads/main/libs/nativewindow/include/android/native_window.h](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/main/libs/nativewindow/include/android/native_window.h)
- ADPF 总览：[developer.android.com/games/optimize/adpf](https://developer.android.com/games/optimize/adpf)
- ADPF NDK 参考（`APerformanceHint_*` 全函数表）：[developer.android.com/ndk/reference/group/a-performance-hint](https://developer.android.com/ndk/reference/group/a-performance-hint)
- Swappy Vulkan 集成（`SwappyVk_initAndGetRefreshCycleDuration` / `setSwapIntervalNS` / `queuePresent`）：[developer.android.com/games/sdk/frame-pacing/vulkan/add-functions](https://developer.android.com/games/sdk/frame-pacing/vulkan/add-functions)
- 刷新率切换优化：[developer.android.com/games/optimize/display-refresh-rate-change](https://developer.android.com/games/optimize/display-refresh-rate-change)

### Vulkan / Qualcomm 扩展与最佳实践

- Qualcomm Snapdragon Game Developer Guide — Adreno GPU on Mobile 最佳实践（UBWC、MUTABLE_FORMAT、load/store、barrier、UBO vs push constant）：[docs.qualcomm.com/doc/80-78185-2/topic/mobile_best_practices.html](https://docs.qualcomm.com/doc/80-78185-2/topic/mobile_best_practices.html)
- Qualcomm — Understanding and resolving GMEM loads：[docs.qualcomm.com/doc/80-78185-2/topic/gmem_loads.html](https://docs.qualcomm.com/doc/80-78185-2/topic/gmem_loads.html)
- Khronos — tile-based rendering best practices：[github.khronos.org/Vulkan-Site/guide/latest/tile_based_rendering_best_practices.html](https://github.khronos.org/Vulkan-Site/guide/latest/tile_based_rendering_best_practices.html)
- Khronos Vulkan Samples — render pass attachments（load/store、transient）：[docs.vulkan.org/samples/latest/samples/performance/render_passes/README.html](https://docs.vulkan.org/samples/latest/samples/performance/render_passes/README.html)
- Khronos Vulkan Samples — pipeline barriers：[docs.vulkan.org/samples/latest/samples/performance/pipeline_barriers/README.html](https://docs.vulkan.org/samples/latest/samples/performance/pipeline_barriers/README.html)
- Arm — Vulkan synchronization on Mali（barrier stage 映射到 TBDR 硬件）：[developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/vulkan-synchronization-how-pipeline-barriers-map-to-mali-gpu-hardware](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/vulkan-synchronization-how-pipeline-barriers-map-to-mali-gpu-hardware)
- `VK_QCOM_render_pass_store_ops`：[registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_render_pass_store_ops.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_render_pass_store_ops.html)
- `VK_QCOM_render_pass_transform`：[registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_render_pass_transform.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_render_pass_transform.html)
- `VK_QCOM_rotated_copy_commands`：[registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_rotated_copy_commands.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_rotated_copy_commands.html)
- `VK_QCOM_tile_shading`：[registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_tile_shading.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_tile_shading.html)
- `VK_QCOM_multiview_per_view_viewports`：[registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_multiview_per_view_viewports.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_multiview_per_view_viewports.html)
- `VK_QCOM_image_processing`：[registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_image_processing.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_image_processing.html)
- `VK_QCOM_ycbcr_degamma`：[registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_ycbcr_degamma.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_QCOM_ycbcr_degamma.html)
- `VK_EXT_fragment_density_map`：[registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_fragment_density_map.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_fragment_density_map.html)
- `VK_EXT_fragment_density_map2`：[registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_fragment_density_map2.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_fragment_density_map2.html)
- `VK_EXT_load_store_op_none`：[registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_load_store_op_none.html](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_load_store_op_none.html)
- `VK_EXT_attachment_feedback_loop_layout` 提案：[docs.vulkan.org/features/latest/features/proposals/VK_EXT_attachment_feedback_loop_layout.html](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_attachment_feedback_loop_layout.html)
- Mesa freedreno / Turnip 驱动文档：[docs.mesa3d.org/drivers/freedreno.html](https://docs.mesa3d.org/drivers/freedreno.html)
- Android GPU Inspector（Adreno 逐 pass 分析）：[gpuinspector.dev/](https://gpuinspector.dev/)

### 本项目内部文档（交叉引用）

- `docs/nvn/android-perf/PLAN.md`（S23 上到 60fps 的完整计划，R1-R8/G1-G5/P1-P3/L1-L3）
- `docs/nvn/android-perf/MEASUREMENTS.md`（S23 实测：replay 16-17ms、GPU 18.7ms、Adreno 编译 15-20ms/个、扩展列表）
- `docs/nvn/ANDROID-PORT.md`（Android 移植研究：workaround 表、tiler 风险、Adreno 专属 gotcha）
- `docs/nvn/PERF-PLAN.md`（Mac 上的 60fps 计划）
- `docs/nvn/studies/fps-unlock-1710.md`（Wonder 的解锁帧率研究：**逻辑绑帧率的证据**）
- `docs/nvn/TILING.md`（block-linear 纹理 untiling，mip 链与 block height）
- `games/p5r/OPTIMIZACION.md`（Adreno 610 上的 ABAB 实测表）
- `GUIA_ARQUITECTURA_PORTS_ANDROID.md`（架构总览）

### 本项目 issue（性能相关，已读正文）

- #143 Snapdragon 870 / Adreno 650：POCO F3 上 v0.5.1 已能 "60 FPS，仅轻微掉到 30"：[github.com/Brandon04Gonzalez/p5r-android/issues/143](https://github.com/Brandon04Gonzalez/p5r-android/issues/143)
- #144 Galaxy A34 / Mali：20-23 fps，**"framerate is the same at 540p and 720p"**（该设备是 CPU-bound，不是分辨率瓶颈）；replay 14-24ms vs fence wait 1-5ms；建议 Swappy + 合并 render pass + 绑快核：[github.com/Brandon04Gonzalez/p5r-android/issues/144](https://github.com/Brandon04Gonzalez/p5r-android/issues/144)
- #154 Snapdragon 865 / Adreno 650：720p/30fps 下某些区域（Kaneshiro 宫殿地下 3 层）"fps 突然大幅下降"，且"某些纹理看起来没优化，看它时 fps 掉很深，转开就恢复"（典型**纹理带宽/采样瓶颈**）：[github.com/Brandon04Gonzalez/p5r-android/issues/154](https://github.com/Brandon04Gonzalez/p5r-android/issues/154)
- #132 请求更多画质选项：[github.com/Brandon04Gonzalez/p5r-android/issues/132](https://github.com/Brandon04Gonzalez/p5r-android/issues/132)
- #136 Helio G85 / 低端：请求更轻的性能与 NX_NO_CROWD 修复：[github.com/Brandon04Gonzalez/p5r-android/issues/136](https://github.com/Brandon04Gonzalez/p5r-android/issues/136)
- #9 Redmi Note 15 Pro+ / Adreno 810：**"最高画质比最低画质 fps 更稳"**（与 `NX_HUD_1080` 和 UBWC 的行为可能相关，值得复查）：[github.com/Brandon04Gonzalez/p5r-android/issues/9](https://github.com/Brandon04Gonzalez/p5r-android/issues/9)
- #104 POCO F8 Pro / Snapdragon 8 Elite / Adreno 830：**Shibuya 主街 17-20 FPS**（与目标设备最接近的数据点）：[github.com/Brandon04Gonzalez/p5r-android/issues/104](https://github.com/Brandon04Gonzalez/p5r-android/issues/104)
- #130 All-Out Attack 时崩溃（Adreno 825，与自定义驱动 PR #133 相关）：[github.com/Brandon04Gonzalez/p5r-android/issues/130](https://github.com/Brandon04Gonzalez/p5r-android/issues/130)

### 未核实项汇总

**已解除的未核实（本轮通过镜像/分支补齐，见开头表格）**：
- ~~Ryujinx 的 `TextureScaleMode` / `ScaleFactor` 实现~~ → **已读到**（[alula/Ryujinx `qlaunch`](https://github.com/alula/Ryujinx)），已写入 1.10 节。
- ~~yuzu 的缩放实现~~ → **已读到**（[Nailington/Yuzu `master`](https://github.com/Nailington/Yuzu)），已写入 1.1–1.8 与 1.10。
- ~~Sudachi 的缩放实现~~ → **已读到**（[p-yukusai/sudachi-emu](https://github.com/p-yukusai/sudachi-emu)），`settings_enums.h` 与 yuzu 逐字相同。
- ~~Strato 的缩放实现~~ → **已读到，且结论是"完全没有实现"**（`Texture::SynchronizeHostImpl()` 断言宿主 extent == guest extent），已写入 1.10。

**仍未核实**：
1. **Skyline 的任何代码**——`ftx1` 分支 tree 只有一个 README blob，源码已被移除。**零代码证据。** Strato 是其直接延续，可作为代理参考。
2. **Eden 的缩放实现**——`eden-emu/eden` 404，GitHub 搜索未找到可用 fork。未覆盖。
3. **Ryujinx 里真正读 `render_scale` 的 GLSL/SPIR-V 生成点**——已核实的是 support buffer 布局（`SupportBuffer.cs`）、写入方（`UpdateRenderScale` / `UpdateRenderScaleFragmentCount`）和 viewport/scissor 路径；**消费端未读到**。
4. **Ryujinx `SupportBufferUpdater.cs`**——按猜测路径 404，未读到。
5. **yuzu 的 `vk_image.cpp` / `blit_screen.frag`**——这两个文件在该树中**不存在**（相关代码在 `vk_texture_cache.cpp` 与 `src/video_core/host_shaders/`），不是"没读到"而是"没有"。
6. **`GENERAL` layout 对 Adreno 的精确代价模型**——没找到任何 Qualcomm 公布的数字。可辩护的说法只有：TBDR 上 attachment 在 pass 期间住在 tile memory，layout 本身不如"驱动能否保持压缩"重要；`GENERAL` 会堵死基于 layout 的优化，且是 UBWC 被关掉的常见嫌疑。
7. **UBWC 被关掉的精确条件清单**——Qualcomm 的 Adreno GPU Developer Guide 抓不到（302 到 `qualcomm.com` 或空壳页）。`VK_IMAGE_USAGE_STORAGE_BIT` / `VK_IMAGE_TILING_LINEAR` / 非 UBWC 兼容格式 / `MUTABLE_FORMAT` 跨不兼容格式类，这些是**合理但未证实**的嫌疑。→ **改用 3.1 的 `VK_EXT_image_compression_control` 直接查询，别再猜。**
8. **Swappy 是否要求 `setWindow`/`setSwapIntervalNS` 与 swapchain 创建同线程**——文档未明说。
9. **`notifyWorkloadSpike` 对 DVFS 的实际提速幅度（20-40%）**——纯推测。
10. **Adreno 840 是否补上 `VK_KHR_present_wait` / `VK_EXT_graphics_pipeline_library` / `VK_EXT_image_compression_control` / `VK_QCOM_queue_perf_hint`**——本项目只在 Adreno 740 上测过扩展列表。
11. **进程退出后 KGSL `PWRCTRL` 是否自动恢复**，以及**该设置是否真的绑定到 fd 生命周期**——两种说法都无法从 UAPI 头文件直接证实。
12. **OnePlus Pad 3 Pro 的 144Hz 面板是否支持 90Hz 模式**——未核实。这是 2.1 方案 (a) 的前提，**必须在真机上用 `Display.getSupportedModes()` 确认**。
13. **`/dev/kgsl-3d0` 在目标设备上是否可打开**——量产机通常 root/vendor-only，普通 app 拿 `EACCES`。这决定了 4.4 ㉑ 的结论是否成立。

**已在本轮被推翻/更正的两条（保留记录，避免团队重犯）**：
- ~~Turnip 不支持 UBWC~~ → **错**。Mesa 提供 `TU_DEBUG=noubwc` 用来*关闭* UBWC，说明默认开启。已在 3.7 更正。
- ~~`adrenotools_set_turbo(true)` 写 1~~ → **错**。源码是 `uint32_t enable{turbo ? 0U : 1U}`，turbo 写 **0**（该属性是 power-control *enable*）。已在 3.8 用源码原文核实，并确认本项目实现一致。
