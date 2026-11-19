#include "wire.hpp"

#include <array>
#include <stdexcept>

namespace wire {
namespace {

using Word = std::array<std::uint8_t, 4>;
constexpr Word magic{'M', 'X', 'D', 'S'};
constexpr std::uint32_t version = 1;

Word to_bytes(std::uint32_t value) {
    return {static_cast<std::uint8_t>(value >> 24), static_cast<std::uint8_t>(value >> 16),
            static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
}

std::uint32_t from_bytes(const Word& bytes) {
    return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) |
           (std::uint32_t{bytes[2]} << 8) | bytes[3];
}

void check_mode(const Header& header) {
    if (!valid_mode(header.width, header.height, header.fps))
        throw std::runtime_error("Unsupported video dimensions or frame rate");
}

void check_size(std::size_t size) {
    if (!size || size > max_frame_bytes)
        throw std::runtime_error("H.264 access unit length is outside protocol limits");
}

} // namespace

bool valid_mode(std::uint32_t width, std::uint32_t height, std::uint32_t fps) noexcept {
    if (width < 48 || width > 4096 || height < 48 || height > 2304 ||
        width % 2 || height % 2 || fps < 1 || fps > 120)
        return false;
    const auto macroblocks = ((width + 15) / 16) * ((height + 15) / 16);
    return macroblocks * fps <= 983040;
}

void send_header(net::Socket& socket, std::uint32_t width, std::uint32_t height, std::uint32_t fps) {
    check_mode({width, height, fps});
    const std::array<Word, 5> header{magic, to_bytes(version), to_bytes(width),
                                     to_bytes(height), to_bytes(fps)};
    socket.write_all(header.data(), sizeof(header));
}

Header receive_header(net::Socket& socket) {
    std::array<Word, 5> data{};
    if (!socket.read_exact(data.data(), sizeof(data)))
        throw std::runtime_error("Peer disconnected before sending the stream header");
    if (data[0] != magic || from_bytes(data[1]) != version)
        throw std::runtime_error("Unrecognized MaxDisplay stream or protocol version");
    Header header{from_bytes(data[2]), from_bytes(data[3]), from_bytes(data[4])};
    check_mode(header);
    return header;
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
    const auto size = from_bytes(length);
    check_size(size);
    access_unit.resize(size);
    if (!socket.read_exact(access_unit.data(), size))
        throw std::runtime_error("Peer disconnected before sending the H.264 access unit");
    return true;
}

} // namespace wire
