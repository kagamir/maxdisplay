#pragma once

#include "media.hpp"
#include "video_frame.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

class Av1Decoder;

class HevcDecoderUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class HardwareDecodeUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Construct, use and destroy on the same worker thread.
class Decoder {
public:
    using Output = std::function<void(VideoFrame&&)>;

    Decoder(uint32_t width, uint32_t height, uint32_t fps, Codec codec = Codec::h264,
            bool require_hardware = false);
    ~Decoder();
    // True only after a decoded frame has arrived on a hardware decoder surface.
    bool hardware_accelerated() const;
    void decode(const std::vector<uint8_t>& access_unit, const Output& output);
    void reset(); // Drop queued samples before submitting a recovery keyframe.
    void finish(const Output& output);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<Av1Decoder> av1_;
};
