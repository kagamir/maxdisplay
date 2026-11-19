// Drives the normal sender in a logged-in session with Screen Recording permission.
#include "app.hpp"
#include "av1.hpp"
#include "cursor_link.hpp"
#include "frame_recovery.hpp"
#include "secure_tcp.hpp"
#include "udp.hpp"
#include "wire.hpp"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreMedia/CoreMedia.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <future>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace std::chrono_literals;

std::vector<CGDirectDisplayID> online_displays() {
    uint32_t count = 0;
    if (CGGetOnlineDisplayList(0, nullptr, &count) != kCGErrorSuccess)
        throw std::runtime_error("Cannot count online displays");
    std::vector<CGDirectDisplayID> ids(count);
    if (CGGetOnlineDisplayList(count, ids.data(), &count) != kCGErrorSuccess)
        throw std::runtime_error("Cannot enumerate online displays");
    ids.resize(count);
    return ids;
}

void check_layout(const std::vector<CGDirectDisplayID>& original, unsigned width, unsigned height) {
    for (auto id : online_displays()) {
        if (std::find(original.begin(), original.end(), id) != original.end()) continue;
        const CGRect bounds = CGDisplayBounds(id);
        if (bounds.size.width == width && bounds.size.height == height) return;
        std::cerr << "Observed virtual desktop bounds " << bounds.size.width << 'x' << bounds.size.height << '\n';
    }
    throw std::runtime_error("No new virtual display with the expected logical desktop dimensions");
}

// The sender writes four-byte Annex B start codes before each parameter set.
// Ask CoreMedia for the geometry described by the encoded bits independently of the wire header.
void check_h26x_geometry(const wire::Frame& frame, Codec codec, unsigned width, unsigned height) {
    constexpr std::array<uint8_t, 4> start{0, 0, 0, 1};
    std::array<const uint8_t*, 3> parameters{};
    std::array<size_t, 3> lengths{};
    const bool hevc = codec == Codec::h265;
    const size_t count = hevc ? 3 : 2;
    auto it = std::search(frame.data.begin(), frame.data.end(), start.begin(), start.end());
    while (it != frame.data.end()) {
        auto nal = it + start.size();
        auto end = std::search(nal, frame.data.end(), start.begin(), start.end());
        if (nal != end) {
            const int index = hevc ? ((*nal >> 1) & 63) - 32 : (*nal & 31) - 7;
            if (index >= 0 && static_cast<size_t>(index) < count) {
                parameters[index] = &*nal;
                lengths[index] = static_cast<size_t>(end - nal);
            }
        }
        if (std::all_of(lengths.begin(), lengths.begin() + count, [](size_t n) { return n > 0; })) break;
        it = end;
    }
    if (std::find(lengths.begin(), lengths.begin() + count, 0u) != lengths.begin() + count)
        throw std::runtime_error("Incomplete parameter sets in captured picture");
    CMFormatDescriptionRef format = nullptr;
    const auto status = hevc
        ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(nullptr, count, parameters.data(), lengths.data(), 4,
                                                              nullptr, &format)
        : CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr, count, parameters.data(), lengths.data(), 4,
                                                              &format);
    if (status != noErr || !format) throw std::runtime_error("Cannot read captured picture format");
    const auto dimensions = CMVideoFormatDescriptionGetPresentationDimensions(format, false, true);
    CFRelease(format);
    if (dimensions.width != width || dimensions.height != height)
        throw std::runtime_error("Encoded dimensions disagree with the requested size");
}

void check_av1(const std::vector<uint8_t>& frame, uint32_t width, uint32_t height) {
    Av1Decoder decoder(width, height);
    bool decoded = false;
    decoder.decode(frame, [&](VideoFrame&& image) {
        if (image.width != width || image.height != height || image.bgra.size() != width * height * 4)
            throw std::runtime_error("Incorrect scaled AV1 desktop dimensions");
        decoded = true;
    });
    if (!decoded) throw std::runtime_error("No picture decoded from the first AV1 desktop frame");
}

// Exercise the production sampler, cipher and ARQ without moving the runner's real mouse.
// Wait for distinct position sequences after acknowledging its bitmap. A stalled sender
// that just retransmits the same tail packet cannot satisfy this check.
void check_cursor(udp::Listener& listener, std::uint64_t token,
                  std::unique_ptr<secure::Datagrams> cipher, bool interrupt, const std::atomic<bool>& video_ready) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    cursor::Receiver receiver(token);
    bool shape_seen = false;
    std::optional<Clock::time_point> reopen_at, first_reply;
    std::optional<std::uint32_t> last_seq, original_floor, fallback_seq;
    unsigned positions = 0;
    while (Clock::now() - start < 30s) {
        net::Endpoint source;
        cursor::Bytes bytes;
        if (!listener.receive_cursor(bytes, source, 50)) continue;
        const auto now = Clock::now();
        // In this profile the stream must start with the pointer in video.
        // Once it hands off to the overlay, simulate a full bidirectional outage.
        if (interrupt && (!video_ready || (reopen_at && now < *reopen_at))) continue;
        if (cipher) {
            auto plain = cipher->open(bytes);
            if (!plain) throw std::runtime_error("Mac cursor datagram did not authenticate");
            bytes = std::move(*plain);
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        auto ack = receiver.receive(elapsed, bytes);
        if (!ack) throw std::runtime_error("Invalid Mac cursor packet");
        listener.send_cursor(cipher ? cipher->seal(*ack) : *ack, source);
        if (reopen_at && !first_reply) first_reply = now;
        if (bytes[8] == cursor::detail::shape_type) shape_seen = true;
        if (!shape_seen || bytes[8] != cursor::detail::state_type) continue;
        const auto seq = static_cast<std::uint32_t>(cursor::detail::get(bytes, 10, 4));
        if (last_seq && !cursor::detail::newer(seq, *last_seq)) continue;
        last_seq = seq;
        if (!interrupt) {
            if (++positions == 2) return;
            continue;
        }

        // The Mac attaches a video floor even for an off-display hotspot. This
        // verifies both completed handoffs without warping the system pointer.
        const bool fenced = (bytes[9] & 2) != 0;
        if (fenced && !reopen_at) {
            original_floor = static_cast<std::uint32_t>(cursor::detail::get(bytes, 30, 4));
            reopen_at = now + 3s;
            std::cout << "Cursor handoff at video sequence " << *original_floor << "; blocking UDP for 3 s\n";
        } else if (first_reply && !fenced && bytes[9] == 0) {
            fallback_seq = seq;
        } else if (first_reply && fenced) {
            const auto floor = static_cast<std::uint32_t>(cursor::detail::get(bytes, 30, 4));
            if (!cursor::detail::newer(floor, *original_floor)) continue;
            if (!fallback_seq) throw std::runtime_error("Cursor overlay did not transition through the video fallback");
            if (now - *first_reply < 980ms)
                throw std::runtime_error("Cursor overlay resumed before a second of healthy ACKs");
            std::cout << "Cursor UDP recovered at fresh video sequence " << floor << '\n';
            return;
        }
    }
    if (!shape_seen) throw std::runtime_error("No valid cursor bitmap received from Mac");
    if (!interrupt) throw std::runtime_error("No acknowledged cursor heartbeats from Mac");
    if (!original_floor) throw std::runtime_error("Mac never handed off from video to the cursor overlay");
    if (!fallback_seq) throw std::runtime_error("Mac never hid the cursor overlay after the UDP outage");
    throw std::runtime_error("Mac never confirmed a fresh cursor-free frame after UDP recovered");
}

void capture(Codec codec, Transport transport, double scale, bool automatic, wire::DisplaySize expected, bool encrypted) {
    SenderOptions options;
    options.codec = codec;
    options.transport = transport;
    options.host = "127.0.0.1";
    options.video_cursor = !encrypted && codec == Codec::av1;
    const bool unavailable_cursor = !encrypted && codec == Codec::h264;
    if (encrypted) options.password = "MaxDisplay test passphrase";
    options.fps = 30;
    // Leave time to decode the scaled AV1 keyframe on a shared runner.
    options.timeout_ms = 1000;
    constexpr uint32_t logical_width = 1280, logical_height = 720;
    options.scale = scale;
    options.width = automatic ? 0 : logical_width;
    options.height = automatic ? 0 : logical_height;
    const auto original = online_displays();

    net::Socket tcp;
    if (transport == Transport::tcp) tcp = net::listen_tcp(options.host, 0);
    auto incoming_udp = std::make_unique<udp::Listener>(options.host, tcp ? tcp.local_port() : 0, options.password);
    options.port = incoming_udp->local_port();

    std::promise<void> stopped;
    auto stopped_future = stopped.get_future();
    auto receive = std::async(std::launch::async, [&] {
        // Release the listener if validation fails, so the sender observes disconnect.
        auto listener = std::move(incoming_udp);
        auto* datagrams = transport == Transport::udp ? listener.get() : nullptr;
        wire::Header mode{};
        net::Socket peer;
        std::optional<secure::TcpChannel> channel;
        bool queried = false, cursor_queried = false;
        std::atomic<bool> video_ready{false};
        std::future<void> side;
        auto offer_cursor = [&]() -> wire::CursorOffer {
            if (queried || cursor_queried || options.video_cursor)
                throw std::runtime_error("Unexpected cursor query");
            cursor_queried = true;
            if (unavailable_cursor) return {};
            const auto token = secure::random_token();
            listener->open_cursor(datagrams ? datagrams->peer_endpoint() : channel->peer_endpoint(), token);
            auto cipher = datagrams ? datagrams->datagrams() : channel->datagrams();
            side = std::async(std::launch::async, check_cursor, std::ref(*listener), token, std::move(cipher),
                              encrypted && transport == Transport::udp, std::cref(video_ready));
            return token;
        };
        auto monitor = [&] {
            if (!automatic || queried) throw std::runtime_error("Unexpected display size query");
            queried = true;
            return expected;
        };
        if (datagrams) {
            if (!datagrams->accept(8000)) throw std::runtime_error("UDP sender did not connect");
            mode = datagrams->receive_header(monitor, {}, offer_cursor);
        } else {
            if (!net::wait_readable(tcp, 8000)) throw std::runtime_error("TCP sender did not connect");
            peer = net::accept_tcp(tcp);
            if (!peer) throw std::runtime_error("TCP connection withdrawn");
            channel.emplace(peer, options.password, secure::Role::receiver);
            mode = channel->receive_header(monitor, offer_cursor);
        }
        if (cursor_queried == options.video_cursor) throw std::runtime_error("Unexpected cursor negotiation mode");
        if (queried != automatic || mode.width != expected.width || mode.height != expected.height || mode.fps != options.fps ||
            mode.codec != codec || mode.timeout_ms != options.timeout_ms)
            throw std::runtime_error("Unexpected captured desktop mode");
        std::cout << "Received v3 header for " << codec_name(codec) << '\n' << std::flush;
        auto poll_frame = [&]() -> std::optional<wire::Frame> {
            std::vector<uint8_t> message;
            if (datagrams) {
                const auto result = datagrams->receive(message, 100);
                if (result == udp::ReadResult::timeout) return {};
                if (result == udp::ReadResult::disconnected) throw std::runtime_error("UDP sender disconnected");
            } else {
                if (!net::wait_readable(peer, 100)) return {};
                if (!channel->receive_frame(message)) throw std::runtime_error("TCP sender disconnected");
            }
            return wire::unpack_frame(message);
        };
        auto receive_frame = [&](std::chrono::steady_clock::time_point deadline, const char* stage) {
            while (std::chrono::steady_clock::now() < deadline)
                if (auto frame = poll_frame()) return std::move(*frame);
            throw std::runtime_error(std::string("Timed out during ") + stage);
        };
        auto feedback = [&](uint32_t sequence, bool request_keyframe) {
            auto bytes = wire::pack_feedback({sequence, request_keyframe});
            if (datagrams) datagrams->send_feedback(bytes);
            else channel->send_feedback(bytes);
        };
        auto check_keyframe = [&](const wire::Frame& key) {
            if (!key.keyframe) throw std::runtime_error("Missing keyframe flag");
            if (codec == Codec::av1) {
                check_av1(key.data, expected.width, expected.height);
                return;
            }
            if (!has_recovery_picture(codec, key.data))
                throw std::runtime_error("Random access slice is missing its codec parameter sets");
            check_h26x_geometry(key, codec, expected.width, expected.height);
        };
        auto wait_key = [&](const char* stage, uint32_t previous, bool request_missing) {
            const auto deadline = std::chrono::steady_clock::now() + 8s;
            unsigned received = 0;
            for (;;) {
                auto next = receive_frame(deadline, stage);
                if (!received++ || next.keyframe)
                    std::cout << stage << ": received sequence " << next.sequence
                              << (next.keyframe ? " keyframe" : " delta") << '\n' << std::flush;
                if (next.keyframe && (!previous || next.sequence > previous)) {
                    check_keyframe(next);
                    return next.sequence;
                }
                // A receiver with missing references repeats its request on every delta.
                if (request_missing) feedback(next.sequence, true);
            }
        };
        auto first = wait_key("initial keyframe", 0, true);
        check_layout(original, logical_width, logical_height);
        feedback(first, true);
        auto recovery = wait_key("requested keyframe", first, true);
        // Simulate the last picture going missing: send no feedback or further request.
        auto retry = wait_key("unacknowledged final picture", recovery, false);
        feedback(retry, false);
        video_ready = true;
        if (side.valid()) {
            while (side.wait_for(0ms) != std::future_status::ready) {
                if (auto next = poll_frame()) feedback(next->sequence, false);
            }
            side.get();
        }
        std::raise(SIGINT);
        const auto end = std::chrono::steady_clock::now() + 4s;
        while (stopped_future.wait_for(0ms) != std::future_status::ready &&
               std::chrono::steady_clock::now() < end) {
            if (!datagrams) {
                stopped_future.wait_for(4s);
                break;
            }
            std::vector<uint8_t> ignored;
            if (datagrams->receive(ignored, 100) == udp::ReadResult::disconnected) break;
        }
    });
    int status = 1;
    std::exception_ptr sender_error;
    try {
        status = run_sender(options);
    } catch (const std::exception& error) {
        std::cerr << "Mac sender: " << error.what() << '\n';
        sender_error = std::current_exception();
    }
    stopped.set_value();
    // Surface a peer-side failure (e.g. missing keyframe) even if it closed the connection.
    receive.get();
    if (sender_error) std::rethrow_exception(sender_error);
    if (status != 0) throw std::runtime_error("Sender did not stop normally");
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) return 2;
    const bool encrypted = argc == 3;
    if (encrypted && std::string_view(argv[2]) != "encrypted") return 2;
    const std::string_view profile = argv[1];
    if (profile != "h264-tcp" && profile != "h265-udp" && profile != "av1-udp") return 2;
    if (!CGPreflightScreenCaptureAccess()) {
        std::cout << "Skipping live desktop capture: Screen Recording permission is required\n";
        return 77;
    }

    try {
        if (profile == "av1-udp") {
            capture(Codec::av1, Transport::udp, 2.0, false, {2560, 1440}, encrypted);
            std::cout << "Captured and decoded a 2560x1440 AV1/UDP desktop at scale 2\n";
        } else if (profile == "h265-udp") {
            capture(Codec::h265, Transport::udp, 1.5, true, {1920, 1080}, encrypted);
            std::cout << "Negotiated a 1920x1080 HEVC stream from a 1280x720 desktop over UDP\n";
        } else {
            capture(Codec::h264, Transport::tcp, 0.75, true, {960, 540}, encrypted);
            std::cout << "Negotiated a 960x540 H.264 stream from a 1280x720 desktop over TCP\n";
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
