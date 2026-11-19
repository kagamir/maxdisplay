#pragma once

#include "video_frame.hpp"

#include <functional>
#include <memory>
#include <vector>

class HevcSoftwareDecoder {
public:
    using Output = std::function<void(VideoFrame&&)>;
    HevcSoftwareDecoder(uint32_t width, uint32_t height);
    ~HevcSoftwareDecoder();
    void decode(const std::vector<uint8_t>& access_unit, const Output& output);
    void finish(const Output& output);
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
