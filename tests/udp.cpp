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

void timed_stream(udp::Listener& listener, const std::string& password = {}) {
    const std::vector<std::uint8_t> video(1024 * 1024, 0x65);
    const std::vector<std::uint8_t> delta(9000, 0x41); // Multiple unreliable MTU-sized fragments.
    const auto port = listener.local_port();
    std::promise<void> queued;
    auto queued_signal = queued.get_future();
    auto sending = std::async(std::launch::async, [&] {
        udp::Sender sender("127.0.0.1", port, password);
        sender.send_header({1920, 1080, 60, Codec::h265, 100});
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
    require(listener.receive_header({}).timeout_ms == 100, "Reliable header did not precede timed frames");
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

void display_negotiation(udp::Listener& listener, bool automatic, const std::string& password = {}) {
    const auto port = listener.local_port();
    auto receiver = std::async(std::launch::async, [&] {
        require(listener.accept(6000), "Display negotiation peer did not connect");
        bool queried = false;
        const auto header = listener.receive_header([&] {
            queried = true;
            return wire::DisplaySize{3840, 2160};
        });
        std::vector<std::uint8_t> bytes;
        const auto deadline = Clock::now() + 5s;
        while (Clock::now() < deadline && listener.receive(bytes, 100) == udp::ReadResult::timeout) {}
        require(bytes == std::vector<std::uint8_t>({0x42, 0x10}), "Lost reliable legacy access unit");
        listener.disconnect();
        require(queried == automatic, "Unexpected UDP display query");
        require(header.width == 1920 && header.height == 1080 && header.fps == 60,
                "Lost UDP stream header after display query");
    });
    udp::Sender sender("127.0.0.1", port, password);
    if (automatic) {
        const auto size = sender.request_display_size();
        require(size.width == 3840 && size.height == 2160, "Incorrect UDP monitor size");
        require(!sender.take_feedback(), "Resolution reply leaked into frame feedback");
    }
    sender.send_header({1920, 1080, 60});
    sender.send({0x42, 0x10});
    receiver.get();
}

void unavailable_display(udp::Listener& listener) {
    const auto port = listener.local_port();
    auto legacy = std::async(std::launch::async, [&] {
        require(listener.accept(6000), "Legacy UDP peer did not connect");
        std::vector<std::uint8_t> first;
        require(listener.receive(first, 5000) == udp::ReadResult::message && first == wire::display_query,
                "Missing display query on legacy connection");
        listener.disconnect(); // Old receivers reject the unfamiliar header.
    });
    udp::Sender sender("127.0.0.1", port);
    bool manual_hint = false;
    try {
        sender.request_display_size();
    } catch (const std::runtime_error& e) {
        manual_hint = std::string(e.what()).find("--size") != std::string::npos;
    }
    legacy.get();
    require(manual_hint, "No manual size hint when a legacy UDP peer rejected the query");
}
void wrong_password(udp::Listener& listener) {
    const auto port = listener.local_port();
    auto receive = std::async(std::launch::async, [&] {
        require(listener.accept(6000), "Wrong-password peer did not reach handshake");
        bool queried = false, rejected = false;
        try {
            listener.receive_header([&] { queried = true; return wire::DisplaySize{640, 480}; });
        } catch (const std::runtime_error&) { rejected = true; }
        listener.disconnect();
        require(rejected && !queried, "Receiver exposed display information before authenticating");
    });
    bool rejected = false;
    try { udp::Sender sender("127.0.0.1", port, "incorrect"); }
    catch (const std::runtime_error&) { rejected = true; }
    receive.get();
    require(rejected, "Wrong UDP password was accepted");
}

void password_mode_mismatch(udp::Listener& listener, const std::string& sender_password) {
    const auto port = listener.local_port();
    auto receive = std::async(std::launch::async, [&] {
        require(!listener.accept(1500), "UDP connection accepted with mismatched password mode");
    });
    bool rejected = false;
    try {
        udp::Sender sender("127.0.0.1", port, sender_password);
        sender.request_display_size(3000);
    } catch (const std::runtime_error&) { rejected = true; }
    receive.get();
    require(rejected, "UDP password mode mismatch silently fell back to plaintext");
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
        display_negotiation(listener, true);
        display_negotiation(listener, false);
        unavailable_display(listener);
        const std::string password = "UDP test password";
        udp::Listener encrypted("127.0.0.1", 0, password);
        wrong_password(encrypted);
        password_mode_mismatch(encrypted, {});
        password_mode_mismatch(listener, password);
        display_negotiation(encrypted, true, password);
        display_negotiation(encrypted, false, password);
        timed_stream(encrypted, password);
        std::cout << "UDP ordering, fragmentation, disconnect and reconnect passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
