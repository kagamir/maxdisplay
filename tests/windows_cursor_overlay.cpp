// Check the clipped background sampling phase on a high-frequency image at fractional zoom.
#include "win/cursor_overlay.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

namespace {
constexpr int width = 30, height = 17;
constexpr RECT client{0, 0, width, height};

struct Surface {
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP dib = nullptr;
    HGDIOBJ original = nullptr;
    std::uint8_t* pixels = nullptr;
    Surface() {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dc || !dib || !(original = SelectObject(dc, dib)))
            throw std::runtime_error("Could not create test surface");
        pixels = static_cast<std::uint8_t*>(bits);
        std::fill_n(pixels, width * height * 4, 0);
    }
    ~Surface() {
        SelectObject(dc, original);
        DeleteObject(dib);
        DeleteDC(dc);
    }
};

void check() {
    Surface surface;
    VideoFrame frame{10, 8, {}};
    for (unsigned i = 0; i < frame.width * frame.height; ++i)
        for (unsigned c : {13, 41, 87, 0})
            frame.bgra.push_back(c ? static_cast<std::uint8_t>(i * c) : 255);
    auto image = std::make_shared<cursor::Bitmap>();
    image->width = 4;
    image->height = 3;
    image->hot_x = image->hot_y = 1;
    const std::array<std::uint8_t, 4> tint{64, 32, 16, 128};
    for (unsigned i = 0; i < image->width * image->height; ++i)
        image->pixels.insert(image->pixels.end(), tint.begin(), tint.end());

    win_cursor::Overlay overlay;
    const auto video = win_cursor::fit_video(client, frame); // 21x17 at (4, 0)
    overlay.paint(surface.dc, video, frame, {}, 0xffffffffu, 1);
    GdiFlush();
    const cursor::Bytes background(surface.pixels, surface.pixels + width * height * 4);

    cursor::View view{{true, -4, 24}, image};
    const RECT expected{4, 4, 9, 10}; // The hotspot is half a source pixel left of the video.
    const RECT visible = overlay.bounds(video, frame, view, 0xffffffffu, 1);
    if (!EqualRect(&expected, &visible)) throw std::runtime_error("Incorrect clipped hotspot bounds");

    view.state.video_floor = 0u;
    const RECT before = overlay.bounds(video, frame, view, 0xffffffffu, 1);
    if (!IsRectEmpty(&before)) throw std::runtime_error("Premature cursor damage rectangle");
    overlay.paint(surface.dc, video, frame, view, 0xffffffffu, 1);
    GdiFlush();
    if (!std::equal(background.begin(), background.end(), surface.pixels))
        throw std::runtime_error("UDP cursor was painted over the previous video surface");

    // Invalidate just part of the cursor: the rest of the surface must stay untouched.
    const RECT dirty{3, 6, 8, 12};
    const int saved = SaveDC(surface.dc);
    IntersectClipRect(surface.dc, dirty.left, dirty.top, dirty.right, dirty.bottom);
    overlay.paint(surface.dc, video, frame, view, 0u, 1);
    RestoreDC(surface.dc, saved);
    GdiFlush();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const POINT point{x, y};
            const bool covered = PtInRect(&dirty, point) && PtInRect(&visible, point);
            for (int c = 0; c < 3; ++c) {
                const int i = (y * width + x) * 4 + c;
                const auto want = covered ? tint[c] + (background[i] * 127u + 127) / 255 : background[i];
                if (surface.pixels[i] != want)
                    throw std::runtime_error("Cursor patch shifted video samples, leaked past the clip or blended incorrectly");
            }
        }
    }
    const RECT long_stream = overlay.bounds(video, frame, view, 0x80000000u, 1);
    if (!EqualRect(&long_stream, &visible)) throw std::runtime_error("Satisfied cursor floor expired");
    const auto reconnected = overlay.bounds(video, frame, view, 0xffffffffu, 2);
    if (!IsRectEmpty(&reconnected)) throw std::runtime_error("Cursor floor survived reconnect");
    view.state.video_floor = 100u;
    const auto changed = overlay.bounds(video, frame, view, 99u, 2);
    if (!IsRectEmpty(&changed)) throw std::runtime_error("New cursor transition used an old video surface");
}
}

int main() {
    try {
        check();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
