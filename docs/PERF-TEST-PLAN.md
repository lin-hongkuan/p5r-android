# 性能测试计划（一加平板 3 Pro / 2K90 分支）

> 依据：`docs/PERF-DESIGN.md`（唯一权威设计）、`CONTRIBUTING-FORK.md`（团队契约）。
> 执行者：`qa-test`。结果记录在 `docs/PERF-TEST-REPORT.md`，本文件只写**用例与判定标准**。
>
> **状态约定**：`已验证` = 有人在本机跑过并能贴出输出；`未验证 — 需要真机` = 必须在一加平板 3 Pro 上跑，
> 在有人跑过之前**任何文档都不得写成"通过"**（CONTRIBUTING-FORK.md §4）。

## 0. 测试环境（真实工具链事实，先读再动手）

这一节的每一条都是本机实测结论，不是推测。踩过的坑直接写在这里，避免下一个人重复踩。

| 项 | 事实 | 影响 |
|---|---|---|
| `G:\mingw64\bin\g++.exe` | **g++ 8.1.0**。**不支持 `-std=c++20`**，必须用 `-std=c++2a` | 用 `-std=c++20` 会直接报 `unrecognized command line option` |
| 同上，PATH 里没有 MSYS2 时 | 实测**可以正常执行**（`--version` exit 0，输出 273 字节）。lead 最初怀疑的 "exit 1 且无输出" 是 DLL 加载失败，实测发生在 **`E:\MSYS2`** 的编译器上 | 见下一行 |
| `E:\MSYS2\mingw64\bin\g++.exe` | **g++ 15.2.0**。**PATH 里没有 `E:\MSYS2\mingw64\bin` 时会静默失败：exit 1、stdout 0 字节、stderr 0 字节** | 必须 `$env:PATH = "E:\MSYS2\mingw64\bin;" + $env:PATH` |
| 静默失败的后果 | "exit 1 且完全无输出" 极容易被 agent 误读成"编译通过、没有警告" | **判定规则：exit≠0 一律 FAIL，即使没有任何输出**；断言产物存在（`.o`/`.exe` 的文件大小 > 0） |
| Vulkan 头 | `private/third_party/Vulkan-Headers-1.4.357/include`（含 `vk_video/`） | `nvn/*.cpp` 可以做语法检查 |
| `-I` 路径 | **必须传绝对路径**；传 `private\...` 相对路径时 g++ 报 `vulkan/vulkan.h: No such file or directory` | 用 `(Resolve-Path ...).Path` |
| `-Wextra` | **不得用于通过/失败判定**（Vulkan 结构体初始化噪声） | 只用 `-Wall`，或只用 `-fsyntax-only` 看 `error:` 条数 |
| `private/` | 已被 `.gitignore` 忽略 | 编译产物一律放 `private/`，不要污染工作树 |

## 1. 静态检查

`tools/verify_perf_settings.ps1` 是这一节的**可执行版本**，reviewer 直接跑它即可；
下面的用例是它覆盖的范围，以及它覆盖不到、必须人看的部分。

### 1.1 开关三件套（解析 / 文档 / 日志）

`docs/PERF-DESIGN.md` §2 与 §4.5.2 的每一个开关，都必须同时满足三条：

| 编号 | 用例 | 判定 |
|---|---|---|
| ST-01 | `perf_mode` 被解析 | `setting("perf_mode")` 或 `source.value("perf_mode")` 出现在 `runtime/src/perf_profile.cpp` |
| ST-02 | `fps_mode` 被解析 | 同上 |
| ST-03 | `refresh_hz` 被解析 | 同上 |
| ST-04 | `render_height` 被解析 | 同上 |
| ST-05 | `present_mode` 被解析 | 同上 |
| ST-06 | `allow_over_1080` 被解析 | 同上 |
| ST-07 | `uncap_present` 被解析 | 同上（`nvn_queue.cpp` 的 `NX_NO_FRAME_LIMIT` 也算） |
| ST-08 | `adpf` 被解析 | `perf_hint.cpp` |
| ST-09 | `thermal_guard` / `dyn_res` / `dyn_res_min` / `thermal_log` 被解析 | §4.5.2 的四个键（`perf_thermal.cpp`） |
| ST-10 | 每个开关在 §2/§4.5.2 表里**有文档** | 表里能 grep 到该 key |
| ST-11 | 每个**新增**开关在启动日志里**有记录** | 该 key 出现在一条 `log(...)` / `logf(...)` / `nx::log(...)` 附近（±3 行） |
| ST-12 | 每个开关的 `NX_<KEY>` 环境变量别名可用 | 字面量 `NX_PERF_MODE` 等，或存在通用 `"NX_" + toupper(key)` 映射（`perf_profile.cpp:34`） |

> 注：`render_scale` / `resolution` / `NX_RENDER_RES` / `uncap_present` / `adpf` 在 §2 表里标注"上游已有"，
> 属于上游开关，**不要求**本分支为它们新增启动日志（ST-11 对它们记 SKIP）。

### 1.2 非 Android 对等实现

| 编号 | 用例 | 判定 |
|---|---|---|
| ST-13 | 本分支**新增**的每一个 `#if defined(__ANDROID__)` 都有 `#else`/`#elif` 分支 | 解析预处理嵌套；只检查本分支**新增行**上的 `#if`（上游遗留的不算）。缺失 → FAIL |
| ST-14 | `runtime/src/perf_profile.cpp` 在非 Android 下可编译 | 用 `-DNX_PERF_STANDALONE` 编译通过（§3.1） |
| ST-15 | 新增的 Android 专用 API 都走 `dlsym`/运行时探测，不硬链 | 人看：`ANativeWindow_setFrameRateWithChangeStrategy`、`APerformanceHint_*`、`AThermal_*` 是否允许旧 Android 优雅降级 |

### 1.3 依赖与仓库卫生

| 编号 | 用例 | 判定 |
|---|---|---|
| ST-16 | 没有新增第三方依赖 | `third_party/` 无改动；`CMakeLists.txt` 无 `FetchContent`/`ExternalProject_Add`；无 `.gitmodules` |
| ST-17 | 没有游戏文件 / keys / `private/` 内容进版本库 | `git status --porcelain` 与 `git ls-files` 都不含 `*.xci/*.nsp/*.ncz/*.keys/private/**` |
| ST-18 | `private/` 确实被忽略 | `git check-ignore -q private/` 返回 0 |

### 1.4 一致性与默认值

| 编号 | 用例 | 判定 |
|---|---|---|
| ST-19 | §2 表与启动器写的 key 一致 | 启动器写的每个 perf 类 key 都在表里；表里每个**新增** key 都有启动器入口（或 `config.put` 的派生写入，如 `allow_over_1080`） |
| ST-20 | `balance` 预设 == 上游默认 | 文档 §3 的 balance 行必须是 `fps 60 / refresh 0 / auto(FIFO)`；代码里 `preset_of(Mode::Balance)` 必须是 `{0, F60, 0, Auto}` |
| ST-21 | 未配置时 `window_size()` 结果与上游逐字节一致 | 单测 + §4.1 的 balance 回归用例（四个 GPU 档位） |
| ST-22 | 每个改动点在 §4 表里登记（文件 → 函数 → 开关 → 默认值 → 影响设备） | 人看 |
| ST-23 | 热保护开关 `thermal_guard=0` 时只记录、不动作 | 代码审查 + 真机日志（§4.5.2） |

### 1.5 静态检查的可信度（对抗性自检）

| 编号 | 用例 | 判定 |
|---|---|---|
| ST-24 | 验证脚本本身**不是橡皮图章**：故意注入缺陷必须被抓到 | 见 `docs/PERF-TEST-REPORT.md` 的"验证脚本自检"：删掉 `fps_mode` 解析点 → FAIL；把 `parse_int` 换成裸 `atoi` → FAIL；恢复后必须回到同一结论（文件哈希一致） |

## 2. 单元测试

`tests/perf_profile_test.cpp`（归属 `perf-core`）+ `tests/perf_thermal_test.cpp`（如已落地）。
构建：`g++ -std=c++20 -O2 -DNX_PERF_STANDALONE -Iruntime/src tests/perf_profile_test.cpp runtime/src/perf_profile.cpp`
或 `cmake -DNX_TESTS=ON && ctest`。

| 编号 | 用例 | 判定 |
|---|---|---|
| UT-01 | 未配置 → `balance`，且所有 `*_configured == false` | `fps==60`、`refresh_hz==0`、`render_height==0`、`present_mode==Auto`、`uncap_present==false`、`adpf==true` |
| UT-02 | 五个预设的解析表 | `quality→{1080,60,60,fifo}`、`2k→{1440,60,90,mailbox}`、`2k90→{1440,90,90,mailbox}`、`ultra→{1600,120,120,mailbox}`、`custom→{0,60,0,auto}` |
| UT-03 | 覆盖优先级：单项 > 预设 > 上游默认 | `perf_mode=2k90` + `fps_mode=60` → fps 60、refresh 仍 90、height 仍 1440 |
| UT-04 | 垃圾输入回退默认，不崩溃 | `perf_mode=abc`→balance；`fps_mode=abc`→60；`refresh_hz=abc`→0；`render_height=abc`→0；`present_mode=abc`→auto |
| UT-05 | 1080 上限**仅在允许时**解除 | `allow_over_1080` 未写：2160 屏 + GPU 1.0 → 高度 1080；写了 `=1` → 2160 |
| UT-06 | 显式 `render_height` 优先于渲染缩放，且不超过屏幕 | `render_height=1600` 在 1440 屏 → 1440 |
| UT-07 | 屏幕尺寸未知 → 720 | `resolve_render_height(p, 0, x) == 720` |
| UT-08 | `present_interval` 计算 | 90fps@90Hz → 1；120fps@60Hz → 1；30fps@60Hz → 2；无上限 → 1；夹在 1..4 |
| UT-09 | 上游 `NX_FPS_CAP` 兼容 | `fps_cap=30`→30；`fps_cap=60`→60；`fps_cap=0`→视为未写；`fps_mode` 优先于 `fps_cap` |
| UT-10 | 热保护纯函数 | 交替好/坏帧后渲染高度必须**收敛**、不来回抖动；非正/荒谬帧时长被忽略；热状态夹到 0..6；`min_height` 夹到 `max_height` |

## 3. 对抗性验证（CONTRIBUTING-FORK.md §4.1）

用户明确要求"对抗性验证测试"。七类攻击面，每类都要有**期望结果**，不能只写"测试一下"。

### 3.1 默认零变化（default-zero-change）

| 编号 | 用例 | 期望 |
|---|---|---|
| AT-01 | 空 `config.txt`、无 `NX_*` 环境变量 | 与上游行为逐字节一致：60Hz vsync、FIFO、渲染高度由 GPU 缩放决定、`window_size()` 同值 |
| AT-02 | 每个开关**单独**关掉/清空 | 回到 AT-01 的状态（"回滚"见 3.6） |
| AT-03 | 四个 GPU 档位（Adreno 610 / 6xx / Mali / 840）在 balance 下的渲染高度 | 必须等于上游 `min(scale, 1080/screen_h)` 的计算值 |

### 3.2 恶意输入（malicious input）

对每个开关依次喂入：空串、`0`、`-1`、`-999999`、`999999`、`abc`、`1440x`、`" 1440 "`（前后空格）、`1440\r`（CRLF）、`60.5`、超长字符串。

| 编号 | 用例 | 期望 |
|---|---|---|
| AT-04 | 数字型开关喂垃圾 | **不崩溃、不 UB、不分配巨量内存**；一律回退到预设/上游默认 |
| AT-05 | `refresh_hz` 超范围 | `-5`/`999999` → 视为未写（0） |
| AT-06 | `render_height` 超范围 | `-1`/`999999`/`1440x` → 视为未写（0） |
| AT-07 | 布尔型开关喂垃圾 | `allow_over_1080=abc`→0；`adpf=abc`→1（上游默认）；`uncap_present=abc`→0 |
| AT-08 | 大小写与空白容错 | `2K90`、` MAILBOX `、`" 2k90 "`、`1440\r` 都必须正确识别 |
| AT-09 | 空值 == 未写 | `perf_mode=`（空）→ balance，而不是"垃圾" |
| AT-10 | **不得有未加范围保护的 `atoi`/`atof`** | `perf_profile.cpp` / `perf_thermal.cpp` 里的数字解析必须有 `errno`/`*end=='\0'`/`parse_int`/`std::clamp` 之类的整体校验；裸 `atoi` → FAIL |

### 3.3 极端组合（extreme combinations）

| 编号 | 用例 | 期望 |
|---|---|---|
| AT-11 | `fps_mode=120` + `refresh_hz=60` | 不崩；`vsync_hz==60`、interval==1；**必须**在报告里注明"120 目标高于 60 单位"的行为（`hle_vi.cpp` 会把单位抬到 120） |
| AT-12 | `render_height=1600` + `refresh_hz=30` | 不崩；高度 1600、单位 30Hz |
| AT-13 | GPU **不支持** mailbox 时 `present_mode=mailbox` | 必须回退链：mailbox → immediate → FIFO，并在日志里写"asked for, not offered: fell back" |
| AT-14 | `allow_over_1080=1` + `render_height=0` | 不崩；上限解除但高度仍由缩放决定 |
| AT-15 | `dyn_res_min` > `render_height` | 必须夹住（`min_height` 夹到 `max_height`），不得渲染高度为负/巨大 |
| AT-16 | `fps_mode=0`（无上限） | `uncap_present==true`、pacing 关闭、`present_interval==1` |
| AT-17 | `perf_mode=custom` + 只写一个单项 | 单项生效，其余保持上游默认 |
| AT-18 | `perf_mode=2k90` + `render_height=720` | **单项赢**（§3）：720 生效，且 `allow_over_1080` 不应被留下 |

### 3.4 平台（非 Android 构建路径）

| 编号 | 用例 | 期望 |
|---|---|---|
| AT-19 | `perf_profile.cpp` 用 `-DNX_PERF_STANDALONE` 编译 | 通过，无警告（`-Wall`） |
| AT-20 | 非 Android 下所有 Android 专用调用为空实现 | `perf_thermal.cpp` / `perf_hint.cpp` / `platform_android.cpp` 的 `#else` 分支存在且可编译 |
| AT-21 | host 构建（`-DNX_TESTS=ON`）与 ctest | 全绿 |

### 3.5 热与内存（thermal / memory）

| 编号 | 用例 | 期望 |
|---|---|---|
| AT-22 | 2560×1440 RGBA8 单目标 = **14.06 MiB**（实测） | 记录在案；并发多目标（MRT/历史帧/FSR）需 ×N |
| AT-23 | 2K/2.5K 档 + 多目标 + 纹理是否 OOM | 真机观察 `dumpsys meminfo`；不得 OOM、不得被 LMK 杀 |
| AT-24 | 热保护三层触发与恢复 | 见 §5.3（真机） |
| AT-25 | 长时间（≥30 min）高温运行 | 不烫手、不卡死、不降频到卡顿；DRS 只降分辨率不降逻辑步进 |

### 3.6 回滚（rollback）

| 编号 | 用例 | 期望 |
|---|---|---|
| AT-26 | 删掉所有 perf 开关（或全写 `balance`） | 与 AT-01 完全一致 |
| AT-27 | 从 2K90 切回 balance | `window_size()`、present mode、pacing 全部回到上游；1080 上限重新生效 |
| AT-28 | 每个开关的"关闭"值真的关闭 | `present_mode=auto` → 上游选择；`dyn_res=0` → 不做 DRS；`thermal_guard=0` → 只记录 |

### 3.7 并发（concurrency）

| 编号 | 用例 | 期望 |
|---|---|---|
| AT-29 | `profile()` 从多线程首次调用 | 函数内 `static const Profile`（C++11 起线程安全），只初始化一次、只打一次日志 |
| AT-30 | 热路径无 I/O | `profile()` 之后没有任何 `setting()`/`getenv()`；`CONTRIBUTING-FORK.md §3.3`（`setting()` 每次都开文件） |
| AT-31 | 热保护模块无线程/无锁竞争 | `perf_thermal` 设计为纯函数，由平台层喂数据；不得自建计时线程 |

## 4. 构建测试

| 编号 | 用例 | 判定 |
|---|---|---|
| BT-01 | host 单元测试编译 + 运行 | g++ 15.2（PATH 已修正）与 g++ 8.1（`-std=c++2a`）**都**能编译 `tests/perf_profile_test.cpp` + `perf_profile.cpp`；运行 exit 0 |
| BT-02 | CMake 配置（`-DNX_TESTS=ON`） | `cmake -S . -B <dir> -G Ninja -DNX_TESTS=ON` exit 0；无游戏数据时警告"building the unit tests only"属正常 |
| BT-03 | ctest | `ctest --test-dir <dir> --output-on-failure` 全绿（含 `perf_profile`） |
| BT-04 | 改动的 runtime 文件语法检查 | `g++ -std=c++20 -fsyntax-only -I<abs> -I<Vulkan-Headers abs> <file>` → `error:` 条数 0 |
| BT-05 | arm64 NDK 构建 | **CI 上跑**（本地无 NDK/SDL3/Vulkan loader）。`libp5r.so` 链接成功 |
| BT-06 | APK 打包 | CI 产出 `p5r-nce.apk`，`aapt2`/`d8`/`zipalign`/`apksigner` 全过 |
| BT-07 | Java 兼容性 | `javac -source 1.8 -target 1.8` exit 0、无警告；`Lang.java` 所有语言分支无 null |

> **本机无法跑完整 CMake 构建**：没有 SDL3、没有 Vulkan loader、没有游戏数据、没有 NDK。
> 因此 BT-05/BT-06 本地一律记 **未验证 — 需要 CI**，不得写成通过。

## 5. 设备验证（一加平板 3 Pro / Adreno 840 / 3.4K 144Hz）

> 本节全部 **未验证 — 需要真机**。下面给出可直接照抄的命令与判定标准。

### 5.0 通用准备

```powershell
adb devices
adb logcat -c
# 只看本 runtime 的日志（tag 来自 game::kInfo.id = "p5r"）
adb logcat -s p5r
# 或者边跑边抓全量到文件（便于事后比对）
adb logcat > private/qa_adv/run.logcat
```

`config.txt` 位置（启动器 `Launcher.dataDir()`）：`/sdcard/Persona 5 Royal/config.txt`。
写配置：

```powershell
adb shell "echo 'perf_mode=2k90' > '/sdcard/Persona 5 Royal/config.txt'"
adb shell "cat '/sdcard/Persona 5 Royal/config.txt'"
```

### 5.1 启动日志必须出现的行（逐开关取证）

| 开关 | 应看到的日志（关键片段） | 来源 |
|---|---|---|
| 全部档位 | `perf: perf_mode=<mode> (fps <n>, refresh <hz> Hz, vsync <hz> Hz, render height <h>, present <m>)` | `perf_profile.cpp` `log_values()` |
| `allow_over_1080` / `adpf` / `uncap_present` | `perf: allow_over_1080=<0/1> adpf=<0/1> uncap_present=<0/1>` | 同上 |
| `refresh_hz` / `fps_mode` | `vi: display at <hz> Hz (perf profile: <how>; fps_mode <fps>), present unit <ms>` | `hle_vi.cpp` |
| `present_mode` | `vk: <who>: present mode <MODE> (perf present_mode <m>; surface offers <n> modes[ (asked for, not offered: fell back)])` | `vk_swapchain.cpp` |
| `fps_mode`（pacing 步长） | `nvn: present pacing step = interval * <ms> (vsync unit; fps_mode <fps>, interval 1 = <fps>)` | `nvn_queue.cpp` |
| `uncap_present` | `nvn: present pacing off (fps_mode <fps>...)` | `nvn_queue.cpp` |
| 渲染尺寸 | `android: setBuffersGeometry <W>x<H> (screen <sw>x<sh>) -> <r>` | `platform_android.cpp` |
| 渲染分辨率 | `android: window frame rate <hz> Hz requested (<r>[ , even if not seamless])` | `platform_android.cpp` |
| GPU 档位/缩放 | `gpu: <deviceName> -> default render scale <s>` | `platform_android.cpp` |
| 帧率实测 | `p5r: game clock <t> s, steps <t> s in <t> s real, <fps> fps (target <lo>-<hi>), ...` | `games/p5r/game.cpp` |
| 每 300 帧 | `vk: frame <n> presented (<r>), <fps> fps (last <s> s), ... worst <ms>; per frame: fences/acquire/present` | `vk_swapchain.cpp` |
| 热保护 | `thermal: ...`（`perf_thermal.cpp` 每 10 s，`thermal_log=1`） | `perf_thermal.cpp` |

### 5.2 五个预设的 `config.txt`（直接照抄）

```ini
# DV-01 balance（默认，必须与上游一致）
#   空文件即可；显式写法：
perf_mode=balance
```

```ini
# DV-02 quality（1080p60）
perf_mode=quality
```

```ini
# DV-03 2k（1440p60）
perf_mode=2k
```

```ini
# DV-04 2k90（一加平板 3 Pro 推荐档，实验）
perf_mode=2k90
```

```ini
# DV-05 ultra（1600p120，需散热）
perf_mode=ultra
```

单项覆盖示例（验证 AT-18：单项 > 预设）：

```ini
perf_mode=2k90
render_height=720
```

### 5.3 逐项设备用例

| 编号 | 用例 | 步骤 | 判定 |
|---|---|---|---|
| DV-01 | balance == 上游 | 空 `config.txt`，跑标题画面 + 一段街道 | `perf: perf_mode=balance (... fps 60, default, refresh 0 Hz follow system, vsync 60 Hz, render height 0 from render scale, present auto)`；`gpu: ... render scale` 与优化前同值 |
| DV-02 | 1440p 真的生效 | `perf_mode=2k`，看日志 | `android: setBuffersGeometry 2560x1440 ...`（宽高按屏幕比例），**不是** 1920x1080 |
| DV-03 | 1080 上限解除 | `perf_mode=2k` vs `render_height=1440` 不给 `allow_over_1080` | 后者必须**仍被夹到 1080**（证明夹子还在，没被全局拆掉） |
| DV-04 | refresh 请求 | `perf_mode=2k90` | `android: window frame rate 90 Hz requested (0, even if not seamless)`；再 `dumpsys display \| grep -i fps` 确认面板实际在 90 |
| DV-05 | present mode | `perf_mode=2k90` | `vk: ...: present mode MAILBOX (perf present_mode mailbox; surface offers N modes)` |
| DV-06 | fps 实测 | `perf_mode=2k90` 跑 60 s | `p5r: game clock ... 90.x fps (target ...)`；**并注意**是否出现"慢放"（`steps` 与 `real` 差值 > 2%） |
| DV-07 | 30/60/90/120 四档 | 依次设 `fps_mode=30/60/90/120` | 日志 fps 与档位相符；90/120 必须记录游戏速度是否异常（§1.1 慢放/加速） |
| DV-08 | 无上限 | `fps_mode=0` | `nvn: present pacing off`；fps 随负载变化；不撕裂/不卡死 |
| DV-09 | 刷新率跟随系统 | `refresh_hz=0` | 日志 `follow system`，面板保持系统值 |
| DV-10 | 面板 144Hz | `refresh_hz=144` | 请求 144；`dumpsys display` 确认；记录发热 |
| DV-11 | 热保护 | 连续跑 20-30 min，`thermal_guard=1`、`thermal_log=1` | 日志出现热状态升级 → fps 降档 60 → 30；**不出现**卡死/闪退；表面温度记录 |
| DV-12 | DRS | `perf_mode=2k90` + `dyn_res=1` | 帧时间劣化 2 s 后渲染高度 −10%；恢复后 +5%；无来回抖动 |
| DV-13 | `thermal_guard=0` | 同上但 `=0` | 只记录不动作：渲染高度/fps 不变 |
| DV-14 | 内存 | 2K/2.5K 长跑 | `adb shell dumpsys meminfo <pkg>` 无持续增长、无 OOM |
| DV-15 | HUD 对位 | 2K 档看 UI | HUD 不错位（风险 §5 "解除 1080 上限后 HUD 错位"）；必要时 `NX_HUD_1080` |
| DV-16 | 后台/前台恢复 | 游戏中切后台再回来 | 窗口/交换链重建后档位仍生效（`setBuffersGeometry` 行再次出现且尺寸正确） |

### 5.4 刷新率查询

```powershell
adb shell dumpsys display | Select-String -Pattern "fps|refresh"
# 或
adb shell dumpsys SurfaceFlinger --latency
```

## 6. 回归矩阵

上游参考设备是 **Lenovo Tab M11（Adreno 610）**。`balance` 必须在每台设备上**等于上游**。

| 设备 / GPU | 预期 `balance` 行为 | 必须额外验证 |
|---|---|---|
| **Adreno 610**（Lenovo Tab M11，1920×1200，90Hz 面板） | 渲染高度 **1080**（`min(0.85, 1080/1920)` → 1080）；60Hz vsync；FIFO；**与上游逐字节一致** | DV-01；确认 `2k/2k90/ultra` 不会因为默认值意外打开（档位默认关）；`allow_over_1080` 默认 0 |
| **Adreno 6xx**（如 2400 高屏） | 渲染高度 **1080**（夹子生效）；60Hz；FIFO | DV-03（夹子还在）；不得出现 1440 渲染 |
| **Mali**（G715 等，2560 高屏） | `default_render_scale` = 1.0，但高度仍被夹到 **1080**；FIFO | mailbox 可能不支持 → AT-13 回退链；`dumpsys` 确认实际 present mode |
| **Adreno 840**（一加平板 3 Pro，3.4K 144Hz） | 缩放 1.0；默认仍 1080（balance）；FIFO | DV-02..DV-16 全部；2K/2.5K/90/120 档 |

> 矩阵里每一格都必须附"低端设备回归说明"（CONTRIBUTING-FORK.md §4）：
> 为什么 Adreno 610/6xx 不会变慢或变糊。结论应基于：档位默认关闭 + 1080 夹子仍在 + balance 预设值等于上游。

## 7. 通过标准（Definition of Done）

1. `tools/verify_perf_settings.ps1` exit 0（FAIL = 0）。
2. `ctest` 全绿，`perf_profile_test` 0 失败。
3. §3 的 31 条对抗性用例全部有明确结论（本机可跑的必须跑过并贴输出；真机的标"未验证"）。
4. CI 上 arm64 NDK 构建 + APK 打包绿。
5. `docs/PERF-TEST-REPORT.md` 里**没有**任何未经执行的"通过"。
