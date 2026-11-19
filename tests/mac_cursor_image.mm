#include "mac/cursor_image.hpp"
#import <AppKit/AppKit.h>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

NSBitmapImageRep* representation(unsigned width, unsigned height, uint8_t green) {
    // Straight RGBA: a translucent red/green top half and an opaque blue bottom half.
    cursor::Bytes pixels(width * height * 4);
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            auto* p = pixels.data() + (y * width + x) * 4;
            const bool top = y < height / 2;
            p[0] = top ? 255 : 0;
            p[1] = top ? green : 0;
            p[2] = top ? 0 : 255;
            p[3] = top ? 128 : 255;
        }
    }
    NSData* data = [NSData dataWithBytes:pixels.data() length:pixels.size()];
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
    CGColorSpaceRef rgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef cg = CGImageCreate(width, height, 8, 32, 4 * width, rgb, kCGImageAlphaLast,
                                  provider, nullptr, false, kCGRenderingIntentDefault);
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:cg];
    CGImageRelease(cg);
    CGColorSpaceRelease(rgb);
    CGDataProviderRelease(provider);
    rep.size = NSMakeSize(4, 8);
    return rep;
}

void raster_and_position() {
    NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(4, 8)];
    [image addRepresentation:representation(4, 8, 0)];
    [image addRepresentation:representation(8, 16, 255)];
    NSCursor* source = [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(1, 2)];
    for (int scale : {1, 2}) {
        auto bitmap = mac_cursor::rasterize(source, scale);
        require(bitmap && bitmap->width == 4 * scale && bitmap->height == 8 * scale, "Wrong raster size");
        require(bitmap->hot_x == scale && bitmap->hot_y == 2 * scale, "Hotspot was flipped or scaled incorrectly");
        const uint8_t* top = bitmap->pixels.data() + 4;
        const uint8_t* bottom = bitmap->pixels.data() + bitmap->pixels.size() - 4;
        require(top[0] < 3 && abs(top[2] - 128) <= 2 && top[3] == 128,
                "Wrong alpha, pixel order, or orientation at top");
        require(abs(top[1] - (scale == 1 ? 0 : 128)) <= 2, "Wrong source resolution");
        require(bottom[0] >= 253 && bottom[1] < 3 && bottom[2] < 3 && bottom[3] == 255, "Wrong bottom row");
    }
    auto capped = mac_cursor::rasterize(source, 80);
    require(capped && capped->width == 128 && capped->height == 256 && capped->hot_x == 32 &&
            capped->hot_y == 64, "Large cursor did not preserve its aspect ratio and hotspot");
    require(!mac_cursor::rasterize(nil, 1), "Missing system cursor returned a bitmap");

    auto shape = mac_cursor::rasterize(source, 2);
    const auto bounds = CGRectMake(-1000, 40, 320, 200);
    auto state = [&](double x, double y, bool visible = true) {
        return mac_cursor::position(CGPointMake(x, y), bounds, 640, 400, *shape, visible);
    };
    auto partially_visible = state(-1000.25, 41.5);
    require(partially_visible.visible && partially_visible.x == -4 && partially_visible.y == 24,
            "Lost fractional or negative hotspot at the left edge");
    require(state(-680, 41.5).visible && !state(-679, 41.5).visible, "Incorrect right-edge clipping");
    require(!state(-1000, 41.5, false).visible && !state(-1004, 41.5).visible,
            "Hidden or offscreen cursor visible");
}
}

int main() {
    @autoreleasepool {
        try {
            raster_and_position();
        } catch (const std::exception& ex) {
            std::cerr << ex.what() << '\n';
            return 1;
        }
    }
}
