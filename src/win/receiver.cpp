#include "app.hpp"
#include "decoder.hpp"
#include "display_info.hpp"
#include "net.hpp"
#include "udp.hpp"
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
#include <variant>
#include <vector>

namespace {

constexpr wchar_t window_class[] = L"MaxDisplayReceiver";
constexpr UINT listener_failed = WM_APP + 1;
constexpr UINT_PTR address_timer = 1;
using Listener = std::variant<net::Socket, udp::Listener>;

struct Receiver {
    HWND window = nullptr;
    win_info::Binding binding{};
    std::uint16_t port = 0;
    Transport transport = Transport::tcp;
    std::wstring address_text;
    bool fullscreen = false;
    WINDOWPLACEMENT windowed_placement{};
    std::exception_ptr listener_error; // Read after joining the worker.
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::shared_ptr<net::Socket> active;
    std::shared_ptr<const VideoFrame> frame;
    const wchar_t* status = L"Waiting for the Mac to connect";

    // Only the UI thread reads and updates the network details shown while idle.
    void refresh_addresses() {
        auto addresses = win_info::listening_addresses(binding);
        std::wstring text;
        for (const auto& ip : addresses) {
            if (!text.empty()) text += L"     ";
            text += ip;
        }
        if (text.empty()) text = L"No active address";
        if (text != address_text) {
            address_text = std::move(text);
            if (window) InvalidateRect(window, nullptr, FALSE);
        }
    }

    void stop() {
        std::lock_guard lock(mutex);
        stopping = true;
        // Wake a TCP read. UDP checks the stop flag on every bounded service poll.
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

    template<class ReadHeader, class ReadFrame>
    void receive_stream(ReadHeader read_header, ReadFrame read_frame) {
        show_status(L"Mac connected. Waiting for video");
        const wchar_t* next_status = L"Stream ended. Waiting for the Mac to reconnect";
        try {
            const auto header = read_header();
            Decoder decoder(header.width, header.height, header.fps, header.codec);
            auto display = [this](VideoFrame&& decoded) { present(std::move(decoded)); };
            std::vector<std::uint8_t> bytes;
            while (!stopping && read_frame(bytes)) {
                decoder.decode(bytes, display);
            }
            if (!stopping) decoder.finish(display);
        } catch (const std::exception& e) {
            if (!stopping) std::cerr << "Stream error: " << e.what() << '\n';
            next_status = L"Stream error. See the console for details. Waiting for a connection";
        }
        if (!stopping) show_status(next_status);
    }

    void listen(net::Socket& listener) {
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
            receive_stream([&] { return wire::receive_header(*client); },
                           [&](auto& bytes) { return wire::receive_frame(*client, bytes); });
            std::lock_guard lock(mutex);
            active.reset();
        }
    }

    void listen(udp::Listener& listener) {
        auto read = [&](std::vector<std::uint8_t>& bytes) {
            while (!stopping) {
                switch (listener.receive(bytes, 100)) {
                case udp::ReadResult::message: return true;
                case udp::ReadResult::disconnected: return false;
                case udp::ReadResult::timeout: break;
                }
            }
            return false;
        };
        while (!stopping) {
            if (!listener.accept(100)) continue;
            receive_stream([&] {
                std::vector<std::uint8_t> bytes;
                if (!read(bytes)) throw std::runtime_error("UDP stream ended before its header");
                return wire::decode_header(bytes);
            }, read);
            listener.disconnect();
        }
    }

    void receive(Listener& listener) {
        try {
            std::visit([this](auto& server) { listen(server); }, listener);
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

void paint_client(Receiver& receiver, HDC dc) {
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
        const SIZE pixels = win_info::monitor_pixels(receiver.window);
        std::wstring details = status;
        if (pixels.cx > 0 && pixels.cy > 0)
            details += L"\nDisplay: " + std::to_wstring(pixels.cx) + L" × " + std::to_wstring(pixels.cy) + L" px";
        details += L"\n\nLocal IP addresses (";
        details += receiver.transport == Transport::udp ? L"UDP " : L"TCP ";
        details += std::to_wstring(receiver.port) + L")\n" + receiver.address_text;
        const auto dpi = GetDpiForWindow(receiver.window);
        HFONT font = CreateFontW(-MulDiv(13, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ previous = SelectObject(dc, font ? static_cast<HGDIOBJ>(font) : GetStockObject(DEFAULT_GUI_FONT));
        const int inset = (std::min)(MulDiv(16, dpi, 96), width / 4);
        RECT bounds{inset, 0, width - inset, height};
        RECT measured = bounds;
        constexpr UINT flags = DT_CENTER | DT_WORDBREAK | DT_NOPREFIX;
        DrawTextW(dc, details.c_str(), -1, &measured, flags | DT_CALCRECT);
        bounds.top = (std::max)(0L, (height - measured.bottom) / 2);
        DrawTextW(dc, details.c_str(), -1, &bounds, flags);
        SelectObject(dc, previous);
        if (font) DeleteObject(font);
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
    case WM_PAINT: {
        PAINTSTRUCT state{};
        HDC dc = BeginPaint(window, &state);
        paint_client(*receiver, dc);
        EndPaint(window, &state);
        return 0;
    }
    case WM_PRINTCLIENT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        const int saved = SaveDC(dc);
        paint_client(*receiver, dc);
        if (saved) RestoreDC(dc, saved);
        GdiFlush(); // The caller can be reading the destination from another thread.
        return 0;
    }
    case WM_SIZE:
    case WM_MOVE:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (wp == address_timer) {
            {
                std::lock_guard lock(receiver->mutex);
                if (receiver->frame) return 0;
            }
            receiver->refresh_addresses();
            return 0;
        }
        break;
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
        InvalidateRect(window, nullptr, FALSE);
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
    win_info::KeepAwake keep_awake;
    Listener listener;
    if (options.transport == Transport::udp)
        listener.emplace<udp::Listener>(options.bind, options.port);
    else
        listener.emplace<net::Socket>(net::listen_tcp(options.bind, options.port));
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
    receiver.binding = win_info::resolve_binding(options.bind, options.transport == Transport::udp);
    receiver.port = options.port;
    receiver.transport = options.transport;
    receiver.refresh_addresses();
    HWND window = CreateWindowExW(0, window_class, L"MaxDisplay", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1000, 620,
                                  nullptr, nullptr, instance, &receiver);
    if (!window) throw std::system_error(GetLastError(), std::system_category(), "CreateWindowExW");
    SetTimer(window, address_timer, 5000, nullptr);
    ShowWindow(window, SW_SHOWNORMAL);
    if (!options.windowed) toggle_fullscreen(receiver);
    std::cout << "Listening on " << (options.transport == Transport::udp ? "UDP " : "TCP ")
              << options.bind << ':' << options.port
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
