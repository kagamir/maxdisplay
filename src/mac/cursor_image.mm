#include "cursor_image.hpp"

#import <AppKit/AppKit.h>
#include <algorithm>
#include <cmath>
#include <dlfcn.h>
#include <limits>

namespace {
constexpr unsigned max_dimension = 256;

template<typename F>
F find_symbol(const char* name) {
    return reinterpret_cast<F>(dlsym(RTLD_DEFAULT, name));
}

std::optional<int> cursor_seed() {
    static const auto seed = find_symbol<int (*)()>("CGSCurrentCursorSeed");
    return seed ? std::optional<int>(seed()) : std::nullopt;
}

CGSize pixel_size(CGSize points, double scale) {
    if (!std::isfinite(points.width) || !std::isfinite(points.height) ||
        !std::isfinite(scale) || points.width <= 0 || points.height <= 0 || scale <= 0)
        return CGSizeZero;
    scale = std::min(scale, max_dimension / std::max(points.width, points.height));
    return CGSizeMake(std::max(1.0, std::round(points.width * scale)),
                      std::max(1.0, std::round(points.height * scale)));
}

cursor::Image draw(CGImageRef source, CGSize points, CGSize size, CGPoint hotspot) {
    if (!source || size.width <= 0 || size.height <= 0 ||
        !std::isfinite(hotspot.x) || !std::isfinite(hotspot.y)) return {};
    auto bitmap = std::make_shared<cursor::Bitmap>();
    bitmap->width = static_cast<uint16_t>(size.width);
    bitmap->height = static_cast<uint16_t>(size.height);
    auto scaled_hotspot = [](double hot, double extent, double pixels) {
        return static_cast<int16_t>(std::clamp(std::round(hot / extent * pixels),
            double(std::numeric_limits<int16_t>::min()), double(std::numeric_limits<int16_t>::max())));
    };
    bitmap->hot_x = scaled_hotspot(hotspot.x, points.width, size.width);
    bitmap->hot_y = scaled_hotspot(hotspot.y, points.height, size.height);
    bitmap->pixels.resize(size_t(bitmap->width) * bitmap->height * 4);
    CGColorSpaceRef rgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (!rgb) return {};
    const auto format = CGBitmapInfo(kCGBitmapByteOrder32Little) | kCGImageAlphaPremultipliedFirst;
    CGContextRef ctx = CGBitmapContextCreate(bitmap->pixels.data(), bitmap->width, bitmap->height,
                                             8, bitmap->width * 4, rgb, format);
    CGColorSpaceRelease(rgb);
    if (!ctx) return {};
    // An unflipped Quartz bitmap context already stores the top scanline first.
    CGContextSetBlendMode(ctx, kCGBlendModeCopy);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    CGContextDrawImage(ctx, CGRectMake(0, 0, size.width, size.height), source);
    CGContextRelease(ctx);
    return bitmap;
}

cursor::Image global_cursor(double scale) {
    // SkyLight only supplies a 1x RGBA bitmap on this path. Resolve all three
    // functions at runtime so a missing private API cannot prevent launch.
    using ReadData = CGError (*)(int, unsigned char*, int*, int*, CGRect*, CGPoint*, int*, int*, int*);
    static const auto connection = find_symbol<int (*)()>("CGSMainConnectionID");
    static const auto data_size = find_symbol<CGError (*)(int, int*)>("CGSGetGlobalCursorDataSize");
    static const auto read_data = find_symbol<ReadData>("CGSGetGlobalCursorData");
    if (!connection || !data_size || !read_data) return {};
    int cid = connection(), length = 0;
    constexpr int max_input = 4 * 1024 * 1024;
    if (data_size(cid, &length) != kCGErrorSuccess || length <= 0 || length > max_input) return {};
    std::vector<uint8_t> data(length);
    int stride = 0, depth = 0, components = 0, bits = 0;
    CGRect rect{};
    CGPoint hotspot{};
    if (read_data(cid, data.data(), &length, &stride, &rect, &hotspot, &depth, &components, &bits) != kCGErrorSuccess ||
        length <= 0 || size_t(length) > data.size() || stride <= 0 || bits != 8 || (depth != 32 && depth != 24) ||
        (components != 3 && components != 4) || !(rect.size.width >= 1 && rect.size.width <= stride / 4) ||
        !(rect.size.height >= 1 && rect.size.height <= length / stride)) return {};
    const CGSize size = pixel_size(rect.size, scale);
    CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, data.data(), length, nullptr);
    if (!provider) return {};
    CGColorSpaceRef rgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef image = rgb ? CGImageCreate(static_cast<size_t>(rect.size.width), static_cast<size_t>(rect.size.height),
                                          8, 32, stride, rgb, kCGImageAlphaPremultipliedLast, provider,
                                          nullptr, true, kCGRenderingIntentDefault) : nullptr;
    if (rgb) CGColorSpaceRelease(rgb);
    CGDataProviderRelease(provider);
    auto result = draw(image, rect.size, size, hotspot);
    if (image) CGImageRelease(image);
    return result;
}
} // namespace

cursor::Image mac_cursor::rasterize(NSCursor* source, double scale) {
    NSImage* image = source.image;
    const CGSize points = image.size;
    const CGSize size = pixel_size(points, scale);
    if (size.width == 0) return {};
    // Select the smallest native representation that covers the output, or
    // the largest available if all would need upsampling.
    NSBitmapImageRep* best = nil;
    double best_ratio = 0;
    for (NSImageRep* rep in image.representations) {
        if (![rep isKindOfClass:NSBitmapImageRep.class]) continue;
        const double ratio = std::min(rep.pixelsWide / size.width, rep.pixelsHigh / size.height);
        if (ratio > 0 && ((best_ratio < 1 && ratio > best_ratio) ||
                          (ratio >= 1 && ratio < best_ratio))) {
            best = (NSBitmapImageRep*)rep;
            best_ratio = ratio;
        }
    }
    NSRect proposed = CGRectMake(0, 0, size.width, size.height);
    CGImageRef cg_image = best.CGImage;
    if (!cg_image) cg_image = [image CGImageForProposedRect:&proposed context:nil hints:nil];
    return draw(cg_image, points, size, source.hotSpot);
}

cursor::State mac_cursor::position(CGPoint location, CGRect display, unsigned width, unsigned height,
                                   const cursor::Bitmap& bitmap, bool visible) {
    if (!visible || CGRectIsEmpty(display)) return {};
    const double x = (location.x - display.origin.x) * width / display.size.width;
    const double y = (location.y - display.origin.y) * height / display.size.height;
    const double left = x - bitmap.hot_x, top = y - bitmap.hot_y;
    if (!(left < width && left + bitmap.width > 0 && top < height && top + bitmap.height > 0)) return {};
    return {true, static_cast<int32_t>(std::round(x * 8)), static_cast<int32_t>(std::round(y * 8))};
}

CursorImageSource::CursorImageSource(CGDirectDisplayID display, unsigned stream_width, unsigned stream_height)
    : display_(display), width_(stream_width), height_(stream_height) {}

cursor::State CursorImageSource::sample(uint64_t now_ms) {
    available_ = false;
    const CGRect bounds = CGDisplayBounds(display_);
    if (CGRectIsEmpty(bounds)) return {};
    const double scale = width_ / bounds.size.width;
    const auto seed = cursor_seed();
    if (!image_ || scale != sampled_scale_ || seed != last_seed_ || (!seed && now_ms - sampled_at_ >= 50)) {
        // A future AppKit may return nil for the global cursor. System and
        // fallback arrows both use the same rasterizer and hotspot conversion.
        auto next = mac_cursor::rasterize(NSCursor.currentSystemCursor, scale);
        if (!next) next = global_cursor(scale);
        if (!next) next = mac_cursor::rasterize(NSCursor.arrowCursor, scale);
        if (!next) {
            image_.reset();
            return {};
        }
        const auto id = cursor::shape_id(*next);
        if (!image_ || image_id_ != id) {
            image_ = std::move(next);
            image_id_ = id;
        }
        last_seed_ = seed;
        sampled_at_ = now_ms;
        sampled_scale_ = scale;
    }
    CGEventRef event = CGEventCreate(nullptr);
    if (!event) return {};
    const CGPoint location = CGEventGetLocation(event);
    CFRelease(event);
    available_ = std::isfinite(location.x) && std::isfinite(location.y);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const bool visible = CGCursorIsVisible();
#pragma clang diagnostic pop
    return mac_cursor::position(location, bounds, width_, height_, *image_, available_ && visible);
}
