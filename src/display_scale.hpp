#pragma once

#include "app.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

// All supported codecs use 4:2:0 frames. Accept a size only when the requested product
// gives whole, even pixel dimensions; fractional input still scales exactly.
inline void set_display_size(SenderOptions& options, std::uint32_t base_width,
                             std::uint32_t base_height, double scale) {
    if (!std::isfinite(scale) || scale <= 0)
        throw std::invalid_argument("--scale must be finite and positive");
    auto pixels = [scale](std::uint32_t base) {
        const double product = base * scale;
        const double nearest = std::round(product);
        // Permit floating-point error in an integer product, such as 100 * 0.58.
        const double tolerance = 2 * std::numeric_limits<double>::epsilon() * nearest;
        if (!std::isfinite(product) || nearest < 1 ||
            nearest > std::numeric_limits<std::uint32_t>::max() ||
            std::abs(product - nearest) > tolerance || static_cast<std::uint32_t>(nearest) % 2)
            throw std::invalid_argument("--size times --scale must give whole, even pixel dimensions");
        return static_cast<std::uint32_t>(nearest);
    };
    const auto width = pixels(base_width);
    const auto height = pixels(base_height);
    options.width = width;
    options.height = height;
    options.scale = scale;
}
