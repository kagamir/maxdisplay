#include "wire.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace wire {
namespace {

using Word = std::array<std::uint8_t, 4>;
constexpr Word magic{'M', 'X', 'D', 'S'};
constexpr Word feedback_magic{'M', 'X', 'F', 'B'};
constexpr std::size_t legacy_size = 20;
constexpr std::size_t extended_size = 24;
constexpr std::size_t timed_size = 28;
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

std::size_t header_size(const std::uint8_t* prefix) {
    if (std::equal(magic.begin(), magic.end(), prefix)) {
        switch (from_bytes(prefix + 4)) {
            case 1: return legacy_size;
            case 2: return extended_size;
            case 3: return timed_size;
        }
    }
    throw std::runtime_error("Unrecognized MaxDisplay stream or protocol version");
}

void check_mode(const Header& header) {
    if (!valid_mode(header.width, header.height, header.fps, header.codec))
        throw std::runtime_error("Unsupported codec, dimensions or frame rate");
    if (header.timeout_ms > max_timeout_ms)
        throw std::runtime_error("Frame deadline exceeds protocol limits");
    if (header.codec == Codec::h265 && !header.timeout_ms)
        throw std::runtime_error("H.265 requires stream protocol version 3");
}

void check_size(std::size_t size) {
    if (!size || size > max_frame_bytes)
        throw std::runtime_error("Video access unit length is outside protocol limits");
}

} // namespace

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
    const unsigned version = header.timeout_ms ? 3 : (header.codec == Codec::h264 ? 1 : 2);
    std::vector<std::uint8_t> data(magic.begin(), magic.end());
    data.reserve(timed_size);
    append_word(data, version);
    append_word(data, header.width);
    append_word(data, header.height);
    append_word(data, header.fps);
    if (version >= 2) append_word(data, static_cast<std::uint32_t>(header.codec));
    if (version == 3) append_word(data, header.timeout_ms);
    return data;
}

Header decode_header(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 8 || bytes.size() != header_size(bytes.data()))
        throw std::runtime_error("Incorrect stream header size");
    Header header{from_bytes(bytes.data() + 8), from_bytes(bytes.data() + 12),
                  from_bytes(bytes.data() + 16)};
    if (bytes.size() >= extended_size) header.codec = static_cast<Codec>(from_bytes(bytes.data() + 20));
    if (bytes.size() == timed_size) {
        header.timeout_ms = from_bytes(bytes.data() + 24);
        if (!header.timeout_ms) throw std::runtime_error("Missing frame deadline");
    }
    check_mode(header);
    return header;
}

void send_header(net::Socket& socket, std::uint32_t width, std::uint32_t height,
                 std::uint32_t fps, Codec codec, std::uint32_t timeout_ms) {
    auto bytes = encode_header({width, height, fps, codec, timeout_ms});
    socket.write_all(bytes.data(), bytes.size());
}

Header receive_header(net::Socket& socket) {
    std::vector<std::uint8_t> bytes(legacy_size);
    if (!socket.read_exact(bytes.data(), legacy_size))
        throw std::runtime_error("Peer disconnected before sending the stream header");
    const auto size = header_size(bytes.data());
    if (size > legacy_size) {
        bytes.resize(size);
        if (!socket.read_exact(bytes.data() + legacy_size, size - legacy_size))
            throw std::runtime_error("Peer disconnected before completing the stream header");
    }
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

void send_frame(net::Socket& socket, const std::vector<std::uint8_t>& access_unit) {
    check_size(access_unit.size());
    const auto length = to_bytes(static_cast<std::uint32_t>(access_unit.size()));
    socket.write_all(length.data(), length.size());
    socket.write_all(access_unit.data(), access_unit.size());
}

bool receive_frame(net::Socket& socket, std::vector<std::uint8_t>& access_unit) {
    Word length{};
    if (!socket.read_exact(length.data(), length.size()))
        return false;
    const auto size = from_bytes(length.data());
    check_size(size);
    access_unit.resize(size);
    if (!socket.read_exact(access_unit.data(), size))
        throw std::runtime_error("Peer disconnected before sending the video access unit");
    return true;
}

} // namespace wire
