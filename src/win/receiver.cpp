#include "app.hpp"
#include "decoder.hpp"
#include "net.hpp"
#include "wire.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t window_class[] = L"MaxDisplayReceiver";
constexpr UINT listener_failed = WM_APP + 1;

struct Receiver {
    HWND window = nullptr;
    bool fullscreen = false;
    WINDOWPLACEMENT windowed_placement{};
    std::exception_ptr listener_error; // Read after joining the worker.
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::shared_ptr<net::Socket> active;
    std::shared_ptr<const VideoFrame> frame;
    const wchar_t* status = L"Waiting for the Mac to connect";

    void stop() {
        std::lock_guard lock(mutex);
        stopping = true;
        // Wake a worker blocked halfway through a stream header or H.264 frame.
        if (active) active->shutdown();
    }

    void show_status(const wchar_t* message) {
        {
            std::lock_guard lock(mutex);
            frame.reset();
            status = message;
        }
        InvalidateRect(window, nullptr, FALSE);
    }

    void present(VideoFrame&& decoded) {
        auto next = std::make_shared<VideoFrame>(std::move(decoded));
        {
            std::lock_guard lock(mutex);
            frame = std::move(next);
        }
        // Windows coalesces invalid regions. A slow repaint takes the newest frame.
        InvalidateRect(window, nullptr, FALSE);
    }

    void receive(net::Socket& listener) {
        try {
            while (!stopping) {
                if (!net::wait_readable(listener, 150)) continue;
                auto accepted = net::accept_tcp(listener);
                if (!accepted) continue;
                auto client = std::make_shared<net::Socket>(std::move(accepted));
                {
                    std::lock_guard lock(mutex);
                    if (stopping) break;
                    active = client;
                }
                show_status(L"Mac connected. Waiting for video");
                const wchar_t* next_status = L"Stream ended. Waiting for the Mac to reconnect";
                try {
                    const auto header = wire::receive_header(*client);
                    Decoder decoder(header.width, header.height, header.fps);
                    auto display = [this](VideoFrame&& decoded) { present(std::move(decoded)); };
                    std::vector<std::uint8_t> bytes;
                    while (!stopping && wire::receive_frame(*client, bytes)) {
                        decoder.decode(bytes, display);
                    }
                    if (!stopping) decoder.finish(display);
                } catch (const std::exception& e) {
                    if (!stopping) std::cerr << "Stream error: " << e.what() << '\n';
                    next_status = L"Stream error. See the console for details. Waiting for a connection";
                }
                {
                    std::lock_guard lock(mutex);
                    active.reset();
                }
                if (!stopping) show_status(next_status);
            }
        } catch (...) {
            listener_error = std::current_exception();
            PostMessageW(window, listener_failed, 0, 0);
        }
    }
};

void fit_monitor(HWND window) {
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const RECT& r = monitor.rcMonitor;
    SetWindowPos(window, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
}

void toggle_fullscreen(Receiver& receiver) {
    auto window = receiver.window;
    if (receiver.fullscreen) {
        receiver.fullscreen = false;
        SetWindowLongPtrW(window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(window, &receiver.windowed_placement);
        SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOZORDER |
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER);
    } else {
        receiver.windowed_placement.length = sizeof(WINDOWPLACEMENT);
        if (!GetWindowPlacement(window, &receiver.windowed_placement)) return;
        receiver.fullscreen = true;
        SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        fit_monitor(window);
    }
}

void paint(Receiver& receiver) {
    PAINTSTRUCT paint_state{};
    HDC dc = BeginPaint(receiver.window, &paint_state);
    RECT area{};
    GetClientRect(receiver.window, &area);
    const int width = area.right;
    const int height = area.bottom;
    const HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));

    std::shared_ptr<const VideoFrame> frame;
    const wchar_t* status;
    {
        std::lock_guard lock(receiver.mutex);
        frame = receiver.frame;
        status = receiver.status;
    }
    if (!frame) {
        FillRect(dc, &area, black);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(224, 224, 224));
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(dc, status, -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else if (width > 0 && height > 0) {
        const int src_width = static_cast<int>(frame->width);
        const int src_height = static_cast<int>(frame->height);
        const int dest_width = std::max(1, static_cast<int>(std::min<int64_t>(
            width, static_cast<int64_t>(height) * src_width / src_height)));
        const int dest_height = std::max(1, static_cast<int>(std::min<int64_t>(
            height, static_cast<int64_t>(width) * src_height / src_width)));
        const int x = (width - dest_width) / 2;
        const int y = (height - dest_height) / 2;
        // Erase just the letterbox bars so that repaints don't flash black between frames.
        const RECT bars[] = {{0, 0, width, y}, {0, y + dest_height, width, height},
                             {0, y, x, y + dest_height}, {x + dest_width, y, width, y + dest_height}};
        for (const RECT& bar : bars) {
            if (!IsRectEmpty(&bar)) FillRect(dc, &bar, black);
        }
        BITMAPINFO bitmap{};
        auto& header = bitmap.bmiHeader;
        header.biSize = sizeof(BITMAPINFOHEADER);
        header.biWidth = src_width;
        header.biHeight = -src_height; // Decoder emits packed top-down BGRA.
        header.biPlanes = 1;
        header.biBitCount = 32;
        header.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, x, y, dest_width, dest_height, 0, 0, src_width, src_height,
                      frame->bgra.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
    }
    EndPaint(receiver.window, &paint_state);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
        auto data = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto receiver = static_cast<Receiver*>(data->lpCreateParams);
        receiver->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(receiver));
    }
    auto receiver = reinterpret_cast<Receiver*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!receiver) return DefWindowProcW(window, message, wp, lp);
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(*receiver);
        return 0;
    case WM_SIZE:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (receiver->fullscreen && LOWORD(lp) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_DPICHANGED:
        if (receiver->fullscreen) {
            fit_monitor(window);
        } else {
            const auto& r = *reinterpret_cast<const RECT*>(lp);
            SetWindowPos(window, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_DISPLAYCHANGE:
        if (receiver->fullscreen) fit_monitor(window);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_F11 && !(lp & (1L << 30))) {
            toggle_fullscreen(*receiver);
            return 0;
        }
        if (wp != VK_ESCAPE) break;
        [[fallthrough]];
    case WM_CLOSE:
    case listener_failed:
        // Keep HWND valid while the networking thread is finishing its last repaint.
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

} // namespace

int run_receiver(const ReceiverOptions& options) {
    net::Socket listener = net::listen_tcp(options.bind, options.port);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.lpszClassName = window_class;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::system_error(GetLastError(), std::system_category(), "RegisterClassExW");

    Receiver receiver;
    HWND window = CreateWindowExW(0, window_class, L"MaxDisplay", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1000, 620,
                                  nullptr, nullptr, instance, &receiver);
    if (!window) throw std::system_error(GetLastError(), std::system_category(), "CreateWindowExW");
    ShowWindow(window, SW_SHOWNORMAL);
    if (!options.windowed) toggle_fullscreen(receiver);
    std::cout << "Listening on " << options.bind << ':' << options.port
              << ". F11 toggles full screen. Esc exits.\n" << std::flush;

    try {
        std::thread worker([&] { receiver.receive(listener); });
        MSG message{};
        BOOL result;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        DWORD error = result == -1 ? GetLastError() : ERROR_SUCCESS;
        receiver.stop();
        worker.join();
        if (error) throw std::system_error(error, std::system_category(), "GetMessageW");
        if (receiver.listener_error) std::rethrow_exception(receiver.listener_error);
    } catch (...) {
        DestroyWindow(window);
        throw;
    }
    DestroyWindow(window);
    return 0;
}
