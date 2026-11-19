#pragma once

#include "app.hpp"
#include "wire.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

struct DisplayGeometry {
    std::uint32_t logical_width, logical_height; // Mac desktop points
    std::uint32_t stream_width, stream_height;   // Encoded video pixels
    std::uint32_t fps;
};

// For an explicit desktop size, retain exact scaling and the codec's 4:2:0 layout.
inline std::uint32_t scaled_video_dimension(std::uint32_t logical, double scale) {
    const double product = logical * scale;
    const double nearest = std::round(product);
    const double tolerance = 2 * std::numeric_limits<double>::epsilon() * nearest;
    if (!std::isfinite(product) || nearest < 1 || nearest > std::numeric_limits<std::uint32_t>::max() ||
        std::abs(product - nearest) > tolerance || static_cast<std::uint32_t>(nearest) % 2)
        throw std::invalid_argument("--size times --scale must give whole, even video dimensions");
    return static_cast<std::uint32_t>(nearest);
}

inline DisplayGeometry resolve_display_geometry(const SenderOptions& options, std::uint32_t monitor_width = 0,
                                                std::uint32_t monitor_height = 0) {
    if (!std::isfinite(options.scale) || options.scale <= 0)
        throw std::invalid_argument("--scale must be finite and positive");
    if (!options.fps || options.fps > 120)
        throw std::invalid_argument("--fps must be between 1 and 120");
    DisplayGeometry mode{options.width, options.height, 0, 0, options.fps};
    // A source mode also needs to fit in a WindowServer display surface. Check it
    // separately from the video output, especially for very small downscales.
    constexpr std::uint32_t max_desktop_dimension = 16384;
    if (!options.width && !options.height) {
        if (!monitor_width || !monitor_height)
            throw std::invalid_argument("Automatic --size needs the Windows monitor resolution");
        auto points = [&](std::uint32_t pixels) {
            const double rounded = std::round(pixels / options.scale);
            if (!(rounded >= 1 && rounded <= max_desktop_dimension))
                throw std::invalid_argument("Windows monitor divided by --scale exceeds the supported desktop range");
            return static_cast<std::uint32_t>(rounded);
        };
        mode.logical_width = points(monitor_width);
        mode.logical_height = points(monitor_height);
        // Keep the peer's pixel target independent of rounding the Mac layout.
        mode.stream_width = monitor_width & ~1u;
        mode.stream_height = monitor_height & ~1u;
    } else {
        if (!options.width || !options.height || options.width > max_desktop_dimension ||
            options.height > max_desktop_dimension)
            throw std::invalid_argument("--size dimensions must be between 1 and 16384 points");
        mode.stream_width = scaled_video_dimension(options.width, options.scale);
        mode.stream_height = scaled_video_dimension(options.height, options.scale);
    }
    if (options.adaptive_fps) {
        while (mode.fps > 1 && !wire::valid_mode(mode.stream_width, mode.stream_height, mode.fps, options.codec))
            --mode.fps;
    }
    if (!wire::valid_mode(mode.stream_width, mode.stream_height, mode.fps, options.codec))
        throw std::invalid_argument("Unsupported " + std::string(codec_name(options.codec)) + " video mode " +
                                    std::to_string(mode.stream_width) + "x" + std::to_string(mode.stream_height) +
                                    " at " + std::to_string(options.fps) +
                                    " fps; choose a smaller --size, a lower --fps or another codec");
    return mode;
}
