#include "wire.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace wire {
namespace {

using Word = std::array<std::uint8_t, 4>;
constexpr Word magic{'M', 'X', 'D', 'S'};
constexpr Word feedback_magic{'M', 'X', 'F', 'B'};
constexpr Word display_magic{'M', 'X', 'D', 'R'};
constexpr std::uint32_t stream_version = 3;
constexpr std::size_t prefix_size = 8;
constexpr std::size_t frame_prefix_size = 16;

Word to_bytes(std::uint32_t value) {
    return {static_cast<std::uint8_t>(value >> 24), static_cast<std::uint8_t>(value >> 16),
            static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
}

std::uint32_t from_bytes(const std::uint8_t* bytes) {
    return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) |
           (std::uint32_t{bytes[2]} << 8) | bytes[3];
}

void append_word(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    const auto word = to_bytes(value);
    bytes.insert(bytes.end(), word.begin(), word.end());
}

void check_header_prefix(const std::uint8_t* prefix) {
    if (!std::equal(magic.begin(), magic.end(), prefix) || from_bytes(prefix + 4) != stream_version)
        throw std::runtime_error("Unrecognized MaxDisplay stream or protocol version");
}

void check_mode(const Header& header) {
    if (!valid_mode(header.width, header.height, header.fps, header.codec))
        throw std::runtime_error("Unsupported codec, dimensions or frame rate");
    if (!header.timeout_ms || header.timeout_ms > max_timeout_ms)
        throw std::runtime_error("Frame deadline is outside protocol limits");
}

void check_size(std::size_t size, std::size_t max_bytes) {
    if (!size || size > max_bytes || size > UINT32_MAX)
        throw std::runtime_error("Video access unit length is outside protocol limits");
}

unsigned wait_remaining(std::chrono::steady_clock::time_point deadline) {
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (ms.count() <= 0) throw std::runtime_error("Handshake timed out");
    return static_cast<unsigned>(std::min<std::int64_t>(ms.count(), 250));
}

void validate_display(DisplaySize size) {
    if (!size.width || !size.height || size.width > 65535 || size.height > 65535)
        throw std::runtime_error("Invalid receiver display dimensions");
}

} // namespace

const std::vector<std::uint8_t> display_query{'M','X','D','Q', 0,0,0,2};

std::vector<std::uint8_t> encode_display_reply(DisplaySize size) {
    validate_display(size);
    std::vector<std::uint8_t> bytes(display_magic.begin(), display_magic.end());
    append_word(bytes, 1);
    append_word(bytes, size.width);
    append_word(bytes, size.height);
    return bytes;
}

DisplaySize decode_display_reply(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != display_reply_size || !std::equal(display_magic.begin(), display_magic.end(), bytes.begin())
        || from_bytes(bytes.data() + 4) != 1) throw std::runtime_error("Invalid display resolution reply");
    DisplaySize size{from_bytes(bytes.data() + 8), from_bytes(bytes.data() + 12)};
    validate_display(size);
    return size;
}

DisplaySize request_display_size(net::Socket& socket, unsigned timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    try {
        std::size_t sent = 0;
        while (sent < display_query.size()) {
            const auto n = socket.write_some_until(display_query.data() + sent, display_query.size() - sent, deadline);
            if (!n) throw std::runtime_error("Sending query timed out");
            sent += n;
        }
        std::vector<std::uint8_t> bytes(display_reply_size);
        if (!socket.read_exact(bytes.data(), bytes.size(), [&] { return wait_remaining(deadline); }))
            throw std::runtime_error("Receiver disconnected");
        return decode_display_reply(bytes);
    } catch (const std::runtime_error& e) {
        throw std::runtime_error(std::string(display_query_error) + e.what());
    }
}

bool valid_mode(std::uint32_t width, std::uint32_t height, std::uint32_t fps, Codec codec) noexcept {
    if (width < 48 || width > 4096 || height < 48 || height > 2304 ||
        width % 2 || height % 2 || fps < 1 || fps > 120)
        return false;
    if (codec == Codec::av1 || codec == Codec::h265) return true;
    const auto macroblocks = ((width + 15) / 16) * ((height + 15) / 16);
    return codec == Codec::h264 && macroblocks * fps <= 983040;
}

std::vector<std::uint8_t> encode_header(Header header) {
    check_mode(header);
    std::vector<std::uint8_t> data(magic.begin(), magic.end());
    data.reserve(header_size);
    append_word(data, stream_version);
    append_word(data, header.width);
    append_word(data, header.height);
    append_word(data, header.fps);
    append_word(data, static_cast<std::uint32_t>(header.codec));
    append_word(data, header.timeout_ms);
    return data;
}

Header decode_header(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != header_size)
        throw std::runtime_error("Incorrect stream header size");
    check_header_prefix(bytes.data());
    Header header{from_bytes(bytes.data() + 8), from_bytes(bytes.data() + 12), from_bytes(bytes.data() + 16),
                  static_cast<Codec>(from_bytes(bytes.data() + 20)), from_bytes(bytes.data() + 24)};
    check_mode(header);
    return header;
}

void send_header(net::Socket& socket, Header header) {
    const auto bytes = encode_header(header);
    socket.write_all(bytes.data(), bytes.size());
}

Header receive_header(net::Socket& socket, const std::function<DisplaySize()>& display_size) {
    const auto deadline = std::chrono::steady_clock::now() + header_timeout;
    auto poll = [&] { return wait_remaining(deadline); };
    std::vector<std::uint8_t> bytes(prefix_size);
    if (!socket.read_exact(bytes.data(), bytes.size(), poll))
        throw std::runtime_error("Peer disconnected before sending the stream header");
    if (bytes == display_query && display_size) {
        const auto reply = encode_display_reply(display_size());
        socket.write_all(reply.data(), reply.size());
        if (!socket.read_exact(bytes.data(), bytes.size(), poll))
            throw std::runtime_error("Peer disconnected after requesting the display resolution");
    }
    check_header_prefix(bytes.data());
    bytes.resize(header_size);
    if (!socket.read_exact(bytes.data() + prefix_size, header_size - prefix_size, poll))
        throw std::runtime_error("Peer disconnected before completing the stream header");
    return decode_header(bytes);
}

std::vector<std::uint8_t> pack_frame(const Frame& frame) {
    if (frame.data.empty() || frame.data.size() > max_frame_bytes - frame_prefix_size)
        throw std::runtime_error("Timed frame length is outside protocol limits");
    std::vector<std::uint8_t> bytes;
    bytes.reserve(frame_prefix_size + frame.data.size());
    append_word(bytes, frame.sequence);
    append_word(bytes, static_cast<std::uint32_t>(frame.timestamp_us >> 32));
    append_word(bytes, static_cast<std::uint32_t>(frame.timestamp_us));
    append_word(bytes, frame.keyframe ? 1 : 0);
    bytes.insert(bytes.end(), frame.data.begin(), frame.data.end());
    return bytes;
}

Frame unpack_frame(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() <= frame_prefix_size || bytes.size() > max_frame_bytes)
        throw std::runtime_error("Invalid timed frame length");
    const auto flags = from_bytes(bytes.data() + 12);
    if (flags > 1) throw std::runtime_error("Unknown timed frame flags");
    const auto timestamp_us = (std::uint64_t{from_bytes(bytes.data() + 4)} << 32) |
                              from_bytes(bytes.data() + 8);
    return {from_bytes(bytes.data()), timestamp_us, flags != 0,
            {bytes.begin() + frame_prefix_size, bytes.end()}};
}

std::vector<std::uint8_t> pack_feedback(Feedback feedback) {
    std::vector<std::uint8_t> bytes(feedback_magic.begin(), feedback_magic.end());
    bytes.reserve(feedback_size);
    append_word(bytes, feedback.sequence);
    append_word(bytes, feedback.request_keyframe ? 1 : 0);
    return bytes;
}

Feedback unpack_feedback(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != feedback_size || !std::equal(feedback_magic.begin(), feedback_magic.end(), bytes.begin()))
        throw std::runtime_error("Invalid receiver feedback");
    const auto flags = from_bytes(bytes.data() + 8);
    if (flags > 1) throw std::runtime_error("Unknown receiver feedback flags");
    return {from_bytes(bytes.data() + 4), flags != 0};
}

void send_frame(net::Socket& socket, const std::vector<std::uint8_t>& access_unit, std::size_t max_bytes) {
    check_size(access_unit.size(), max_bytes);
    const auto length = to_bytes(static_cast<std::uint32_t>(access_unit.size()));
    socket.write_all(length.data(), length.size());
    socket.write_all(access_unit.data(), access_unit.size());
}

bool send_frame_before(net::Socket& socket, const std::vector<std::uint8_t>& access_unit,
                       std::chrono::steady_clock::time_point deadline, std::size_t max_bytes) {
    check_size(access_unit.size(), max_bytes);
    const auto length = to_bytes(static_cast<std::uint32_t>(access_unit.size()));
    const auto count = socket.write_some_until(length.data(), length.size(), deadline);
    if (!count) return false;
    socket.write_all(length.data() + count, length.size() - count);
    socket.write_all(access_unit.data(), access_unit.size());
    return true;
}

bool receive_frame(net::Socket& socket, std::vector<std::uint8_t>& access_unit,
                   const std::function<unsigned()>& poll_interval, std::size_t max_bytes) {
    Word length{};
    if (!socket.read_exact(length.data(), length.size(), poll_interval))
        return false;
    const auto size = from_bytes(length.data());
    check_size(size, max_bytes);
    access_unit.resize(size);
    if (!socket.read_exact(access_unit.data(), size, poll_interval))
        throw std::runtime_error("Peer disconnected before sending the video access unit");
    return true;
}

} // namespace wire
