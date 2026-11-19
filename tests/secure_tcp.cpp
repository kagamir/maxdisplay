#include "secure_tcp.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;
using secure::Bytes;
using secure::Kind;
using secure::Role;
using secure::TcpChannel;
using Clock = std::chrono::steady_clock;
constexpr char password[] = "a test passphrase";

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Operation>
void rejects(Operation&& operation, const char* message) {
    try {
        operation();
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error(message);
}

struct Connection {
    net::Socket sender, receiver;
    Connection() {
        auto listener = net::listen_tcp("127.0.0.1", 0);
        sender = net::connect_tcp("127.0.0.1", listener.local_port());
        require(net::wait_readable(listener, 1000), "TCP connection unavailable");
        receiver = net::accept_tcp(listener);
        require(static_cast<bool>(receiver), "accept failed");
    }
};

Bytes read_feedback(TcpChannel& client, net::Socket& socket) {
    const auto deadline = Clock::now() + 2s;
    while (Clock::now() < deadline) {
        if (net::wait_readable(socket, 30)) {
            if (auto record = client.take_feedback()) return *record;
        }
    }
    throw std::runtime_error("Expected a complete TCP feedback record");
}

void encrypted_roundtrip() {
    Connection connection;
    const wire::Header header{1920, 1080, 60, Codec::h265, 200};
    // Encryption overhead must not reduce the existing maximum payload.
    const Bytes image(wire::max_frame_bytes, 0x65);
    const auto ack = wire::pack_feedback({1234, true});
    auto server = std::async(std::launch::async, [&] {
        TcpChannel channel(connection.receiver, password, Role::receiver);
        auto received = channel.receive_header([] { return wire::DisplaySize{1920, 1080}; });
        require(received.codec == header.codec && received.timeout_ms == header.timeout_ms, "Incorrect protected header");
        Bytes frame;
        require(channel.receive_frame(frame) && frame == image, "Incorrect large protected frame");
        channel.send_feedback(ack);
    });
    TcpChannel client(connection.sender, password, Role::sender);
    require(client.request_display_size().width == 1920, "Incorrect protected display reply");
    client.send_header(header);
    require(!client.send_frame_before({1, 2, 3}, Clock::now() - 1ms), "Expired protected frame sent");
    require(client.send_frame_before(image, Clock::now() + 5s), "Fresh protected frame dropped");
    server.get();
    require(read_feedback(client, connection.sender) == ack, "Incorrect protected feedback");
}

void raw_peer_checks() {
    Connection connection;
    const Bytes image(4096, 0x66);
    const auto ack = wire::pack_feedback({7, false});
    std::promise<void> split;
    auto prefix_written = split.get_future();
    std::promise<void> ready;
    auto send_rest = ready.get_future();
    auto server = std::async(std::launch::async, [&] {
        Bytes client_hello(secure::hello_size);
        require(connection.receiver.read_exact(client_hello.data(), client_hello.size()), "Missing client hello");
        const auto server_hello = secure::make_hello(Role::receiver);
        connection.receiver.write_all(server_hello.data(), server_hello.size());
        secure::Session session(password, client_hello, server_hello, Role::receiver);
        auto proof = session.seal(Kind::confirm, {});
        connection.receiver.write_all(proof.data(), proof.size());
        require(connection.receiver.read_exact(proof.data(), proof.size()), "Missing client proof");
        session.open(Kind::confirm, proof);

        Bytes record;
        require(wire::receive_frame(connection.receiver, record), "Missing encrypted access unit");
        require(std::search(record.begin(), record.end(), image.begin(), image.begin() + 32) == record.end(),
                "Video pattern visible on protected TCP");
        require(session.open(Kind::frame, record) == image, "Wire record does not authenticate");
        auto feedback = session.seal(Kind::feedback, ack);
        connection.receiver.write_all(feedback.data(), 3);
        split.set_value();
        require(send_rest.wait_for(5s) == std::future_status::ready, "Feedback read stalled");
        connection.receiver.write_all(feedback.data() + 3, feedback.size() - 3);
        feedback = session.seal(Kind::feedback, ack);
        feedback.back() ^= 1;
        connection.receiver.write_all(feedback.data(), feedback.size());
    });
    TcpChannel client(connection.sender, password, Role::sender);
    client.send_frame(image);
    require(prefix_written.wait_for(5s) == std::future_status::ready, "Partial feedback was not sent");
    require(net::wait_readable(connection.sender, 1000) && !client.take_feedback(), "Partial protected feedback accepted");
    ready.set_value();
    server.get();
    require(read_feedback(client, connection.sender) == ack, "Split feedback record lost");
    rejects([&] { read_feedback(client, connection.sender); }, "Modified TCP feedback tag accepted");
}

void wrong_password() {
    Connection connection;
    auto server = std::async(std::launch::async, [&] {
        rejects([&] { TcpChannel channel(connection.receiver, password, Role::receiver); }, "Wrong client confirmed");
    });
    rejects([&] { TcpChannel channel(connection.sender, "incorrect", Role::sender); }, "Wrong server confirmed");
    connection.sender.shutdown();
    server.get();
}

void modes_and_cleartext_compatibility() {
    const wire::Header legacy{1280, 720, 30};
    Connection plain;
    TcpChannel client(plain.sender, "", Role::sender);
    client.send_header(legacy);
    client.send_frame({4, 5, 6});
    require(wire::receive_header(plain.receiver).height == 720, "Old receiver cannot read cleartext header");
    Bytes frame;
    require(wire::receive_frame(plain.receiver, frame) && frame == Bytes{4, 5, 6}, "Old receiver cannot read cleartext frame");
    const auto ack = wire::pack_feedback({7, false});
    plain.receiver.write_all(ack.data(), ack.size());
    require(read_feedback(client, plain.sender) == ack, "Old receiver cannot send cleartext feedback");

    Connection mismatched;
    wire::send_header(mismatched.sender, 1280, 720, 30);
    rejects([&] { TcpChannel protected_receiver(mismatched.receiver, password, Role::receiver); },
            "Password-protected receiver accepted a cleartext peer");
    require(!net::wait_readable(mismatched.sender, 0), "Protected receiver replied to a cleartext peer");

    Connection other;
    auto server = std::async(std::launch::async, [&] {
        TcpChannel plain_receiver(other.receiver, "", Role::receiver);
        rejects([&] { plain_receiver.receive_header(); }, "Plain receiver accepted an encrypted peer");
        other.receiver.shutdown();
    });
    rejects([&] { TcpChannel protected_client(other.sender, password, Role::sender); }, "Protected sender fell back");
    server.get();
}
} // namespace

int main() {
    try {
        encrypted_roundtrip();
        raw_peer_checks();
        wrong_password();
        modes_and_cleartext_compatibility();
        std::cout << "Encrypted TCP checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
