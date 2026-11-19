#include "udp.hpp"

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
} // namespace

int main() {
    try {
        udp::Listener listener("127.0.0.1", 0);
        require(listener.local_port() != 0, "No bound UDP port");
        require(!listener.accept(10), "Unexpected idle connection");
        exercise(listener, false);
        exercise(listener, true); // Same listener, new peer. Drain queued frames on shutdown.
        std::cout << "UDP ordering, fragmentation, disconnect and reconnect passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
