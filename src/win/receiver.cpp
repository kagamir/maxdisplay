#include "app.hpp"
#include "decoder.hpp"
#include "display_info.hpp"
#include "cursor_overlay.hpp"
#include "waiting_screen.hpp"
#include "cursor_packet.hpp"
#include "frame_recovery.hpp"
#include "net.hpp"
#include "secure_tcp.hpp"
#include "udp.hpp"
#include "wire.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <cstdint>
#include <exception>
#include <future>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t window_class[] = L"MaxDisplayReceiver";
constexpr UINT listener_failed = WM_APP + 1;
constexpr UINT cursor_changed = WM_APP + 2;
constexpr UINT_PTR address_timer = 1;
constexpr UINT_PTR motion_timer = 2;

using Clock = std::chrono::steady_clock;
using Reason = FrameRecovery::Reason;
constexpr const char* request_names[] = {
    "startup", "sequence_gap", "clock_reset", "transit_deadline", "decode_deadline",
    "decoder_backlog", "malformed_frame", "decoder_error", "idle_drain"
};
constexpr auto reason_count = static_cast<std::size_t>(Reason::count);
static_assert(std::size(request_names) == reason_count);

struct StreamStats {
    std::uint64_t received = 0, decoded = 0, decode_us = 0, idle_drains = 0;
    std::array<std::uint64_t, reason_count> key_requests{};
    Clock::time_point last_report;
};

// Charge drain time as well: buffered decoders may produce their picture in finish().
struct DecodeTimer {
    std::uint64_t* total;
    Clock::time_point start;
    explicit DecodeTimer(std::uint64_t* total)
        : total(total), start(total ? Clock::now() : Clock::time_point{}) {}
    ~DecodeTimer() {
        if (total) *total += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
    }
};

struct PaintStats {
    std::uint64_t presented = 0, paints = 0, paint_us = 0;
    std::uint64_t cursor_updates = 0, cursor_gaps = 0, cursor_repaints = 0;
};

struct Receiver {
    explicit Receiver(const std::string& password) : password(password) {}
    const std::string& password;
    HWND window = nullptr;
    win_info::Binding binding{};
    std::uint16_t port = 0;
    std::wstring address_text;
    SIZE monitor_size{};
    const Clock::time_point opened = Clock::now();
    bool motion_timer_running = false; // UI thread only.
    bool require_hardware = false;
    bool stats_enabled = false;
    bool fullscreen = false;
    WINDOWPLACEMENT windowed_placement{};
    std::exception_ptr listener_error; // Read after joining the worker.
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::shared_ptr<net::Socket> active_tcp;
    std::shared_ptr<const VideoFrame> frame;
    std::uint32_t frame_sequence = 0; // Published under the same lock as the decoded pixels.
    std::uint64_t session_id = 0;
    PaintStats paint_stats; // Updated by WM_PAINT and exchanged by the worker under mutex.
    std::weak_ptr<const VideoFrame> last_painted; // UI thread only.
    const wchar_t* status = L"Waiting for the Mac to connect";
    std::optional<udp::Listener> udp_listener;
    net::Socket ipv6_cursor; // ENet uses IPv4. IPv6 TCP video still supports a same-port UDP cursor.
    std::thread cursor_worker;
    std::atomic<bool> cursor_stopping{false};
    struct SessionCursor {
        std::uint64_t session = 0;
        cursor::View view{};
    };
    SessionCursor pending_cursor; // Published by the cursor worker under mutex.
    bool cursor_posted = false;
    SessionCursor visible_cursor; // UI thread only, consistent across a WM_PAINT.
    bool painting_cursor_update = false;
    win_cursor::Overlay overlay;

    void publish_cursor(cursor::View view) {
        std::lock_guard lock(mutex);
        pending_cursor = {session_id, std::move(view)};
        if (!cursor_posted) cursor_posted = PostMessageW(window, cursor_changed, 0, 0) != FALSE;
    }

    void apply_cursor() {
        SessionCursor next;
        std::shared_ptr<const VideoFrame> video_frame;
        std::uint32_t sequence;
        std::uint64_t session;
        {
            std::lock_guard lock(mutex);
            cursor_posted = false;
            next = pending_cursor;
            video_frame = frame;
            sequence = frame_sequence;
            session = session_id;
        }
        RECT old_bounds{}, new_bounds{};
        if (video_frame) {
            RECT client{};
            GetClientRect(window, &client);
            const RECT video = win_cursor::fit_video(client, *video_frame);
            if (visible_cursor.session == session)
                old_bounds = overlay.bounds(video, *video_frame, visible_cursor.view, sequence, session);
            if (next.session == session)
                new_bounds = overlay.bounds(video, *video_frame, next.view, sequence, session);
        }
        visible_cursor = std::move(next);
        if (!visible_cursor.view.image) overlay.forget_shape();
        if (!IsRectEmpty(&old_bounds)) InvalidateRect(window, &old_bounds, FALSE);
        if (!IsRectEmpty(&new_bounds)) InvalidateRect(window, &new_bounds, FALSE);
        if (!IsRectEmpty(&old_bounds) || !IsRectEmpty(&new_bounds)) {
            painting_cursor_update = true;
            UpdateWindow(window);
            painting_cursor_update = false;
        }
    }

    bool read_cursor(cursor::Bytes& bytes, net::Endpoint& from) {
        if (udp_listener) return udp_listener->receive_cursor(bytes, from, 25);
        return net::wait_readable(ipv6_cursor, 25) && ipv6_cursor.receive_from(from, bytes) &&
               cursor::packet::unwrap(bytes);
    }

    void reply_cursor(const cursor::Bytes& bytes, const net::Endpoint& to) {
        if (udp_listener) udp_listener->send_cursor(bytes, to);
        else ipv6_cursor.send_to(to, cursor::packet::wrap(bytes));
    }

    void cursor_loop(net::Endpoint video_peer, std::uint64_t token, std::unique_ptr<secure::Datagrams> crypto) {
        try {
            cursor::Receiver link(token);
            const auto start = Clock::now();
            auto elapsed_ms = [&] {
                return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            };
            std::uint64_t revision = link.revision();
            cursor::ReceiverStats previous{};
            std::optional<net::Endpoint> cursor_peer;
            while (!stopping && !cursor_stopping) {
                net::Endpoint source{};
                cursor::Bytes bytes;
                const bool received = read_cursor(bytes, source);
                const auto now = elapsed_ms();
                if (received && source.same_address(video_peer) && (!cursor_peer || source == *cursor_peer)) {
                    if (crypto) {
                        auto packet = crypto->open(bytes);
                        if (packet) bytes = std::move(*packet);
                        else bytes.clear();
                    }
                    if (auto ack = link.receive(now, bytes)) {
                        // The first valid state or shape identifies the Mac's UDP source port.
                        cursor_peer = source;
                        reply_cursor(crypto ? crypto->seal(*ack) : *ack, source);
                    }
                }
                link.expire(now);
                if (link.revision() != revision) {
                    revision = link.revision();
                    publish_cursor(link.view());
                }
                if (stats_enabled) {
                    const auto totals = link.stats();
                    if (totals.updates != previous.updates || totals.gaps != previous.gaps) {
                        std::lock_guard lock(mutex);
                        paint_stats.cursor_updates += totals.updates - previous.updates;
                        paint_stats.cursor_gaps += totals.gaps - previous.gaps;
                        previous = totals;
                    }
                }
            }
        } catch (const std::exception& e) {
            if (!stopping && !cursor_stopping) std::cerr << "Cursor channel stopped: " << e.what() << '\n';
        }
        publish_cursor({});
    }

    template<class Channel>
    wire::CursorOffer offer_cursor(Channel& channel) {
        try {
            const auto peer = channel.peer_endpoint();
            const auto token = secure::random_token();
            auto crypto = channel.datagrams();
            if (udp_listener) udp_listener->open_cursor(peer, token);
            cursor_stopping = false;
            cursor_worker = std::thread(&Receiver::cursor_loop, this, peer, token, std::move(crypto));
            return token;
        } catch (const std::exception& e) {
            if (udp_listener) udp_listener->close_cursor();
            std::cerr << "Cursor channel unavailable: " << e.what() << '\n';
            return {};
        }
    }

    void end_cursor() {
        cursor_stopping = true;
        if (udp_listener) udp_listener->close_cursor();
        if (cursor_worker.joinable()) cursor_worker.join();
    }

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

    void animate_waiting_screen(bool animate) {
        if (animate == motion_timer_running) return;
        if (animate) {
            motion_timer_running = SetTimer(window, motion_timer, 250, nullptr) != 0;
        } else {
            KillTimer(window, motion_timer);
            motion_timer_running = false;
        }
    }

    void stop() {
        std::lock_guard lock(mutex);
        stopping = true;
        // Wake a TCP read. UDP checks the stop flag on every bounded service poll.
        if (active_tcp) active_tcp->shutdown();
    }

    void show_status(const wchar_t* message) {
        {
            std::lock_guard lock(mutex);
            frame.reset();
            status = message;
        }
        InvalidateRect(window, nullptr, FALSE);
    }

    void present(VideoFrame&& decoded, std::uint32_t sequence) {
        auto next = std::make_shared<VideoFrame>(std::move(decoded));
        {
            std::lock_guard lock(mutex);
            frame = std::move(next);
            frame_sequence = sequence;
        }
        // Windows coalesces invalid regions. A slow repaint takes the newest frame.
        InvalidateRect(window, nullptr, FALSE);
    }

    void report_if_due(StreamStats& stats, const Decoder& decoder, Codec codec) {
        const auto now = Clock::now();
        const std::chrono::duration<double> elapsed = now - stats.last_report;
        if (elapsed.count() < 1.0) return;
        PaintStats painted;
        {
            std::lock_guard lock(mutex);
            painted = std::exchange(paint_stats, PaintStats{});
        }
        std::ostringstream line;
        line << std::fixed << std::setprecision(2)
             << "receiver stats window_s=" << elapsed.count() << " codec=" << codec_name(codec)
             << " decoder=" << decoder.backend_name() << " received=" << stats.received
             << " decoded=" << stats.decoded << " presented=" << painted.presented
             << " decode_ms=";
        if (stats.decoded) line << stats.decode_us / (1000.0 * stats.decoded);
        else line << "n/a";
        line << " paint_ms=";
        if (painted.paints) line << painted.paint_us / (1000.0 * painted.paints);
        else line << "n/a";
        line << " cursor_updates=" << painted.cursor_updates << " cursor_gaps=" << painted.cursor_gaps
             << " cursor_repaints=" << painted.cursor_repaints
             << " idle_drains=" << stats.idle_drains << " key_requests=";
        for (std::size_t i = 0; i < reason_count; ++i)
            line << (i ? "," : "") << request_names[i] << '=' << stats.key_requests[i];
        std::clog << line.str() << '\n';
        stats = {};
        stats.last_report = now;
    }

    template<class ReadFrame, class SendFeedback>
    void receive_live(wire::Header header, Decoder& decoder, StreamStats& stats,
                      ReadFrame& read_frame, SendFeedback& feedback) {
        FrameRecovery recovery(header.timeout_ms);
        bool used_decoder = false;
        // The low-delay codecs produce in input order, including frames emitted during finish().
        struct QueuedFrame { std::uint32_t sequence; Clock::time_point deadline; };
        std::deque<QueuedFrame> pending;
        Clock::time_point drain_at;
        auto acknowledge = [&] {
            feedback(wire::pack_feedback({recovery.sequence(), recovery.waiting_for_keyframe()}));
            if (stats_enabled && recovery.waiting_for_keyframe())
                ++stats.key_requests[static_cast<std::size_t>(recovery.reason())];
        };
        auto display = [&](VideoFrame&& decoded) {
            if (stats_enabled) ++stats.decoded;
            if (pending.empty()) return;
            const auto ready = pending.front();
            pending.pop_front();
            if (Clock::now() <= ready.deadline && !recovery.waiting_for_keyframe())
                present(std::move(decoded), ready.sequence);
            else if (!recovery.waiting_for_keyframe())
                recovery.recover(Reason::decode_deadline);
        };
        auto poll_interval = [&]() -> unsigned {
            if (stats_enabled) report_if_due(stats, decoder, header.codec);
            if (pending.empty() || recovery.waiting_for_keyframe()) return 100;
            const auto now = Clock::now();
            if (now < drain_at) {
                const auto ms = std::chrono::ceil<std::chrono::milliseconds>(drain_at - now).count();
                return static_cast<unsigned>(std::min<std::int64_t>(100, ms));
            }
            // Some MFTs buffer an input even in low-latency mode. A static Mac
            // desktop might never send the next sample that would release it.
            if (stats_enabled) ++stats.idle_drains;
            try {
                DecodeTimer timer(stats_enabled ? &stats.decode_us : nullptr);
                decoder.finish(display);
            } catch (const HardwareDecodeUnavailable&) {
                throw;
            } catch (const std::runtime_error& e) {
                std::cerr << "Failed to drain video decoder: " << e.what() << '\n';
                recovery.recover();
            }
            const bool need_image = recovery.waiting_for_keyframe() || !pending.empty();
            pending.clear();
            if (!recovery.waiting_for_keyframe()) recovery.recover(Reason::idle_drain);
            // A successfully displayed static picture can stay idle indefinitely.
            // Request a new key only on the next delta, or if the drain failed.
            if (need_image) acknowledge();
            return 100;
        };

        std::vector<std::uint8_t> bytes;
        while (!stopping && read_frame(bytes, poll_interval)) {
            if (stats_enabled) ++stats.received;
            const auto now = Clock::now();
            const auto arrival = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch());
            wire::Frame packet;
            try {
                packet = wire::unpack_frame(bytes);
            } catch (const std::runtime_error&) {
                recovery.recover(Reason::malformed_frame);
                acknowledge();
                continue;
            }
            if (!recovery.waiting_for_keyframe()) {
                if (pending.size() >= 32) recovery.recover(Reason::decoder_backlog);
                else if (!pending.empty() && now > pending.front().deadline) recovery.recover(Reason::decode_deadline);
            }
            const bool keyframe = packet.keyframe && has_recovery_picture(header.codec, packet.data);
            const auto action = recovery.inspect(packet.sequence, packet.timestamp_us, keyframe, arrival.count());
            if (action != FrameRecovery::Action::drop) {
                if (action == FrameRecovery::Action::restart) {
                    pending.clear();
                    if (used_decoder) decoder.reset();
                }
                used_decoder = true;
                pending.push_back({packet.sequence, now + std::chrono::microseconds(recovery.remaining_us())});
                try {
                    DecodeTimer timer(stats_enabled ? &stats.decode_us : nullptr);
                    if (!decoder.decode(packet.data, display)) recovery.recover();
                } catch (const HardwareDecodeUnavailable&) {
                    throw;
                } catch (const std::runtime_error& e) {
                    std::cerr << "Discarding damaged " << codec_name(header.codec)
                              << " frame " << packet.sequence << ": " << e.what() << '\n';
                    recovery.recover();
                }
                if (!pending.empty()) {
                    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
                        pending.front().deadline - now).count();
                    const auto delay = recovery.idle_drain_us(header.fps, std::max<std::int64_t>(0, remaining));
                    drain_at = now + std::chrono::microseconds(delay);
                }
            }
            acknowledge();
        }
        if (!stopping && !pending.empty() && !recovery.waiting_for_keyframe()) {
            DecodeTimer timer(stats_enabled ? &stats.decode_us : nullptr);
            decoder.finish(display);
        }
    }

    template<class ReadHeader, class ReadFrame, class SendFeedback>
    const wchar_t* receive_stream(ReadHeader read_header, ReadFrame read_frame, SendFeedback feedback) {
        {
            std::lock_guard lock(mutex);
            ++session_id;
            paint_stats = {};
        }
        show_status(L"Mac connected. Waiting for video");
        const wchar_t* next_status = L"Stream ended. Waiting for the Mac to reconnect";
        try {
            const auto header = read_header();
            Decoder decoder(header.width, header.height, header.fps, header.codec, require_hardware);
            win_info::KeepAwake keep_awake;
            StreamStats stats;
            if (stats_enabled) stats.last_report = Clock::now();
            receive_live(header, decoder, stats, read_frame, feedback);
            if (stats_enabled) report_if_due(stats, decoder, header.codec);
        } catch (const std::exception& e) {
            if (!stopping) std::cerr << "Stream error: " << e.what() << '\n';
            next_status = L"Stream error. See the console for details. Waiting for a connection";
        }
        end_cursor();
        return next_status;
    }

    wire::DisplaySize display_size() const {
        const SIZE pixels = win_info::monitor_pixels(window);
        if (pixels.cx <= 0 || pixels.cy <= 0)
            throw std::runtime_error("Cannot read the resolution of the receiver window's monitor");
        return {static_cast<std::uint32_t>(pixels.cx), static_cast<std::uint32_t>(pixels.cy)};
    }

    const wchar_t* receive_tcp(net::Socket& client) {
        std::optional<secure::TcpChannel> channel;
        return receive_stream([&] {
            channel.emplace(client, password, secure::Role::receiver);
            return channel->receive_header([&] { return display_size(); }, [&] { return offer_cursor(*channel); });
        }, [&](auto& bytes, const auto& poll) { return channel->receive_frame(bytes, poll); },
           [&](const auto& ack) { channel->send_feedback(ack); });
    }

    const wchar_t* receive_udp() {
        auto& listener = *udp_listener;
        auto read = [&](std::vector<std::uint8_t>& bytes, const auto& poll) {
            while (!stopping) {
                switch (listener.receive(bytes, poll())) {
                case udp::ReadResult::message: return true;
                case udp::ReadResult::disconnected: return false;
                case udp::ReadResult::timeout: break;
                }
            }
            return false;
        };
        return receive_stream([&] {
            return listener.receive_header([&] { return display_size(); }, [&] { return stopping.load(); },
                                           [&] { return offer_cursor(listener); });
        }, read, [&](const auto& ack) { listener.send_feedback(ack); });
    }

    void receive(net::Socket& tcp) {
        // This thread owns admission across both transports. While the decode worker
        // displays one session, competing connections are accepted and closed.
        std::future<const wchar_t*> video;
        bool serving_udp = false;
        auto finish_if_ready = [&] {
            if (!video.valid() || video.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
            const auto message = video.get();
            if (serving_udp) udp_listener->disconnect();
            else {
                std::lock_guard lock(mutex);
                active_tcp.reset();
            }
            serving_udp = false;
            if (!stopping) show_status(message);
        };
        try {
            while (!stopping) {
                finish_if_ready();
                if (udp_listener && !serving_udp && udp_listener->accept(0)) {
                    if (video.valid()) {
                        udp_listener->disconnect();
                    } else {
                        serving_udp = true;
                        video = std::async(std::launch::async, &Receiver::receive_udp, this);
                    }
                }
                if (!net::wait_readable(tcp, 15)) continue;
                auto accepted = net::accept_tcp(tcp);
                finish_if_ready();
                if (!accepted || video.valid()) continue;
                auto client = std::make_shared<net::Socket>(std::move(accepted));
                {
                    std::lock_guard lock(mutex);
                    if (stopping) break;
                    active_tcp = client;
                }
                video = std::async(std::launch::async, [this, client] { return receive_tcp(*client); });
            }
        } catch (...) {
            listener_error = std::current_exception();
            stop();
            PostMessageW(window, listener_failed, 0, 0);
        }
        if (video.valid()) video.wait();
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

void paint_client(Receiver& receiver, HDC dc, bool track_paint) {
    RECT area{};
    GetClientRect(receiver.window, &area);
    const int width = area.right;
    const int height = area.bottom;
    const HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));

    std::shared_ptr<const VideoFrame> frame;
    const wchar_t* status;
    std::uint64_t session_id;
    std::uint32_t sequence;
    {
        std::lock_guard lock(receiver.mutex);
        session_id = receiver.session_id;
        frame = receiver.frame;
        sequence = receiver.frame_sequence;
        status = receiver.status;
    }
    receiver.animate_waiting_screen(!frame && !IsIconic(receiver.window));
    if (!frame) {
        const SIZE pixels = receiver.monitor_size;
        std::wstring details = status;
        if (pixels.cx > 0 && pixels.cy > 0)
            details += L"\nDisplay: " + std::to_wstring(pixels.cx) + L" × " + std::to_wstring(pixels.cy) + L" px";
        details += L"\n\nLocal IP addresses (";
        details += receiver.udp_listener ? L"TCP and UDP " : L"TCP video and UDP cursor ";
        details += std::to_wstring(receiver.port) + L")\n" + receiver.address_text;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - receiver.opened);
        win_waiting::paint(dc, area, details, GetDpiForWindow(receiver.window), elapsed.count());
    } else if (width > 0 && height > 0) {
        const auto start = track_paint ? Clock::now() : Clock::time_point{};
        const auto video = win_cursor::fit_video(area, *frame);
        const RECT bars[] = {{0, 0, width, video.top}, {0, video.bottom, width, height},
                             {0, video.top, video.left, video.bottom}, {video.right, video.top, width, video.bottom}};
        for (const RECT& bar : bars) {
            if (!IsRectEmpty(&bar)) FillRect(dc, &bar, black);
        }
        const auto& shown = receiver.visible_cursor;
        const bool drew = receiver.overlay.paint(dc, video, *frame,
            shown.session == session_id ? shown.view : cursor::View{}, sequence, session_id);
        if (track_paint && drew) {
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
            std::lock_guard lock(receiver.mutex);
            if (session_id != receiver.session_id) return;
            if (receiver.last_painted.lock() != frame) ++receiver.paint_stats.presented;
            receiver.last_painted = frame;
            ++receiver.paint_stats.paints;
            if (receiver.painting_cursor_update) ++receiver.paint_stats.cursor_repaints;
            receiver.paint_stats.paint_us += us;
        }
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
    case WM_SETCURSOR:
        if (reinterpret_cast<HWND>(wp) == window && LOWORD(lp) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_WINDOWPOSCHANGED: {
        const auto result = DefWindowProcW(window, message, wp, lp);
        // Showing or resizing the viewer can put it under a stationary pointer.
        POINT point{};
        if (IsWindowVisible(window) && !IsIconic(window) && !GetCapture() &&
            GetCursorPos(&point) && WindowFromPoint(point) == window) {
            const auto hit = SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
            SendMessageW(window, WM_SETCURSOR, reinterpret_cast<WPARAM>(window), MAKELPARAM(hit, 0));
        }
        return result;
    }
    case cursor_changed:
        receiver->apply_cursor();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT state{};
        HDC dc = BeginPaint(window, &state);
        paint_client(*receiver, dc, receiver->stats_enabled);
        EndPaint(window, &state);
        return 0;
    }
    case WM_PRINTCLIENT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        const int saved = SaveDC(dc);
        paint_client(*receiver, dc, false);
        if (saved) RestoreDC(dc, saved);
        GdiFlush(); // The caller can be reading the destination from another thread.
        return 0;
    }
    case WM_SIZE:
    case WM_MOVE:
        receiver->monitor_size = win_info::monitor_pixels(window);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (wp == motion_timer) {
            std::lock_guard lock(receiver->mutex);
            if (receiver->frame || IsIconic(window)) receiver->animate_waiting_screen(false);
            else InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (wp == address_timer) {
            {
                std::lock_guard lock(receiver->mutex);
                if (receiver->frame) return 0;
            }
            receiver->refresh_addresses();
            return 0;
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
        receiver->monitor_size = win_info::monitor_pixels(window);
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
    auto tcp = net::listen_tcp(options.bind, options.port);
    Receiver receiver(options.password);
    receiver.port = tcp.local_port();
    receiver.binding = win_info::socket_binding(tcp.native_handle());
    const auto& address = receiver.binding.address;
    const auto bind = address.empty() ? (receiver.binding.family == AF_INET6 ? "::" : "0.0.0.0")
                                      : std::string(address.begin(), address.end());
    if (receiver.binding.family == AF_INET6) receiver.ipv6_cursor = net::bind_udp(bind, receiver.port);
    else receiver.udp_listener.emplace(bind, receiver.port, options.password);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = window_class;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::system_error(GetLastError(), std::system_category(), "RegisterClassExW");

    receiver.require_hardware = options.require_hardware;
    receiver.stats_enabled = options.stats;
    receiver.refresh_addresses();
    HWND window = CreateWindowExW(0, window_class, L"MaxDisplay", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1000, 620,
                                  nullptr, nullptr, instance, &receiver);
    if (!window) throw std::system_error(GetLastError(), std::system_category(), "CreateWindowExW");
    SetTimer(window, address_timer, 5000, nullptr);
    ShowWindow(window, SW_SHOWNORMAL);
    if (!options.windowed) toggle_fullscreen(receiver);
    std::cout << "Listening on " << (receiver.udp_listener ? "TCP and UDP " : "TCP video and UDP cursor ")
              << (receiver.binding.family == AF_INET6 ? "[" + bind + "]" : bind) << ':' << receiver.port
              << (options.password.empty() ? "" : " (AES-128-GCM required)")
              << ". F11 toggles full screen. Esc exits.\n" << std::flush;

    try {
        std::thread worker([&] { receiver.receive(tcp); });
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
