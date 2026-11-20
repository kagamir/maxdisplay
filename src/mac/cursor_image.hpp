#pragma once

#include "cursor_link.hpp"

#include <CoreGraphics/CoreGraphics.h>
#include <cstdint>
#include <optional>

#ifdef __OBJC__
@class NSCursor;
namespace mac_cursor {
cursor::Image rasterize(NSCursor* source, double scale);
}
#endif

namespace mac_cursor {
cursor::State position(CGPoint location, CGRect display, unsigned width, unsigned height,
                       const cursor::Bitmap& bitmap, bool visible);
}

// Used exclusively by the pointer sampling thread.
class CursorImageSource {
public:
    CursorImageSource(CGDirectDisplayID display, unsigned stream_width, unsigned stream_height);
    cursor::State sample(std::uint64_t now_ms);
    const cursor::Image& image() const { return image_; }
    bool available() const { return available_; }

private:
    CGDirectDisplayID display_;
    unsigned width_, height_;
    cursor::Image image_;
    std::uint64_t image_id_ = 0;
    std::optional<int> last_seed_;
    std::uint64_t sampled_at_ = 0;
    double sampled_scale_ = 0;
    bool available_ = false;
};
