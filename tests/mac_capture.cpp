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

namespace {
using namespace std::chrono_literals;

void check_av1(const std::vector<uint8_t>& frame) {
    Av1Decoder decoder(1280, 720);
    bool decoded = false;
    decoder.decode(frame, [&](VideoFrame&& image) {
        if (image.width != 1280 || image.height != 720 || image.bgra.size() != 1280 * 720 * 4)
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
    if (codec == Codec::av1) set_display_size(options, 640, 360, 2.0);
    else set_display_size(options, 1280, 720, 1.0);

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
        if (mode.width != 1280 || mode.height != 720 || mode.fps != options.fps || mode.codec != codec)
            throw std::runtime_error("Unexpected captured desktop mode");
        if (codec == Codec::av1) check_av1(frame);
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

int main() {
    if (!CGPreflightScreenCaptureAccess()) {
        std::cout << "Skipping live desktop capture: Screen Recording permission is required\n";
        return 77;
    }

    try {
        capture(Codec::h264, Transport::tcp);
        capture(Codec::av1, Transport::udp);
        std::cout << "Captured H.264/TCP and a scaled 1280x720 AV1/UDP desktop\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
