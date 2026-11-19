// Drives the normal sender in a logged-in test session with Screen Recording
// permission. The loopback peer exercises the same wire reader as Windows.
#include "app.hpp"
#include "wire.hpp"

#include <CoreGraphics/CoreGraphics.h>

#include <chrono>
#include <csignal>
#include <future>
#include <iostream>
#include <stdexcept>

int main() {
    if (!CGPreflightScreenCaptureAccess()) {
        std::cout << "Skipping live desktop capture: Screen Recording permission is required\n";
        return 77;
    }

    try {
        auto listener = net::listen_tcp("127.0.0.1", 0);
        SenderOptions options;
        options.host = "127.0.0.1";
        options.port = listener.local_port();
        options.width = 1280;
        options.height = 720;
        options.fps = 30;

        // Only raise SIGINT after the producer has connected and sent video.
        std::promise<void> stopped;
        auto stopped_future = stopped.get_future();
        auto receive = std::async(std::launch::async, [&] {
            if (!net::wait_readable(listener, 8000)) throw std::runtime_error("Sender did not connect");
            auto peer = net::accept_tcp(listener);
            if (!peer) throw std::runtime_error("Connection withdrawn");
            auto mode = wire::receive_header(peer);
            if (mode.width != options.width || mode.height != options.height || mode.fps != options.fps)
                throw std::runtime_error("Unexpected captured mode");
            std::vector<uint8_t> frame;
            if (!wire::receive_frame(peer, frame) || frame.size() < 5 ||
                frame[0] || frame[1] || frame[2] || frame[3] != 1 || (frame[4] & 31) != 7)
                throw std::runtime_error("Expected an Annex B access unit starting with SPS");
            std::raise(SIGINT);
            // Stay connected so shutdown is driven by the same Ctrl+C path a user takes.
            stopped_future.wait_for(std::chrono::seconds(4));
        });
        const int status = run_sender(options);
        stopped.set_value();
        receive.get();
        if (status != 0) throw std::runtime_error("Sender did not stop normally");
        std::cout << "Captured virtual desktop and received H.264 over TCP\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
