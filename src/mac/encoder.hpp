#pragma once

#include "media.hpp"

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class Encoder {
public:
    using Output = std::function<void(std::vector<uint8_t>, bool keyframe)>;

    Encoder(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate, Output output,
            Codec codec = Codec::h264, bool require_hardware = false);
    ~Encoder();

    // Call from the serial capture queue. Output is delivered before encode() returns.
    // Returns false when the codec skipped the input. Zero fps uses the configured rate.
    bool encode(CVPixelBufferRef pixels, CMTime pts, bool force_keyframe = false, uint32_t current_fps = 0);
    // Quality of the latest encoded picture. Read on the capture queue after encode().
    std::optional<double> last_psnr() const;
    std::optional<int> last_quantizer() const;
    void set_bitrate(uint32_t bps);
    bool hardware_accelerated() const;
    // Call after capture has stopped and its queue is drained.
    void finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
