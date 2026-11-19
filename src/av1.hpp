#pragma once

#include "video_frame.hpp"
#include <functional>
#include <memory>

// Low-delay AV1 Main Profile. A wire access unit carries one temporal unit of
// low-overhead OBUs, as produced and consumed directly by libaom.
class Av1Encoder {
public:
    using Output = std::function<void(std::vector<uint8_t>, bool keyframe)>;
    Av1Encoder(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate);
    ~Av1Encoder();

    void encode_nv12(uint8_t* y, size_t y_stride, uint8_t* uv, size_t uv_stride,
                     int64_t pts, const Output& output, bool force_keyframe = false);
    void set_bitrate(uint32_t bps);
    void finish(const Output& output);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Av1Decoder {
public:
    using Output = std::function<void(VideoFrame&&)>;
    Av1Decoder(uint32_t width, uint32_t height);
    ~Av1Decoder();

    bool can_start_with(const std::vector<uint8_t>& access_unit) const;
    void decode(const std::vector<uint8_t>& access_unit, const Output& output);
    void reset();
    void finish(const Output& output);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
