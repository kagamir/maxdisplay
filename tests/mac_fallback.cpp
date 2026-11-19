// Simulate losing the hardware encoder during an established stream. All other
// framework calls, including encoding the replacement picture in software, are real.
#include "mac/encoder.hpp"
#include "frame_recovery.hpp"

#include <VideoToolbox/VideoToolbox.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace {
bool reject_next = false;
}

extern "C" decltype(VTCompressionSessionEncodeFrame) TestVTEncodeFrame;
extern "C" OSStatus TestVTEncodeFrame(VTCompressionSessionRef session, CVImageBufferRef image,
                                      CMTime pts, CMTime duration, CFDictionaryRef properties,
                                      void* refcon, VTEncodeInfoFlags* flags) {
    if (reject_next) {
        reject_next = false;
        return kVTVideoEncoderMalfunctionErr;
    }
    return VTCompressionSessionEncodeFrame(session, image, pts, duration, properties, refcon, flags);
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string_view name = argv[1];
    if (name != "h264" && name != "h265") return 2;
    const Codec codec = name == "h265" ? Codec::h265 : Codec::h264;
    try {
        constexpr unsigned width = 320, height = 180;
        CVPixelBufferRef pixel = nullptr;
        if (CVPixelBufferCreate(nullptr, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                nullptr, &pixel) != kCVReturnSuccess)
            throw std::runtime_error("Could not create fallback test image");
        std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)>
            release(pixel, &CVPixelBufferRelease);
        if (CVPixelBufferLockBaseAddress(pixel, 0) != kCVReturnSuccess)
            throw std::runtime_error("Could not lock fallback test image");
        for (size_t i = 0; i < CVPixelBufferGetPlaneCount(pixel); ++i)
            std::memset(CVPixelBufferGetBaseAddressOfPlane(pixel, i), i ? 128 : 80,
                        CVPixelBufferGetHeightOfPlane(pixel, i) * CVPixelBufferGetBytesPerRowOfPlane(pixel, i));
        CVPixelBufferUnlockBaseAddress(pixel, 0);

        unsigned pictures = 0;
        Encoder encoder(width, height, 30, 1'000'000, [&](std::vector<uint8_t> au, bool key) {
            if (pictures < 2 && (!key || !has_recovery_picture(codec, au)))
                throw std::runtime_error("Backend started with a dependent picture");
            ++pictures;
        }, codec);
        if (!encoder.hardware_accelerated()) {
            std::cout << "Hardware encoder unavailable for runtime failure injection\n";
            return 77;
        }
        encoder.encode(pixel, CMTimeMake(0, 30));
        reject_next = true;
        encoder.encode(pixel, CMTimeMake(1, 30));
        encoder.encode(pixel, CMTimeMake(2, 30));
        if (reject_next || encoder.hardware_accelerated() || pictures != 3)
            throw std::runtime_error("Failed hardware input was not recovered exactly once in software");
        encoder.finish();
        std::cout << "Recovered hardware submission failure with a software " << codec_name(codec) << " sync picture\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
