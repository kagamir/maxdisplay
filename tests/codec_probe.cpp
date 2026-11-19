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
constexpr unsigned frame_count = 8, recovery_frame = 5;
using Color = std::array<uint8_t, 4>;

Color color_at(unsigned x, unsigned y, unsigned index) {
    if (y < height / 2) {
        if (x < width / 2) return {20, 20, 220, 255};  // red, in BGRA order
        return {20, 220, 20, 255};                      // green
    }
    if (x < width / 2) return {220, 20, 20, 255};       // blue
    auto shade = static_cast<uint8_t>(30 + index * 25);
    return {shade, shade, shade, 255};                 // changing grayscale
}

#ifdef __APPLE__
constexpr unsigned moving_frames = 12, still_frames = 30;

// Scroll dense, high-contrast glyphs at a constrained bitrate, then hold the
// last page still. The interop quadrants are too easy to compress for this test.
uint8_t text_luma(unsigned x, unsigned y, unsigned index) {
    y += 13 * std::min(index, moving_frames - 1);
    const unsigned col = x % 7, row = y % 10;
    if (col >= 5 || row >= 8) return 218;
    uint32_t glyph = (x / 7 + 101 * (y / 10)) * 2654435761u;
    glyph ^= glyph >> 16;
    return (glyph >> ((5 * row + col) % 32)) & 1 ? 36 : 218;
}

bool probe(const char* path, Codec codec, bool strict, bool topoff = false) {
    std::ofstream stream(path, std::ios::binary);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    unsigned packets = 0;
    const unsigned count = topoff ? moving_frames + still_frames : frame_count;
    std::vector<uint8_t> reference(topoff ? width * height : 0);
    Encoder encoder(width, height, fps, topoff ? 250'000 : 2'000'000, [&](std::vector<uint8_t> au, bool keyframe) {
        if ((packets == 0 || (!topoff && packets == recovery_frame)) && !keyframe)
            throw std::runtime_error("Missing requested random access frame at " + std::to_string(packets));
        // The receiver loss test skips index 3 and then delivers this dependent picture.
        if (!topoff && packets == recovery_frame - 1 && keyframe)
            throw std::runtime_error("Expected a predicted picture immediately before recovery");
        const auto n = static_cast<uint32_t>(au.size());
        const uint8_t length[] = {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)};
        stream.write(reinterpret_cast<const char*>(length), sizeof(length));
        stream.write(reinterpret_cast<const char*>(au.data()), n);
        ++packets;
    }, codec, strict, !topoff);
    const bool hardware = encoder.hardware_accelerated();
    if (strict && !hardware) throw std::runtime_error("Required hardware encoder was not selected");
    std::cout << "Encoder: " << (hardware ? "hardware" : "software") << '\n';
    for (unsigned i = 0; i < count; ++i) {
        CVPixelBufferRef pixel = nullptr;
        if (CVPixelBufferCreate(nullptr, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, nullptr, &pixel) != kCVReturnSuccess)
            throw std::runtime_error("Could not allocate test pixels");
        std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)> guard(pixel, &CVPixelBufferRelease);
        if (CVPixelBufferLockBaseAddress(pixel, 0) != kCVReturnSuccess)
            throw std::runtime_error("Could not lock test pixels");
        // Generate video-range BT.709 NV12, matching the ScreenCaptureKit source.
        CVBufferSetAttachment(pixel, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                              kCVAttachmentMode_ShouldPropagate);
        auto* luma = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixel, 0));
        auto* chroma = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixel, 1));
        const auto luma_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel, 0);
        const auto chroma_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel, 1);
        for (unsigned y = 0; y < height; ++y) {
            if (topoff) {
                auto* line = luma + y * luma_stride;
                for (unsigned x = 0; x < width; ++x) line[x] = text_luma(x, y, i);
                if (y % 2 == 0) std::fill_n(chroma + y / 2 * chroma_stride, width, 128);
                if (i == moving_frames - 1) std::copy_n(line, width, reference.data() + y * width);
                continue;
            }
            for (unsigned x = 0; x < width; ++x) {
                const auto color = color_at(x, y, i);
                const double luminance = 0.2126 * color[2] + 0.7152 * color[1] + 0.0722 * color[0];
                luma[y * luma_stride + x] = static_cast<uint8_t>(std::lround(16 + 219 * luminance / 255));
                if (y % 2 == 0 && x % 2 == 0) {
                    auto* uv = chroma + y / 2 * chroma_stride + x;
                    uv[0] = static_cast<uint8_t>(std::lround(128 + (112.0 / 255) * (color[0] - luminance) / (1 - 0.0722)));
                    uv[1] = static_cast<uint8_t>(std::lround(128 + (112.0 / 255) * (color[2] - luminance) / (1 - 0.2126)));
                }
            }
        }
        CVPixelBufferUnlockBaseAddress(pixel, 0);
        if (!topoff && i == 2) encoder.set_bitrate(950'000);
        if (!topoff && i == recovery_frame) encoder.set_bitrate(1'500'000);
        encoder.encode(pixel, CMTimeMake(i, fps), !topoff && i == recovery_frame);
    }
    const bool final_hardware = encoder.hardware_accelerated();
    encoder.finish();
    if (packets != count) throw std::runtime_error("Encoder did not return all pattern frames");
    stream.close();
    if (topoff) {
        // Save the exact last moving luma plane and the backend that finished
        // the run (a hardware session can fall back during submission).
        std::ofstream pgm(std::string(path) + ".pgm", std::ios::binary);
        pgm.exceptions(std::ios::badbit | std::ios::failbit);
        pgm << "P5\n# encoder=" << (final_hardware ? "hardware" : "software")
            << "\n" << width << ' ' << height << "\n255\n";
        pgm.write(reinterpret_cast<const char*>(reference.data()), reference.size());
        pgm.close();
    }
    return hardware;
}
#else
bool probe(const char* path, Codec codec, bool strict, unsigned start = 0) {
    std::ifstream stream(path, std::ios::binary);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    Decoder decoder(width, height, fps, codec, strict);
    unsigned decoded = start;
    auto check = [&](VideoFrame&& frame) {
        if (frame.width != width || frame.height != height || frame.bgra.size() != width * height * 4)
            throw std::runtime_error("Unexpected decoded geometry");
        // Realtime AV1 chroma blocks can span the quadrant boundary. Check
        // solid colors inside each patch instead of the transition band.
        const unsigned margin = codec == Codec::av1 ? 24 : 8;
        for (unsigned y : {height / 4, height / 2 - margin, height / 2 + margin, height * 3 / 4}) {
            for (unsigned x : {width / 4, width / 2 - margin, width / 2 + margin, width * 3 / 4}) {
                const auto expected = color_at(x, y, decoded);
                for (unsigned c = 0; c < 3; ++c) {
                    const auto actual = frame.bgra[(y * width + x) * 4 + c];
                    if (std::abs(static_cast<int>(actual) - expected[c]) > 12)
                        throw std::runtime_error("Wrong pixel at frame " + std::to_string(decoded) +
                            " x=" + std::to_string(x) + " y=" + std::to_string(y) + " channel=" +
                            std::to_string(c) + ", expected " + std::to_string(expected[c]) +
                            ", got " + std::to_string(actual));
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
        if (i < start) continue;
        decoder.decode(access_unit, check);
        std::cout << "After access unit " << i + 1 << ", displayed " << decoded << " frame(s)\n";
        if (i == start + 2 && decoded == start) throw std::runtime_error("Decoder did not present a live startup frame");
    }
    decoder.finish(check);
    if (decoded != frame_count) throw std::runtime_error("Did not decode all pattern frames");
    const bool hardware = decoder.hardware_accelerated();
    if (strict && !hardware) throw std::runtime_error("Required hardware decoder did not produce hardware output");
    std::cout << "Decoder: " << (hardware ? "hardware" : "software") << '\n';
    return hardware;
}
#endif
} // namespace

int main(int argc, char** argv) {
#ifdef __APPLE__
    const bool topoff = argc == 4 && std::string(argv[3]) == "topoff";
    if (!topoff && argc > 3) return 2;
#else
    if (argc > 3) return 2;
#endif
    if (argc < 2) return 2;
    const std::string name = argc >= 3 ? argv[2] : "h264";
    if (name != "av1" && name != "h264" && name != "h265") return 2;
    const Codec codec = name == "h265" ? Codec::h265 : name == "av1" ? Codec::av1 : Codec::h264;
    try {
#ifdef __APPLE__
        if (topoff) {
            probe(argv[1], codec, false, true);
            std::cout << "Encoded " << moving_frames << " moving frames and " << still_frames
                      << " repeats of the final frame\n";
            return 0;
        }
#endif
        const bool hardware = probe(argv[1], codec, false);
#ifdef __APPLE__
        if (hardware) probe(argv[1], codec, true);
#else
        // A fresh decoder must recover using only the requested keyframe and its successors.
        probe(argv[1], codec, hardware, recovery_frame);
#endif
        std::cout << "Checked " << frame_count << " " << name << " pattern frames\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
