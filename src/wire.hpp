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
};

constexpr std::uint32_t max_frame_bytes = 16 * 1024 * 1024;
bool valid_mode(std::uint32_t width, std::uint32_t height, std::uint32_t fps,
                Codec codec = Codec::h264) noexcept;

std::vector<std::uint8_t> encode_header(Header header);
Header decode_header(const std::vector<std::uint8_t>& bytes);
void send_header(net::Socket& socket, std::uint32_t width, std::uint32_t height,
                 std::uint32_t fps, Codec codec = Codec::h264);
Header receive_header(net::Socket& socket);
void send_frame(net::Socket& socket, const std::vector<std::uint8_t>& access_unit);
bool receive_frame(net::Socket& socket, std::vector<std::uint8_t>& access_unit);

} // namespace wire
