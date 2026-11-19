# MaxDisplay

[![Build](https://github.com/kagamir/maxdisplay/actions/workflows/build.yml/badge.svg)](https://github.com/kagamir/maxdisplay/actions/workflows/build.yml)
[![Latest release](https://img.shields.io/github/v/release/kagamir/maxdisplay)](https://github.com/kagamir/maxdisplay/releases/latest)

**Use a Windows PC as a second display for your Mac.**

MaxDisplay adds a virtual display named **MaxDisplay** to your Mac and streams it to a Windows PC, which shows it full screen. Keep your code on the Mac's built-in screen and drag a browser, or any other window, onto the Windows one. The keyboard and pointer stay on the Mac.

[Quick start](#quick-start) · [Configuration](#configuration) · [Command reference](#command-reference) · [Troubleshooting](#troubleshooting) · [Building from source](#building-from-source) · [How it works](#how-it-works)

## Features

- **A real extended display.** macOS treats MaxDisplay like any other monitor, so you arrange it in System Settings and drag windows across.
- **Fits the Windows screen automatically.** The stream matches the pixel resolution of the monitor the receiver is on. `--size` and `--scale` give you HiDPI rendering or more desktop room.
- **H.264, H.265 and AV1.** The Mac encodes with VideoToolbox hardware, Windows decodes with hardware when it can, and software fallbacks are built in on both sides.
- **Adapts to your network.** Bitrate and frame rate adjust on their own, up to 60 fps and 12 Mbps by default.
- **Sharp when still.** Once the desktop stops changing, the Mac keeps refining the last picture so text and edges clear up.
- **TCP or UDP.** TCP is the default. UDP keeps latency low on networks that drop packets.
- **Recovers by itself.** After loss or a timeout the receiver holds the last picture and resumes at the next keyframe. After a disconnect it waits for the Mac to come back.
- **One executable per platform.** There is no installer and no background service.

## Requirements

| | Mac (sender) | Windows (receiver) |
| --- | --- | --- |
| System | macOS 13 or later | Windows 10 version 1703 or later, or Windows 11, x64 |
| Prebuilt download | Apple Silicon | x64 |
| Setup | Screen Recording permission | Windows N editions need the Media Feature Pack, followed by a restart |

Both computers must be on the same trusted local network.

> [!WARNING]
> MaxDisplay sends video unencrypted over both TCP and UDP. Use it only on a local or private network you trust.

## Quick start

### 1. Download

Get both files from the [latest release](https://github.com/kagamir/maxdisplay/releases/latest), under **Assets**.

| Platform | File |
| --- | --- |
| Windows x64 | `maxdisplay-windows-x64.exe` |
| Mac with Apple Silicon | `maxdisplay-macos-arm64.tar.gz` |

On the Mac, unpack the archive to get the `maxdisplay` executable:

```bash
tar -xzf maxdisplay-macos-arm64.tar.gz
```

### 2. Start the receiver on Windows

Double-click `maxdisplay-windows-x64.exe`, or run it from PowerShell:

```powershell
.\maxdisplay-windows-x64.exe receive
```

If Windows Firewall asks about network access, allow inbound connections on private networks.

The waiting screen lists the addresses the receiver is listening on and the pixel resolution of the monitor it is on. Addresses on the adapter with the default route come first. The examples below use `192.168.1.50`.

### 3. Start the sender on the Mac

Replace the address with one from the Windows waiting screen:

```bash
./maxdisplay send 192.168.1.50
```

The first run needs Screen Recording permission. If macOS asks you to restart the app:

1. Open **System Settings → Privacy & Security → Screen Recording**. Some macOS versions call it **Screen & System Audio Recording**.
2. Allow the terminal or app named in the prompt.
3. Quit that app completely, reopen it, and run the command again.

### 4. Arrange the display

On the Mac, open **System Settings → Displays → Arrange** and drag **MaxDisplay** to the side where the Windows PC actually sits. Then drag a window across that edge.

### Controls

| Where | Key | Action |
| --- | --- | --- |
| Windows | **F11** | Toggle full screen |
| Windows | **Esc** | Exit the receiver |
| Mac | **Ctrl+C** | Stop sharing and remove the virtual display. The receiver keeps waiting for the next connection |

### Defaults

| Setting | Default |
| --- | --- |
| Codec | H.264 |
| Transport | TCP, port 49300 |
| Stream resolution | The pixel resolution of the monitor the receiver window is on |
| Frame rate | Up to 60 fps |
| Bitrate | Up to 12 Mbps |
| Frame timeout | 200 ms |

The receiver listens on all IPv4 interfaces and accepts one Mac at a time. While it is waiting or playing, it stops Windows from turning off the display or going to sleep when idle, and it releases that request on exit. When the Mac's screen is still, the last frame stays on screen.

## Configuration

Both executables print their options with `--help`. This example moves the receiver to a specific address and port in a window, and sends a 2560×1440 desktop at up to 24 Mbps:

```powershell
.\maxdisplay-windows-x64.exe receive --bind 192.168.1.50 --port 49301 --windowed
```

```bash
./maxdisplay send 192.168.1.50 --port 49301 --size 2560x1440 --fps 60 --bitrate-mbps 24
```

### Resolution and scaling

Two options on the Mac control the picture size:

- **`--size`** sets the logical resolution of the Mac's virtual desktop, which decides how much fits on it.
- **`--scale`** sets how many stream pixels each logical point gets. The default is 1.

The stream size is `--size` multiplied by `--scale`. A scale above 1 gives sharper, larger content. A scale below 1 gives more room.

Without `--size`, the Mac asks the receiver at connection time for the current pixel resolution of the monitor the receiver window is on, divides the width and height by `--scale`, and rounds the result to get the logical size. The Windows display scaling percentage plays no part.

| Windows screen | Mac options | Mac virtual desktop | Stream |
| --- | --- | --- | --- |
| 1920×1080 | none | 1920×1080 | 1920×1080 |
| 1920×1080 | `--scale 0.75` | 2560×1440 | 1920×1080 |
| 2560×1440 | `--scale 2` | 1280×720 | 2560×1440 |
| 1920×1080 | `--size 1280x720 --scale 1.5` | 1280×720 | 1920×1080 |

Details worth knowing:

- With an explicit `--size`, the logical size stays exactly as given. The width and height multiplied by `--scale` must both be even integers.
- In automatic mode, if one side of the Windows screen has an odd number of pixels, the stream is 1 pixel smaller on that side, as 4:2:0 video requires.
- A scale above 1 asks macOS to draw the virtual desktop at 2× HiDPI and then resamples it to the stream size. If macOS cannot enable HiDPI, the logical desktop size stays the same.
- The Mac's startup log prints the virtual desktop size and the stream size separately.

**Limits.** The stream can be at most 4096×2304. H.264 is also bound by the Level 5.1 macroblock rate, so the automatic frame rate caps 3840×2160 at 30 fps. If the Windows screen is larger than the supported range, pass a smaller `--size` and `--scale` explicitly.

### Codecs and hardware acceleration

Choose the codec on the Mac with `--codec h264`, `--codec h265` or `--codec av1`. The receiver detects it automatically.

| Codec | Mac encoder | Windows decoder |
| --- | --- | --- |
| H.264 (default) | VideoToolbox hardware, then the system software encoder | Hardware when available, then software |
| H.265 | VideoToolbox hardware, then the system software encoder | Hardware when available, then software |
| AV1 | Built-in libaom software encoder | Hardware when available, then software |

- On the Mac, if hardware setup, encoding or a bitrate change fails, MaxDisplay logs the reason and switches to the system's software encoder for the same codec.
- On Windows, if hardware decoding fails, MaxDisplay prints a warning and switches to software. The fallbacks are Windows Media Foundation and the built-in HEVC and AV1 software decoders, so H.265 works even without the system HEVC component.
- Pass `--require-hardware` on either side to make that side use hardware only. On the Mac it applies to H.264 and H.265.

### TCP or UDP

TCP is the default. It delivers everything in order, so data already written to the connection still has to arrive, even when it is late.

UDP splits each picture into fragments. Missing keyframe fragments are retransmitted individually, and a regular frame that loses a fragment is skipped. Choose UDP when low latency matters more than every frame on a network that drops packets, such as Wi-Fi.

Set `--transport udp` on both sides, and allow that UDP port inbound in Windows Firewall. For example, this sends a 2560×1440 H.265 stream at up to 60 fps over Wi-Fi:

```powershell
.\maxdisplay-windows-x64.exe receive --transport udp
```

```bash
./maxdisplay send 192.168.1.50 --codec h265 --transport udp --size 1280x720 --scale 2 --fps 60 --bitrate-mbps 16 --frame-timeout-ms 150
```

### Adaptive bitrate and frame rate

Both adapt by default. `--bitrate-mbps` and `--fps` set the ceilings.

- When the network starts to queue, MaxDisplay lowers the bitrate first.
- It lowers the frame rate only when encoding stays too slow, or when a backlog remains after repeated bitrate cuts. The frame rate moves in whole divisors of the capture ceiling, such as 60, 30, 20 and 15 fps.
- Healthy feedback adds up across the idle gaps in normal desktop use, and recovery returns to the last stable bitrate first.
- `--fixed-bitrate` holds the bitrate target while the frame rate still adapts. `--fixed-fps` holds the frame rate target while the bitrate still adapts. You can combine them.

The console reports each change to the targets.

### Refinement when the screen is still

When the desktop stops changing, the Mac re-encodes the last picture for up to 2 seconds so text and edges get progressively cleaner. It stops early when:

- a VideoToolbox encoder that reports quality metrics reaches a luma PSNR of 42 dB, or improves by less than 0.2 dB over 500 ms,
- the AV1 quantizer drops to 24 or below, or
- a frame is dropped or the network becomes congested.

### Frame timeout and recovery

`--frame-timeout-ms` sets the real-time budget for a frame. The default is 200 ms.

- The sender drops frames that have waited in its queue too long.
- The receiver estimates added queueing delay from the change in timing between frames, so the two clocks do not need to be synchronized.
- After a sequence gap, a timeout or a decoding error, the receiver keeps the last displayed picture, waits for a keyframe it can decode on its own, and then resumes.
- Keyframes are requested only at stream start and during recovery, and they get a wider time limit for sending and acknowledgement. Over UDP, at most one keyframe awaits acknowledgement at a time.

This budget does not guarantee the total delay from capture to the screen lighting up.

### Statistics

Add `--stats` on either side to print per-second statistics on that device. Use it on both when you are chasing blur or stutter.

- **Mac:** target and actual bitrate and frame rate, round-trip time, estimated queueing delay, encoding time, available quality metrics, and counts of step-downs and keyframes by cause.
- **Windows:** frames received, decoded and actually drawn, with their timings, the decoder type, and the reasons for keyframe requests.

### Older receivers

A current receiver accepts every earlier version of the stream protocol. For a current Mac sending to an older Windows receiver:

- If the receiver speaks protocol v3 but predates automatic size detection, pass `--size` explicitly.
- For a v1 or v2 receiver, add `--legacy`. This uses the old H.264 or AV1 handshake with a fixed bitrate and frame rate, and a 1920×1080 logical desktop when `--size` is omitted. It cannot be combined with `--codec h265` or `--frame-timeout-ms`.

## Command reference

### `maxdisplay send <windows-host>` on macOS

| Option | Default | Description |
| --- | --- | --- |
| `--port PORT` | `49300` | Receiver port |
| `--size WIDTHxHEIGHT` | Windows monitor ÷ scale | Logical size of the virtual desktop |
| `--scale FACTOR` | `1` | Stream pixels per logical point. Any positive number, such as `1.5` or `2` |
| `--codec NAME` | `h264` | `h264`, `h265` (or `hevc`) or `av1` |
| `--transport NAME` | `tcp` | `tcp` or `udp`. Must match the receiver |
| `--fps FPS` | `60` | Frame rate ceiling, 1 to 120 |
| `--bitrate-mbps MBPS` | `12` | Bitrate ceiling, 1 to 100 |
| `--frame-timeout-ms MS` | `200` | Real-time frame budget, 1 to 60000 |
| `--fixed-bitrate` | off | Hold the bitrate at its target |
| `--fixed-fps` | off | Hold the frame rate at its target |
| `--require-hardware` | off | Use the hardware encoder only. H.264 and H.265 |
| `--legacy` | off | Talk to a v1 or v2 receiver |
| `--stats` | off | Print per-second statistics |

### `maxdisplay [receive]` on Windows

| Option | Default | Description |
| --- | --- | --- |
| `--bind IP` | `0.0.0.0` | IPv4 address to listen on |
| `--port PORT` | `49300` | Port to listen on |
| `--transport NAME` | `tcp` | `tcp` or `udp`. Must match the sender |
| `--windowed` | off | Start in a window instead of full screen |
| `--require-hardware` | off | Use a hardware decoder only |
| `--stats` | off | Print per-second statistics |

### Both platforms

| Option | Description |
| --- | --- |
| `--help`, `-h` | Show usage |
| `--licenses` | Print the notices for bundled third-party software |

## Troubleshooting

**The Mac cannot connect.**
Start the Windows receiver first. Use an address from its waiting screen, and make sure `--port` and `--transport` match on both sides. In Windows Firewall, allow inbound connections for MaxDisplay on private networks. That is TCP 49300 by default, or the UDP port when you use `--transport udp`.

**The sender stops and asks for Screen Recording.**
Allow the terminal or app under **System Settings → Privacy & Security → Screen Recording**, quit it completely, reopen it, and run the command again.

**The picture is soft or it stutters.**
Add `--stats` on both sides to see where frames are lost or delayed. Raise `--bitrate-mbps` if the network has room. On Wi-Fi or any network that drops packets, try `--transport udp` on both sides.

**The sender rejects the size.**
The stream cannot exceed 4096×2304, and with an explicit `--size` the width and height multiplied by `--scale` must be even integers. Pass a smaller or different `--size` and `--scale`.

**3840×2160 runs at 30 fps.**
That is the H.264 Level 5.1 limit at that size.

**The Windows receiver is an older build.**
See [Older receivers](#older-receivers).

## Building from source

You need CMake 3.20 or later and Git. The first configure step downloads pinned versions of ENet, libaom and, on Windows, libde265, and links them statically. Run the commands from the repository root.

### macOS 13 or later

Install Xcode, or the Command Line Tools with a macOS SDK.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/maxdisplay --help
```

### Windows 10 (1703 or later) or Windows 11, x64

Install Visual Studio 2022 with the **Desktop development with C++** workload and a Windows SDK, and put NASM on your `PATH`.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
.\build\Release\maxdisplay.exe --help
```

### Tests

```bash
ctest --test-dir build -C Release --output-on-failure
```

The tests cover the protocol, option parsing, rate adaptation and keyframe recovery. On macOS they also start a virtual display and capture from it, which needs Screen Recording permission. CI additionally encodes test patterns on the Mac and checks decoded pixels, frame order and TCP and UDP reception on Windows. The Windows HEVC tests include the software fallback used when the system HEVC component is missing.

To check a real pair of machines by hand:

1. Drag a window onto the virtual display and move the pointer across it.
2. Confirm that the Windows screen shows only the extended area.
3. Stop the sender and check that MaxDisplay is gone from the Mac's display list.
4. Connect again to confirm the receiver can be reused.

### Releases

Pushing a Git tag makes CI build both platforms, run the cross-platform interoperability tests and, once they pass, publish the binaries to the GitHub Release of the same name.

## How it works

```
Mac (sender)                                          Windows (receiver)
virtual display → capture → encode  ── TCP or UDP ──▶  decode → scale → window
                                    ◀── feedback ────
```

MaxDisplay is one C++17 codebase and one CMake project that builds a single executable for each operating system. macOS system APIs are called through Objective-C++.

- **Virtual display and capture.** macOS creates the display through CoreGraphics' private `CGVirtualDisplay` interface and captures the picture and pointer with ScreenCaptureKit.
- **Encoding.** VideoToolbox runs in real-time mode with frame reordering off. H.264 keyframes carry SPS and PPS, and HEVC Main keyframes carry VPS, SPS and PPS, all sent as Annex B. AV1 uses libaom in a low-latency configuration.
- **Protocol.** The v3 handshake negotiates codec, picture size, frame rate and timeout. Every access unit carries a sequence number, a sender timestamp and a flag marking real keyframes. The receiver reports its progress and requests keyframes.
- **Transport.** TCP prefixes each frame with a big-endian length. UDP uses ENet: the handshake and keyframes travel reliably, and all other frames go as droppable, sequenced messages.
- **Display.** Windows builds the picture from the decoded YUV data, scales it proportionally and adds black bars when needed. The D3D11 path reads the actual decoded surface.

## Third-party software

MaxDisplay links these libraries statically. Run `maxdisplay --licenses` for the full notices.

| Library | Used for | License |
| --- | --- | --- |
| [ENet](http://enet.bespin.org/) | UDP transport | MIT |
| [libaom](https://aomedia.googlesource.com/aom/) | AV1 encoding and decoding | BSD 2-Clause, with the AOM Patent License 1.0 |
| [libde265](https://github.com/strukturag/libde265) | HEVC software decoding on Windows | LGPL-3.0-or-later |

To replace the bundled HEVC decoder library and relink the Windows program, download `maxdisplay-windows-relink.zip` from the same release.
