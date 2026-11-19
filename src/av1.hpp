#pragma once

#include "video_frame.hpp"
#include <functional>
#include <memory>

// Low-delay AV1 Main Profile. A wire access unit carries one temporal unit of
// low-overhead OBUs, as produced and consumed directly by libaom.
class Av1Encoder {
public:
    using Output = std::function<void(std::vector<uint8_t>, bool keyframe)>;
    Av1Encoder(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate,
               bool periodic_keyframes = true);
    ~Av1Encoder();

    // PTS is in ticks of the configured (maximum) frame rate; current_fps is
    // its active divisor. Zero uses the configured rate.
    bool encode_nv12(uint8_t* y, size_t y_stride, uint8_t* uv, size_t uv_stride,
                     int64_t pts, const Output& output, bool force_keyframe = false,
                     uint32_t current_fps = 0);
    int last_quantizer() const;
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
