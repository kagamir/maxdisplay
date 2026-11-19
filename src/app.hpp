#pragma once

#include "media.hpp"

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
    Codec codec = Codec::h264;
    Transport transport = Transport::tcp;
    double scale = 1.0;
};

struct ReceiverOptions {
    std::string bind = "0.0.0.0";
    std::uint16_t port = default_port;
    bool windowed = false;
    Transport transport = Transport::tcp;
};

int run_cli(int argc, char** argv);
int run_sender(const SenderOptions& options);
int run_receiver(const ReceiverOptions& options);
