# MaxDisplay

把 Windows 笔记本用作 Mac 的第二块屏幕。Mac 在系统中注册一台名为 **MaxDisplay** 的虚拟显示器，Windows 全屏显示这块桌面。你可以在 Mac 内置屏上写代码，同时把浏览器或其他窗口拖到 Windows 屏上。键盘和指针由 Mac 控制。

MaxDisplay 使用一套 C++17 代码和一个 CMake 项目，按操作系统生成一个可执行文件。macOS 的系统接口通过 Objective-C++ 调用。H.264 使用系统编解码框架，AV1 使用内置的 libaom 软件编解码器。两个发行文件均可独立运行。

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

默认使用 H.264、TCP 49300、1920×1080、最高 60 fps 和 12 Mbps 目标码率。Windows 接收端在等待和播放期间都会阻止系统因空闲而自动关屏或睡眠，退出时释放该请求。屏幕静止时保留最后一帧。Windows 接收端默认监听所有 IPv4 接口，一次接收一个 Mac。TCP 和 UDP 均使用明文传输，请使用可信局域网或私有网络连接。

## 调整画质和监听地址

```powershell
.\maxdisplay-windows-x64.exe receive --bind 192.168.1.50 --port 49301 --windowed
```

```bash
./maxdisplay send 192.168.1.50 --port 49301 --size 2560x1440 --fps 60 --bitrate-mbps 24
```

Mac 使用 `--scale` 分别乘以 `--size` 的宽和高，乘积决定虚拟屏、采集、编码和接收画面的实际像素数。比例默认是 1，允许小数，两个乘积都必须是偶数整数。例如 `--size 1280x720 --scale 1.5` 得到 1920×1080。指定 `--scale 2` 会启用 macOS 的 2× HiDPI 模式，此时基础尺寸也是逻辑桌面尺寸。其他比例使用相乘后的原生像素模式。H.264 按最终像素数检查 Level 5.1 限制，例如 3840×2160 可选 30 fps。实际帧率取决于画面变化、编码速度、带宽和 Windows 解码速度。如果提示接收端过慢，可先试 `--size 1920x1080 --fps 30 --bitrate-mbps 8`。两端可用 `--help` 查看参数。

### AV1 和 UDP

`--codec av1` 在 Mac 端选择 AV1，Windows 根据连接头自动选择解码器。H.264 和 AV1 均可搭配 TCP 或 UDP。UDP 使用 IPv4，双方都要指定 `--transport udp`，且 Windows 防火墙要允许相应 UDP 端口入站。下面的例子使用 UDP，将 1280×720 的逻辑桌面以 2560×1440 像素传输。

```powershell
.\maxdisplay-windows-x64.exe receive --transport udp
```

```bash
./maxdisplay send 192.168.1.50 --transport udp --codec av1 --size 1280x720 --scale 2 --fps 30
```

AV1 使用 CPU 实时编码，高分辨率和高帧率会增加 CPU 开销。UDP 模式通过 ENet 在同一 UDP 端口完成连接、分片、重传和断线检测，丢包重传期间可能增加画面延迟。

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

- macOS 通过 CoreGraphics 的私有 `CGVirtualDisplay` 系列接口建立虚拟屏。这些接口可能随系统更新改变，程序会在启动时检查所需接口。ScreenCaptureKit 按新建显示器的 ID 捕获画面和指针，VideoToolbox 编码 H.264，关键帧携带 SPS/PPS。AV1 将 NV12 画面交给 libaom 编码为 AV1 访问单元。
- H.264 使用 20 字节 v1 握手并兼容原有 TCP 端点。AV1 使用 24 字节 v2 握手，在原有宽、高和帧率后追加大端 uint32 编码标识。TCP 对每个访问单元使用四字节大端长度前缀。UDP 使用可靠有序的 ENet 消息，第一条是握手，之后每条承载一个完整的访问单元。
- Windows 使用 Media Foundation 解码 H.264，使用 libaom 解码 AV1，按宽高比显示。图像大小与接收屏不匹配时会留黑边。
- 构建后执行 `ctest --test-dir build -C Release --output-on-failure` 可运行协议和命令行测试。macOS 上还会检查虚拟屏的注册、尺寸和释放，当前进程已获得屏幕录制权限时会实际采集并通过 TCP 读取一帧 H.264。Linux 可构建并验证公共传输层。要单独验证编解码互通，可在 Mac 的构建目录运行 `./codec_probe test-pattern.frames`，把生成的文件复制到 Windows，再运行 `.\codec_probe.exe test-pattern.frames`。测试会检查解码后的颜色、方向、尺寸和帧顺序。
- 双机验收可将窗口拖到虚拟屏，移动指针，观察 Windows 桌面是否只显示扩展区域。停止发送后检查 Mac 的显示器列表，再发起一次连接确认可以重新使用。
