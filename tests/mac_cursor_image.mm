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

void rasterize_cursor() {
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
}

void display_boundary() {
    const auto bounds = CGRectMake(-1000, 40, 320, 200);
    auto state = [&](double x, double y, bool visible = true) {
        return mac_cursor::position(CGPointMake(x, y), bounds, 640, 400, visible);
    };
    require(cursor::same(state(-1000, 40), {true, 0, 0}) &&
            cursor::same(state(-680.25, 239.5), {true, 5116, 3192}) &&
            cursor::same(state(-680.001, 239.999), {true, 5119, 3199}),
            "On-display hotspots were hidden or mapped to the wrong video pixel");
    for (const auto point : {CGPointMake(-1000.25, 41.5), CGPointMake(-680, 41.5),
                             CGPointMake(-990, 39.75), CGPointMake(-990, 240), CGPointMake(0, 0)})
        require(cursor::same(state(point.x, point.y), {}), "Off-display hotspot revealed its position");
    require(cursor::same(state(-999, 41.5, false), {}), "Invisible on-display hotspot revealed its position");

    cursor::Sender sender(123);
    cursor::Receiver receiver(123);
    auto image = std::make_shared<cursor::Bitmap>();
    image->width = image->height = 8;
    image->hot_x = image->hot_y = 4;
    image->pixels.resize(8 * 8 * 4, 255);
    auto tick = [&](std::uint64_t now, double x, double y) {
        const auto sample = state(x, y);
        std::vector<cursor::Bytes> packets;
        sender.update(now, sample, sample.visible ? image : nullptr,
                      [&](const cursor::Bytes& bytes) { packets.push_back(bytes); });
        return packets;
    };
    auto deliver = [&](std::uint64_t now, const std::vector<cursor::Bytes>& packets) {
        for (const auto& packet : packets) {
            auto ack = receiver.receive(now, packet);
            require(ack.has_value(), "Receiver rejected a cursor packet");
            sender.acknowledge(now, *ack);
        }
    };
    auto hidden_packet = [](const std::vector<cursor::Bytes>& packets) {
        return packets.size() == 1 && packets.front().size() == 30 &&
               packets.front()[8] == cursor::detail::state_type && packets.front()[9] == 0 &&
               cursor::detail::get(packets.front(), 14, 8) == 0 && // x and y on the wire.
               cursor::detail::get(packets.front(), 22, 8) == 0;   // shape ID.
    };
    deliver(0, tick(0, -999, 45));
    require(receiver.view().state.visible, "Missing pointer before display exit");
    const auto hide = tick(4, -1000.25, 45); // Drop the first hide while the bitmap still overlaps the edge.
    require(hidden_packet(hide), "Display exit transmitted coordinates or failed to hide");
    const auto retry = tick(8, -1001, 48);
    require(retry == hide, "Outside movement changed the retransmitted hide");
    deliver(8, retry);
    require(!receiver.view().state.visible, "Receiver failed to hide after retry");
    require(tick(12, -1002, 60).empty(), "Outside movement triggered a cursor update");
    const auto heartbeat = tick(260, -1003, 75);
    require(hidden_packet(heartbeat), "Hidden heartbeat transmitted coordinates");
    deliver(260, heartbeat);
    deliver(264, tick(264, -999.5, 47));
    require(cursor::same(receiver.view().state, {true, 8, 112}), "Reentry did not restore the mapped hotspot");
}
}

int main() {
    @autoreleasepool {
        try {
            rasterize_cursor();
            display_boundary();
        } catch (const std::exception& ex) {
            std::cerr << ex.what() << '\n';
            return 1;
        }
    }
}
