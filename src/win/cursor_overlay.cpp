#include "cursor_overlay.hpp"

#include <algorithm>
#include <cstdint>

namespace win_cursor {
namespace {
using I64 = std::int64_t;

struct Placement {
    I64 left{}, top{}, width{}, height{};
    RECT clip{};
};

I64 rounded(I64 n, I64 d) { return (n < 0 ? n - d / 2 : n + d / 2) / d; }

Placement place(const RECT& video, const VideoFrame& frame, const cursor::View& view) {
    Placement p{};
    if (!view.state.visible || !view.image || IsRectEmpty(&video)) return p;
    const auto& image = *view.image;
    const I64 w = video.right - video.left, h = video.bottom - video.top;
    p.width = std::max<I64>(1, rounded(w * image.width, frame.width));
    p.height = std::max<I64>(1, rounded(h * image.height, frame.height));
    p.left = video.left + rounded((I64(view.state.x) - 8 * image.hot_x) * w, 8 * I64(frame.width));
    p.top = video.top + rounded((I64(view.state.y) - 8 * image.hot_y) * h, 8 * I64(frame.height));
    // A hotspot may be offscreen. Clip in wide arithmetic before converting to GDI coordinates.
    const I64 left = std::max<I64>(video.left, p.left), right = std::min<I64>(video.right, p.left + p.width);
    const I64 top = std::max<I64>(video.top, p.top), bottom = std::min<I64>(video.bottom, p.top + p.height);
    if (left < right && top < bottom)
        p.clip = {static_cast<LONG>(left), static_cast<LONG>(top), static_cast<LONG>(right), static_cast<LONG>(bottom)};
    return p;
}

BITMAPINFO bitmap_info(int width, int height) {
    BITMAPINFO bitmap{};
    auto& h = bitmap.bmiHeader;
    h.biSize = sizeof(h);
    h.biWidth = width;
    h.biHeight = -height;
    h.biPlanes = 1;
    h.biBitCount = 32;
    h.biCompression = BI_RGB;
    return bitmap;
}

bool draw_video(HDC dc, const RECT& dest, const VideoFrame& frame, int offset_x = 0, int offset_y = 0) {
    const auto info = bitmap_info(frame.width, frame.height);
    SetStretchBltMode(dc, COLORONCOLOR);
    return StretchDIBits(dc, dest.left - offset_x, dest.top - offset_y, dest.right - dest.left,
                         dest.bottom - dest.top, 0, 0, frame.width, frame.height, frame.bgra.data(),
                         &info, DIB_RGB_COLORS, SRCCOPY) > 0;
}
} // namespace

RECT fit_video(const RECT& client, const VideoFrame& frame) {
    const int w = client.right - client.left, h = client.bottom - client.top;
    if (w <= 0 || h <= 0) return {};
    const auto dw = std::clamp<I64>(I64(h) * frame.width / frame.height, 1, w);
    const auto dh = std::clamp<I64>(I64(w) * frame.height / frame.width, 1, h);
    const LONG x = client.left + static_cast<LONG>((w - dw) / 2), y = client.top + static_cast<LONG>((h - dh) / 2);
    return {x, y, static_cast<LONG>(x + dw), static_cast<LONG>(y + dh)};
}

bool Overlay::eligible(const cursor::State& state, std::uint32_t sequence, std::uint64_t session) {
    if (session_ != session) {
        session_ = session;
        reached_floor_.reset();
        forget_shape();
    }
    // Once a surface has crossed the transition, later cursor movements remain
    // independent of video, including streams that run for another 2^31 frames.
    if (!state.video_floor || state.video_floor == reached_floor_) return true;
    if (std::uint32_t(sequence - *state.video_floor) >= 0x80000000u) return false;
    reached_floor_ = state.video_floor;
    return true;
}

RECT Overlay::bounds(const RECT& video, const VideoFrame& frame, const cursor::View& view,
                     std::uint32_t sequence, std::uint64_t session) {
    return eligible(view.state, sequence, session) ? place(video, frame, view).clip : RECT{};
}

Overlay::~Overlay() {
    if (original_) SelectObject(memory_, original_);
    if (dib_) DeleteObject(dib_);
    if (memory_) DeleteDC(memory_);
}

void Overlay::forget_shape() {
    cached_.reset();
    scaled_.clear();
}

bool Overlay::prepare_dib(HDC dc, int w, int h) {
    if (dib_ && w == dib_width_ && h == dib_height_) return true;
    if (!memory_) memory_ = CreateCompatibleDC(dc);
    if (!memory_) return false;
    const auto info = bitmap_info(w, h);
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) return false;
    const auto old = SelectObject(memory_, bitmap);
    if (!old || old == HGDI_ERROR) {
        DeleteObject(bitmap);
        return false;
    }
    if (dib_) DeleteObject(dib_);
    else original_ = old;
    dib_ = bitmap;
    dib_width_ = w;
    dib_height_ = h;
    pixels_ = static_cast<std::uint8_t*>(bits);
    return true;
}

void Overlay::scale_shape(const cursor::Image& image, I64 w, I64 h) {
    if (cached_ == image && scaled_width_ == w && scaled_height_ == h) return;
    cached_ = image;
    scaled_width_ = w;
    scaled_height_ = h;
    scaled_.clear();
    // A tiny negotiated video can magnify one cursor past the whole screen.
    // Sample just its clipped portion below instead of allocating the entire scaled image.
    if ((image->width == w && image->height == h) || w > 4'194'304 / h) return;
    scaled_.resize(static_cast<std::size_t>(4 * w * h));
    for (I64 y = 0; y < h; ++y) {
        const auto* row = image->pixels.data() + (y * image->height / h) * image->width * 4;
        auto* dest = scaled_.data() + y * w * 4;
        for (I64 x = 0; x < w; ++x)
            std::copy_n(row + (x * image->width / w) * 4, 4, dest + x * 4);
    }
}

bool Overlay::paint(HDC dc, const RECT& video, const VideoFrame& frame, const cursor::View& view,
                    std::uint32_t sequence, std::uint64_t session) {
    if (!eligible(view.state, sequence, session)) return draw_video(dc, video, frame);
    const auto p = place(video, frame, view);
    const auto& clip = p.clip;
    const int w = clip.right - clip.left, h = clip.bottom - clip.top;
    if (!w || !h || !RectVisible(dc, &clip) || !prepare_dib(dc, w, h))
        return draw_video(dc, video, frame);
    // Shift the full video transform by the patch origin to preserve GDI's sampling phase.
    if (!draw_video(memory_, video, frame, clip.left, clip.top)) return draw_video(dc, video, frame);
    GdiFlush(); // Complete the GDI write before reading the same DIB bits on the CPU.
    scale_shape(view.image, p.width, p.height);
    const auto& image = *view.image;
    for (int y = 0; y < h; ++y) {
        const I64 dest_y = clip.top - p.top + y;
        const I64 first_x = clip.left - p.left;
        const bool native = image.width == p.width && image.height == p.height;
        const auto* row = &image.pixels[(dest_y * image.height / p.height) * image.width * 4];
        const auto* direct = scaled_.empty() ? (native ? row + first_x * 4 : nullptr)
                                              : &scaled_[(dest_y * p.width + first_x) * 4];
        auto* pixel = pixels_ + static_cast<std::size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x, pixel += 4) {
            const auto* src = direct ? direct + x * 4 : row + ((first_x + x) * image.width / p.width) * 4;
            const unsigned inverse_alpha = 255u - src[3];
            for (int c = 0; c < 3; ++c)
                pixel[c] = static_cast<std::uint8_t>(std::min(255u, src[c] + (pixel[c] * inverse_alpha + 127) / 255));
        }
    }
    const int saved = SaveDC(dc);
    if (saved) ExcludeClipRect(dc, clip.left, clip.top, clip.right, clip.bottom);
    const bool drew = RectVisible(dc, &video) && draw_video(dc, video, frame);
    if (saved) RestoreDC(dc, saved);
    if (BitBlt(dc, clip.left, clip.top, w, h, memory_, 0, 0, SRCCOPY)) return true;
    return draw_video(dc, video, frame) || drew;
}

} // namespace win_cursor
