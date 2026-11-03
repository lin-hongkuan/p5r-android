# 本 fork 的构建与发版说明（P5R / 一加平板 3 Pro 优化分支）

本文件讲**怎么拿到能装到平板上的 APK**，以及装完之后怎么验证性能开关真的生效了。

- 分支：`feat/op3pro-2k90`
- 目标设备：一加平板 3 Pro（骁龙 8 Elite Gen 5 / Adreno 840 / 3.4K 144Hz / 16GB）
- 设计依据：`docs/PERF-DESIGN.md`（开关表在 §2，预设见 §3，热管理见 §4.5）
- 协作规范：`CONTRIBUTING-FORK.md`

> **这个 APK 只供你本人使用。** 它跑的是**你自己 dump 出来的游戏代码**（NCE 模式：APK 里没有游戏代码，
> 运行时直接跑 `exefs/main`），不含任何游戏数据、密钥或 `private/` 内容。不要分发。

---

## 0. 三种拿 APK 的方式，怎么选

| 方式 | 需要什么 | 耗时 | 适合 |
|---|---|---|---|
| **A. GitHub Actions 云构建**（推荐） | 只要浏览器和 GitHub 账号 | 首次约 40–60 分钟（含 SDK/NDK 下载），之后 20–30 分钟 | **没有 Android SDK 的机器**，或者只想拿 APK |
| **B. 本地 Windows** | Android Studio（或 SDK + JDK 17/21）、CMake、Ninja、Vulkan 头文件 | 首次 1–2 小时 | 要改代码、要调试 |
| **C. 本地 Linux / macOS** | 同上，另加 `bash` 环境 | 首次 1–2 小时 | 同上，或做 CI 复现 |

三条路产出的 APK **是同一个东西**：CI 直接调用 `android/build_apk.sh --game p5r --nce`，
和你本地敲的命令完全一致（这是故意的，见 §1.3）。

---

## 1. 方式 A：GitHub Actions 云构建（不需要本地 SDK）

### 1.1 触发

推到 `feat/**` 分支会自动触发；也可以在网页上手动触发并选择参数：

1. 打开仓库的 **Actions** 页面 → 左侧选 **build-apk** → 右上 **Run workflow**。
2. 分支选 `feat/op3pro-2k90`。
3. 两个可选项：

| 参数 | 默认 | 含义 |
|---|---|---|
| `build_harness` | `true` | 是否顺便构建 `libspirvharness.so`（着色器翻译器）。它需要 .NET 10 NativeAOT + NuGet，慢且可能失败；**失败不影响出 APK**，见 §1.4 |
| `release_build` | `false` | 公开发布版（`NX_RELEASE=ON`：不写 `nx.log` 文件，诊断只走 logcat）。日常调试用 `false`，要抓 `nx.log` 就别开 |

### 1.2 下载产物

构建完成后，进入那次 run 的页面，底部 **Artifacts** 区域：

| 产物 | 内容 |
|---|---|
| `p5r-nce-apk` | **`p5r.apk`（要的就是这个）** + 一个 `README.md` 说明 |
| `spirv-harness` | `libspirvharness.so`（只有 `build_harness` 成功时才有） |
| `debug-keystore` | `debug.keystore`（签名用的调试证书，见 §1.5） |
| `build-log` | 只有失败时才有：构建目录、CMakeCache、ninja 日志 |

产物保留 30 天。下载下来是个 zip，解压出 `p5r.apk`。

### 1.3 为什么 CI 和本地不会走偏

workflow 里**只做环境准备**（装 SDK/NDK、下 Vulkan 头文件、恢复缓存），
打包那一步是：

```bash
bash android/build_apk.sh --game p5r --nce
```

和你在本地敲的是**同一条命令**。所以 CI 能出包，本地就能出包；本地出的问题，CI 也会出。

### 1.4 关于 `libspirvharness.so`（重要）

`libspirvharness.so` 是**着色器翻译器**：把游戏里的 Maxwell 着色器翻成 SPIR-V。
APK 里的"准备游戏"（Prepare the game）这一步靠它工作。

- **有它**：在平板上点"准备游戏"，导入 NSP/XCI + `prod.keys`，它会自动提取并翻译着色器，然后就能玩。
- **没它**：APK 照样装、照样能开，但"准备游戏"会在翻译阶段失败，报
  `the shader translator is missing`。**只有当你已经把 `spirv/` 目录拷进
  `Internal storage/Persona 5 Royal/` 时，游戏才启动得起来。**

CI 里它的构建是**尽力而为**：

- `harness` job 用仓库自带的 `games/p5r/tools/build_harness_android.sh`（原样不改）在 runner 上直接跑，
  不用 Docker（runner 本身就是 NativeAOT for linux-bionic 需要的 Linux x64 宿主）。
- 它要 .NET 10 SDK + NuGet + 从 Codeberg 下 Ryubing 源码 + NDK r28c，任一环节挂掉都算正常。
- **挂了之后 `apk` job 仍然会跑**（`needs: harness` + `if: always()`），只是不带翻译器，
  并在 job summary 和 `::warning::` 里用大写警告说明。

**自己补一个翻译器**（两种办法）：

```bash
# 办法 1：有 Docker 的机器
docker run --rm -v "$PWD:/src" -w /src mcr.microsoft.com/dotnet/sdk:10.0 \
  sh games/p5r/tools/build_harness_android.sh
# 产出 private/build/spirv_harness_android/libspirvharness.so
```

```bash
# 办法 2：Linux 上直接跑（需要有 Ryubing 源码在 private/third_party/ryubing，见 docs/nvn/TRANSLATOR.md）
sh games/p5r/tools/build_harness_android.sh
```

拿到 `.so` 之后**重跑一次 `android/build_apk.sh --game p5r --nce`**，它会自动打进 APK。
（脚本会检测这个文件：有就打包，没有就打警告。）

### 1.5 签名与覆盖安装

APK 用 Android 的**调试证书**签名（`~/.android/debug.keystore`，口令都是 `android`）。
CI 会把它缓存并在多次构建之间复用，所以你连续两次下载的 APK 签名一致，可以 `adb install -r` 直接覆盖升级。

如果签名不一致（比如你在本地用另一台机器生成的 keystore 打过包），先卸载再装：

```bash
adb uninstall io.github.p5rrecomp
adb install p5r.apk
```

`debug-keystore` 产物可以下载下来放到本地的 `~/.android/debug.keystore`，这样本地和 CI 的包可以互相覆盖。

---

## 2. 方式 B：本地 Windows 构建

### 2.1 一次性准备

| 组件 | 版本 | 说明 |
|---|---|---|
| Android SDK | cmdline-tools | 有 Android Studio 就有；`ANDROID_HOME` 指过去 |
| NDK | **28.2.13676358** | 版本写死在脚本里，必须一致 |
| build-tools | **35.0.0** | 同上 |
| platform | **android-35** | `android.jar` 从这来 |
| JDK | 17 或 21 | `apksigner`/`d8` 要它；Android Studio 自带的 `jbr` 也行 |
| CMake + Ninja | 任意较新版本 | 驱动 NDK 构建 |
| Vulkan 头文件 | **1.4.357** | `VULKAN_SDK` 必须指向一个含 **`Include/`（大写 I）** 的目录 |
| .NET 10 SDK | 10.0.x | **只在要自己构建翻译器时需要**（§1.4） |

用 `sdkmanager` 装（Android Studio 的 SDK Manager 也可以勾）：

```powershell
$env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk"
& "$env:ANDROID_HOME\cmdline-tools\latest\bin\sdkmanager.bat" `
    "ndk;28.2.13676358" "build-tools;35.0.0" "platforms;android-35" "platform-tools"
```

Vulkan 头文件：

```powershell
git clone --depth 1 --branch v1.4.357 https://github.com/KhronosGroup/Vulkan-Headers `
    private\third_party\Vulkan-Headers-1.4.357
mkdir private\third_party\vk-android -Force
Copy-Item -Recurse private\third_party\Vulkan-Headers-1.4.357\include private\third_party\vk-android\Include
$env:VULKAN_SDK = "$PWD\private\third_party\vk-android"
```

> **注意**：`VULKAN_SDK` 下的目录名必须是 `Include`（大写 I），这是 `CMakeLists.txt` 里
> `"${NX_VULKAN_SDK}/Include"` 直接拼出来的。Linux 上大小写敏感，写错了会报 `vulkan.h not found`。

### 2.2 构建

```powershell
cd <仓库根目录>
. .\dev.ps1                                        # 可选：把 cmake/ninja 放进 PATH
android\build_apk.ps1 -Game p5r -Nce
```

产物：`private\p5r\apk_nce\p5r.apk`

### 2.3 常用变体

```powershell
android\build_apk.ps1 -Game p5r -Nce -Release      # 公开发布版：不写 nx.log
android\build_apk.ps1 -Game p5r -Nce -Install      # 装完自动 adb install -r
android\build_apk.ps1 -Game p5r -Nce -TestPackage  # 包名加 .nce、标题加 (NCE)，可与正式包共存
```

---

## 3. 方式 C：本地 Linux / macOS 构建

### 3.1 一次性准备

```bash
# Linux（Debian/Ubuntu 系）
sudo apt install -y cmake ninja-build openjdk-21-jdk zip unzip
# macOS
brew install cmake ninja openjdk@21 zip
```

SDK + NDK + build-tools（版本和 §2.1 完全一致）：

```bash
export ANDROID_HOME="$PWD/private/third_party/android-sdk"
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64    # macOS: $(brew --prefix openjdk@21)
mkdir -p "$ANDROID_HOME/cmdline-tools"
# 下载 commandlinetools-linux-*.zip（macOS 用 -mac-），解压后放到 cmdline-tools/latest/
curl -sSL -o /tmp/clt.zip "https://dl.google.com/android/repository/commandlinetools-linux-13114758_latest.zip"
unzip -q /tmp/clt.zip -d /tmp/clt
mv /tmp/clt/cmdline-tools "$ANDROID_HOME/cmdline-tools/latest"
yes | "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" --licenses
"$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" \
    "ndk;28.2.13676358" "build-tools;35.0.0" "platforms;android-35" "platform-tools"
```

Vulkan 头文件（Linux 大小写敏感，`Include` 必须大写 I）：

```bash
curl -sSL -o /tmp/vk.tar.gz \
  "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.357.tar.gz"
tar -xzf /tmp/vk.tar.gz -C /tmp
mkdir -p private/third_party/vk-android
cp -r /tmp/Vulkan-Headers-1.4.357/include private/third_party/vk-android/Include
export VULKAN_SDK="$PWD/private/third_party/vk-android"
```

### 3.2 构建

```bash
bash android/build_apk.sh --game p5r --nce
# 产物：private/p5r/apk_nce/p5r.apk
```

`--nce` 是**本 fork 新增的**（上游的 `build_apk.sh` 只有 recompiled 路径，P5R 用不了）。它和
`build_apk.ps1 -Nce` 一一对应：

| | `build_apk.ps1 -Nce` | `build_apk.sh --nce` |
|---|---|---|
| 构建目录 | `private\<game>\ba_nce` | `private/<game>/ba_nce` |
| 产物目录 | `private\<game>\apk_nce` | `private/<game>/apk_nce` |
| CMake | `-DNX_NCE=ON` | `-DNX_NCE=ON` |
| 打包的库 | `libp5r.so`、`libp5rimport.so`、4 个 adrenotools hook、`libspirvharness.so`（有就带） | 同左 |
| 需要 `private/<game>/spirv` | 不需要（NCE 在设备上翻译） | 不需要 |
| keystore | 必须已存在 | 不存在时用 `keytool` 自动生成 |

**默认行为没变**：不加 `--nce` 时，`build_apk.sh` 和上游逐字一致（`private/<game>/ba`、`private/<game>/apk`）。

### 3.3 不带参数的老用法（hades / wonder）

```bash
bash android/build_apk.sh                       # 默认 game=hades
bash android/build_apk.sh --game wonder         # wonder 的 launcher 路径
LAUNCHER_ONLY=1 bash android/build_apk.sh --game wonder   # 只要 launcher，不含 lib
```

---

## 4. 装到平板上

### 4.1 用 adb（推荐，能看日志）

```bash
adb devices                      # 确认平板已连接（需在开发者选项里开 USB 调试）
adb install -r p5r.apk
```

`-r` 是覆盖安装；签名不一致时报 `INSTALL_FAILED_UPDATE_INCOMPATIBLE`，先
`adb uninstall io.github.p5rrecomp` 再装。

### 4.2 不用 adb（拷贝安装）

1. 把 `p5r.apk` 拷到平板（U 盘、微信文件传输、网盘都行）。
2. 用文件管理器点开它。
3. 第一次会提示"未知来源"，允许你用的那个文件管理器"安装未知应用"。
4. 装完桌面出现 **P5R** 图标。

### 4.3 首次运行要做的两件事

1. **给"所有文件访问"权限**。启动器会主动申请（跳系统设置页，找到 P5R 打开开关）。
   没有这个权限，它读不到 `Internal storage/Persona 5 Royal/`。
2. **准备游戏数据**。在 `Internal storage/Persona 5 Royal/` 下需要：

```
Internal storage/Persona 5 Royal/
├── exefs/            # 你 dump 的 exefs（main、sdk 等），NCE 直接跑它
├── extract/romfs/    # RomFS 解出来的文件
├── prod.keys         # 你的 prod.keys
└── spirv/            # 翻译好的着色器（"准备游戏"会自动生成）
```

用启动器里的 **"准备游戏"** 从 NSP/XCI 自动导入并翻译（需要 APK 里带 `libspirvharness.so`，见 §1.4）。

---

## 5. 抓日志（adb）

### 5.1 实时看

```bash
adb logcat -c                                  # 先清空，避免旧日志干扰
adb logcat | grep -iE 'perf:|thermal:|vi:|nvn:' # 只看关心的行
```

平板上启动游戏，日志就会滚出来。

### 5.2 存成文件

```bash
adb logcat -d > p5r-logcat.txt                 # -d 是把当前缓冲全部倒出来
```

### 5.3 用启动器自带的导出

启动器里有 **"导出日志"**：它会打包 `logcat -d -t 500` + `nx.log` + `nx.log.prev` 成一个 zip，
存到 `Download/`。没有 adb 的时候用这个最方便。

### 5.4 `nx.log` 在哪

`Internal storage/Persona 5 Royal/nx.log`（上一次的在 `nx.log.prev`）。
**公开发布版（`NX_RELEASE=ON`）不写这个文件**，只有 logcat。

### 5.5 崩溃栈

```bash
adb logcat -d | grep -A 40 'signal 11\|Fatal signal\|backtrace'
```

---

## 6. 验证性能开关真的生效了

### 6.1 开关写在哪

启动器的设置项会写进 `Internal storage/Persona 5 Royal/config.txt`，格式是 `KEY=value` 一行一个。
也可以手动改（改完重启游戏）。完整的键、取值、默认值见 `docs/PERF-DESIGN.md` §2 和 §4.5.2。

一加平板 3 Pro 推荐（启动器里有"一键 2K90"按钮，等价于下面这组）：

```ini
perf_mode=2k90
fps_mode=90
refresh_hz=90
render_height=1440
present_mode=mailbox
thermal_guard=1
dyn_res=1
dyn_res_min=1080
```

> 环境变量形式 `NX_<KEY>` 优先级高于 `config.txt`，那是给 PC 调试用的；平板上用 `config.txt`。

### 6.2 启动日志里的"证据行"

游戏一启动，日志开头就会打印解析结果。**这是判断开关是否生效的第一手证据**：

```
perf: perf_mode=2k90 (fps 90, refresh 90 Hz, vsync 90 Hz, render height 1440, present mailbox)
perf: allow_over_1080=1 adpf=1 uncap_present=0
```

逐项对照：

| 日志字段 | 对应开关 | 期望 |
|---|---|---|
| `perf_mode=` | `perf_mode` | 你选的预设；没写就是 `balance` |
| `fps 90` | `fps_mode` | 你的目标帧率；`0` 表示无上限 |
| `refresh 90 Hz` | `refresh_hz` | 请求的刷新率；`follow system` 表示跟随系统 |
| `vsync 90 Hz` | 生效的呈现节拍 | = `refresh_hz`，没写就是 60（上游行为） |
| `render height 1440` | `render_height` | 渲染高度；`from render scale` 表示按 GPU 自动 |
| `present mailbox` | `present_mode` | `fifo`（上游默认）/ `mailbox` / `immediate` |
| `allow_over_1080=1` | `allow_over_1080` | 解除 1080 高度上限 |
| `adpf=1` | `adpf` | Android 性能提示开 |
| `uncap_present=0` | `uncap_present` | 1 表示关闭 present pacing |

```bash
adb logcat -d | grep '^.*perf:' | head -5
```

**如果这行不出现**，说明 `config.txt` 没被读到——检查权限和路径。

### 6.3 热管理日志

```
thermal: guard 1 (standard), dynamic resolution on, floor 1080, ceiling 1440, target 90 fps
```

- `guard 0 (record only)`：`thermal_guard=0`，只记录不动作（测试用）。
- `guard 2 (aggressive)`：`thermal_guard=2`，更早降档。
- `floor`/`ceiling`：DRS 能降到的下限和设定的上限。

热降档发生时会再打一行（降到 60 或 30），用：

```bash
adb logcat -d | grep -i 'thermal:'
```

### 6.4 显示链路

```
vi: display at 90 Hz (refresh_hz; fps_mode 90), present unit 11.111 ms
```

`present unit` = 1000/hz 毫秒。如果这里还是 `16.667 ms`，说明 `refresh_hz` 没生效（多半还是 60Hz）。

### 6.5 实测帧率

日志之外，最直接的验证：

```bash
# 系统级的帧率/掉帧统计（需要开"GPU 呈现模式分析"或用 SurfaceFlinger 数据）
adb shell dumpsys gfxinfo io.github.p5rrecomp framestats | head -40
```

或者在平板开发者选项里打开 **"显示刷新率"** 叠加层，直接看屏幕是不是跑在 90/120Hz。

### 6.6 静态检查（改完代码先跑这个）

```powershell
pwsh tools\fork_selftest.ps1            # 静态检查 + host 构建 + ctest
pwsh tools\fork_selftest.ps1 -SkipBuild # 只跑静态检查（几秒钟）
```

它会检查：`#if defined(__ANDROID__)` 块有没有非 Android 分支、workflow YAML 是否合法、
**每个性能开关是否"有文档 / 被解析 / 有日志"**、有没有误提交游戏文件、单测是否注册。
`-SkipBuild` 时找不到 C++20 编译器会 **SKIP 而不是 FAIL**，并告诉你怎么装。

---

## 7. 故障排查

| 现象 | 原因与处理 |
|---|---|
| CI 报 `Could NOT find SDL3` | 不应该发生：host-tests 走的是 `NX_TESTS=ON` 的早退路径（无游戏代码时只构建单测），不需要 SDL3。如果出现了，说明有单测目标开始依赖 SDL3，去看那个 `add_executable` |
| CI 报 `vulkan.h not found` | Vulkan 头文件没放对：`VULKAN_SDK` 下必须是 `Include/`（大写 I） |
| CI 报 NDK/build-tools 找不到 | 必须是 `ndk/28.2.13676358` 和 `build-tools/35.0.0`，版本不能差 |
| CI 报 `sdkmanager: command not found` | cmdline-tools 没放到 `cmdline-tools/latest/`（`sdkmanager` 要求这个层级） |
| APK 里没有 `libspirvharness.so` | `harness` job 失败了。看它的日志；或按 §1.4 本地构建后重跑 |
| 装完点"准备游戏"报 `the shader translator is missing` | 同上：APK 里缺翻译器 |
| 装不上，`INSTALL_FAILED_UPDATE_INCOMPATIBLE` | 签名不一致：`adb uninstall io.github.p5rrecomp` 后重装 |
| 游戏起不来，黑屏 | `spirv/` 缺失或数量不对。看 `adb logcat \| grep -i spirv` |
| 2K 生效了但 HUD 错位 | 已知风险（`docs/PERF-DESIGN.md` §5）。用 `NX_HUD_RES` 调整，或回退 `perf_mode=balance` |
| 90fps 档游戏速度不对 | **预期内**：90/120 是实验档（`docs/PERF-DESIGN.md` §1.1）。游戏逻辑按 1/target 步进，跑不满就慢放。回退 `fps_mode=60` |
| 平板发烫 | 热保护会自动降档（§6.3）。想更早降档：`thermal_guard=2`；想更安静：`perf_mode=balance` |
| 本地 `cmake configure` 失败且**没有任何输出** | 编译器自己跑不起来（缺 DLL / 不在 PATH）。`tools/fork_selftest.ps1` 会把编译器目录加进 PATH 再探测；手动构建时确保用的是 `E:\MSYS2\mingw64\bin` 这类完整工具链，别用老的 mingw64 g++ 8.1（不支持 C++20） |
| 本地 host 构建报 `building the unit tests only` | 正常警告：没有 `private/p5r/gen`，只构建单测 |

---

## 8. 相关文件索引

| 文件 | 作用 |
|---|---|
| `.github/workflows/build-apk.yml` | 云构建 arm64 NCE APK（`workflow_dispatch` + push 到 `feat/**`） |
| `.github/workflows/host-tests.yml` | host 构建 + ctest（不需要游戏数据） |
| `android/build_apk.sh` | Linux/macOS 打包脚本（本 fork 加了 `--nce`） |
| `android/build_apk.ps1` | Windows 打包脚本（`-Nce` 原本就有） |
| `games/p5r/tools/build_harness_android.sh` | 构建 `libspirvharness.so`（Docker / Linux） |
| `games/p5r/AndroidManifest.xml` | 包名 `io.github.p5rrecomp`、minSdk 29、targetSdk 35 |
| `docs/PERF-DESIGN.md` | 性能开关的**唯一权威**文档 |
| `docs/BUILD.md` | 上游的通用构建说明（PC / Android，wonder 为主） |
| `docs/nvn/TRANSLATOR.md` | 着色器翻译器与 Ryubing 源码的准备 |
| `tools/fork_selftest.ps1` | 本地自检：静态检查 + host 构建 + ctest |
