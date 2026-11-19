# MaxDisplay

把 Windows 笔记本用作 Mac 的第二块屏幕。Mac 在系统中注册一台名为 **MaxDisplay** 的虚拟显示器，Windows 全屏显示这块桌面。你可以在 Mac 内置屏上写代码，同时把浏览器或其他窗口拖到 Windows 屏上。键盘和指针由 Mac 控制。

MaxDisplay 使用一套 C++17 代码和一个 CMake 项目，按操作系统生成一个可执行文件。macOS 的系统接口通过 Objective-C++ 调用。H.264 和 H.265（HEVC）使用系统编解码框架，优先启用硬件加速。AV1 使用随程序静态链接的 libaom 软件编解码器。

## 开始使用

从 [GitHub Releases](https://github.com/kagamir/maxdisplay/releases) 打开所需版本，在 **Assets** 下载对应平台的程序。Windows x64 下载并运行 `maxdisplay-windows-x64.exe`。Apple Silicon Mac 下载 `maxdisplay-macos-arm64.tar.gz`，运行 `tar -xzf maxdisplay-macos-arm64.tar.gz` 即可取出 `maxdisplay`。推送 Git 标签后，CI 会在双平台构建及互通测试通过后，将这两个文件发布到同名 Release。

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

默认使用 H.264、TCP 49300、1920×1080、最高 60 fps，动态码率上限为 12 Mbps，帧超时为 200 ms。Windows 接收端在等待和播放期间都会阻止系统因空闲而自动关屏或睡眠，退出时释放该请求。屏幕静止时保留最后一帧。Windows 接收端默认监听所有 IPv4 接口，一次接收一个 Mac。TCP 和 UDP 均使用明文传输，请使用可信局域网或私有网络连接。

## 调整画质和监听地址

```powershell
.\maxdisplay-windows-x64.exe receive --bind 192.168.1.50 --port 49301 --windowed
```

```bash
./maxdisplay send 192.168.1.50 --port 49301 --size 2560x1440 --fps 60 --bitrate-mbps 24
```

Mac 使用 `--scale` 分别乘以 `--size` 的宽和高，乘积决定虚拟屏、采集、编码和接收画面的实际像素数。比例默认是 1，允许小数，两个乘积都必须是偶数整数。例如 `--size 1280x720 --scale 1.5` 得到 1920×1080。指定 `--scale 2` 会优先请求 macOS 的 2× HiDPI 模式，此时基础尺寸也是逻辑桌面尺寸。如果系统无法提供该模式，程序会使用相同像素分辨率的标准模式。其他比例使用相乘后的原生像素模式。H.264 按最终像素数检查 Level 5.1 限制，例如 3840×2160 可选 30 fps。实际帧率取决于画面变化、编码速度、带宽和 Windows 解码速度。若播放不流畅，可先试 `--size 1920x1080 --fps 30 --bitrate-mbps 8`。两端可用 `--help` 查看参数。

### H.265、硬件加速和 UDP

Mac 用 `--codec h264`、`--codec h265` 或 `--codec av1` 选择编码，Windows 会自动识别。比如，两台电脑通过 Wi-Fi 连接时，可用下面的命令传输 2560×1440 的 H.265 画面，每秒最多 60 帧。两端都选用 UDP，Windows 防火墙需允许该 UDP 端口入站。

**Windows**

```powershell
.\maxdisplay-windows-x64.exe receive --transport udp --require-hardware
```

**Mac**

```bash
./maxdisplay send 192.168.1.50 --codec h265 --transport udp --size 1280x720 --scale 2 --fps 60 --bitrate-mbps 16 --frame-timeout-ms 150 --require-hardware
```

H.264 和 H.265 均优先在 Mac 通过 VideoToolbox 硬编码，在 Windows 通过 Media Foundation 和 D3D11/DXVA 硬解码。两端可分别添加 `--require-hardware` 强制本机使用硬件。若省略该参数，程序在支持的系统上也可使用软件编解码。Windows 控制台在解出画面后显示实际后端。Windows 播放 H.265 需要系统提供 HEVC 解码器，缺少时请安装微软的「HEVC 视频扩展」或在 Mac 上选择 H.264。AV1 始终使用 CPU，在高分辨率和高帧率下开销较高。

### 动态码率和超时恢复

- 默认开启动态码率。`--bitrate-mbps` 设置起始码率和上限。发送端根据接收反馈、往返时间、积压和近期交付速率判断拥塞，拥塞时快速降码率，稳定后逐步升回上限。`--fixed-bitrate` 可固定编码目标，仍会丢弃过期帧并请求关键帧。
- `--frame-timeout-ms` 设置实时帧预算，默认 200 ms。发送端丢弃排队过久的帧，接收端用两帧之间的相对时间变化估算新增排队延迟，无需两台机器校时。发生序号缺口、超时或解码错误时，接收端保留最后一幅已显示的画面，等待可独立解码的关键帧，然后恢复。该预算无法保证从采集到屏幕点亮的总延迟。
- TCP 保证按序交付，已经写入 TCP 的数据仍需传完。UDP 会分片并发送实时画面，遗失视频分片时可跳过该帧。需要在有丢包的网络上优先保持低延迟时，可在两端选择 `--transport udp`。
- 新接收端可读取旧版 Mac 的 H.264 和 AV1 流。新 Mac 连接旧版 Windows 时加 `--legacy`，使用原有协议和固定码率。H.265、双向反馈和按时限恢复需要两端都升级。

## 从源码构建

在仓库根目录运行，要求 CMake 3.20 或更新版以及 Git。首次配置时 CMake 会获取固定版本的 ENet 和 libaom 并静态链接。可执行文件的 `--licenses` 选项列出随程序分发的第三方声明。

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
- v3 握手协商编码、画面尺寸、帧率和超时，每个访问单元附带序号、发送端时间戳及实际关键帧标记。Windows 反馈接收进度与关键帧请求。TCP 对每帧使用大端长度前缀，UDP 通过 ENet 可靠传递握手，并使用可丢弃的有序消息传输实时帧。
- Windows 从解码器的 NV12 输出生成显示画面，按比例缩放并在需要时留黑边。D3D11 路径读取实际解码表面，AV1 由 libaom 解码。
- 构建后执行 `ctest --test-dir build -C Release --output-on-failure`。测试覆盖协议、参数、码率调整和关键帧恢复。macOS 测试在已获得屏幕录制权限的环境中启动虚拟显示器并采集画面。CI 还会编码测试图，在 Windows 上验证解码像素、帧序和 TCP/UDP 接收。Windows 的 HEVC 解码测试需要运行机安装相应系统组件。
- 双机验收可将窗口拖到虚拟屏，移动指针，观察 Windows 桌面是否只显示扩展区域。停止发送后检查 Mac 的显示器列表，再发起一次连接确认可以重新使用。
