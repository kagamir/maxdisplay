#pragma once

#import <CoreGraphics/CoreGraphics.h>
#include <cstdint>

@class MDDisplay;

// The strong member keeps WindowServer's virtual display registered for this
// object's lifetime, including while the capture loop holds only its numeric ID.
class VirtualDisplay {
public:
    VirtualDisplay(std::uint32_t width, std::uint32_t height, std::uint32_t fps);
    VirtualDisplay(const VirtualDisplay&) = delete;
    VirtualDisplay& operator=(const VirtualDisplay&) = delete;

    CGDirectDisplayID id() const;

private:
    MDDisplay* __strong display_;
};
