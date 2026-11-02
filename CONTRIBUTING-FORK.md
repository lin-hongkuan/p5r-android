# 本 fork 的协作与文档规范（OnePlus Pad 3 Pro 优化分支）

本文件是本次优化工作的**团队契约**。任何人（人或 agent）在本 fork 里改代码前先读这里。

- 上游：`Brandon04Gonzalez/p5r-android`（MIT）
- 本 fork：`lin-hongkuan/p5r-android`，工作分支 `feat/op3pro-2k90`
- 目标设备：一加平板 3 Pro / 骁龙 8 Elite Gen 5 (SM8850) / Adreno 840 / 3.4K 144Hz / 16GB
- 目标：在**不破坏低端设备**的前提下，提供 2K 渲染与 90/120fps 路径，帧率策略可自选（无上限 / 增强 / 锁 60 / 90 / 120）

## 1. 目录约定

| 路径 | 内容 | 谁写 |
|---|---|---|
| `docs/DEVICE-ONEPLUS-PAD-3-PRO.md` | 设备与 SoC 调研 | research-device |
| `docs/OPTIMIZATION-RESEARCH-EMULATORS.md` | 模拟器/Adreno 调研 | research-emu |
| `docs/PERF-DESIGN.md` | 最终设计说明（唯一权威设计文档） | lead |
| `docs/PERF-TEST-PLAN.md` | 测试计划与用例 | qa-test |
| `docs/PERF-TEST-REPORT.md` | 测试结果报告 | qa-test |
| `docs/BUILD-FORK.md` | 本 fork 的构建与发版说明 | build-ci |

调研文档只写调研结论，**设计决策一律回写到 `docs/PERF-DESIGN.md`**，避免多处描述冲突。

## 2. 代码所有权（写范围不重叠）

| 范围 | 负责人 | 说明 |
|---|---|---|
| `runtime/src/perf_profile.*`（新增）、`runtime/src/perf_hint.cpp`、`runtime/src/platform_android.cpp`（仅性能/分辨率/刷新率相关函数） | perf-core | 核心性能档位与刷新率 |
| `runtime/src/nvn/nvn_queue.cpp`（pacing）、`runtime/src/hle_vi.cpp`（vsync 源）、`runtime/src/vk_swapchain.cpp`（present mode） | frame-pacing | 帧率/呈现 |
| `games/p5r/game.cpp`（fps 目标与 interval） | p5r-glue | 游戏侧帧率策略 |
| `games/p5r/java/**`、`games/p5r/AndroidManifest.xml` | launcher-ui | 启动器 UI 与设置项 |
| `tools/**`、`.github/workflows/**`、`android/build_apk.*` | build-ci | 构建与 CI |
| `tests/**`、`docs/PERF-TEST-*` | qa-test | 测试 |

**跨范围修改必须先找 lead 协调**，禁止两人同时改同一文件。

## 3. 硬性规则（避免 bug）

1. **默认行为不变**：所有新特性必须默认关闭或默认等于上游行为。上游设备（Adreno 610 等）跑出来的结果必须和优化前一致。
2. **所有新开关都要有环境变量/配置项**，并写进 `docs/PERF-DESIGN.md` 的开关表。
3. **禁止在 `config.txt` 解析路径上做热路径工作**：`setting()` 每次都开文件，只许在初始化时调用一次并缓存。
4. **新增 `#if defined(__ANDROID__)` 代码必须同时给出非 Android 的空实现**，否则 PC/macOS 构建会挂（CI 会验证）。
5. **不得引入新的第三方依赖**（除已有 `third_party/`）。
6. **不得提交**游戏文件、keys、生成的 `private/` 内容。
7. 每个改动点都要在 `docs/PERF-DESIGN.md` 里登记：文件 → 函数 → 开关 → 默认值 → 影响设备。

## 4. 测试规则

### 4.0 本机可用工具链（重要，别用错编译器）

| 工具 | 路径 | 说明 |
|---|---|---|
| **g++ 15.2（唯一支持 C++20 的）** | `E:\MSYS2\mingw64\bin\g++.exe` | **必须先 `$env:PATH = "E:\MSYS2\mingw64\bin;" + $env:PATH`**，否则进程直接 exit 1 且无任何输出（DLL 加载失败，不是编译错误） |
| g++ 8.1 | `G:\mingw64\bin\g++.exe` | **不支持 `-std=c++20`，不要用** |
| Vulkan 头文件 1.4.357 | `private/third_party/Vulkan-Headers-1.4.357/include` | 已下载，含 `vk_video/`；语法检查 nvn/*.cpp 必需 |
| python | `C:\Users\lin20\AppData\Local\Programs\Python\Python312\python.exe` | |

语法检查模板（nvn 文件也能查）：
```powershell
$env:PATH = "E:\MSYS2\mingw64\bin;" + $env:PATH
cd <repo>
cmd /c "g++.exe -std=c++20 -fsyntax-only -Iruntime/include -Iruntime/src -Iruntime/src/nvn -Iprivate/third_party/Vulkan-Headers-1.4.357/include runtime/src/你的文件.cpp > o.txt 2>&1"
```
注意：**不要加 `-Wextra`**，上游 Vulkan 结构体初始化会刷几百条 `-Wmissing-field-initializers`，掩盖真正的错误。

- 每次提交前必须跑 `pwsh tools/fork_selftest.ps1`（本地静态+单元测试，见下）。
- CI（`.github/workflows/build-apk.yml`）必须绿：NDK 编译 arm64 + host 编译 + 单元测试。
- 涉及渲染的改动必须附"低端设备回归说明"：为什么 Adreno 610/6xx 不会变慢或变糊。
- 无法在本机验证的（真机帧率），必须在 `docs/PERF-TEST-REPORT.md` 标 **未验证**，并给出真机验证步骤。

### 4.1 对抗性验证（强制）

**每个模块交付前，必须自己先当"敌人"攻击自己的代码**，并把攻击结果写进 `docs/PERF-TEST-REPORT.md`：

1. **默认零变更攻击**：不写任何 config.txt 键时，逐行对照上游分支（`git diff origin/main -- 文件`），证明行为未变。
2. **恶意输入攻击**：空值、`0`、负数、超大字（`999999`）、非数字（`abc`）、`1440x`、空白、大小写、带尾随空格/CRLF——都必须回退默认，不得崩溃或产生非法尺寸。
3. **极端组合攻击**：`fps=120` + `refresh=60`；`render_height=1600` + `refresh=30`；`present_mode=mailbox` 但 GPU 不支持；`allow_over_1080=1` 但 `render_height=0`；`dyn_res` 下限 > 设定高度。
4. **平台攻击**：非 Android 平台（Windows/Linux/macOS）编译路径必须存在且行为退化安全。
5. **热与内存攻击**：2K 下每个 render target 的显存占用要算出来（2560x1440 RGBA8 = 14.7 MB），乘以同屏 target 数，确认不会 OOM。
6. **回退攻击**：任何一个新开关设为 0/关闭后，系统必须回到上游行为，不留半开状态。
7. **并发攻击**：`profile()` 是函数内 static，必须线程安全；不得有跨线程未同步状态。

发现问题的模块负责人必须先修再交付，不许"已知问题留待后续"。

## 5. 提交规范

```
<scope>: <一句话>

- 为什么改
- 改了哪些文件/函数
- 开关与默认值
- 测试：跑了什么，结果
```

scope 取值：`perf`、`pacing`、`p5r`、`launcher`、`build`、`docs`、`test`。
