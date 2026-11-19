// Drives the normal sender in a logged-in session with Screen Recording permission.
#include "app.hpp"
#include "av1.hpp"
#include "frame_recovery.hpp"
#include "udp.hpp"
#include "wire.hpp"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreMedia/CoreMedia.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <future>
#include <exception>
#include <iostream>
#include <memory>
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

void capture(Codec codec, Transport transport, double scale, bool automatic, wire::DisplaySize expected) {
    SenderOptions options;
    options.codec = codec;
    options.transport = transport;
    options.host = "127.0.0.1";
    options.fps = 30;
    // Leave time to decode the scaled AV1 keyframe on a shared runner.
    options.timeout_ms = 1000;
    constexpr uint32_t logical_width = 1280, logical_height = 720;
    options.scale = scale;
    options.width = automatic ? 0 : logical_width;
    options.height = automatic ? 0 : logical_height;
    const auto original = online_displays();

    net::Socket tcp;
    std::unique_ptr<udp::Listener> incoming_udp;
    if (transport == Transport::udp) {
        incoming_udp = std::make_unique<udp::Listener>(options.host, 0);
        options.port = incoming_udp->local_port();
    } else {
        tcp = net::listen_tcp(options.host, 0);
        options.port = tcp.local_port();
    }

    std::promise<void> stopped;
    auto stopped_future = stopped.get_future();
    auto receive = std::async(std::launch::async, [&] {
        // Release the listener if validation fails, so the sender observes disconnect.
        auto datagrams = std::move(incoming_udp);
        wire::Header mode{};
        net::Socket peer;
        bool queried = false;
        auto monitor = [&] {
            if (!automatic || queried) throw std::runtime_error("Unexpected display size query");
            queried = true;
            return expected;
        };
        if (datagrams) {
            if (!datagrams->accept(8000)) throw std::runtime_error("UDP sender did not connect");
            mode = datagrams->receive_header(monitor);
        } else {
            if (!net::wait_readable(tcp, 8000)) throw std::runtime_error("TCP sender did not connect");
            peer = net::accept_tcp(tcp);
            if (!peer) throw std::runtime_error("TCP connection withdrawn");
            mode = wire::receive_header(peer, monitor);
        }
        if (queried != automatic || mode.width != expected.width || mode.height != expected.height || mode.fps != options.fps ||
            mode.codec != codec || mode.timeout_ms != options.timeout_ms)
            throw std::runtime_error("Unexpected captured desktop mode");
        std::cout << "Received v3 header for " << codec_name(codec) << '\n' << std::flush;
        auto receive_frame = [&](std::chrono::steady_clock::time_point deadline, const char* stage) {
            std::vector<uint8_t> message;
            while (std::chrono::steady_clock::now() < deadline) {
                if (datagrams) {
                    const auto result = datagrams->receive(message, 100);
                    if (result == udp::ReadResult::timeout) continue;
                    if (result == udp::ReadResult::disconnected)
                        throw std::runtime_error(std::string("UDP disconnected during ") + stage);
                } else {
                    if (!net::wait_readable(peer, 100)) continue;
                    if (!wire::receive_frame(peer, message))
                        throw std::runtime_error(std::string("TCP disconnected during ") + stage);
                }
                return wire::unpack_frame(message);
            }
            throw std::runtime_error(std::string("Timed out during ") + stage);
        };
        auto feedback = [&](uint32_t sequence, bool request_keyframe) {
            auto bytes = wire::pack_feedback({sequence, request_keyframe});
            if (datagrams) datagrams->send_feedback(bytes);
            else peer.write_all(bytes.data(), bytes.size());
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
    if (argc != 2) return 2;
    const std::string_view profile = argv[1];
    if (profile != "h264-tcp" && profile != "h265-udp" && profile != "av1-udp") return 2;
    if (!CGPreflightScreenCaptureAccess()) {
        std::cout << "Skipping live desktop capture: Screen Recording permission is required\n";
        return 77;
    }

    try {
        if (profile == "av1-udp") {
            capture(Codec::av1, Transport::udp, 2.0, false, {2560, 1440});
            std::cout << "Captured and decoded a 2560x1440 AV1/UDP desktop at scale 2\n";
        } else if (profile == "h265-udp") {
            capture(Codec::h265, Transport::udp, 1.5, true, {1920, 1080});
            std::cout << "Negotiated a 1920x1080 HEVC stream from a 1280x720 desktop over UDP\n";
        } else {
            capture(Codec::h264, Transport::tcp, 0.75, true, {960, 540});
            std::cout << "Negotiated a 960x540 H.264 stream from a 1280x720 desktop over TCP\n";
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
