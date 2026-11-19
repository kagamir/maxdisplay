#include "udp.hpp"
#include "wire.hpp"

#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void exercise(udp::Listener& listener, bool close_sender) {
    const std::vector<std::uint8_t> hello{0x4d, 0x58, 0x44, 0x53};
    std::vector<std::uint8_t> fragmented(512 * 1024);
    for (std::size_t i = 0; i < fragmented.size(); ++i)
        fragmented[i] = static_cast<std::uint8_t>((i >> 8) ^ i);

    auto receive = std::async(std::launch::async, [&] {
        require(listener.accept(6000), "UDP session did not connect");
        std::vector<std::uint8_t> bytes;
        auto deadline = Clock::now() + 8s;
        unsigned received = 0;
        while (received < 3 && Clock::now() < deadline) {
            auto result = listener.receive(bytes, 100);
            if (result == udp::ReadResult::timeout) continue;
            require(result == udp::ReadResult::message, "UDP disconnected before delivering all messages");
            require(bytes == (received == 1 ? fragmented : hello), "UDP message contents or order changed");
            ++received;
        }
        require(received == 3, "UDP messages timed out");
        if (close_sender) {
            while (Clock::now() < deadline) {
                if (listener.receive(bytes, 100) == udp::ReadResult::disconnected) return;
            }
            throw std::runtime_error("Sender disconnect was not observed");
        }
        listener.disconnect();
    });

    udp::Sender sender("127.0.0.1", listener.local_port());
    sender.send(hello);
    sender.send(fragmented);
    sender.send(hello);
    if (close_sender) sender.shutdown();
    receive.get();
    auto deadline = Clock::now() + 8s;
    while (!sender.peer_closed() && Clock::now() < deadline) std::this_thread::sleep_for(10ms);
    require(sender.peer_closed(), "Receiver disconnect was not observed");
}

void timed_stream(udp::Listener& listener) {
    const std::vector<std::uint8_t> video(1024 * 1024, 0x65);
    const std::vector<std::uint8_t> delta(9000, 0x41); // Multiple unreliable MTU-sized fragments.
    const auto port = listener.local_port();
    std::promise<void> queued;
    auto queued_signal = queued.get_future();
    auto sending = std::async(std::launch::async, [&] {
        udp::Sender sender("127.0.0.1", port);
        sender.send(wire::encode_header({1920, 1080, 60, Codec::h265, 100}));
        const auto first = wire::pack_frame({1, 5000, true, video});
        require(sender.send_frame(first, true), "First timed frame dropped");
        require(!sender.send_frame(first, true), "Queued duplicate sync while first still in flight");
        // Frame 2 missed its sender deadline. The receiver must see the gap and be able to request an IDR.
        require(sender.send_frame(wire::pack_frame({3, 38000, false, delta}), false), "Second timed frame dropped");
        queued.set_value();
        for (auto deadline = Clock::now() + 5s; Clock::now() < deadline;) {
            auto bytes = sender.take_feedback();
            if (bytes) {
                const auto feedback = wire::unpack_feedback(*bytes);
                require(feedback.sequence == 3 && feedback.request_keyframe, "Incorrect loss feedback");
                require(sender.send_frame(wire::pack_frame({4, 55000, true, video}), true), "Recovery frame dropped");
                while (!sender.peer_closed() && Clock::now() < deadline)
                    std::this_thread::sleep_for(2ms);
                return;
            }
            std::this_thread::sleep_for(2ms);
        }
        throw std::runtime_error("Reverse UDP feedback timed out");
    });
    require(listener.accept(6000), "Timed UDP session did not connect");
    require(queued_signal.wait_for(5s) == std::future_status::ready, "Timed UDP frames were not queued");
    std::vector<std::uint8_t> bytes;
    auto next = [&] {
        const auto deadline = Clock::now() + 6s;
        while (Clock::now() < deadline) {
            const auto result = listener.receive(bytes, 100);
            if (result == udp::ReadResult::message) return;
            require(result != udp::ReadResult::disconnected, "Timed UDP session disconnected early");
        }
        throw std::runtime_error("Timed UDP message timed out");
    };
    next();
    require(wire::decode_header(bytes).timeout_ms == 100, "Reliable header did not precede timed frames");
    next();
    auto frame = wire::unpack_frame(bytes);
    require(frame.sequence == 1 && frame.keyframe && frame.data == video, "Reliable key reassembly failed");
    next();
    frame = wire::unpack_frame(bytes);
    require(frame.sequence == 3 && !frame.keyframe && frame.data == delta, "Fragmented delta or frame gap lost");
    listener.send_feedback(wire::pack_feedback({frame.sequence, true}));
    next();
    frame = wire::unpack_frame(bytes);
    require(frame.sequence == 4 && frame.keyframe && frame.data == video, "UDP recovery frame mismatch");
    listener.disconnect();
    sending.get();
}
} // namespace

int main() {
    try {
        udp::Listener listener("127.0.0.1", 0);
        require(listener.local_port() != 0, "No bound UDP port");
        require(!listener.accept(10), "Unexpected idle connection");
        exercise(listener, false);
        exercise(listener, true); // Same listener, new peer. Drain queued frames on shutdown.
        timed_stream(listener);
        std::cout << "UDP ordering, fragmentation, disconnect and reconnect passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
