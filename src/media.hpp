#pragma once

#include <cstdint>

enum class Codec : std::uint32_t { h264 = 1, av1 = 2 };
enum class Transport { tcp, udp };
