// Feed the Mac codec fixture through the real TCP receiver and sample the window's
// client rendering. WM_PRINTCLIENT works even when another window obscures the viewer.
#include "app.hpp"
#include "net.hpp"
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
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using Frames = std::array<std::vector<std::uint8_t>, 6>;
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
    if (final) return near_color(colors[1], RGB(210, 210, 210), 15);
    for (int shade : {30, 66, 102}) {
        if (near_color(colors[1], RGB(shade, shade, shade), 15)) return true;
    }
    return false;
}

net::Socket start_stream(const Frames& frames, std::uint16_t port, HWND window) {
    auto socket = net::connect_tcp("127.0.0.1", port);
    wire::send_header(socket, width, height, fps);
    for (int i = 0; i < 3; ++i) wire::send_frame(socket, frames[i]);
    if (!wait_until([&] { return sees_pattern(window, false); }, 8s))
        throw std::runtime_error("The live window did not paint the initial green and gray pattern");
    return socket;
}

void check(const Frames& frames) {
    ReceiverOptions options;
    options.bind = "127.0.0.1";
    options.port = net::listen_tcp(options.bind, 0).local_port();
    options.windowed = true;
    std::promise<DWORD> started;
    auto ui_id = started.get_future();
    auto ui = std::async(std::launch::async, [&] {
        started.set_value(GetCurrentThreadId());
        return run_receiver(options);
    });
    const DWORD thread = ui_id.get();
    HWND window = nullptr;
    net::Socket client; // Remains connected while Escape interrupts the blocked receiver.
    std::exception_ptr failure;
    try {
        wait_until([&] {
            EnumThreadWindows(thread, find_receiver, reinterpret_cast<LPARAM>(&window));
            return window || ui.wait_for(0s) == std::future_status::ready;
        }, 8s);
        if (!window) throw std::runtime_error("No receiver window appeared");
        client = start_stream(frames, options.port, window);
        for (unsigned i = 3; i < frames.size(); ++i) wire::send_frame(client, frames[i]);
        if (!wait_until([&] { return sees_pattern(window, true); }, 8s))
            throw std::runtime_error("The live window never painted the final gray level");
        client = net::Socket{};
        if (!wait_until([&] { return read_colors(window)[0] == RGB(0, 0, 0); }, 4s))
            throw std::runtime_error("Disconnected window did not clear its last frame");
        client = start_stream(frames, options.port, window);
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
    if (argc != 2) return 2;
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
        check(frames);
        std::cout << "Verified live window pixels, receiver reconnect and Escape shutdown\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
