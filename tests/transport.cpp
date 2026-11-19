#include "wire.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Fn>
void expect_failure(Fn operation, const char* message) {
    try {
        operation();
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error(message);
}

struct Connection {
    net::Socket writer;
    net::Socket reader;

    Connection() {
        auto listener = net::listen_tcp("127.0.0.1", 0);
        writer = net::connect_tcp("127.0.0.1", listener.local_port());
        require(net::wait_readable(listener, 1000), "No connection available");
        reader = net::accept_tcp(listener);
        require(static_cast<bool>(reader), "Accept failed");
        require(!net::accept_tcp(listener), "Unexpected second connection");
    }
};

const Bytes hello{'M', 'X', 'D', 'S', 0, 0, 0, 1, 0, 0, 7, 128, 0, 0, 4, 56, 0, 0, 0, 60};

void round_trip() {
    Connection connection;
    require(!connection.writer.peer_closed(), "Connected receiver reported as closed");
    wire::send_header(connection.writer, 1920, 1080, 60);
    const Bytes first{0, 0, 0, 1, 0x67, 0xaa, 0xbb};
    const Bytes second{0, 0, 1, 0x41, 0x42};
    wire::send_frame(connection.writer, first);
    wire::send_frame(connection.writer, second);
    connection.writer = {};
    const auto header = wire::receive_header(connection.reader);
    require(header.width == 1920 && header.height == 1080 && header.fps == 60, "Header mismatch");
    Bytes frame;
    require(wire::receive_frame(connection.reader, frame) && frame == first, "First frame mismatch");
    require(wire::receive_frame(connection.reader, frame) && frame == second, "Second frame mismatch");
    require(!wire::receive_frame(connection.reader, frame), "Missing frame-boundary EOF");
}

void check_layout() {
    Connection connection;
    wire::send_header(connection.writer, 1920, 1080, 60);
    Bytes bytes(hello.size());
    require(connection.reader.read_exact(bytes.data(), bytes.size()) && bytes == hello,
            "Header byte layout does not match protocol");
    wire::send_frame(connection.writer, {0, 0, 1, 0xff});
    bytes.resize(8);
    require(connection.reader.read_exact(bytes.data(), bytes.size()) &&
            bytes == Bytes{0, 0, 0, 4, 0, 0, 1, 0xff}, "Invalid wire frame");
}

void large_frame() {
    Connection connection;
    Bytes payload(6 * 1024 * 1024);
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<std::uint8_t>(i);
    auto sending = std::async(std::launch::async, [&] {
        auto sender = std::move(connection.writer);
        wire::send_frame(sender, payload);
    });
    Bytes received;
    require(wire::receive_frame(connection.reader, received) && received == payload,
            "Large access unit did not survive the TCP transfer");
    sending.get();
}

void malformed_header(Bytes bytes) {
    Connection connection;
    connection.writer.write_all(bytes.data(), bytes.size());
    connection.writer = {};
    expect_failure([&] { wire::receive_header(connection.reader); }, "Invalid header was accepted");
}

void malformed_frame(const Bytes& bytes) {
    Connection connection;
    connection.writer.write_all(bytes.data(), bytes.size());
    connection.writer = {};
    Bytes frame;
    expect_failure([&] { wire::receive_frame(connection.reader, frame); }, "Invalid frame was accepted");
}

void cancel_read() {
    Connection connection;
    std::promise<void> reading;
    auto started = reading.get_future();
    auto worker = std::async(std::launch::async, [&] {
        reading.set_value();
        try {
            Bytes frame;
            return !wire::receive_frame(connection.reader, frame);
        } catch (const std::runtime_error&) {
            // Both a socket shutdown error and a clean EOF can close a pending read.
            return true;
        }
    });
    started.wait();
    connection.reader.shutdown();
    require(worker.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
            "Read did not terminate on shutdown");
    require(worker.get(), "Got an unexpected frame");
    require(net::wait_readable(connection.writer, 1000) && connection.writer.peer_closed(),
            "Sender missed receiver disconnect");
}

} // namespace

int main() {
    try {
        round_trip();
        check_layout();
        large_frame();
        auto header = hello;
        header[0] = 0;
        malformed_header(header);
        header = hello;
        header[7] = 2;
        malformed_header(header);
        header = hello;
        header[11] = 129; // Odd width, incompatible with 4:2:0.
        malformed_header(header);
        malformed_header({0});
        malformed_header({});
        malformed_frame({0, 0});
        malformed_frame({0, 0, 0, 0});
        malformed_frame({1, 0, 0, 1});
        malformed_frame({0, 0, 0, 2});
        malformed_frame({0, 0, 0, 2, 1});
        require(wire::valid_mode(3840, 2160, 30), "4K30 must be valid");
        require(!wire::valid_mode(3840, 2160, 60), "4K60 exceeds level 5.1");
        require(!wire::valid_mode(4098, 2304, 1), "Oversize width must be rejected");
        require(!wire::valid_mode(48, 48, 0), "Zero frame rate must be rejected");
        cancel_read();
        std::cout << "Transport tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
