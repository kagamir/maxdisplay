#pragma once

#include "media.hpp"

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class Encoder {
public:
    using Output = std::function<void(std::vector<uint8_t>, bool keyframe)>;

    Encoder(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate, Output output,
            Codec codec = Codec::h264, bool require_hardware = false);
    Encoder(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate,
            std::function<void(std::vector<uint8_t>)> output, Codec codec = Codec::h264,
            bool require_hardware = false);
    ~Encoder();

    // Call from the serial capture queue. Output is delivered before encode() returns.
    void encode(CVPixelBufferRef pixels, CMTime pts, bool force_keyframe = false);
    void set_bitrate(uint32_t bps);
    bool hardware_accelerated() const;
    // Call after capture has stopped and its queue is drained.
    void finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
