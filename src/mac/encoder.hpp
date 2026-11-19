#pragma once

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class Encoder {
public:
    using Output = std::function<void(std::vector<uint8_t>)>;

    Encoder(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate, Output output);
    ~Encoder();

    // Call from the serial capture queue. Output can run on a VideoToolbox thread.
    void encode(CVPixelBufferRef pixels, CMTime pts);
    // May be polled from another thread, including while the desktop is idle.
    void check_error() const;
    // Call after capture has stopped and its queue is drained.
    void finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
