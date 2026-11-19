// Feed the Mac codec fixture through the real TCP or UDP receiver and sample the window's
// client rendering. WM_PRINTCLIENT works even when another window obscures the viewer.
#include "app.hpp"
#include "net.hpp"
#include "secure_tcp.hpp"
#include "udp.hpp"
#include "wire.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
using Frames = std::array<std::vector<std::uint8_t>, 8>;
constexpr unsigned width = 320, height = 180, fps = 30;

Frames read_frames(const char* path) {
    Frames frames;
    std::ifstream file(path, std::ios::binary);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    for (auto& frame : frames) {
        std::uint32_t size = 0;
        for (int i = 0; i < 4; ++i) size = (size << 8) | static_cast<std::uint8_t>(file.get());
        if (!size || size > wire::max_frame_bytes) throw std::runtime_error("Invalid fixture frame size");
        frame.resize(size);
        file.read(reinterpret_cast<char*>(frame.data()), size);
    }
    if (file.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Extra fixture frames");
    return frames;
}

template<class Check>
bool wait_until(Check check, std::chrono::milliseconds timeout) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    do {
        if (check()) return true;
        std::this_thread::sleep_for(25ms);
    } while (std::chrono::steady_clock::now() < end);
    return false;
}

BOOL CALLBACK find_receiver(HWND candidate, LPARAM context) {
    wchar_t name[64]{};
    if (GetClassNameW(candidate, name, 64) && std::wcscmp(name, L"MaxDisplayReceiver") == 0 &&
        IsWindowVisible(candidate)) {
        *reinterpret_cast<HWND*>(context) = candidate;
        return FALSE;
    }
    return TRUE;
}

struct Surface {
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    ~Surface() {
        if (previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
};

// Sample the center of each right-hand quadrant on the receiver's letterboxed image.
std::array<COLORREF, 2> read_colors(HWND window) {
    RECT client{};
    if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
        throw std::runtime_error("Viewer has no paintable client area");
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = client.right;
    info.bmiHeader.biHeight = -client.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    Surface surface;
    void* pixels = nullptr;
    surface.bitmap = CreateDIBSection(surface.dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!surface.dc || !surface.bitmap || !(surface.previous = SelectObject(surface.dc, surface.bitmap)))
        throw std::runtime_error("Cannot create client readback surface");
    DWORD_PTR result{};
    if (!SendMessageTimeoutW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(surface.dc), PRF_CLIENT,
                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &result)) {
        // A timed-out window procedure could still hold the DC.
        std::cerr << "Viewer did not complete the paint request\n" << std::flush;
        std::_Exit(1);
    }
    const int image_w = std::min<int>(client.right, client.bottom * width / height);
    const int image_h = std::min<int>(client.bottom, client.right * height / width);
    const int x = (client.right - image_w) / 2 + 3 * image_w / 4;
    const int top = (client.bottom - image_h) / 2;
    return {GetPixel(surface.dc, x, top + image_h / 4), GetPixel(surface.dc, x, top + 3 * image_h / 4)};
}

bool near_color(COLORREF value, COLORREF expected, int tolerance) {
    return value != CLR_INVALID && std::abs(GetRValue(value) - GetRValue(expected)) <= tolerance &&
           std::abs(GetGValue(value) - GetGValue(expected)) <= tolerance &&
           std::abs(GetBValue(value) - GetBValue(expected)) <= tolerance;
}

bool sees_pattern(HWND window, bool final) {
    auto colors = read_colors(window);
    if (!near_color(colors[0], RGB(20, 220, 20), 18)) return false;
    if (final) return near_color(colors[1], RGB(205, 205, 205), 10);
    for (int shade : {30, 55, 80}) {
        if (near_color(colors[1], RGB(shade, shade, shade), 15)) return true;
    }
    return false;
}

class TestSender {
public:
    TestSender(const ReceiverOptions& options, Codec codec, uint32_t timeout_ms, HWND query_window = nullptr)
        : timed_(timeout_ms != 0) {
        if (options.transport == Transport::udp) {
            udp_.emplace(options.bind, options.port, options.password);
        } else {
            socket_ = net::connect_tcp(options.bind, options.port);
            tcp_.emplace(socket_, options.password, secure::Role::sender);
        }
        if (query_window) {
            const auto size = udp_ ? udp_->request_display_size() : tcp_->request_display_size();
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(MonitorFromWindow(query_window, MONITOR_DEFAULTTONEAREST), &info))
                throw std::runtime_error("Could not inspect receiver window's monitor");
            // This test process is per-monitor DPI aware, so monitor coordinates are physical pixels.
            if (size.width != static_cast<unsigned>(info.rcMonitor.right - info.rcMonitor.left) ||
                size.height != static_cast<unsigned>(info.rcMonitor.bottom - info.rcMonitor.top))
                throw std::runtime_error("Receiver reported a display size other than the window's monitor");
        }
        const wire::Header header{width, height, fps, codec, timeout_ms};
        if (udp_) udp_->send_header(header);
        else tcp_->send_header(header);
    }

    static uint64_t now_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void send(const Frames& frames, unsigned index, uint32_t sequence,
              bool expect_request = false, uint64_t stamp = now_us()) {
        if (!timed_) {
            if (udp_) udp_->send(frames[index]);
            else tcp_->send_frame(frames[index]);
            return;
        }
        const bool keyframe = index == 0 || index == 5;
        auto bytes = wire::pack_frame({sequence, stamp, keyframe, frames[index]});
        if (udp_) {
            if (!udp_->send_frame(bytes, keyframe)) throw std::runtime_error("UDP loopback queue is full");
        } else {
            tcp_->send_frame(bytes);
        }
        std::optional<std::vector<uint8_t>> reply;
        if (!wait_until([&] {
            reply = udp_ ? udp_->take_feedback() : tcp_->take_feedback();
            return reply.has_value();
        }, 8s)) throw std::runtime_error("Timed out waiting for receiver feedback");
        auto ack = wire::unpack_feedback(*reply);
        if (ack.sequence != sequence || ack.request_keyframe != expect_request)
            throw std::runtime_error("Unexpected recovery feedback for sequence " + std::to_string(sequence));
    }
private:
    net::Socket socket_;
    std::optional<secure::TcpChannel> tcp_;
    std::optional<udp::Sender> udp_;
    bool timed_;
};

void send_initial_frames(TestSender& sender, const Frames& frames, HWND window) {
    for (unsigned i = 0; i < 3; ++i) sender.send(frames, i, i + 1);
    if (!wait_until([&] { return sees_pattern(window, false); }, 8s))
        throw std::runtime_error("The live window did not paint the initial green and gray pattern");
}

void check(const Frames& frames, Codec codec, Transport transport, bool encrypted) {
    ReceiverOptions options;
    options.bind = "127.0.0.1";
    options.transport = transport;
    if (transport == Transport::udp)
        options.port = udp::Listener(options.bind, 0).local_port();
    else
        options.port = net::listen_tcp(options.bind, 0).local_port();
    if (encrypted) options.password = "MaxDisplay test passphrase";
    options.windowed = true;
    options.stats = true;
    std::promise<DWORD> started;
    auto ui_id = started.get_future();
    auto ui = std::async(std::launch::async, [&] {
        started.set_value(GetCurrentThreadId());
        return run_receiver(options);
    });
    const DWORD thread = ui_id.get();
    HWND window = nullptr;
    std::optional<TestSender> client; // Remain connected during Escape shutdown.
    std::exception_ptr failure;
    try {
        wait_until([&] {
            EnumThreadWindows(thread, find_receiver, reinterpret_cast<LPARAM>(&window));
            return window || ui.wait_for(0s) == std::future_status::ready;
        }, 8s);
        if (!window) throw std::runtime_error("No receiver window appeared");
        constexpr uint32_t timeout_ms = 1000;
        for (bool late : {false, true}) {
            client.emplace(options, codec, timeout_ms, late ? nullptr : window);
            if (late) {
                // A static desktop may produce just one picture. It must paint while the session stays open.
                client->send(frames, 0, 1);
                if (!wait_until([&] { return sees_pattern(window, false); }, 8s))
                    throw std::runtime_error("A single keyframe never appeared on the idle connection");
                // Sequence 2 arrives in order, but its timestamp is older than the deadline.
                const auto stamp = TestSender::now_us();
                std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms) + 150ms);
                client->send(frames, 1, 2, true, stamp);
            } else {
                send_initial_frames(*client, frames, window);
                // Sequence 4 (a reference picture) was lost. Deliver the following delta.
                client->send(frames, 4, 5, true);
            }
            for (unsigned i = 5; i < frames.size(); ++i)
                client->send(frames, i, late ? i - 2 : i + 1);
            if (!wait_until([&] { return sees_pattern(window, true); }, 8s))
                throw std::runtime_error("The live window never painted the recovered gray level");
            client.reset();
            if (!wait_until([&] { return !near_color(read_colors(window)[0], RGB(20, 220, 20), 18); }, 4s))
                throw std::runtime_error("Disconnected window did not clear its last frame");
        }
        // Use the legacy wire format for the codecs it supports, and keep the socket open during Escape.
        client.emplace(options, codec, codec == Codec::h265 ? timeout_ms : 0);
        send_initial_frames(*client, frames, window);
    } catch (...) {
        failure = std::current_exception();
    }
    if (window) PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
    else PostThreadMessageW(thread, WM_QUIT, 0, 0);
    if (ui.wait_for(5s) != std::future_status::ready) {
        std::cerr << "Receiver did not exit after Escape\n" << std::flush;
        std::_Exit(1);
    }
    if (ui.get() != 0) throw std::runtime_error("Receiver exit code was nonzero");
    if (failure) std::rethrow_exception(failure);
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 5) return 2;
    const bool encrypted = argc == 5;
    if (encrypted && std::string(argv[4]) != "encrypted") return 2;
    const std::string codec = argc > 2 ? argv[2] : "h264";
    const std::string transport = argc > 3 ? argv[3] : "tcp";
    if ((codec != "av1" && codec != "h264" && codec != "h265") || (transport != "tcp" && transport != "udp")) return 2;
    try {
        const auto frames = read_frames(argv[1]);
        // Probe desktop access with a system window class. Lack of monitor visibility
        // alone is fine: a noninteractive runner can still execute the real paint code.
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        HWND probe = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 8, 8,
                                     nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!probe) {
            const DWORD error = GetLastError();
            if (error == ERROR_ACCESS_DENIED || error == ERROR_REQUIRES_INTERACTIVE_WINDOWSTATION) {
                std::cerr << "Desktop unavailable (Win32 error " << error << ")\n";
                return 77;
            }
            throw std::system_error(error, std::system_category(), "Create GUI capability probe");
        }
        DestroyWindow(probe);
        check(frames, codec == "h265" ? Codec::h265 : codec == "av1" ? Codec::av1 : Codec::h264,
              transport == "udp" ? Transport::udp : Transport::tcp, encrypted);
        std::cout << codec << "/" << transport << (encrypted ? " (encrypted)" : "") << ": verified missing and late frame recovery, window pixels, reconnect and Escape shutdown\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
