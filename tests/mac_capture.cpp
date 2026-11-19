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
#include <optional>
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
    const bool scaled = codec == Codec::av1;
    set_display_size(options, 1280, 720, scaled ? 2.0 : 1.0);
    const uint32_t expected_width = scaled ? 2560 : 1280;
    const uint32_t expected_height = scaled ? 1440 : 720;

    net::Socket tcp;
    std::optional<udp::Listener> datagrams;
    if (transport == Transport::udp) {
        datagrams.emplace(options.host, 0);
        options.port = datagrams->local_port();
    } else {
        tcp = net::listen_tcp(options.host, 0);
        options.port = tcp.local_port();
    }

    std::promise<void> stopped;
    auto stopped_future = stopped.get_future();
    auto receive = std::async(std::launch::async, [&] {
        wire::Header mode{};
        std::vector<uint8_t> frame;
        net::Socket peer;
        if (datagrams) {
            if (!datagrams->accept(8000)) throw std::runtime_error("UDP sender did not connect");
            std::vector<uint8_t> header;
            if (datagrams->receive(header, 15000) != udp::ReadResult::message)
                throw std::runtime_error("No UDP stream header");
            mode = wire::decode_header(header);
            if (datagrams->receive(frame, 15000) != udp::ReadResult::message)
                throw std::runtime_error("No UDP video frame");
        } else {
            if (!net::wait_readable(tcp, 8000)) throw std::runtime_error("TCP sender did not connect");
            peer = net::accept_tcp(tcp);
            if (!peer) throw std::runtime_error("TCP connection withdrawn");
            mode = wire::receive_header(peer);
            if (!wire::receive_frame(peer, frame)) throw std::runtime_error("No TCP video frame");
        }
        if (mode.width != expected_width || mode.height != expected_height || mode.fps != options.fps || mode.codec != codec)
            throw std::runtime_error("Unexpected captured desktop mode");
        if (codec == Codec::av1) check_av1(frame, expected_width, expected_height);
        else if (frame.size() < 5 || frame[0] || frame[1] || frame[2] ||
                 frame[3] != 1 || (frame[4] & 31) != 7)
            throw std::runtime_error("Expected an H.264 access unit beginning with SPS");
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
    if (profile != "h264-tcp" && profile != "av1-udp") return 2;
    if (!CGPreflightScreenCaptureAccess()) {
        std::cout << "Skipping live desktop capture: Screen Recording permission is required\n";
        return 77;
    }

    try {
        if (profile == "av1-udp") {
            capture(Codec::av1, Transport::udp);
            std::cout << "Captured and decoded a 2560x1440 AV1/UDP desktop at scale 2\n";
        } else {
            capture(Codec::h264, Transport::tcp);
            std::cout << "Captured a 1280x720 H.264/TCP desktop at scale 1\n";
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
