#pragma once

#include <cstdint>
#include <string>

constexpr std::uint16_t default_port = 49300;

struct SenderOptions {
    std::string host;
    std::uint16_t port = default_port;
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    std::uint32_t fps = 60;
    std::uint32_t bitrate = 12'000'000;
};

struct ReceiverOptions {
    std::string bind = "0.0.0.0";
    std::uint16_t port = default_port;
    bool windowed = false;
};

int run_sender(const SenderOptions& options);
int run_receiver(const ReceiverOptions& options);
