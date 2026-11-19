#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

struct VideoFrame {
    uint32_t width;
    uint32_t height;
    std::vector<uint8_t> bgra; // Top-down, tightly packed pixels.
};

// Construct, use and destroy on the same worker thread.
class Decoder {
public:
    using Output = std::function<void(VideoFrame&&)>;

    Decoder(uint32_t width, uint32_t height, uint32_t fps);
    ~Decoder();
    void decode(const std::vector<uint8_t>& access_unit, const Output& output);
    void finish(const Output& output);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
