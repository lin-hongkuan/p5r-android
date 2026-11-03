# 性能测试报告（一加平板 3 Pro / 2K90 分支）

> 对应计划：`docs/PERF-TEST-PLAN.md`。分支：`feat/op3pro-2k90`。
> 本报告**只记录真的跑过的东西**，并贴出可核对的输出。凡是没跑过的，一律标 **未验证**。
>
> 最后更新：2026-11-03 03:2x（本机 Windows，g++ 15.2 / g++ 8.1 / CMake 4.1.2 / Ninja）

## 0. 一句话结论

| 项 | 结论 |
|---|---|
| 静态检查 | **不通过（1 个真实缺陷）**：`thermal_log` 有文档、有日志，但**没有任何代码解析它** |
| 单元测试 | **通过**：`perf_profile_test` 190 checks、`perf_thermal_test` 65 checks，ctest 6/6 全绿 |
| 对抗性验证 | **本机可跑的部分通过**：自建 harness 89/89；§3 的 31 条用例中真机项仍 **未验证** |
| host 构建 | **通过**（测试目标）；完整 `libp5r` 构建**未验证 — 本机无 SDL3/Vulkan loader/NDK/游戏数据** |
| arm64 NDK / APK | **未验证 — 需要 CI** |
| 真机（1440p / 90fps / 热保护） | **未验证 — 需要真机** |

## 1. 测试环境（实测，非推测）

| 项 | 实测结果 |
|---|---|
| `G:\mingw64\bin\g++.exe` | **g++ 8.1.0**；不支持 `-std=c++20`（必须 `-std=c++2a`）；PATH 里没有 MSYS2 时**可正常执行** |
| `E:\MSYS2\mingw64\bin\g++.exe` | **g++ 15.2.0**；PATH 未前置时**静默失败**（exit 1、0 字节输出）。必须 `$env:PATH = "E:\MSYS2\mingw64\bin;" + $env:PATH` |
| CMake / Ninja / ctest | 4.1.2 / 可用 / 可用（`E:\MSYS2\mingw64\bin`） |
| Python | 3.12.10 |
| Vulkan 头 | `private/third_party/Vulkan-Headers-1.4.357/include`（含 `vulkan/`、`vk_video/`） |
| PowerShell | 5.1（脚本兼容 5.1+，无需 pwsh 7） |

> **判定规则（本报告全程遵守）**：`exit != 0` 一律 FAIL，**即使没有任何输出**；同时断言产物存在且大小 > 0。
> 这条规则是针对上面 g++ 15.2 静默失败写的——"没有输出"绝不等于"编译通过"。

## 2. 静态检查结果

### 2.1 `tools/verify_perf_settings.ps1` 汇总

```
$ powershell -NoProfile -ExecutionPolicy Bypass -File tools/verify_perf_settings.ps1
Switches in section 2: 15 (perf_mode, fps_mode, refresh_hz, render_height, render_scale,
  resolution, NX_RENDER_RES, uncap_present, allow_over_1080, present_mode, adpf,
  thermal_guard, dyn_res, dyn_res_min, thermal_log)
  of which new in this branch: 10
...
PASS : 93
FAIL : 2
WARN : 1
SKIP : 8
VERDICT: FAIL
exit code: 1
```

**FAIL 明细（2 条，同一个根因）**：

```
- [A1 parsed] thermal_log: no parse site (setting("thermal_log") or its NX_ alias) in any C++ source
- [S10 thermal] thermal_log: documented in 4.5.2 but not read by perf_thermal.cpp
```

**WARN 明细（1 条）**：

```
- [S4 launcher] companion keys: upstream key the launcher keeps in step but the switch table
  does not register: NX_FPS_CAP
```

### 2.2 逐开关三件套（解析 / 文档 / 日志 / 环境变量）

`PASS` = 已验证；`SKIP` = 上游已有开关，本分支不要求为其新增启动日志。

| 开关 | A1 解析 | A2 文档 | A3 日志 | A4 `NX_` 别名 |
|---|---|---|---|---|
| `perf_mode` | PASS `perf_profile.cpp:197` | PASS | PASS | PASS |
| `fps_mode` | PASS `:198` | PASS | PASS | PASS |
| `refresh_hz` | PASS `:200` | PASS | PASS | PASS |
| `render_height` | PASS `:201` | PASS | PASS | PASS |
| `render_scale` | PASS `platform_android.cpp` | PASS | SKIP（上游） | SKIP（无别名） |
| `resolution` | PASS `platform_android.cpp` | PASS | PASS | SKIP（无别名） |
| `NX_RENDER_RES` | PASS `svc_am.cpp` | PASS | SKIP（上游） | SKIP（同名） |
| `uncap_present` | PASS `perf_profile.cpp` | PASS | PASS | PASS `NX_NO_FRAME_LIMIT` |
| `allow_over_1080` | PASS `:203` | PASS | PASS | PASS |
| `present_mode` | PASS `:202` | PASS | PASS | PASS |
| `adpf` | PASS `perf_hint.cpp` | PASS | PASS | PASS `NX_ADPF` |
| `thermal_guard` | PASS `perf_thermal_live.cpp:56` | PASS | PASS | PASS |
| `dyn_res` | PASS `perf_thermal_live.cpp:58` | PASS | PASS | PASS |
| `dyn_res_min` | PASS `perf_thermal_live.cpp:59` | PASS | PASS | PASS |
| **`thermal_log`** | **FAIL** | PASS | PASS | PASS |

> 说明：`NX_PERF_MODE` 这类别名没有字面量，解析器用 `"NX_" + toupper(key)` 通用映射
> （`perf_profile.cpp:34`），脚本识别该模式后判 PASS。

### 2.3 缺陷 D-01：`thermal_log` 有文档、无解析

- **文档**：`docs/PERF-DESIGN.md` §4.5.2 声明 `thermal_log` 默认 `1`，"每 10 s 记录一次"。
- **代码**：`runtime/src/perf_thermal_live.cpp:56-59` 只读了三个开关（`thermal_guard`/`dyn_res`/`dyn_res_min`），
  **没有** `setting_int("thermal_log", ...)`。
- **后果**：`:126-131` 的 10 s 摘要日志**无条件打印**。玩家把 `thermal_log=0` 设上，日志照样每 10 秒一行，
  与文档承诺不符（违反 `CONTRIBUTING-FORK.md` §3.2「所有新开关都要有环境变量/配置项并写进开关表」的一致性要求）。
- **建议**：`Config` 加 `bool log`，`config_from_profile()` 里 `c.log = setting_int("thermal_log", 1) != 0;`，
  10 s 摘要按 `l.cfg.log` 门控；"状态变化立即播报"保持无条件（那是玩家能感知的降级，不该被静音）。
- 已同步给 `perf-core`。

### 2.4 其余静态检查

| 检查 | 结果 | 证据 |
|---|---|---|
| S1 新增 `#if defined(__ANDROID__)` 有非 Android 分支 | **PASS** | `perf_thermal_live.cpp:71` 的 Android 块有 `#else`（`:92`）。`:13` 的 `#include <cstdio>` 是头文件选择器、非代码路径，不要求 `#else` |
| S2 无新增第三方依赖 | **PASS** | `third_party/` 无改动；无 `FetchContent`/`ExternalProject_Add`；无 `.gitmodules` |
| S3 无游戏文件/keys/`private/` 入库 | **PASS** | `git ls-files` 无违禁路径；`git check-ignore -q private/` → 0 |
| S4 文档与启动器一致 | **PASS**（+1 WARN） | 启动器写的 32 个 key 全部在表里；10 个新开关都有入口；`allow_over_1080` 经 `config.put` 派生写入 |
| S5 `balance` == 上游 | **PASS** | 文档 §3 balance 行 = `60 / 0 / auto(FIFO)`；代码 `preset_of(Balance) = {0, F60, 0, Auto}` |
| S6 单测存在并注册 | **PASS** | `tests/perf_profile_test.cpp`、`perf_thermal_test.cpp`；`CMakeLists.txt:65,70` 已注册 |
| S7 无未加保护的 `atoi`/`atof` | **PASS** | `perf_profile.cpp` 全部走 `parse_int`（`errno` + `*end=='\0'`）；`perf_thermal_live.cpp` 有范围夹取 |
| S8 回滚用例存在 | **PASS** | 单测里有空输入 == 上游的断言 |
| S9 `profile()` 并发安全 | **PASS** | 函数内 `static const Profile`（C++11 起线程安全）；热路径无 I/O |
| S10 热开关被 `perf_thermal` 读取 | **1 FAIL** | 见 D-01（`thermal_log`） |
| S11 `NX_FPS_CAP` 只写它能表达的 30/60 | **PASS** | 启动器 `save()` 用 `"30".equals(fps) \|\| "60".equals(fps)` 门控，否则 `remove`；2 处 purge（`save()` + `applyPreset()`）；文档 §3.2 有记载 |
| S12 每个 `Profile` 字段都被**决策**消费（不只是被解析和打日志） | **PASS（8/8）** | 见 §2.6 |

### 2.6 S12：接线 ≠ 引用计数（这条规则抓到了一个真实高危缺陷）

`perf-core` 在自查时发现了一个我最初的检查抓不到的缺陷，值得单独记下来：

> **`Profile::adpf` 被解析、被校验、被写进启动日志，但从未被真正使用。**
> `perf_hint.cpp::api()` 直接读 `getenv("NX_ADPF")`。玩家在 `config.txt` 里写 `adpf=0` 时，
> 因为没有 `NX_` 前缀，`android_main` 不会把它变成环境变量，那个裸 `getenv` 永远看不到 →
> **`adpf=0` 被静默忽略**。而 §2 把它列为 config.txt 开关、§4 声称 `perf_hint_frame()` 消费它，两处都不成立。

`perf-core` 的原话是关键：他先前用"数消费者引用数（`adpf: 2 consumer sites`）"来"验证"，但那个计数
**被解析点和日志行自己满足了**——它从来没证明这个值影响过任何决策。**引用计数不是接线。**

因此新增 S12：对 `Profile` 的每个字段，在**解析器以外**、**排除日志行**、且排除"写入字段本身"的行，
寻找真正读取它的决策点。

| 字段 | 决策点 |
|---|---|
| `adpf` | `perf_hint.cpp:56` `if (!nx::perf::profile().adpf) return x;` |
| `allow_over_1080` | `perf_profile.cpp:346` `if (p.allow_over_1080) return 1.0f;`（`render_scale_clamp`） |
| `fps` | `games/p5r/game.cpp:182` |
| `fps_mode` | `hle_vi.cpp:47` |
| `present_mode` | `perf_profile.cpp:327`（`resolve_present_mode` 的入参） |
| `refresh_hz` | `perf_profile.cpp:260` |
| `render_height` | `perf_profile.cpp:326`（`explicit_render_height`） |
| `uncap_present` | `nvn_queue.cpp:599` `pf.uncap_present \|\| getenv("NX_NO_FRAME_LIMIT")` |

S12 已用**故障注入**证明有效（见 §2.5 第三次注入）：把 `adpf` 那行门控删掉，S12 立刻 FAIL 并指名
`adpf: parsed and logged but never read by a decision`，恢复后文件哈希一致。

### 2.5 验证脚本自检（对抗性：脚本本身不是橡皮图章）

光跑通不算数，得证明它**抓得住缺陷**。做了两次故障注入，每次注入后跑脚本、再恢复并核对哈希：

| 注入 | 脚本反应 | 恢复后 |
|---|---|---|
| 删除 `fps_mode` 解析点（`raw.fps_mode = "";`） | **exit 1**；`A1 parsed fps_mode FAIL` | 文件 SHA256 与注入前一致 `924EF6D3…C09966` |
| 把 `parse_int` 换成裸 `std::atoi` | **exit 1**；`S7 adversarial perf_profile.cpp:250 FAIL` | 同上，哈希一致 |
| 删掉 `perf_hint.cpp` 的 `if (!nx::perf::profile().adpf) return x;` | **exit 1**；`S12 wiring adpf FAIL: parsed and logged but never read by a decision` | `perf_hint.cpp` 哈希一致 |

> 这三次注入也暴露并修掉了脚本自身三处过松的判定：
> 早期把"已文档但未解析"一律降级成 WARN（会漏掉 D-01 这类真实缺陷）；
> 早期 S7 的"附近有 `if (` 就算已保护"过于宽松（`if (true)` 也能骗过）；
> 早期 S12 把"同一行附近有日志"误判为不是决策点（`nvn_queue.cpp:599` 就是决策行，下一行才是日志）。
> 三者都已收紧。

## 3. 单元测试结果

### 3.1 直接编译运行（g++ 15.2，`-Wall`）

```
$ g++ -std=c++20 -O2 -Wall -DNX_PERF_STANDALONE -Iruntime/src \
      tests/perf_profile_test.cpp runtime/src/perf_profile.cpp -o perf_profile_test
BUILD_EXIT=0   （0 warnings）
$ ./perf_profile_test
perf_profile_test: 190 checks, all passed
RUN_EXIT=0
```

```
$ g++ -std=c++20 -O2 -Wall -DNX_PERF_STANDALONE -Iruntime/src \
      tests/perf_thermal_test.cpp runtime/src/perf_thermal.cpp -o perf_thermal_test
BUILD_EXIT=0   （0 warnings）
$ ./perf_thermal_test
perf_thermal_test: 65 checks, all passed
RUN_EXIT=0
```

g++ **8.1**（`-std=c++2a`）同样编译通过：`EXIT=0`，0 warnings。

### 3.2 CMake + ctest

```
$ cmake -S . -B <dir> -G Ninja -DNX_TESTS=ON -DCMAKE_BUILD_TYPE=Release
CMake Warning at CMakeLists.txt:86 (message):
  No generated code in .../private/p5r/gen/main: building the unit tests only
-- Configuring done (3.1s)          CFG=0
$ cmake --build <dir>               BUILD=0
$ ctest --test-dir <dir> --output-on-failure
1/6 Test #1: gpu_pick ............... Passed    0.06 sec
2/6 Test #2: tiling ................. Passed    2.96 sec
3/6 Test #3: astc ................... Passed    0.14 sec
4/6 Test #4: memguard ............... Passed    0.16 sec
5/6 Test #5: perf_profile ........... Passed    0.07 sec
6/6 Test #6: perf_thermal ........... Passed    0.06 sec

100% tests passed, 0 tests failed out of 6
CTEST=0
```

> "building the unit tests only" 是预期行为：本机没有 `private/p5r/gen/main`（游戏数据不入库）。

## 4. 对抗性验证（CONTRIBUTING-FORK.md §4.1）

### 4.1 方法

自建 harness（`private/qa_adv/adv_test.cpp`，**不入库**）直接链接真实 `perf_profile.cpp`，
对 `parse(source_from_map(...))` 喂 89 条断言，覆盖计划 §3 的七类攻击面。

```
$ g++ -std=c++20 -O1 -DNX_PERF_STANDALONE -Iruntime/src adv_test.cpp \
      runtime/src/perf_profile.cpp -o adv_test
BUILD=0
$ ./adv_test
SUMMARY: 89 passed, 0 failed
RUN=0
```

### 4.2 分类结果

| 类别 | 用例数 | 结果 | 关键证据 |
|---|---|---|---|
| 3.1 默认零变化 | 11 | **PASS** | 空输入 → `balance / 60 dflt / refresh 0 / height 0 / auto`、`uncap 0`、`adpf 1`、`allow1080 0`、`vsync 60`、`interval 1` |
| 3.1 balance == 上游（4 档 GPU） | 4 | **PASS** | Adreno 610(1920)/6xx(2400)/Mali(2560)/840(2160) 四种屏高，`resolve_render_height` 与上游 `min(scale,1080/h)` 公式逐一相等（均 1080） |
| 3.2 恶意输入 | 18 | **PASS** | `abc`/`-1`/`999999`/`1440x`/`60.5` 全部回退默认，不崩；`fps_mode=999999`→60、`refresh_hz=-5`→0、`render_height=999999`→0 |
| 3.2 空白与大小写容错 | 5 | **PASS** | `' 2k90 '`、`2K90`、`'1440\r'`（CRLF）、`' MAILBOX '` 均正确识别 |
| 3.3 极端组合 | 7 | **PASS** | `fps120+refresh60`→unit 60/interval 1；`height1600+refresh30`→unit 30；`allow1080=1+height0`→夹子解除但高度仍 0；`fps_mode=0`→uncapped+interval 1 |
| 3.3 覆盖优先级 | 4 | **PASS** | `2k90+fps_mode=60`→fps 60 但 refresh 仍 90、height 仍 1440 |
| 3.5 内存占用 | 5 | **PASS（信息）** | 1280×720=3.52 / 1920×1080=7.91 / **2560×1440=14.06** / 2880×1600=17.58 / 3840×2160=31.64 MiB（RGBA8，单目标） |
| 3.6 回滚 | — | **PASS** | 见 3.1（空输入即回滚态）；单测另有逐项断言 |
| 3.7 并发 | — | **PASS（静态）** | `profile()` 函数内 `static const`；热路径无 `setting()`/`getenv()` |
| **3.5 真机热/内存** | — | **未验证 — 需要真机** | 见 §6 |
| **3.4 NDK/arm64** | — | **未验证 — 需要 CI** | 本机无 NDK |

### 4.3 未覆盖（必须诚实说明）

- 对抗性 harness 只测**纯解析逻辑**。`hle_vi.cpp` 的 vsync 线程、`vk_swapchain.cpp` 的真实 present mode 协商、
  `perf_thermal_live.cpp` 的 `read_thermal_status()`（读 `/sys/class/thermal`）**都只能在真机验证**。
- `perf_thermal_live.cpp:76-79` 自己就写着 `未验证 on the OnePlus Pad 3 Pro`：JNI 到
  `PowerManager.getCurrentThermalStatus()` 从 NativeActivity 的原生线程不可达，走的是 `/sys/class/thermal` 代理，
  各厂商 zone 名不同。**这是真机第一优先验证项。**

## 5. 构建测试

| 编号 | 用例 | 结果 | 说明 |
|---|---|---|---|
| BT-01 | host 单测编译运行 | **PASS** | g++ 15.2 与 8.1 双编译器，见 §3.1 |
| BT-02 | CMake 配置 `-DNX_TESTS=ON` | **PASS** | exit 0（仅单测模式） |
| BT-03 | ctest | **PASS** | 6/6 |
| BT-04 | 改动 runtime 文件语法检查 | **PASS** | `perf_hint.cpp` / `hle_vi.cpp` / `nvn_queue.cpp` / `vk_swapchain.cpp` / `nvn_vk.cpp` 全部 `error:` 0 条（`-fsyntax-only`，绝对路径 `-I`，含 Vulkan-Headers） |
| BT-05 | `platform_android.cpp` 语法检查 | **未验证 — 缺 NDK 头** | `aaudio/AAudio.h` 不存在（需要 NDK）。**不是缺陷**，是环境限制 |
| BT-06 | arm64 NDK 构建 | **未验证 — 需要 CI** | 本机无 NDK |
| BT-07 | APK 打包 | **未验证 — 需要 CI** | 同上 |
| BT-08 | Java 兼容性 | **未验证（他人自测）** | `launcher-ui` 报告 `javac -source 1.8 -target 1.8` exit 0 / 0 warnings / 73 classes，但那是**stub 树**编译，非真 `android.jar`。我未独立复现 |

> **明确跳过**：完整 CMake 构建（`libp5r` 链接）本机**跑不了**——没有 SDL3、没有 Vulkan loader、
> 没有游戏数据、没有 NDK。任何"完整构建通过"的说法都必须来自 CI，不能来自本机。

## 6. 设备验证：全部 **未验证 — 需要真机**

以下是**必须在一加平板 3 Pro 上跑**的项。在有人跑过并贴出 logcat 之前，本报告不判定它们通过。

| 编号 | 用例 | 状态 | 验证步骤（照抄） |
|---|---|---|---|
| DV-01 | balance == 上游 | **未验证 — 需要真机** | 空 `config.txt` → `adb logcat -s p5r`，应见 `perf: perf_mode=balance (fps 60, default, refresh 0 Hz follow system, vsync 60 Hz, render height 0 from render scale, present auto)` |
| DV-02 | 1440p 真的生效 | **未验证 — 需要真机** | `perf_mode=2k` → 日志 `android: setBuffersGeometry 2560x1440 ...`（**不是** 1920x1080） |
| DV-03 | 1080 夹子仍在 | **未验证 — 需要真机** | 只写 `render_height=1440`、不给 `allow_over_1080` → 必须**仍夹到 1080** |
| DV-04 | 刷新率请求 | **未验证 — 需要真机** | `perf_mode=2k90` → `android: window frame rate 90 Hz requested`；`adb shell dumpsys display \| grep -i fps` 确认面板 90Hz |
| DV-05 | present mode | **未验证 — 需要真机** | `perf_mode=2k90` → `vk: ...: present mode MAILBOX (perf present_mode mailbox; surface offers N modes)` |
| DV-06 | 90fps 实测 | **未验证 — 需要真机** | 跑 60 s → `p5r: game clock ... 90.x fps`；**同时**看 `steps` 与 `real` 差值是否 > 2%（慢放） |
| DV-07 | 30/60/90/120 四档 | **未验证 — 需要真机** | 依次设 `fps_mode`，核对日志 fps 与档位 |
| DV-08 | 无上限 | **未验证 — 需要真机** | `fps_mode=0` → `nvn: present pacing off` |
| DV-09/10 | 跟随系统 / 144Hz | **未验证 — 需要真机** | `refresh_hz=0` / `=144` |
| DV-11 | 热保护三层 | **未验证 — 需要真机** | 连续 20-30 min，`thermal_guard=1`+`thermal_log=1`，观察 `thermal: zone N` 升级 → fps 降 60 → 30；记录表面温度 |
| DV-12 | DRS | **未验证 — 需要真机** | `perf_mode=2k90`+`dyn_res=1`，帧时间劣化 2 s 后渲染高度 −10%，恢复 +5%，不得抖动 |
| DV-13 | `thermal_guard=0` | **未验证 — 需要真机** | 只记录不动作 |
| DV-14 | 内存/OOM | **未验证 — 需要真机** | `adb shell dumpsys meminfo <pkg>`，2K/2.5K 长跑无增长无 OOM（参考：1440p 单目标 14.06 MiB） |
| DV-15 | HUD 对位 | **未验证 — 需要真机** | 2K 档看 UI 是否错位 |
| DV-16 | 前后台恢复 | **未验证 — 需要真机** | 切后台再回，`setBuffersGeometry` 尺寸仍正确 |

### 6.1 回归矩阵（全部未验证 — 需要真机）

| 设备 / GPU | `balance` 期望（静态已证） | 真机状态 |
|---|---|---|
| Adreno 610（Lenovo Tab M11，上游参考机） | 渲染高度 **1080**（`min(0.85, 1080/1920)`），60Hz，FIFO | **未验证 — 需要真机** |
| Adreno 6xx（2400 高屏） | 渲染高度 **1080**（夹子生效） | **未验证 — 需要真机** |
| Mali（G715，2560） | 缩放 1.0 但高度仍 **1080**；mailbox 可能不支持 → 回退链 | **未验证 — 需要真机** |
| Adreno 840（一加平板 3 Pro） | 缩放 1.0；默认仍 1080；FIFO | **未验证 — 需要真机** |

> 矩阵四格的"balance 渲染高度"是**纯函数验证**（§4.2 已 PASS，与上游公式逐一相等）。
> 但"真机上确实按这个值渲染"**没有验证**。

## 7. `git diff --stat`

### 7.1 已提交内容（`8767cc2`）

```
$ git show --stat --oneline HEAD
8767cc2 perf: 2K render, selectable frame rate (30/45/60/90/120/uncapped), thermal guard
 CMakeLists.txt                                   |   11 +
 android/build_apk.sh                             |  117 ++-
 docs/DEVICE-ONEPLUS-PAD-3-PRO.md                 |  545 ++++++++++
 docs/OPTIMIZATION-RESEARCH-EMULATORS.md          | 1186 ++++++++++++++++++++++
 docs/PERF-DESIGN.md                              |   63 +-
 docs/PERF-TEST-PLAN.md                           |  313 ++++++
 games/p5r/game.cpp                               |  131 ++-
 games/p5r/java/io/github/p5rrecomp/Lang.java     |  202 ++++
 games/p5r/java/io/github/p5rrecomp/Launcher.java |  255 ++++-
 runtime/src/hle_vi.cpp                           |   52 +-
 runtime/src/nvn/nvn_queue.cpp                    |   34 +-
 runtime/src/nvn/nvn_vk.cpp                       |   30 +-
 runtime/src/perf_hint.cpp                        |   26 +-
 runtime/src/perf_profile.cpp                     |  406 ++++++++
 runtime/src/perf_profile.h                       |  119 +++
 runtime/src/perf_thermal.cpp                     |  140 +++
 runtime/src/perf_thermal.h                       |  118 +++
 runtime/src/perf_thermal_live.cpp                |  167 +++
 runtime/src/platform_android.cpp                 |  127 ++-
 runtime/src/vk_bridge_shared.h                   |   19 +
 runtime/src/vk_swapchain.cpp                     |   82 +-
 tests/perf_profile_test.cpp                      |  461 +++++++++
 tests/perf_thermal_test.cpp                      |  300 ++++++
 tools/verify_perf_settings.ps1                   | 1055 ++++++++++++++++++++
 24 files changed, 5879 insertions(+), 80 deletions(-)
```

### 7.2 工作树未提交改动

```
$ git diff --stat
 docs/DEVICE-ONEPLUS-PAD-3-PRO.md        | 37 +++++++++++++++++++++---------
 docs/OPTIMIZATION-RESEARCH-EMULATORS.md | 40 ++++++++++++++++++++++++++++++++-
 runtime/src/perf_hint.cpp               | 10 +++++++--
 tools/verify_perf_settings.ps1          | 15 +++++++++++++
 4 files changed, 89 insertions(+), 13 deletions(-)
```

## 8. 缺陷清单

| 编号 | 严重度 | 描述 | 状态 |
|---|---|---|---|
| D-01 | 中 | `thermal_log` 文档化但无代码解析；10 s 摘要日志无条件打印，`thermal_log=0` 静音无效 | **未修复**（已通知 `perf-core`，其已独立确认） |
| D-02 | 中 | 启动器曾漏写 `allow_over_1080`，2K 档会静默按 1080 渲染 | **已修复**（`launcher-ui`，已验证 `save()` 有 `config.put`/`remove`） |
| D-03 | 高 | 启动器曾把 `NX_FPS_CAP=fps_mode`，`fps_mode=90` 写成 `NX_FPS_CAP=90` → `game.cpp` 按 `>=60?60:30` **塌成 60**，UI 说 90 实跑 60 | **已修复**（改为仅 30/60 写入、其余 `remove`；`applyPreset()` 另加 purge）。已加 S11 规则固化 |
| D-04 | 中 | 启动器 `perf_mode=2k90` + `render_height=0` 时，`resolution=720p` 会先于 `render_height` 命中 `window_size()`，2K 档静默按 720p 渲染 | **已修复**（auto 高度改为取预设隐含高度） |
| D-05 | 低 | 验证脚本自身三处过松判定（WARN 降级、S7 启发式、S12 日志邻行） | **已修复**（§2.5 的三次故障注入就是为此加的） |
| D-06 | 高 | **`adpf=0` 被静默忽略**：`Profile::adpf` 被解析/校验/记录日志，却从未被消费；`perf_hint.cpp` 只读裸 `getenv("NX_ADPF")`，而 config.txt 的 `adpf` 键没有 `NX_` 前缀、`android_main` 不会把它变成环境变量 | **已修复**（`perf-core` 自查发现并修；现在 `perf_hint.cpp:56` 额外门控 `profile().adpf`，同时保留上游逐字节的 `NX_ADPF[0]=='0'` 判定以免回归）。已加 S12 规则固化 |

> **D-06 的教训（已写进 S12）**：`perf-core` 最初用"数消费者引用"来验证开关接线，
> 而解析点和日志行本身就满足了那个计数——**引用计数不是接线**。必须把值追到决策点。

## 9. 结论与后续

**可以交付的部分**：解析逻辑、默认值等于上游、覆盖优先级、垃圾输入回退、1080 夹子条件解除、
热保护纯决策函数、以及**每个开关都真的接到了一个决策点**——这些都有**真实执行**的单元测试与对抗性
harness 支撑（190 + 65 + 89 checks 全绿），并用 3 次故障注入证明了检查脚本抓得住缺陷。

**不能声称的部分**：

1. `thermal_log` 缺陷未修 → 静态检查仍是 FAIL。
2. **90fps 是否真的在平板上呈现、1440p 是否真的出图、热保护是否真的降温** —— 全部未验证。
3. arm64 NDK 构建与 APK 打包 —— 需要 CI。
4. 完整 `libp5r` 链接 —— 本机环境不具备。

**建议的下一步（按优先级）**：

1. 修 D-01，重跑 `tools/verify_perf_settings.ps1` 至 exit 0。
2. CI 上跑通 BT-06/BT-07。
3. **真机**先验证 `perf_thermal_live.cpp` 的 `/sys/class/thermal` 代理读数是否合理（那是热保护的地基），
   再验证 DV-02（1440p 出图）、DV-06（90fps 与慢放）、DV-11（热保护）。
4. 每台回归设备（尤其 Adreno 610 的 Lenovo Tab M11）跑 DV-01，确认 balance 与上游一致。

---

# 附录 A：云端构建验证（CI，2026-11-03）

此前报告把"arm64 NDK 构建 + APK 打包"列为 **未验证（CI only）**。现已完成并验证。

## A.1 结果

| 项目 | 结果 | 证据 |
|---|---|---|
| host-tests 工作流（ubuntu-latest） | **成功**，ctest **6/6** | run 41586337509 之前的 host-tests 全部 success |
| uild-apk 工作流（ubuntu-latest） | **成功** | run 41586337509，job p5r NCE arm64 APK |
| libspirvharness.so（.NET 10 NativeAOT，android-arm64） | **成功构建** | artifact `spirv-harness` |
| APK 产出 | **p5r.apk，3.4 MB，已签名** | artifact `p5r-nce-apk` |
| APK 内容 | 见 A.2 | python zipfile 逐条校验，`testzip()` 全部条目 OK |
| manifest 包名 | `io.github.p5rrecomp` 存在（UTF-16 检查） | 同上 |

## A.2 APK 内容（已逐条校验）

```
AndroidManifest.xml                          已签名 AXML（包名 io.github.p5rrecomp）
classes.dex                                  0.11 MB  启动器 + 13 键「2K90」预设
lib/arm64-v8a/libp5r.so                      3.19 MB  运行时（全部性能改动）
lib/arm64-v8a/libp5rimport.so                0.95 MB  机内导入器
lib/arm64-v8a/libspirvharness.so             4.36 MB  着色器翻译器（"准备游戏"用）
lib/arm64-v8a/lib{file_redirect,gsl_alloc,main}_hook.so, libhook_impl.so
res/ + resources.arsc + META-INF（ANDROIDD.RSA 签名）
```

## A.3 构建过程中被抓到并修掉的 4 个真 bug

这三个只有真实 CI 才能暴露，本地静态检查全部通过：

| # | 症状 | 根因 | 修复 |
|---|---|---|---|
| B-01 | `host-tests` 失败：`INT_MIN was not declared` | 测试缺 `<climits>`，MSYS2 g++ 隐式包含而 Linux 不包含 | 加 `#include <climits>` |
| B-02 | 装 SDK 步骤 exit 1 | `yes \| sdkmanager --licenses` 中 `yes` 收到 SIGPIPE，`set -o pipefail` 判失败 | 两侧都改为非致命 |
| B-03 | 链接失败：`undefined symbol: nx::thermal::on_frame` | `perf_thermal.cpp` / `perf_thermal_live.cpp` 未加入 `NX_RUNTIME_SOURCES` | 加入 CMake 源列表 |
| B-04 | `d8` 失败：`NoSuchFileException: ./.../Launcher\.class` | d8 的 `@argfile` 传入含 `\$` 的匿名内部类路径 | 先 `jar cf classes.jar` 再喂 d8（标准输入形式） |

## A.4 仍然 未验证（需要真机，无法在 CI 完成）

与 §6 一致：各档位**实际达成帧率**、1440p **实际渲染**（看 `android: setBuffersGeometry 2560x1440`）、
面板是否真的切到 90Hz、MAILBOX 是否协商成功、热保护 L1/L2/L3 与恢复、动态分辨率、
以及 `perf_device.cpp` 依赖的 `ro.soc.model == SM8850`。

真机一次性采集命令：

```powershell
adb shell getprop ro.soc.model; adb shell getprop ro.board.platform; adb shell getprop ro.hardware.egl
adb logcat -c; adb logcat -s p5r:V > p5r-log.txt
# 启动游戏后另开一个窗口：
adb shell dumpsys display | findstr /i "fps refresh"
```

关键日志行（证明各开关生效）：
```
perf: device soc=... -> target tablet (2K90), N setting(s) from the device profile
perf: perf_mode=2k90 (... refresh 90 Hz ...), render height 1440 ...
am: the game renders at 2560x1440 (docked)
android: setBuffersGeometry 2560x1440 (screen 3392x2400) -> 0
p5r: fps mode 90: target 90, present interval 1, display 90 Hz
thermal: guard 1 (standard), dynamic resolution on, floor 1080, ceiling 1440, target 90 fps
```
---

# 附录 B：UI 与诊断改进（第 3 轮）

| 改动 | 文件 | 验证 |
|---|---|---|
| 选项分组标题（文本/画面/性能/画面细节/屏幕控制/渲染开销/手柄与灵敏度） | `Launcher.java` | javac 解析通过（止于 `android.*` 包解析，属本机预期上限）；CI APK 构建成功 |
| 6 个专家开关折叠到「高级性能设置」后面，默认只显示性能模式与帧率 | `Launcher.java` | 同上 |
| 「游戏内：」生效配置回显行，随每次改动刷新 | `Launcher.java` | 同上 |
| 11 条新语言项，全 8 语言（fr/de/it/pt/ja/ko/zh-Hans/zh-Hant） | `Lang.java` | 脚本校验：11 行插入、引号成对、无 U+FFFD、中文表项存在 |
| 呈现间隔直方图由 7 桶改 8 桶（9/12/15/18/24/35/100 ms） | `frame_stats.cpp` | `-Wall -Wextra` 编译干净 |

## B.1 为什么改直方图（真正的深度优化点）

旧分桶 `<14, 14-19, 19-25, ...` 把 **144、120、90、60 fps 全部塞进 `<14 ms` 同一桶**。
也就是说：11.1 ms（90fps）和 16.7 ms（60fps）在日志里**无法区分**——90/120 档位根本没法用日志调优。
新桶边沿能把 144+、120-83、83-67、67-56、56-42、42-29、35-100、>100（卡顿）分开。

## B.2 CI 证据

| run | 内容 | 结果 |
|---|---|---|
| `41589814467` | UI 分组/折叠/回显 + Lang | **成功** |
| `41589897078` | 直方图 8 桶 | **成功**，APK 3.45 MB、24 项、`testzip` 全部 OK |

## B.3 仍未验证

UI 的**实际视觉效果**（分组间距、折叠交互、中文显示）只能在真机看。合并进 APK 只证明它能编译、能打包。