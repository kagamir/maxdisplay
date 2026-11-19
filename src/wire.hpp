#pragma once

#include "net.hpp"

#include <cstdint>
#include <vector>

namespace wire {

struct Header {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t fps;
};

constexpr std::uint32_t max_frame_bytes = 16 * 1024 * 1024;
bool valid_mode(std::uint32_t width, std::uint32_t height, std::uint32_t fps) noexcept;

void send_header(net::Socket& socket, std::uint32_t width, std::uint32_t height, std::uint32_t fps);
Header receive_header(net::Socket& socket);
void send_frame(net::Socket& socket, const std::vector<std::uint8_t>& access_unit);
bool receive_frame(net::Socket& socket, std::vector<std::uint8_t>& access_unit);

} // namespace wire
