# MaxDisplay

把 Windows 笔记本用作 Mac 的第二块屏幕。Mac 在系统中注册一台名为 **MaxDisplay** 的虚拟显示器，Windows 全屏显示这块桌面。你可以在 Mac 内置屏上写代码，同时把浏览器或其他窗口拖到 Windows 屏上。键盘和指针由 Mac 控制。

MaxDisplay 使用一套 C++17 代码和一个 CMake 项目，按操作系统生成一个可执行文件。macOS 的系统接口通过 Objective-C++ 调用。Mac 使用 VideoToolbox 编码 H.264 和 H.265（HEVC），也可以用 libaom 软件编码 AV1。Windows 根据发送端选择的格式解码。

## 开始使用

从 [GitHub Releases](https://github.com/kagamir/maxdisplay/releases) 打开所需版本，在 **Assets** 下载对应平台的程序。Windows x64 下载并运行 `maxdisplay-windows-x64.exe`。Apple Silicon Mac 下载 `maxdisplay-macos-arm64.tar.gz`，运行 `tar -xzf maxdisplay-macos-arm64.tar.gz` 即可取出 `maxdisplay`。推送 Git 标签后，CI 会在双平台构建及互通测试通过后，将程序发布到同名 Release。Windows 用户运行单个 EXE 即可。需要替换内置 HEVC 解码库并重新链接 Windows 程序时，可下载同一 Release 内的 `maxdisplay-windows-relink.zip`。

1. 将两台电脑接入同一个可信局域网。Windows 接收端启动后会在等待界面列出监听地址和窗口所在显示器的当前像素分辨率。有默认路由的网卡地址优先，再列其他有路由的地址。下文用 `192.168.1.50` 举例。
2. 在 Windows PowerShell 中启动接收端。直接双击下载的程序也会进入接收模式。防火墙若询问网络访问，请允许专用网络上的入站连接。

   ```powershell
   .\maxdisplay-windows-x64.exe receive
   ```

3. 在 Mac 终端启动发送端，把地址换成 Windows 的实际地址。

   ```bash
   ./maxdisplay send 192.168.1.50
   ```

4. 首次运行需要授予屏幕录制权限。如果系统要求重启，在「系统设置 → 隐私与安全性 → 屏幕录制」（部分版本称「屏幕与系统音频录制」）允许系统提示的终端或程序，完全退出并重新打开它，再执行上面的命令。
5. 在 Mac 的「系统设置 → 显示器 → 排列」中把 **MaxDisplay** 拖到 Windows 电脑实际所在的一侧，随后把一个窗口拖过该边缘。Windows 上按 **F11** 切换全屏，按 **Esc** 退出。在 Mac 上按 **Ctrl+C** 会释放虚拟屏，Windows 接收端可继续等待下次连接。

默认使用 H.264 和 TCP 49300，串流分辨率跟随 Windows 接收窗口所在的显示器。帧率上限为 60 fps，码率上限为 12 Mbps，帧超时为 200 ms。Windows 接收端在等待和播放期间都会阻止系统因空闲而自动关屏或睡眠，退出时释放该请求。屏幕静止时保留最后一帧。Windows 接收端默认监听所有 IPv4 接口，一次接收一个 Mac。TCP 和 UDP 均使用明文传输，请使用可信局域网或私有网络连接。

## 调整画质和监听地址

```powershell
.\maxdisplay-windows-x64.exe receive --bind 192.168.1.50 --port 49301 --windowed
```

```bash
./maxdisplay send 192.168.1.50 --port 49301 --size 2560x1440 --fps 60 --bitrate-mbps 24
```

**`--size` 定义 Mac 虚拟桌面的逻辑分辨率**，它决定桌面能排下多少内容。`--scale` 决定每个逻辑点对应多少个串流像素，默认 1。省略 `--size` 时，Mac 在建立连接时查询 Windows 接收窗口所在显示器的当前像素分辨率，用宽、高分别除以 `--scale`，四舍五入得到逻辑尺寸。Windows 上的系统缩放百分比不参与计算。

| Windows 屏幕 | Mac 参数 | Mac 虚拟桌面 | 串流画面 |
| --- | --- | --- | --- |
| 1920×1080 | 使用默认参数 | 1920×1080 | 1920×1080 |
| 1920×1080 | `--scale 0.75` | 2560×1440 | 1920×1080 |
| 2560×1440 | `--scale 2` | 1280×720 | 2560×1440 |
| 1920×1080 | `--size 1280x720 --scale 1.5` | 1280×720 | 1920×1080 |

显式指定 `--size` 时，逻辑尺寸保持不变，宽、高分别乘以 `--scale` 得到编码尺寸，乘积都须为偶数整数。自动取值时，如果 Windows 屏幕某一边为奇数，编码尺寸在该边少 1 像素，以满足 4:2:0 格式要求。大于 1 的比例会优先请求系统以 2× HiDPI 绘制虚拟桌面，再采样到目标视频尺寸。系统未能启用 HiDPI 时会保留相同的逻辑桌面尺寸。Mac 的启动日志会分别列出虚拟桌面和串流尺寸。

串流尺寸受编解码器约束，目前最大为 4096×2304。H.264 还受 Level 5.1 的宏块速率限制，自动帧率会把 3840×2160 的上限降至 30 fps。如果接收屏超出支持范围，可以显式指定较小的桌面和串流尺寸。两端可用 `--help` 查看参数。

### H.265、硬件加速和 UDP

Mac 用 `--codec h264`、`--codec h265` 或 `--codec av1` 选择编码，Windows 会自动识别。比如，两台电脑通过 Wi-Fi 连接时，可用下面的命令传输 2560×1440 的 H.265 画面，每秒最多 60 帧。两端都选用 UDP，Windows 防火墙需允许该 UDP 端口入站。

**Windows**

```powershell
.\maxdisplay-windows-x64.exe receive --transport udp
```

**Mac**

```bash
./maxdisplay send 192.168.1.50 --codec h265 --transport udp --size 1280x720 --scale 2 --fps 60 --bitrate-mbps 16 --frame-timeout-ms 150
```

H.264 和 H.265 默认在 Mac 优先尝试 VideoToolbox 硬编码。硬件初始化、编码或码率调整失败时，Mac 会记录原因并尝试系统的同格式软件编码器。Mac 选择 AV1 时使用内置的 libaom 软件编码。Windows 会优先尝试可用的硬件解码器，失败时输出警告并切换到软件。Windows 自带的 Media Foundation、内置的 HEVC 和 AV1 软件解码器提供回退路径。两个平台都可以单独指定 `--require-hardware` 强制本机使用硬件。

### 动态码率、帧率和超时恢复

- 默认同时开启动态码率与帧率。`--bitrate-mbps` 和 `--fps` 分别设置上限。出现网络排队时先降码率。编码持续过慢或连续降低码率后仍有积压，才降低帧率。帧率按采集上限的整数约数调整，例如 60、30、20、15 fps。健康反馈可以跨桌面操作的空闲间隙累计，恢复时优先回到此前稳定的码率。
- 桌面停止变化后，Mac 在最多 2 秒内重复编码最后一幅画面，逐步补清文字和边缘。在支持质量指标的 VideoToolbox 编码器上，亮度 PSNR 达到 42 dB 或连续 500 ms 的提升不足 0.2 dB 时提前结束。AV1 的量化值降到 24 或更低时提前结束。丢帧或拥塞也会停止本轮补帧。
- 用 `--fixed-fps` 固定帧率目标并保留动态码率，用 `--fixed-bitrate` 固定码率目标并保留动态帧率。可以同时使用两项。控制台会报告目标值的调整。
- 排查画质和卡顿时，两端分别添加 `--stats`。Mac 每秒输出目标和实际发送的码率、帧率、RTT、估算排队延迟、编码耗时、可用画质指标以及分原因降档和关键帧统计。Windows 每秒输出收帧、解码、实际绘制的帧数和耗时、解码器类型，以及请求关键帧的原因。
- `--frame-timeout-ms` 设置实时帧预算，默认 200 ms。反馈模式下仅在起流和恢复时主动请求关键帧，关键帧的发送和确认使用较宽的时限。发送端丢弃排队过久的帧，接收端用两帧之间的相对时间变化估算新增排队延迟，无需两台机器校时。发生序号缺口、超时或解码错误时，接收端保留最后一幅已显示的画面，等待可独立解码的关键帧，然后恢复。该预算无法保证从采集到屏幕点亮的总延迟。
- TCP 保证按序交付，已经写入 TCP 的数据仍需传完。UDP 会分片发送画面。关键帧缺失的分片可单独重传，同一时间最多有一个关键帧等待传输确认。普通视频帧丢失分片后直接跳过。需要在有丢包的网络上优先保持低延迟时，可在两端选择 `--transport udp`。
- 新接收端兼容已有的流协议。若 Windows 支持 v3 但尚未升级到自动尺寸查询版本，请显式传 `--size`。连接更早的 v1/v2 接收端时，加 `--legacy` 使用旧版 H.264/AV1 握手和固定码率、帧率，省略尺寸时使用 1920×1080 的逻辑桌面。

## 从源码构建

在仓库根目录运行，要求 CMake 3.20 或更新版以及 Git。首次配置时 CMake 会获取固定版本的 ENet、libaom 及 Windows 所需的 libde265 并静态链接。可执行文件的 `--licenses` 选项列出随程序分发的第三方声明。

**macOS 13 或更新版**，使用 Xcode 或对应的命令行工具和 macOS SDK。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/maxdisplay --help
```

**Windows 10（1703 或更新版）或 Windows 11 x64**，使用 Visual Studio 2022 的「使用 C++ 的桌面开发」工作负载和 Windows SDK，并将 NASM 加入 PATH。Windows N 版需要安装 Media Feature Pack 并重启。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
.\build\Release\maxdisplay.exe --help
```

## 实现与验证

- macOS 通过 CoreGraphics 的私有 `CGVirtualDisplay` 接口建立虚拟屏，用 ScreenCaptureKit 捕获画面和指针。VideoToolbox 使用实时模式并关闭帧重排。H.264 关键帧携带 SPS/PPS，HEVC Main 关键帧携带 VPS/SPS/PPS，均以 Annex B 发送。AV1 使用低延迟的 libaom 编码。
- v3 握手协商编码、画面尺寸、帧率和超时，每个访问单元附带序号、发送端时间戳及实际关键帧标记。Windows 反馈接收进度与关键帧请求。TCP 对每帧使用大端长度前缀，UDP 通过 ENet 可靠传递握手及关键帧，其他视频帧使用可丢弃的有序消息。
- Windows 从解码后的 YUV 数据生成显示画面，按比例缩放并在需要时留黑边。D3D11 路径读取实际解码表面。
- 构建后执行 `ctest --test-dir build -C Release --output-on-failure`。测试覆盖协议、参数、码率调整和关键帧恢复。macOS 测试在已获得屏幕录制权限的环境中启动虚拟显示器并采集画面。CI 还会编码测试图，在 Windows 上验证解码像素、帧序和 TCP/UDP 接收。Windows 上的 HEVC 测试也覆盖缺少系统 HEVC 组件时的软件回退。
- 双机验收可将窗口拖到虚拟屏，移动指针，观察 Windows 桌面是否只显示扩展区域。停止发送后检查 Mac 的显示器列表，再发起一次连接确认可以重新使用。
