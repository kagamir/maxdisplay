#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

struct VideoFrame {
    uint32_t width;
    uint32_t height;
    std::vector<uint8_t> bgra; // Top-down, tightly packed pixels.
};

struct YuvMatrix {
    int y_offset, y_scale, r_v, g_u, g_v, b_u;
};

constexpr YuvMatrix bt709_video_range{16, 298, 459, -55, -136, 541};

// For NV12, U and V point to alternating bytes (chroma_step == 2).
// For I420, they point to distinct planes (chroma_step == 1).
inline VideoFrame convert_yuv420(uint32_t width, uint32_t height, const YuvMatrix& matrix,
                                 const uint8_t* y_plane, size_t y_stride,
                                 const uint8_t* u_plane, size_t u_stride,
                                 const uint8_t* v_plane, size_t v_stride, unsigned chroma_step) {
    VideoFrame frame{width, height, std::vector<uint8_t>(static_cast<size_t>(width) * height * 4)};
    auto channel = [](int value) { return static_cast<uint8_t>(std::clamp((value + 128) >> 8, 0, 255)); };
    for (uint32_t y = 0; y < height; ++y) {
        const auto* luma = y_plane + y * y_stride;
        const auto* u_row = u_plane + (y / 2) * u_stride;
        const auto* v_row = v_plane + (y / 2) * v_stride;
        auto* pixel = frame.bgra.data() + static_cast<size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; x += 2) {
            const auto c = (x / 2) * chroma_step;
            const int u = u_row[c] - 128, v = v_row[c] - 128;
            const int blue = matrix.b_u * u, green = matrix.g_u * u + matrix.g_v * v;
            const int red = matrix.r_v * v;
            for (uint32_t i = 0; i < 2; ++i, pixel += 4) {
                const int luminance = matrix.y_scale * (luma[x + i] - matrix.y_offset);
                pixel[0] = channel(luminance + blue);
                pixel[1] = channel(luminance + green);
                pixel[2] = channel(luminance + red);
                pixel[3] = 255;
            }
        }
    }
    return frame;
}
