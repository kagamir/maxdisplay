// Drives the normal sender in a logged-in session with Screen Recording permission.
#include "app.hpp"
#include "av1.hpp"
#include "display_scale.hpp"
#include "udp.hpp"
#include "wire.hpp"

#include <CoreGraphics/CoreGraphics.h>

#include <chrono>
#include <csignal>
#include <future>
#include <iostream>
#include <memory>
#include <utility>
#include <stdexcept>
#include <string_view>

namespace {
using namespace std::chrono_literals;

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

void capture(Codec codec, Transport transport) {
    SenderOptions options;
    options.codec = codec;
    options.transport = transport;
    options.host = "127.0.0.1";
    options.fps = 30;
    // Leave time to decode the scaled AV1 keyframe on a shared runner.
    options.timeout_ms = 1000;
    const bool scaled = codec == Codec::av1;
    set_display_size(options, 1280, 720, scaled ? 2.0 : 1.0);
    const uint32_t expected_width = scaled ? 2560 : 1280;
    const uint32_t expected_height = scaled ? 1440 : 720;

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
        if (datagrams) {
            if (!datagrams->accept(8000)) throw std::runtime_error("UDP sender did not connect");
            std::vector<uint8_t> header;
            if (datagrams->receive(header, 15000) != udp::ReadResult::message)
                throw std::runtime_error("No UDP stream header");
            mode = wire::decode_header(header);
        } else {
            if (!net::wait_readable(tcp, 8000)) throw std::runtime_error("TCP sender did not connect");
            peer = net::accept_tcp(tcp);
            if (!peer) throw std::runtime_error("TCP connection withdrawn");
            mode = wire::receive_header(peer);
        }
        if (mode.width != expected_width || mode.height != expected_height || mode.fps != options.fps ||
            mode.codec != codec || mode.timeout_ms != options.timeout_ms)
            throw std::runtime_error("Unexpected captured desktop mode");
        auto receive_frame = [&] {
            std::vector<uint8_t> message;
            if (datagrams) {
                if (datagrams->receive(message, 8000) != udp::ReadResult::message)
                    throw std::runtime_error("No UDP video frame");
            } else if (!net::wait_readable(peer, 8000) || !wire::receive_frame(peer, message)) {
                throw std::runtime_error("No TCP video frame");
            }
            return wire::unpack_frame(message);
        };
        auto feedback = [&](uint32_t sequence, bool request_keyframe) {
            auto bytes = wire::pack_feedback({sequence, request_keyframe});
            if (datagrams) datagrams->send_feedback(bytes);
            else peer.write_all(bytes.data(), bytes.size());
        };
        auto check_keyframe = [&](const wire::Frame& key) {
            if (!key.keyframe) throw std::runtime_error("Missing keyframe flag");
            if (codec == Codec::av1) {
                check_av1(key.data, expected_width, expected_height);
                return;
            }
            const auto& au = key.data;
            if (au.size() < 6 || au[0] || au[1] || au[2] || au[3] != 1)
                throw std::runtime_error("Expected an Annex B keyframe");
            if (codec == Codec::h264 ? (au[4] & 31) != 7 : ((au[4] >> 1) & 63) != 32)
                throw std::runtime_error("Recovery frame did not begin with codec parameter sets");
        };
        auto first = receive_frame();
        check_keyframe(first);
        feedback(first.sequence, true);
        // Request a standalone picture even when no application is updating the desktop.
        auto wait_key = [&](uint32_t previous, bool acknowledge_deltas) {
            const auto deadline = std::chrono::steady_clock::now() + 8s;
            do {
                auto next = receive_frame();
                if (next.keyframe && next.sequence > previous) {
                    check_keyframe(next);
                    return next.sequence;
                }
                if (acknowledge_deltas) feedback(next.sequence, false);
            } while (std::chrono::steady_clock::now() < deadline);
            throw std::runtime_error("Mac did not send a recovery keyframe");
        };
        auto recovery = wait_key(first.sequence, true);
        // Simulate the last picture going missing: send no feedback or further request.
        auto retry = wait_key(recovery, false);
        feedback(retry, false);
        std::raise(SIGINT);
        stopped_future.wait_for(4s);
    });
    const int status = run_sender(options);
    stopped.set_value();
    receive.get();
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
            capture(Codec::av1, Transport::udp);
            std::cout << "Captured and decoded a 2560x1440 AV1/UDP desktop at scale 2\n";
        } else if (profile == "h265-udp") {
            capture(Codec::h265, Transport::udp);
            std::cout << "Captured H.265/UDP desktop and requested a fresh keyframe\n";
        } else {
            capture(Codec::h264, Transport::tcp);
            std::cout << "Captured a 1280x720 H.264/TCP desktop at scale 1\n";
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
