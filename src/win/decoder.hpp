#pragma once

#include "media.hpp"
#include "video_frame.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class Av1Decoder;

// Construct, use and destroy on the same worker thread.
class Decoder {
public:
    using Output = std::function<void(VideoFrame&&)>;

    Decoder(uint32_t width, uint32_t height, uint32_t fps, Codec codec = Codec::h264);
    ~Decoder();
    void decode(const std::vector<uint8_t>& access_unit, const Output& output);
    void finish(const Output& output);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<Av1Decoder> av1_;
};
