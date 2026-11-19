# MaxDisplay

把 Windows 笔记本用作 Mac 的第二块屏幕。Mac 在系统中注册一台名为 **MaxDisplay** 的虚拟显示器，Windows 全屏显示这块桌面。你可以在 Mac 内置屏上写代码，同时把浏览器或其他窗口拖到 Windows 屏上。键盘和指针由 Mac 控制。

MaxDisplay 使用一套 C++17 代码和一个 CMake 项目，按操作系统生成一个可执行文件。macOS 的系统接口通过 Objective-C++ 调用。视频编解码使用系统框架，运行时无需安装 FFmpeg、第三方播放器或显示驱动。

## 开始使用

从 [GitHub Releases](https://github.com/kagamir/maxdisplay/releases) 打开所需版本，在 **Assets** 下载对应平台的程序。Windows x64 下载并运行 `maxdisplay-windows-x64.exe`。Apple Silicon Mac 下载 `maxdisplay-macos-arm64.tar.gz`，运行 `tar -xzf maxdisplay-macos-arm64.tar.gz` 即可取出 `maxdisplay`。推送 Git 标签后，CI 会在双平台构建及互通测试通过后，将这两个文件发布到同名 Release。

1. 将两台电脑接入同一个可信局域网。在 Windows 上运行 `ipconfig` 找到当前网卡的 IPv4 地址。下文用 `192.168.1.50` 举例。
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

默认使用 TCP 49300、1920×1080、最高 60 fps 和 12 Mbps 目标码率。屏幕静止时保留最后一帧。Windows 接收端默认监听所有 IPv4 接口，一次接收一个 Mac。传输使用未经加密和身份验证的 TCP，请使用可信局域网或已经建立的私有网络连接。

## 调整画质和监听地址

```powershell
.\maxdisplay-windows-x64.exe receive --bind 192.168.1.50 --port 49301 --windowed
```

```bash
./maxdisplay send 192.168.1.50 --port 49301 --size 2560x1440 --fps 60 --bitrate-mbps 24
```

宽和高使用偶数像素。程序按 H.264 Level 5.1 的限制检查分辨率与帧率组合，例如 1920×1080@60 和 3840×2160@30。实际帧率取决于画面变化、编码速度、带宽和 Windows 解码速度。如果提示接收端过慢，可先试 `--size 1920x1080 --fps 30 --bitrate-mbps 8`。两端可用 `--help` 查看参数。

## 从源码构建

在仓库根目录运行，要求 CMake 3.20 或更新版。

**macOS 13 或更新版**，使用 Xcode 或对应的命令行工具和 macOS SDK。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/maxdisplay --help
```

**Windows 10（1703 或更新版）或 Windows 11 x64**，使用 Visual Studio 2022 的「使用 C++ 的桌面开发」工作负载和 Windows SDK。Windows N 版需要安装 Media Feature Pack 并重启。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
.\build\Release\maxdisplay.exe --help
```

## 实现与验证

- macOS 通过 CoreGraphics 的私有 `CGVirtualDisplay` 系列接口建立虚拟屏。这些接口可能随系统更新改变，程序会在启动时检查所需接口。ScreenCaptureKit 按新建显示器的 ID 捕获画面和指针，VideoToolbox 编码 H.264，关键帧携带 SPS/PPS。
- 一条 TCP 连接传输一场会话。20 字节握手包含 `MXDS`、版本、宽、高和帧率，后四项各为大端 uint32。每个视频访问单元以大端 uint32 长度开头，随后为 Annex B 数据。接收端检查版本、尺寸、帧率和单帧长度。
- Windows 使用 Media Foundation 的 H.264 解码器，将 NV12 转换为 BGRA 并按宽高比绘制。图像大小与接收屏不匹配时会留黑边。
- 构建后执行 `ctest --test-dir build -C Release --output-on-failure` 可运行协议和命令行测试。macOS 上还会检查虚拟屏的注册、尺寸和释放，当前进程已获得屏幕录制权限时会实际采集并通过 TCP 读取一帧 H.264。Linux 可构建并验证公共传输层。要单独验证编解码互通，可在 Mac 的构建目录运行 `./codec_probe test-pattern.frames`，把生成的文件复制到 Windows，再运行 `.\codec_probe.exe test-pattern.frames`。测试会检查解码后的颜色、方向、尺寸和帧顺序。
- 双机验收可将窗口拖到虚拟屏，移动指针，观察 Windows 桌面是否只显示扩展区域。停止发送后检查 Mac 的显示器列表，再发起一次连接确认可以重新使用。
