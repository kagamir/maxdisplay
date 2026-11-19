#include "wire.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace wire {
namespace {

using Word = std::array<std::uint8_t, 4>;
constexpr Word magic{'M', 'X', 'D', 'S'};
constexpr std::size_t legacy_size = 20;
constexpr std::size_t extended_size = 24;

Word to_bytes(std::uint32_t value) {
    return {static_cast<std::uint8_t>(value >> 24), static_cast<std::uint8_t>(value >> 16),
            static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
}

std::uint32_t from_bytes(const std::uint8_t* bytes) {
    return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) |
           (std::uint32_t{bytes[2]} << 8) | bytes[3];
}

std::size_t header_size(const std::uint8_t* prefix) {
    if (std::equal(magic.begin(), magic.end(), prefix)) {
        switch (from_bytes(prefix + 4)) {
            case 1: return legacy_size;
            case 2: return extended_size;
        }
    }
    throw std::runtime_error("Unrecognized MaxDisplay stream or protocol version");
}

void check_mode(const Header& header) {
    if (!valid_mode(header.width, header.height, header.fps, header.codec))
        throw std::runtime_error("Unsupported codec, dimensions or frame rate");
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
    if (codec == Codec::av1) return true;
    const auto macroblocks = ((width + 15) / 16) * ((height + 15) / 16);
    return codec == Codec::h264 && macroblocks * fps <= 983040;
}

std::vector<std::uint8_t> encode_header(Header header) {
    check_mode(header);
    // Keep the byte-for-byte v1 handshake when sending H.264 to an older peer.
    const bool legacy = header.codec == Codec::h264;
    std::vector<std::uint8_t> data(magic.begin(), magic.end());
    data.reserve(legacy ? legacy_size : extended_size);
    auto append_word = [&](std::uint32_t n) {
        auto word = to_bytes(n);
        data.insert(data.end(), word.begin(), word.end());
    };
    append_word(legacy ? 1 : 2);
    append_word(header.width);
    append_word(header.height);
    append_word(header.fps);
    if (!legacy) append_word(static_cast<std::uint32_t>(header.codec));
    return data;
}

Header decode_header(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 8 || bytes.size() != header_size(bytes.data()))
        throw std::runtime_error("Incorrect stream header size");
    Header header{from_bytes(bytes.data() + 8), from_bytes(bytes.data() + 12),
                  from_bytes(bytes.data() + 16)};
    if (bytes.size() == extended_size) header.codec = static_cast<Codec>(from_bytes(bytes.data() + 20));
    check_mode(header);
    return header;
}

void send_header(net::Socket& socket, std::uint32_t width, std::uint32_t height,
                 std::uint32_t fps, Codec codec) {
    auto bytes = encode_header({width, height, fps, codec});
    socket.write_all(bytes.data(), bytes.size());
}

Header receive_header(net::Socket& socket) {
    std::vector<std::uint8_t> bytes(legacy_size);
    if (!socket.read_exact(bytes.data(), legacy_size))
        throw std::runtime_error("Peer disconnected before sending the stream header");
    if (header_size(bytes.data()) == extended_size) {
        bytes.resize(extended_size);
        if (!socket.read_exact(bytes.data() + legacy_size, extended_size - legacy_size))
            throw std::runtime_error("Peer disconnected before sending the codec");
    }
    return decode_header(bytes);
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
