# 性能设计说明（一加平板 3 Pro / 2K90）

> 本文件是本次优化的**唯一权威设计文档**。调研文档（`DEVICE-*.md`、`OPTIMIZATION-RESEARCH-*.md`）只提供依据，
> 任何设计决策以本文件为准。改设计必须同时改本文件。

## 0. 目标与非目标

**目标**
1. 在一加平板 3 Pro（骁龙 8 Elite Gen 5 / Adreno 840 / 3.4K 144Hz / 16GB）上提供 **2K 渲染** 与 **90fps 帧率**档位。
2. 帧率策略**可选**：省电 30 / 均衡 60 / 高帧率 90 / 极速 120 / 无上限。
3. 提供启动器一键预设，中文界面。
4. 提供可复现的云端构建（GitHub Actions）与自动化测试。

**非目标**
1. 不改变 P5R 的游戏逻辑步进语义（不重写游戏代码）。
2. 不降低低端设备（Adreno 610/6xx）的默认表现。
3. 不引入新的第三方依赖。

## 1. 关键约束（来自代码事实，务必遵守）

### 1.1 P5R 的逻辑步进与帧率绑定
`games/p5r/game.cpp` 顶部注释明确：游戏**大部分逻辑每帧步进 `1/target`**，`target` 由 `main+0xf73740(fps)` 设定，
每帧通过虚拟 setter（`main+0x1048ee0`）把 present interval（1..4）交给 `nvnWindowSetPresentInterval`。

推论：
- `target = 60` 且实际跑满 60fps → 速度正确。
- `target = 60` 但只跑到 45fps → **游戏慢放**（75% 速度），音频跑在实时线程上会"跑到前面"。
- `target = 90` 且实际跑满 90fps → 理论上速度正确，但引擎与动画是按 1/30、1/60 调校的，**属于实验档**。
- `target = 90` 但只跑到 60fps → 慢放 2/3。**这是必须警告用户的**。

因此"帧率档位"不是一个开关，而是 **target + present interval + 显示器刷新率** 三者的一致组合。

### 1.2 分辨率链路
```
config.txt resolution / NX_RES  ─┐
                                 ├─→ plat::window_size()   （platform_android.cpp，含 1080 高度硬上限）
config.txt render_scale ─────────┘
config.txt NX_RENDER_RES ──→ svc_am.cpp GetDefaultDisplayResolution（游戏据此决定内部 RT 尺寸）
```
当前 `window_size()` 有 `scale = min(scale, 1080/sh)` 的**硬上限 1080**，2K 必须解除（且只在显式档位下解除）。

### 1.3 呈现与 pacing
`nvn_queue.cpp::pace_present(interval)` 用 `interval * display_period_ns()` 作为步长，
`display_period_ns()` 固定 16.67ms（除 `NX_DISPLAY_HZ`）。`hle_vi.cpp` 的 vsync 线程同源。

## 2. 开关表（唯一权威）

所有开关写入 `config.txt`（key=value）。环境变量形式为 `NX_<KEY>`，便于 PC 调试。

| config.txt key | 环境变量 | 取值 | 默认 | 作用 |
|---|---|---|---|---|
| `perf_mode` | `NX_PERF_MODE` | `balance` / `quality` / `2k` / `2k90` / `ultra` / `custom` | `balance` | 预设组合，见 §3 |
| `fps_mode` | `NX_FPS_MODE` | `30` / `45` / `60` / `90` / `120` / `0`(=无上限) | **未设置**（沿用上游 `NX_FPS_CAP` 的 30） | 目标帧率档 |
| `refresh_hz` | `NX_REFRESH_HZ` | `0`(跟随系统) / `60` / `90` / `120` / `144` | `0` | 请求的显示刷新率 |
| `render_height` | `NX_RENDER_HEIGHT` | `720` / `1080` / `1440` / `1600` / `0`(原生) | `0` | 渲染高度档（0=按 perf_mode 推导） |
| `render_scale` | — | `0.25`..`1.0` | 按 GPU | 渲染缩放（上游已有，保留） |
| `resolution` | — | `WxH` / `360p`..`2160p` | 空 | 精确渲染尺寸（上游已有，扩展档位） |
| `NX_RENDER_RES` | `NX_RENDER_RES` | `WxH` | 空 | 游戏内部 RT 尺寸（上游已有） |
| `uncap_present` | `NX_NO_FRAME_LIMIT` | `0`/`1` | `0` | 关闭 present pacing（上游已有） |
| `allow_over_1080` | `NX_ALLOW_OVER_1080` | `0`/`1` | `0` | 解除 `window_size()` 的 1080 高度上限 |
| `present_mode` | `NX_PRESENT_MODE` | `auto` / `fifo` / `mailbox` / `immediate` | `auto` | 交换链呈现模式 |
| `adpf` | `NX_ADPF` | `0`/`1` | `1` | Android ADPF 性能提示（上游已有） |

**默认值全部等于上游行为**：未写 `perf_mode` 时 `perf_mode=balance`，且 `balance` 必须产生与上游逐字节一致的
`window_size()` 与 present 配置。

## 3. 预设（perf_mode）

| perf_mode | render_height | fps_mode | refresh_hz | present_mode | 适用 |
|---|---|---|---|---|---|
| `balance`（默认） | 上游逻辑（GPU 决定） | 60 | 0 | auto(FIFO) | 所有设备，**必须与上游一致** |
| `quality` | 1080 | 60 | 60 | fifo | 中高端，1080p60 |
| `2k` | 1440 | 60 | 90 | mailbox | 8 Elite 级，1440p60 |
| `2k90` | 1440 | 90 | 90 | mailbox | **一加平板 3 Pro 推荐档**（实验） |
| `ultra` | 1600 | 120 | 120 | mailbox | 极限，需散热 |
| `custom` | 由上面各项单独决定 | | | | 手调 |

`perf_mode` 只提供默认值：任何显式写出的单项 key 都**覆盖**预设值（覆盖顺序：单项 > 预设 > 上游默认）。

### 3.1 fps_mode 与 refresh_hz 的耦合规则（重要）

P5R 的 present interval 是**整数** vsync 数（1..4），并且游戏逻辑按 `1/target` 步进（§1.1）。因此
**"帧率 = 刷新率 / interval"** 是唯一不会让游戏变速的表达方式。由此定下三条硬规则：

1. **显式设 `fps_mode=90` 或 `120` 而未设 `refresh_hz` 时，`refresh_hz` 自动取该帧率（90/120）。**
   理由：用户选"90帧"的语义就是"跑 90 帧"，而面板支持 90/120/144Hz，不请求刷新率就只能跑 60，
   等于这个选项是空操作——这是必须避免的坑（frame-pacing 的评审发现）。
2. **`fps > refresh` 时钳制 `fps = refresh` 并打警告**，绝不通过把 interval 设成 0 去"凑"高帧率
   （那会让 vsync 单元与游戏步进脱钩，导致慢放或加速）。
3. **`fps_mode=30/60` 不自动改刷新率**（跟随系统），因为 60Hz 面板上 30/60 是原生整数倍，
   无需请求高刷，避免无谓功耗与发热（§4.5 的首要目标）。

对应的 `present_interval = max(1, round(refresh_hz / fps))` 取值表：

| fps_mode | refresh_hz（自动） | interval | 实际帧率 | 游戏速度 |
|---|---|---|---|---|
| 30 | 0(跟随) | 2 | 30 | 正确 |
| 60 | 0(跟随) | 1 | 60 | 正确 |
| 90 | 90 | 1 | 90 | 正确（实验） |
| 120 | 120 | 1 | 120 | 正确（实验） |
| 0(无上限) | 0(跟随) | 1 | 由 GPU 决定 | **可能错误**，仅调试用 |

### 3.2 `NX_FPS_CAP` 与 `fps_mode` 的冲突规则（血泪教训）

上游 `NX_FPS_CAP` 的语义是 **`>= 60 ? 60 : 30`**（`games/p5r/game.cpp` 原实现），即它**只能表达 30 或 60**。
且它**优先级高于 `fps_mode`**（谁让它是上游开关）。

因此：

> **禁止在 90/120/无上限档位下写 `NX_FPS_CAP`。**
> 写了 `NX_FPS_CAP=90` 会被钳成 **60**，使"2K90"预设实际只跑 60fps —— 启动器 UI 显示 90、实际 60，
> 这是最坏的一类 bug（用户以为生效了）。

规则：
- `fps_mode=30` → 写 `NX_FPS_CAP=30`（上游语义一致）
- `fps_mode=60` → 写 `NX_FPS_CAP=60`（上游语义一致）
- `fps_mode=90/120/0` → **删除 `NX_FPS_CAP`**，只留 `fps_mode`
- 读旧配置时 `NX_FPS_CAP=60` → 迁移为 `fps_mode=60`；`NX_FPS_CAP=30` → `fps_mode=30`

此规则必须被 `tools/verify_perf_settings.ps1` 与真机验证双向覆盖。

### 3.3 整数 interval 的固有局限（必须对用户说明）

present interval 只能是整数 vsync 数，所以 **"帧率 = 刷新率 ÷ 整数"** 才可达：

| 刷新率 | 可达帧率（interval 1/2/3/4） |
|---|---|
| 60 Hz | 60 / 30 / 20 / 15 |
| 90 Hz | 90 / 45 / 30 / 22.5 |
| 120 Hz | 120 / 60 / 40 / 30 |
| 144 Hz | **144 / 72 / 48 / 36** |

**推论（写进用户文档与启动器提示）**：在 144Hz 面板上若把 `refresh_hz` 钉成 144，则 `fps_mode=90` 只能得到 **72fps**，
因为 144/90 = 1.6 取不到整。所以：

- **"90 帧"必须靠把面板切到 90Hz 来实现**（§3.1 的自动推导正是为此）。
- 若设备/系统拒绝了 90Hz 请求（**未验证**：一加平板 3 Pro 是否暴露 90Hz `Display.Mode` 需真机确认），
  实际会落到 72fps 或 60fps，**必须在 logcat 里给出 `WARNING:` 行**说明被钳制，而不是静默降级。
- `fps_clamped` 一旦为真，`hle_vi.cpp` 会打印可 grep 的警告。这是对抗"选项看起来生效了其实没有"的关键防线。

## 4. 各模块改动点

| 文件 | 函数 | 改动 | 开关 |
|---|---|---|---|
| `runtime/src/perf_profile.h/.cpp` | 新增 | 档位解析、设备探测、生效值计算 | 全部 |
| `runtime/src/platform_android.cpp` | `window_size()` | 1080 上限可解除；支持 1440/1600 | `allow_over_1080`、`render_height` |
| `runtime/src/platform_android.cpp` | `default_render_scale()` | Adreno 8xx 返回 1.0（已满足），新增日志 | — |
| `runtime/src/platform_android.cpp` | 新增 `apply_refresh_rate()` | `ANativeWindow_setFrameRateWithChangeStrategy` | `refresh_hz` |
| `runtime/src/perf_hint.cpp` | `perf_hint_frame()` | ADPF 目标帧时间跟随 fps_mode | `adpf`、`fps_mode` |
| `runtime/src/hle_vi.cpp` | `display_period_ns()` | 跟随 refresh_hz / fps_mode | `refresh_hz` |
| `runtime/src/nvn/nvn_queue.cpp` | `pace_present()` | 步长按生效帧率；无上限时关闭 | `fps_mode`、`uncap_present` |
| `runtime/src/vk_swapchain.cpp` | 交换链创建 | present mode 档位化 | `present_mode` |
| `games/p5r/game.cpp` | `cap_fps()` / `interval_setter_hook()` | 支持 30/45/60/90/120/无上限 | `fps_mode` |
| `games/p5r/java/**` | 启动器 | 预设按钮 + 设置项 + 中文 | — |

## 4.5 热管理与动态分辨率（散热是第一优先级）

用户明确要求：**不能发热严重、不能卡死**。因此所有"高画质高帧率"档位都必须挂在热保护之上。
原则：**宁可自动降分辨率，也不许降帧率到卡顿；宁可降帧率，也不许降频到烫手。**

### 4.5.1 三层保护

| 层 | 机制 | 触发 | 动作 | 恢复 |
|---|---|---|---|---|
| L1 动态分辨率 (DRS) | 按帧时间调整渲染高度 | 平均帧时间 > 目标 × 1.10 持续 2 s | 渲染高度 −10%（最低 720p） | 帧时间 < 目标 × 0.85 持续 5 s，每次 +5%，最高回到设定值 |
| L2 帧率降档 | 热状态驱动的帧率上限 | Android `ThermalStatus` ≥ MODERATE (2) | fps_mode 降到 60；≥ SEVERE (3) 降到 30 | 热状态回到 LIGHT (1) 以下持续 30 s 才恢复 |
| L3 渲染降档 | 关闭昂贵效果 | 热状态 ≥ SEVERE | 关 FSR 锐化、关 `NX_HUD_1080`、渲染高度锁 1080 | 同 L2 |

- L1 用**已经存在**的帧时间数据源（`games/p5r/game.cpp` 的 `game_clock()` 有逐帧真实时长；`nvn_queue.cpp`
  的 `pace_present` 有 present 间隔），不新增计时线程。
- L2/L3 的热状态从 `android.os.PowerManager.getCurrentThermalStatus()`（API 29+）读取，用 JNI 或
  `AThermal`/`PowerManager` 的 NDK 替代；若不可用则退化为**纯帧时间判定**（帧时间持续劣化 10 s 即视为热降频）。
  实现放在 `runtime/src/perf_thermal.cpp`，非 Android 平台为空实现。
- **热保护永远不能被用户开关关闭**（这是安全阀）。用户可以调"积极程度"，不能关掉。

### 4.5.2 新增开关（追加到 §2 表）

| config.txt key | 环境变量 | 取值 | 默认 | 作用 |
|---|---|---|---|---|
| `thermal_guard` | `NX_THERMAL_GUARD` | `0`(仅记录) / `1`(标准) / `2`(激进) | `1` | 热保护强度；`0` 只记录不动作，仅供测试 |
| `dyn_res` | `NX_DYN_RES` | `0`/`1` | `1` | L1 动态分辨率（仅在渲染高度 > 1080 时生效） |
| `dyn_res_min` | `NX_DYN_RES_MIN` | `720`/`900`/`1080` | `1080` | DRS 的下限高度 |
| `thermal_log` | `NX_THERMAL_LOG` | `0`/`1` | `1` | 每 10 s 记录一次热状态/帧时间/渲染高度 |

### 4.5.3 为什么这样不会卡死

1. DRS 只改**渲染高度**，不改游戏逻辑步进 → 永远不会让游戏慢放。
2. 帧率降档只走**已经验证过的整数档**（60/30），且只在热状态持续劣化时触发。
3. 所有降档都有**滞回**（hysteresis），不会来回抖动。
4. 最坏情况：渲染高度 1080 + 30 fps，即上游默认行为，绝不会比上游更差。

## 5. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 90/120 档游戏加速或慢放 | 玩法异常 | 标注"实验"；启动器二次确认；`docs/PERF-TEST-REPORT.md` 记录实测 |
| 2K 渲染导致 GPU 过载掉帧 | 反而更卡 | 档位默认关闭；提供 `render_scale` 回退；ADPF 联动 |
| 解除 1080 上限后 HUD 错位 | 界面元素偏移 | 只在显式档位下解除；HUD 走 `NX_HUD_1080` 独立开关 |
| 高刷新率增加功耗/发热 | 降频后掉帧 | `refresh_hz` 跟随系统为默认；提供 `balance` 回退 |
| 低端设备被波及 | 回归 | 默认档等于上游；CI 跑 host 测试；改动均有开关 |

## 6. 测试要求

见 `docs/PERF-TEST-PLAN.md`。最低要求：
1. `balance` 档与上游行为一致（静态断言 + 单元测试）。
2. 所有开关的解析有单元测试（`tests/perf_profile_test.cpp`）。
3. host 构建（Linux/Windows）+ ctest 通过。
4. arm64 NDK 构建通过（CI）。
5. 真机验证项单列，标"未验证"直到有人跑过。
