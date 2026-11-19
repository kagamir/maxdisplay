// The macOS job encodes the pattern and the Windows job decodes the same access
// units. Frame boundaries are preserved by a four-byte big-endian length.
#include "wire.hpp"
#ifdef __APPLE__
#include "mac/encoder.hpp"
#else
#include "win/decoder.hpp"
#endif

#include <algorithm>
#include <type_traits>
#include <memory>
#include <string>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
constexpr uint32_t width = 320, height = 180, fps = 30;
constexpr unsigned frame_count = 6;
using Color = std::array<uint8_t, 4>;

Color color_at(unsigned x, unsigned y, unsigned index) {
    if (y < height / 2) {
        if (x < width / 2) return {20, 20, 220, 255};  // red, in BGRA order
        return {20, 220, 20, 255};                      // green
    }
    if (x < width / 2) return {220, 20, 20, 255};       // blue
    auto shade = static_cast<uint8_t>(30 + index * 36);
    return {shade, shade, shade, 255};                 // changing grayscale
}

#ifdef __APPLE__
void probe(const char* path) {
    std::ofstream stream(path, std::ios::binary);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    unsigned packets = 0;
    Encoder encoder(width, height, fps, 2'000'000, [&](std::vector<uint8_t> au) {
        const auto n = static_cast<uint32_t>(au.size());
        const uint8_t length[] = {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)};
        stream.write(reinterpret_cast<const char*>(length), sizeof(length));
        stream.write(reinterpret_cast<const char*>(au.data()), n);
        ++packets;
    });
    for (unsigned i = 0; i < frame_count; ++i) {
        CVPixelBufferRef pixel = nullptr;
        if (CVPixelBufferCreate(nullptr, width, height, kCVPixelFormatType_32BGRA, nullptr, &pixel) != kCVReturnSuccess)
            throw std::runtime_error("Could not allocate test pixels");
        std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)> guard(pixel, &CVPixelBufferRelease);
        if (CVPixelBufferLockBaseAddress(pixel, 0) != kCVReturnSuccess)
            throw std::runtime_error("Could not lock test pixels");
        auto* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pixel));
        for (unsigned y = 0; y < height; ++y) {
            auto* row = base + y * CVPixelBufferGetBytesPerRow(pixel);
            for (unsigned x = 0; x < width; ++x) {
                auto color = color_at(x, y, i);
                std::copy(color.begin(), color.end(), row + 4 * x);
            }
        }
        CVPixelBufferUnlockBaseAddress(pixel, 0);
        encoder.encode(pixel, CMTimeMake(i, fps));
    }
    encoder.finish();
    if (packets != frame_count) throw std::runtime_error("Encoder did not return all pattern frames");
    stream.close();
}
#else
void probe(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    Decoder decoder(width, height, fps);
    unsigned decoded = 0;
    auto check = [&](VideoFrame&& frame) {
        if (frame.width != width || frame.height != height || frame.bgra.size() != width * height * 4)
            throw std::runtime_error("Unexpected decoded geometry");
        for (unsigned y : {height / 4, height * 3 / 4}) {
            for (unsigned x : {width / 4, width * 3 / 4}) {
                const auto expected = color_at(x, y, decoded);
                for (unsigned c = 0; c < 3; ++c) {
                    const auto actual = frame.bgra[(y * width + x) * 4 + c];
                    if (std::abs(static_cast<int>(actual) - expected[c]) > 28)
                        throw std::runtime_error("Wrong pixel color or frame order at frame " + std::to_string(decoded));
                }
            }
        }
        ++decoded;
    };
    for (unsigned i = 0; i < frame_count; ++i) {
        uint32_t n = 0;
        for (unsigned j = 0; j < 4; ++j) n = (n << 8) | static_cast<uint8_t>(stream.get());
        if (n == 0 || n > wire::max_frame_bytes) throw std::runtime_error("Invalid test access unit");
        std::vector<uint8_t> access_unit(n);
        stream.read(reinterpret_cast<char*>(access_unit.data()), n);
        decoder.decode(access_unit, check);
    }
    decoder.finish(check);
    if (decoded != frame_count) throw std::runtime_error("Did not decode all pattern frames");
}
#endif
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        probe(argv[1]);
        std::cout << "Checked " << frame_count << " H.264 pattern frames\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
