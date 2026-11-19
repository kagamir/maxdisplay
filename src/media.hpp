#pragma once

#include <cstdint>

enum class Codec : std::uint32_t { h264 = 1, av1 = 2, h265 = 3 };
enum class Transport { tcp, udp };

inline const char* codec_name(Codec codec) noexcept {
    switch (codec) {
    case Codec::h264: return "H.264";
    case Codec::h265: return "H.265";
    case Codec::av1: return "AV1";
    }
    return "unknown codec";
}
