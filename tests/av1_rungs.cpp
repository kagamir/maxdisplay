#include "av1.hpp"

#include <iostream>
#include <stdexcept>

namespace {
constexpr unsigned width = 160, height = 96, maximum_fps = 30;

size_t encode_sequence(unsigned rung) {
    Av1Encoder encoder(width, height, maximum_fps, 300'000);
    std::vector<uint8_t> y(width * height), uv(width * height / 2, 128);
    uint32_t random = 42;
    size_t bytes = 0;
    for (unsigned i = 0; i < 100; ++i) {
        for (auto& luma : y) {
            random = random * 1664525u + 1013904223u;
            luma = static_cast<uint8_t>(16 + (random >> 24) * 219 / 255);
        }
        const bool force = i == 90;
        auto output = [&](std::vector<uint8_t> packet, bool key) {
            if ((i == 0 || force) && !key) throw std::runtime_error("Missing AV1 recovery keyframe");
            if (i && !force && key) throw std::runtime_error("Unexpected periodic AV1 keyframe");
            // Compare steady-state inter frames at identical PTS and pixel complexity.
            if (i >= 30 && i < 90) bytes += packet.size();
        };
        if (!encoder.encode_nv12(y.data(), width, uv.data(), width, 3 * i, output, force, rung))
            throw std::runtime_error("AV1 skipped a test frame");
        const int quantizer = encoder.last_quantizer();
        if (quantizer < 0 || quantizer > 63) throw std::runtime_error("Invalid AV1 quantizer");
    }
    return bytes;
}
} // namespace

int main() {
    try {
        const auto full = encode_sequence(maximum_fps);
        const auto reduced = encode_sequence(maximum_fps / 3);
        std::cout << "AV1 bytes with 1-tick / 3-tick duration: " << full << " / " << reduced << '\n';
        if (reduced < full * 3 / 2)
            throw std::runtime_error("Lower FPS did not increase the AV1 frame budget");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
