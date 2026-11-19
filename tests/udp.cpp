#include "cursor_link.hpp"
#include "cursor_packet.hpp"
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

void timed_stream(udp::Listener& listener, bool close_sender, const std::string& password = {}) {
    std::vector<std::uint8_t> video(1024 * 1024);
    for (std::size_t i = 0; i < video.size(); ++i)
        video[i] = static_cast<std::uint8_t>((i >> 8) ^ i);
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
                if (close_sender) {
                    sender.shutdown();
                } else {
                    while (!sender.peer_closed() && Clock::now() < deadline)
                        std::this_thread::sleep_for(2ms);
                    require(sender.peer_closed(), "Receiver disconnect was not observed");
                }
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
    if (close_sender) sending.get(); // Close the sender before the video consumer drains its last frame.
    next();
    frame = wire::unpack_frame(bytes);
    require(frame.sequence == 4 && frame.keyframe && frame.data == video, "UDP recovery frame mismatch");
    if (close_sender) {
        listener.send_feedback(wire::pack_feedback({frame.sequence, false}));
        const auto deadline = Clock::now() + 6s;
        auto result = udp::ReadResult::timeout;
        while (result == udp::ReadResult::timeout && Clock::now() < deadline)
            result = listener.receive(bytes, 100);
        require(result == udp::ReadResult::disconnected, "Sender disconnect was not observed");
    } else {
        listener.disconnect();
        sending.get();
    }
}

void display_negotiation(udp::Listener& listener, bool automatic, bool virtual_cursor,
                         const std::string& password = {}) {
    const auto port = listener.local_port();
    const auto token = secure::random_token();
    auto cursor_socket = net::bind_udp("127.0.0.1");
    const auto first = wire::pack_frame({1, 5000, true, {0x42, 0x10}});
    auto receiver = std::async(std::launch::async, [&] {
        require(listener.accept(6000), "Display negotiation peer did not connect");
        bool queried = false, cursor_queried = false;
        std::unique_ptr<secure::Datagrams> keys;
        const auto header = listener.receive_header([&] {
            queried = true;
            return wire::DisplaySize{3840, 2160};
        }, {}, [&] {
            cursor_queried = true;
            keys = listener.datagrams();
            require(static_cast<bool>(keys) == !password.empty(), "Wrong ENet cursor protection");
            listener.open_cursor(listener.peer_endpoint(), token);
            return token;
        });
        require(cursor_queried == virtual_cursor, "Unexpected UDP cursor query");
        if (virtual_cursor) {
            // Leave all video frames in the listener while servicing the pointer.
            cursor::Receiver link(token);
            net::Endpoint from;
            cursor::Bytes inner;
            require(listener.receive_cursor(inner, from, 2000), "Cursor stalled behind pending ENet video");
            require(from.port == cursor_socket.local_port(), "Wrong UDP source for cursor");
            const auto clear = keys ? keys->open(inner) : std::optional<cursor::Bytes>(inner);
            require(clear.has_value(), "Cursor datagram authentication failed");
            const auto ack = link.receive(1, *clear);
            require(ack && listener.send_cursor(keys ? keys->seal(*ack) : *ack, from), "Cursor ACK failed");
        }
        std::vector<std::uint8_t> bytes;
        const auto deadline = Clock::now() + 5s;
        while (Clock::now() < deadline && listener.receive(bytes, 100) == udp::ReadResult::timeout) {}
        require(bytes == first, "First timed frame did not follow the UDP stream header");
        require(queried == automatic, "Unexpected UDP display query");
        require(header.width == 1920 && header.height == 1080 && header.fps == 60 &&
                header.codec == Codec::h265 && header.timeout_ms == 100,
                "Lost UDP stream header after display query");
    });
    udp::Sender sender("127.0.0.1", port, password);
    require(sender.peer_endpoint().port == port, "ENet peer endpoint lost the port");
    std::unique_ptr<secure::Datagrams> keys;
    if (virtual_cursor) {
        require(sender.request_cursor() == token, "Incorrect ENet cursor token");
        require(!net::wait_readable(cursor_socket, 50), "Receiver sent an unsolicited cursor packet");
        keys = sender.datagrams();
        require(static_cast<bool>(keys) == !password.empty(), "Wrong cursor sender protection");
    }
    if (automatic) {
        const auto size = sender.request_display_size();
        require(size.width == 3840 && size.height == 2160, "Incorrect UDP monitor size");
        require(!sender.take_feedback(), "Resolution reply leaked into frame feedback");
    }
    sender.send_header({1920, 1080, 60, Codec::h265, 100});
    require(sender.send_frame(first, true), "First UDP frame dropped");
    if (virtual_cursor) {
        cursor::Sender link(token);
        auto peer = sender.peer_endpoint();
        link.update(0, {}, {}, [&](const cursor::Bytes& inner) {
            require(cursor_socket.send_to(peer, cursor::packet::wrap(keys ? keys->seal(inner) : inner)),
                    "Could not send cursor on the video port");
        });
        net::Endpoint from;
        cursor::Bytes ack;
        require(net::wait_readable(cursor_socket, 2000) && cursor_socket.receive_from(from, ack) &&
                from == peer && cursor::packet::unwrap(ack), "Missing framed cursor ACK from the video port");
        auto clear = keys ? keys->open(ack) : std::optional<cursor::Bytes>(ack);
        require(clear && link.acknowledge(2, *clear), "Invalid cursor ACK on shared video port");
    }
    receiver.get();
    if (virtual_cursor) listener.close_cursor();
    listener.disconnect();
}

void shared_port_without_udp_video() {
    udp::Listener listener("127.0.0.1", 0);
    auto tcp = net::listen_tcp("127.0.0.1", listener.local_port());
    auto video = net::connect_tcp("127.0.0.1", tcp.local_port());
    require(net::wait_readable(tcp, 1000), "Missing TCP connection on the shared port");
    auto accepted = net::accept_tcp(tcp);
    const auto video_peer = accepted.peer_endpoint();
    const auto target = video.peer_endpoint();
    auto sender = net::bind_udp("127.0.0.1");
    constexpr std::uint64_t old_token = 0x1122334455667788ull, token = old_token + 1;
    auto packet = [](std::uint64_t session, unsigned seq) {
        return cursor::detail::state_packet(session, seq, {}, 0);
    };
    auto send = [&](const cursor::Bytes& bytes) {
        require(sender.send_to(target, bytes), "Could not send cursor test datagram");
    };
    net::Endpoint from;
    cursor::Bytes inner;
    auto wrong_ip = video_peer;
    wrong_ip.address[3] ^= 1;
    listener.open_cursor(wrong_ip, old_token);
    send(cursor::packet::wrap(packet(old_token, 1)));
    require(!listener.receive_cursor(inner, from, 50), "Accepted a datagram from a different video peer");
    listener.close_cursor();

    listener.open_cursor(video_peer, old_token);
    auto cursor_peer = video_peer;
    cursor_peer.port = sender.local_port();
    require(!listener.send_cursor(packet(old_token, 1), cursor_peer),
            "Listener sent cursor data before receiving the first packet");
    require(!sender.send_to(target, {}), "Empty cursor datagram was sent");
    send({0xff});
    send({'M', 'X', 'C', 'P'});
    send(cursor::packet::wrap({0, 0}));
    send(packet(old_token, 1)); // Missing the shared-port envelope.
    send(cursor::packet::wrap(packet(token, 1)));
    send(cursor::packet::wrap(packet(old_token, 2)));
    send(cursor::packet::wrap(packet(old_token, 3)));
    require(listener.receive_cursor(inner, from, 1000) && inner == packet(old_token, 2) &&
            from == cursor_peer, "Invalid data reached the TCP session's cursor worker");
    cursor::Receiver receiver(old_token);
    const auto ack = receiver.receive(1, inner);
    require(ack && listener.send_cursor(*ack, from), "Could not ACK on the TCP session's UDP port");
    require(net::wait_readable(sender, 1000) && sender.receive_from(from, inner) && from == target &&
            cursor::packet::unwrap(inner) && inner == *ack, "Wrong TCP session reply socket");
    listener.close_cursor(); // May still have state 3 from the previous session in the queue.
    require(!listener.send_cursor(*ack, cursor_peer), "Closed cursor session still sent ACKs");
    listener.open_cursor(video_peer, token);
    send(cursor::packet::wrap(packet(old_token, 4)));
    send(cursor::packet::wrap(packet(token, 1)));
    require(listener.receive_cursor(inner, from, 1000) && inner == packet(token, 1),
            "Stale cursor packet delivered after opening the next session");

    // The new ENet connection goes through the same loopback receive queue,
    // after the cursor burst. Leave the cursor queue unread until then.
    for (unsigned seq = 2; seq <= 100; ++seq) send(cursor::packet::wrap(packet(token, seq)));
    udp::Sender client("127.0.0.1", listener.local_port());
    require(listener.accept(4000), "Cursor burst prevented a subsequent ENet connection");
    std::size_t queued = 0;
    cursor::Bytes last;
    while (listener.receive_cursor(inner, from, 0)) {
        ++queued;
        last = inner;
    }
    require(queued > 0 && queued <= 64 && last == packet(token, 100),
            "Cursor backlog did not retain the latest state within the queue bound");
    listener.close_cursor();
    listener.disconnect();
}

void unavailable_display(udp::Listener& listener) {
    const auto port = listener.local_port();
    auto receiver = std::async(std::launch::async, [&] {
        require(listener.accept(6000), "UDP peer did not connect");
        bool queried = false;
        try {
            listener.receive_header([&]() -> wire::DisplaySize {
                queried = true;
                throw std::runtime_error("Monitor unavailable");
            });
        } catch (const std::runtime_error&) {
            listener.disconnect();
        }
        require(queried, "Missing display query");
    });
    udp::Sender sender("127.0.0.1", port);
    bool rejected = false;
    try {
        sender.request_display_size();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    receiver.get();
    require(rejected, "Missing display reply silently accepted");
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
        shared_port_without_udp_video();
        udp::Listener listener("127.0.0.1", 0);
        require(listener.local_port() != 0, "No bound UDP port");
        require(!listener.accept(10), "Unexpected idle connection");
        timed_stream(listener, false);
        timed_stream(listener, true); // Accept a new session, then drain its last frame on sender shutdown.
        display_negotiation(listener, true, true);
        display_negotiation(listener, false, true);
        display_negotiation(listener, true, false); // --video-cursor with automatic resolution.
        unavailable_display(listener);
        const std::string password = "UDP test password";
        udp::Listener encrypted("127.0.0.1", 0, password);
        wrong_password(encrypted);
        password_mode_mismatch(encrypted, {});
        password_mode_mismatch(listener, password);
        display_negotiation(encrypted, true, true, password);
        display_negotiation(encrypted, false, true, password);
        display_negotiation(encrypted, true, false, password);
        timed_stream(encrypted, false, password);
        std::cout << "UDP ordering, fragmentation, disconnect and reconnect passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
