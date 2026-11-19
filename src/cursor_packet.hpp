#pragma once

#include "net.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace cursor::packet {

// Distinguish cursor traffic from ENet on the shared UDP socket. The inner
// packet starts with the session token in both plaintext and encrypted mode.
inline constexpr std::array<std::uint8_t, 4> magic{'M', 'X', 'C', 'P'};
inline constexpr std::size_t prefix_size = magic.size();
inline constexpr std::size_t max_payload = net::max_datagram_size - prefix_size;

inline bool matches(const std::uint8_t* bytes, std::size_t size) {
    return size >= prefix_size && std::equal(magic.begin(), magic.end(), bytes);
}

inline std::vector<std::uint8_t> wrap(const std::vector<std::uint8_t>& payload) {
    if (payload.empty() || payload.size() > max_payload)
        throw std::invalid_argument("Invalid cursor packet length");
    std::vector<std::uint8_t> bytes;
    bytes.reserve(prefix_size + payload.size());
    bytes.insert(bytes.end(), magic.begin(), magic.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

inline bool unwrap(std::vector<std::uint8_t>& bytes) {
    if (bytes.size() <= prefix_size || bytes.size() > net::max_datagram_size || !matches(bytes.data(), bytes.size()))
        return false;
    bytes.erase(bytes.begin(), bytes.begin() + prefix_size);
    return true;
}

} // namespace cursor::packet
