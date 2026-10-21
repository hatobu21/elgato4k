# 03 · Windows 实施计划

云端已完成：界面、跨平台核心（34 个单元测试通过）、全部 Windows 源码（MinGW 类型检查 19/19 通过）、6 个着色器（DXC 编译通过）。还没做过的：链接、在 Windows 上运行、接真机测试。下面按“先跑通画面，再逐项加功能”的顺序安排，每个阶段都有可量化的验收标准。

## 0. 环境准备（半天）

| 项 | 版本 / 位置 |
|---|---|
| Visual Studio 2022（17.10 以上）或 Build Tools，勾选“使用 C++ 的桌面开发” | MSVC v143、Windows SDK 10.0.26100 |
| CMake 3.28+、Ninja | VS 自带即可 |
| NVIDIA 驱动 | 支持 DLSS 4.5 的最新 Game Ready 驱动 |
| DLSS SDK | `git clone https://github.com/NVIDIA/DLSS`（头文件、`nvsdk_ngx_d.lib`、`nvngx_dlss.dll`、`nvngx_dlssg.dll`） |
| Optical Flow SDK | developer.nvidia.com 下载 5.x，解压后包含 `NvOFInterface/nvOpticalFlowD3D12.h` |
| RTX Video SDK | developer.nvidia.com 下载，取 `nvngx_vsr.dll`、`nvngx_truehdr.dll` |
| WebView2 Runtime | Windows 11 自带；SDK 由 CMake 从 NuGet 自动下载 |
| nlohmann/json | vcpkg 或 CMake 自动下载 |
| Elgato 4K Pro 驱动 | Elgato 官网最新版；运行 Vela 时关闭 4K Capture Utility / OBS（采集设备是独占的） |

系统设置：

- 设置 → 系统 → 屏幕 → 显示卡 → 默认图形设置 → **硬件加速 GPU 计划：开**（DLSS 帧生成必需），重启。
- 设置 → 系统 → 屏幕 → **使用 HDR：开**（HDR 显示器）。
- NVIDIA 控制面板：G-SYNC 开（窗口和全屏模式），垂直同步“由 3D 应用程序决定”。
- PS5：视频输出 4K、60 Hz 或 120 Hz（见风险 6），HDR 自动；Switch 2：电视输出 4K、HDR 开。

## 1. 构建（半天）

```powershell
git clone https://github.com/hatobu21/elgato4k.git; cd elgato4k
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DVELA_DLSS_SDK=D:/sdk/DLSS -DVELA_NVOF_SDK=D:/sdk/Optical_Flow_SDK_5.0
cmake --build build
ctest --test-dir build                      # 核心单元测试
copy D:\sdk\RTXVideo\bin\nvngx_vsr.dll     build\win\runtime\
copy D:\sdk\RTXVideo\bin\nvngx_truehdr.dll build\win\runtime\
build\win\vela.exe
```

运行目录：

```
vela.exe
shaders\  CaptureToWorking.cso Analysis.cso ColorApply.cso FlowToMotion.cso Encode.cso Compose.cso
ui\       index.html app.css app.js
runtime\  nvngx_dlss.dll nvngx_dlssg.dll nvngx_vsr.dll nvngx_truehdr.dll
```

预计第一次编译会有少量 MSVC 特有的报错（MinGW 检查覆盖不到 `__try` SEH 块、`NvOf.cpp`，以及 NVIDIA 静态库的链接）。修完即为本阶段验收。

## 2. 分阶段实施

每个阶段结束都看界面“延迟”面板的数字，并保存 `%LOCALAPPDATA%\Vela\logs\vela.log`。

### P1 采集直通（2–3 天）

关闭全部效果，只看画面。

- 设备枚举能找到 “Elgato 4K Pro”；格式列表里有 3840×2160@60 NV12 / P010。
- SDR（PS5 关闭 HDR）：颜色与直接接显示器一致，黑位不发灰。
- HDR（PS5 开 HDR）：日志里 `HDR InfoFrame eotf=2`，交换链为 HDR10，高光不过曝、色度不过饱和（验证非 MK.2 色度全范围修正）。
- 主机在 SDR / HDR 之间切换时，1 秒内自动重建，画面不需要重开。

验收：10 分钟内采集丢帧为 0；管线 P50 < 5 ms；帧间隔 P99 < 源间隔 + 1 ms。

**如果 Media Foundation 读不到 P010，或采集回调间隔明显不均匀**，改用 DirectShow：把上游 `CaptureCardSource` + `NativeCaptureSink` 的 DirectShow 路径移植进 `capture/`，接口保持 `capture::Source` 不变。预留 2 天。

### P2 补帧（3–5 天）

- 先 2×：日志里有 `dlssg create ... result=0x00000001`（NGX 成功码），`multiFrameCountMax` 大于等于 3。
- **运动矢量方向验证**：用一段匀速横向平移的画面（游戏里平移镜头，或用 PS5 系统界面横向滚动），分别把 `DlssFg.cpp` 的 `mvecScale` 设为 `-1/W` 和 `+1/W` 录屏对比。方向错时生成帧的物体会出现“回跳”重影。上游实测是 `-1/W`，这里先沿用。
- 再试 3×、4×（240 Hz 显示器）以及 6×（需要 DLSS 4.5 运行库）。
- 性能模式开 / 关各跑一次，记录帧生成阶段耗时。

验收：4K60 → 2×，帧间隔 P50 8.33 ms、P99 < 9.5 ms，跳过的生成帧每分钟 < 1；4× 时帧生成阶段 P95 < 7 ms（不开性能模式）。

### P3 延迟监控（2 天）

- 面板、角标、帧间隔曲线的数字与日志一致。
- **校准采集卡延迟**：把 PS5 同时接到 4K Pro 的 HDMI 输入与直通输出（直通接第二台显示器），两块屏幕并排放，手机 240 fps 慢动作拍摄同一个闪烁画面（例如 PS5 设置菜单里快速切换高亮项）。两块屏幕的帧数差 × 4.17 ms 减去 Vela 的管线 P50，就是采集卡延迟加两台显示器的差值。重复 10 次取中位数，填入面板。
- 显示器延迟按厂商规格或 RTINGS 实测值填写。

验收：端到端估算与慢动作实测差值 < 5 ms。

### P4 超分（2 天）

- PS5 设 1440p 或 1080p 输出，4K 显示器：SDR 走 RTX Video（路线 2），质量 1–4 逐档对比；HDR 走 DLSS SR（路线 6）。
- 与补帧同时开启时，确认超分阶段 + 帧生成阶段的 GPU 合计 P95 小于一个输出帧间隔。

验收：1440p120 → 4K + 2× 时 GPU 批次 P95 < 7 ms，无丢帧。

### P5 HDR（2 天）

- 路线 3 / 4：Switch 2 SDR 画面在 HDR 显示器上走 RTX HDR，调峰值亮度、对比度、饱和度、中灰看效果。
- HDR 信号接 SDR 显示器（或 Windows HDR 关闭）：确认改为 NV12 采集 + 卡上色调映射。
- 交换链 HDR 元数据与信号 MaxCLL 一致（用显示器的 HDR 信息 OSD 查看）。

### P6 自动调色（2–3 天）

- 用实际游戏画面录制几段素材（明亮户外、夜景、偏色场景、雾天），各段开 / 关对比。
- 主要调 `AutoColor.cpp` 里的阈值：偏色判定 0.12 / 0.30、鲜艳度门限 0.18、中间调区间 0.25–0.60。
- 确认场景切换时 0.4 秒内收敛，平稳画面无可见“呼吸”。

### P7 打包（1 天）

- 便携 zip：上面的运行目录 + `LICENSE` + `THIRD_PARTY_NOTICES.md`。
- NVIDIA 运行库按 DLSS SDK / RTX Video SDK 的许可分发；如果只是自用，可以不打包。
- GPL-3.0：Vela 移植了 Veyra 的代码，对外分发时须同时提供源码。

合计约 15–20 个工作日（不含 DirectShow 备用路径）。

## 3. 风险与待验证项

| # | 风险 | 处理 |
|---|---|---|
| 1 | Media Foundation 对 4K Pro 的 P010 支持与回调节奏未验证 | P1 第一天测；不行就移植上游 DirectShow 路径 |
| 2 | DLSS-G 运动矢量方向 | P2 的平移测试 |
| 3 | 镜头切换时 DLSS-G 生成帧出现混合残影 | 上游用镜头检测给 DLSS-G 发 reset；可在 `CaptureToWorking` 后加一个低分辨率帧差检测，当帧设置 reset。P2 观察后再决定 |
| 4 | 采集卡自身延迟未知 | P3 校准 |
| 5 | VRR 下生成帧的节奏 | 帧间隔曲线；必要时把同步改为“垂直同步”对比 |
| 6 | “自动”格式固定选 4K60；主机输出 1440p120 时需要手动选格式 | 研究 Elgato 私有属性里是否有输入时序（分辨率 / 帧率）；有的话让“自动”跟随信号 |
| 7 | WebView2 覆盖窗口显示时会多一次 DWM 合成 | 只在调设置时出现；角标走 GPU 绘制，不依赖覆盖窗口 |
| 8 | Switch 2 的 HDR InfoFrame 与 PS5 是否同样规范 | P5 用日志里的 InfoFrame 字节核对 |
| 9 | 音频只支持 PCM | PS5 音频输出设为“线性 PCM” |

## 4. 测试矩阵

| 信号 | 显示器 | 期望路线 | 补帧 |
|---|---|---|---|
| PS5 4K60 HDR | 4K 144 Hz HDR | 5 | 2× |
| PS5 4K60 HDR | 4K 240 Hz HDR | 5 | 2× / 3× / 4× |
| PS5 1440p120 SDR | 4K 240 Hz | 2 | 2× |
| PS5 1440p120 HDR | 4K 240 Hz HDR | 6 | 2× |
| NS2 4K60 HDR | 4K 144 Hz HDR | 5 | 2× |
| NS2 1080p60 SDR | 4K 240 Hz HDR | 4 | 4× |
| PS5 4K60 HDR | 1440p 165 Hz SDR | 1（卡上色调映射）+ 缩放 | 2× |

每格记录：管线 P50 / P95、帧生成阶段 P95、帧间隔 P99、10 分钟丢帧数、主观画质。
