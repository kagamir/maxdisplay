#pragma once

#include "media.hpp"
#include "net.hpp"

#include <cstdint>
#include <vector>

namespace wire {

struct Header {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t fps;
    Codec codec = Codec::h264;
    std::uint32_t timeout_ms = 0; // Zero identifies the legacy access-unit-only stream.
};

struct Frame {
    std::uint32_t sequence;
    std::uint64_t timestamp_us;
    bool keyframe;
    std::vector<std::uint8_t> data;
};

struct Feedback {
    std::uint32_t sequence;
    bool request_keyframe;
};

constexpr std::uint32_t max_frame_bytes = 16 * 1024 * 1024;
constexpr std::uint32_t max_timeout_ms = 60000;
constexpr std::size_t feedback_size = 12;
bool valid_mode(std::uint32_t width, std::uint32_t height, std::uint32_t fps,
                Codec codec = Codec::h264) noexcept;

std::vector<std::uint8_t> encode_header(Header header);
Header decode_header(const std::vector<std::uint8_t>& bytes);
void send_header(net::Socket& socket, std::uint32_t width, std::uint32_t height,
                 std::uint32_t fps, Codec codec = Codec::h264, std::uint32_t timeout_ms = 0);
Header receive_header(net::Socket& socket);
std::vector<std::uint8_t> pack_frame(const Frame& frame);
Frame unpack_frame(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> pack_feedback(Feedback feedback);
Feedback unpack_feedback(const std::vector<std::uint8_t>& bytes);
void send_frame(net::Socket& socket, const std::vector<std::uint8_t>& access_unit);
// Drops an expired frame only before writing any part of its length prefix.
bool send_frame_before(net::Socket& socket, const std::vector<std::uint8_t>& access_unit,
                       std::chrono::steady_clock::time_point deadline);
bool receive_frame(net::Socket& socket, std::vector<std::uint8_t>& access_unit,
                   const std::function<unsigned()>& poll_interval = {});

} // namespace wire
