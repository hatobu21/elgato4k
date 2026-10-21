# Vela · Windows 使用说明

把 PS5 / Switch 2 经 Elgato 4K Pro 采集进电脑，在 RTX 5070 Ti 上用 DLSS 帧生成补帧后全屏播放。本文从零讲到能玩：准备、编译、第一次运行、日常使用、出问题怎么查。

> **现状**：源码在云端写成，只做过类型检查和着色器编译，**还没在 Windows 上链接和运行过**。第一次编译大概率会有几处 MSVC 报错要修（见 [第 2 步](#2-编译)）。逐项验收的顺序见 [docs/03 实施计划](docs/03-Windows实施计划.md)。

## 1. 准备

### 软件

| 需要 | 说明 |
|---|---|
| Windows 11 | 只支持 Windows 11 |
| Visual Studio 2022 17.10+（或 Build Tools） | 勾选“使用 C++ 的桌面开发”，含 MSVC v143、Windows SDK 10.0.26100、CMake、Ninja |
| Git | 克隆本仓库和 DLSS SDK |
| NVIDIA 驱动 | 最新 Game Ready 驱动 |
| [DLSS SDK](https://github.com/NVIDIA/DLSS) | `git clone`，提供头文件、`nvsdk_ngx_d.lib`、`nvngx_dlss.dll`、`nvngx_dlssg.dll` |
| Optical Flow SDK 5.x | developer.nvidia.com 下载，解压后应有 `NvOFInterface/nvOpticalFlowD3D12.h` |
| RTX Video SDK | developer.nvidia.com 下载，只要 `nvngx_vsr.dll`、`nvngx_truehdr.dll` 两个文件 |
| Elgato 4K Pro 驱动 | Elgato 官网最新版 |

WebView2 运行库 Windows 11 自带；WebView2 SDK 和 nlohmann/json 由 CMake 自动下载（首次配置需要联网）。

下面假设 SDK 放在：

```
D:\sdk\DLSS
D:\sdk\Optical_Flow_SDK_5.0
D:\sdk\RTXVideo
```

### 系统设置

1. 设置 → 系统 → 屏幕 → 显示卡 → 默认图形设置 → **硬件加速 GPU 计划：开**，然后重启。DLSS 帧生成必须开这个。
2. 设置 → 系统 → 屏幕 → **使用 HDR：开**（HDR 显示器）。
3. NVIDIA 控制面板：G-SYNC 对窗口和全屏模式都开启；垂直同步设为“由 3D 应用程序决定”。

### 主机设置

| 主机 | 设置 |
|---|---|
| PS5 | 视频输出 4K、60 Hz（要 120 Hz 时见 [常见问题](#5-常见问题)），HDR 自动；**音频输出格式选“线性 PCM”** |
| Switch 2 | 电视输出 4K，HDR 开 |

Vela 运行时要关掉 Elgato 4K Capture Utility、OBS 等占用采集卡的程序，采集设备同一时间只能给一个程序用。

## 2. 编译

在“x64 Native Tools Command Prompt for VS 2022”里打开 PowerShell（`powershell`）：

```powershell
git clone https://github.com/hatobu21/elgato4k.git
cd elgato4k
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DVELA_DLSS_SDK=D:/sdk/DLSS -DVELA_NVOF_SDK=D:/sdk/Optical_Flow_SDK_5.0
cmake --build build
ctest --test-dir build        # 核心单元测试，应为 54 个全部通过
```

编译会顺带：用 Windows SDK 的 `dxc.exe` 编译 6 个着色器、把 `ui\` 复制到程序旁边、把两个 DLSS 运行库复制到 `runtime\`。RTX Video 的两个 DLL 要手动复制：

```powershell
copy D:\sdk\RTXVideo\bin\nvngx_vsr.dll     build\win\runtime\
copy D:\sdk\RTXVideo\bin\nvngx_truehdr.dll build\win\runtime\
```

（RTX Video SDK 里 DLL 的具体子目录以你下载的版本为准。）

编译好后的目录应该是这样：

```
build\win\
  vela.exe
  shaders\  CaptureToWorking.cso  Analysis.cso  ColorApply.cso  FlowToMotion.cso  Encode.cso  Compose.cso
  ui\       index.html  app.css  app.js  fonts\
  runtime\  nvngx_dlss.dll  nvngx_dlssg.dll  nvngx_vsr.dll  nvngx_truehdr.dll
```

**第一次编译的已知风险**：云端只能用 MinGW 做类型检查，覆盖不到 MSVC 的 `__try` 异常块、`NvOf.cpp` 和 NVIDIA 静态库的链接，预计会有少量报错。按报错修即可；修完能链接出 `vela.exe` 就算过了这一步。

CMake 报 `Set VELA_DLSS_SDK…` 或 `Set VELA_NVOF_SDK…`：路径没指到 SDK 根目录。DLSS SDK 根目录下应有 `include\nvsdk_ngx.h`，光流 SDK 根目录下应有 `NvOFInterface\`。

## 3. 第一次运行

1. 主机开机，HDMI 接到 4K Pro 的 **输入** 口，4K Pro 用 USB 接电脑。
2. 运行 `build\win\vela.exe`。
3. 打开“设备”页，看最下面的 **系统检查**：GPU、硬件加速 GPU 计划、DLSS 帧生成运行库、DLSS 超分运行库、RTX Video SDK、NVIDIA 光流、Windows HDR、采集卡应该都是绿色。哪项不是，按右边的提示处理。
4. 有画面后按 **Alt+Enter** 或双击画面进入全屏。

设置自动保存在 `%LOCALAPPDATA%\Vela\settings.json`，日志在 `%LOCALAPPDATA%\Vela\logs\vela.log`。

## 4. 使用

### 两种界面

- **窗口模式**：左边是画面，下方是帧间隔曲线，右边是延迟仪表和各功能的标签页。Home 收起 / 展开右栏。
- **全屏模式**：画面铺满，左侧有一条窄栏，显示补帧倍数、延迟、超分、HDR、调色预设，点哪项就在旁边展开它的面板。4 秒不动鼠标，界面和光标都会隐藏，只剩显卡画进视频的延迟角标。动一下鼠标界面就回来。

### 快捷键

| 按键 | 作用 |
|---|---|
| Alt+Enter / 双击画面 | 切换全屏 |
| Esc | 退出全屏 |
| Home | 全屏时显示 / 隐藏界面；窗口时收起 / 展开右栏 |
| F | 开关补帧 |
| 2 – 6 | 补帧倍数 2× – 6× |
| L | 开关延迟角标 |
| M | 静音 |

全屏时用快捷键改设置，只在顶部闪一条提示，不会弹出界面。

### 补帧

- **内容帧率**：游戏帧率低于信号帧率时（比如 60 Hz 信号里跑 30 fps 的画质模式），重复的画面会被跳过，倍数按游戏帧率算：30 fps × 4 = 120 fps。选“自动”会持续检测，约 1 秒跟上切换；HUD 刷新率和画面不一致的游戏如果被干扰，就手动选 30 / 60 / 120。
- **倍数**：可选范围受显示器刷新率和驱动上限限制，不可用的会变灰。
- **帧节奏**：“均匀”最顺；“低延迟”让真实帧更早上屏，代价是间隔略不均匀。
- 推荐 PS5 4K60 + 240 Hz 显示器用 4×，144 Hz 用 2×。

### 延迟监控

- 仪表上的端到端延迟 = 采集卡 + 管线 + 扫描 + 显示器。前后两项软件测不到，没填时显示“未校准”。
- 校准方法（手机慢动作拍两块屏幕对比）见 [docs/03 · P3](docs/03-Windows实施计划.md#p3-延迟监控2-天)，结果填进“延迟”页的“采集卡延迟”和“显示器延迟”。
- 角标位置在“延迟”页选，四个角都可以；它是显卡画进视频的，录屏也能录到。

### 超分辨率与 HDR

全自动，一般不用管：SDR 用 RTX Video 超分，HDR 用 DLSS 超分；HDR 信号直通到 HDR 显示器，SDR 信号可以用 RTX HDR 转成 HDR，HDR 信号接 SDR 显示器时自动色调映射。主机切换 HDR 时 1 秒内自动重建，不用重开。

### 自动调色

1. 打开“调色”页，顶部的 **画面诊断** 会说出当前画面的问题，比如“发灰 → 推荐「去灰」”，点“应用”。
2. 觉得不够或过头，用下面的滑杆微调；改过会显示“已微调 · 恢复预设”。
3. 修正是缓慢渐变的；“适应速度”决定跟随画面变化的快慢。

| 预设 | 对应 |
|---|---|
| 自然 | 日常（默认） |
| 去灰 | 发灰 |
| 压高光 | 过曝 |
| 降饱和 | 过饱和 |
| 提亮暗部 | 偏暗 |
| 去偏色 | 偏色 |

**HDR 信号下只调白平衡和饱和度**，去灰 / 压高光 / 提亮暗部不起作用，也不会被推荐；HDR 画面的亮度和对比度在“HDR”页调。

## 5. 常见问题

| 现象 | 处理 |
|---|---|
| 没有信号 | 主机是否开机；HDMI 是否接在 4K Pro 的输入口；关掉 4K Capture Utility / OBS；换一根 HDMI 2.1 线 |
| 补帧不可用 | 看补帧页的提示：没开硬件加速 GPU 计划（开启后重启）；`runtime\` 里缺 `nvngx_dlssg.dll`；驱动太旧 |
| 倍数被限制 | 显示器刷新率放不下：内容 60 fps × 4 = 240，需要 240 Hz 显示器。可在“设备”页把同步改成“允许撕裂”解除限制 |
| 超分 / RTX HDR 显示“未找到组件” | `runtime\` 里缺 `nvngx_vsr.dll` / `nvngx_truehdr.dll` |
| HDR 画面发灰或过亮 | Windows 的“使用 HDR”是否开启；PS5 的 HDR 调整是否做过 |
| 主机输出 1440p120 时画面不对 | “自动”格式目前固定选 4K60，到“设备”页手动选 2560×1440 · 120 Hz |
| 没有声音或有杂音 | PS5 音频输出改为“线性 PCM”；Vela 只支持 PCM |
| 界面打不开但有画面 | WebView2 运行库缺失或损坏，到微软官网重新安装 “WebView2 Runtime” |
| 其他问题 | 把 `%LOCALAPPDATA%\Vela\logs\vela.log` 发出来 |

## 6. 分发与许可

- 自用可以直接运行编译目录。打包给别人时，带上整个运行目录、`LICENSE` 和 `THIRD_PARTY_NOTICES.md`。
- Vela 移植了 Veyra 的代码，许可证是 GPL-3.0：对外分发程序时必须同时提供源码。
- NVIDIA 的运行库按 DLSS SDK / RTX Video SDK 各自的许可分发。

更多设计细节见 [docs/02 后端架构](docs/02-后端架构.md)。
