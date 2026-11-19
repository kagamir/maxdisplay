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

void codec_headers() {
    require(wire::encode_header({1920, 1080, 60}) == hello, "Legacy H.264 header changed");
    Connection connection;
    wire::send_header(connection.writer, 3840, 2160, 60, Codec::av1);
    wire::send_frame(connection.writer, {0x12, 0x00});
    const auto header = wire::receive_header(connection.reader);
    require(header.codec == Codec::av1 && header.width == 3840 && header.fps == 60, "AV1 header mismatch");
    Bytes frame;
    require(wire::receive_frame(connection.reader, frame) && frame == Bytes{0x12, 0x00}, "Lost frame boundary");
    auto bad = wire::encode_header(header);
    bad.back() = 255;
    expect_failure([&] { wire::decode_header(bad); }, "Accepted unknown codec identifier");
    bad.resize(20);
    expect_failure([&] { wire::decode_header(bad); }, "Accepted truncated extended header");
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

void timed_frames_and_feedback() {
    Connection connection;
    wire::send_header(connection.writer, 3840, 2160, 60, Codec::h265, 125);
    const auto header = wire::receive_header(connection.reader);
    require(header.codec == Codec::h265 && header.timeout_ms == 125, "Timed header lost codec or deadline");
    const auto bytes = wire::pack_frame({0xffffffff, 0x0102030405060708ull, true, {0x65}});
    require(bytes == Bytes{255,255,255,255, 1,2,3,4,5,6,7,8, 0,0,0,1, 0x65}, "Timed frame byte layout changed");
    wire::send_frame(connection.writer, bytes);
    Bytes payload;
    require(wire::receive_frame(connection.reader, payload), "Expected frame envelope");
    const auto frame = wire::unpack_frame(payload);
    require(frame.sequence == 0xffffffff && frame.timestamp_us == 0x0102030405060708ull &&
            frame.keyframe && frame.data == Bytes{0x65}, "Timed frame mismatch");
    require(wire::unpack_frame(wire::pack_frame({0, 0, false, {1}})).sequence == 0, "Cannot wrap sequence");

    auto request = wire::pack_feedback({0x01020304, true});
    require(request == Bytes{'M','X','F','B', 1,2,3,4, 0,0,0,1}, "Feedback layout changed");
    Bytes record(wire::feedback_size);
    require(connection.writer.try_read(record.data(), record.size()) == 0, "No feedback expected yet");
    connection.reader.write_all(request.data(), 1);
    require(net::wait_readable(connection.writer, 1000) && !connection.writer.peer_closed(),
            "Feedback mistaken for disconnect");
    require(connection.writer.try_read(record.data(), record.size()) == 1, "Split feedback lost");
    connection.reader.write_all(request.data() + 1, request.size() - 1);
    require(connection.writer.read_exact(record.data() + 1, record.size() - 1), "Feedback truncated");
    const auto feedback = wire::unpack_feedback(record);
    require(feedback.sequence == 0x01020304 && feedback.request_keyframe, "Keyframe request lost");
    connection.reader = {};
    require(net::wait_readable(connection.writer, 1000), "Missing EOF");
    expect_failure([&] { connection.writer.try_read(record.data(), record.size()); }, "EOF mistaken for would-block");

    auto bad = wire::encode_header(header);
    bad.back() = 0;
    expect_failure([&] { wire::decode_header(bad); }, "v3 accepted zero deadline");
    bad[24] = 1;
    expect_failure([&] { wire::decode_header(bad); }, "v3 accepted excessive deadline");
    bad.pop_back();
    expect_failure([&] { wire::decode_header(bad); }, "v3 accepted truncated header");
    payload[15] = 2;
    expect_failure([&] { wire::unpack_frame(payload); }, "Invalid frame flags accepted");
    payload.resize(16);
    expect_failure([&] { wire::unpack_frame(payload); }, "Empty timed access unit accepted");
    expect_failure([&] { wire::unpack_feedback({}); }, "Empty feedback accepted");
    request.back() = 2;
    expect_failure([&] { wire::unpack_feedback(request); }, "Unknown feedback flags accepted");
    request.back() = 1;
    request.front() = 0;
    expect_failure([&] { wire::unpack_feedback(request); }, "Invalid feedback magic accepted");
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

void partial_frame_polling() {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    Connection connection;
    const Bytes prefix{0, 0};
    connection.writer.write_all(prefix.data(), prefix.size());
    unsigned stage = 0;
    auto resume = Clock::now() + 12ms;
    auto poll = [&] {
        if (stage < 2 && Clock::now() >= resume) {
            // Resume first a partial length prefix, then a partial frame body.
            const Bytes next = stage == 0 ? Bytes{0, 3, 0xaa} : Bytes{0xbb, 0xcc};
            connection.writer.write_all(next.data(), next.size());
            ++stage;
            resume = Clock::now() + 12ms;
        }
        return 2u;
    };
    Bytes frame;
    require(wire::receive_frame(connection.reader, frame, poll) && frame == Bytes{0xaa, 0xbb, 0xcc},
            "Polling a partial TCP frame lost framing");
    require(stage == 2, "No idle poll during partial TCP frame");
}

void frame_commit_deadline() {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    Connection connection;
    require(!wire::send_frame_before(connection.writer, {0x41}, Clock::now() - 1ms),
            "An expired frame was committed to TCP");
    require(!net::wait_readable(connection.reader, 0), "Dropped TCP frame left a length prefix");
    require(wire::send_frame_before(connection.writer, {0x65}, Clock::now() + 2s),
            "Fresh frame missed the TCP commit deadline");
    Bytes frame;
    require(wire::receive_frame(connection.reader, frame) && frame == Bytes{0x65},
            "Dropped frame corrupted the subsequent TCP frame");
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
        codec_headers();
        timed_frames_and_feedback();
        partial_frame_polling();
        frame_commit_deadline();
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
