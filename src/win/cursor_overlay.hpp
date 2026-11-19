#pragma once

#include "cursor_link.hpp"
#include "video_frame.hpp"

#include <windows.h>

namespace win_cursor {

RECT fit_video(const RECT& client, const VideoFrame& frame);

// Owned by the UI thread. The memory DIB holds only the visible cursor patch.
class Overlay {
public:
    ~Overlay();
    Overlay() = default;
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;
    RECT bounds(const RECT& video, const VideoFrame& frame, const cursor::View& view,
                std::uint32_t frame_sequence, std::uint64_t session);
    bool paint(HDC dc, const RECT& video, const VideoFrame& frame, const cursor::View& view,
               std::uint32_t frame_sequence, std::uint64_t session);
    void forget_shape();

private:
    bool eligible(const cursor::State& state, std::uint32_t frame_sequence, std::uint64_t session);
    std::uint64_t session_ = 0;
    std::optional<std::uint32_t> reached_floor_;
    bool prepare_dib(HDC dc, int width, int height);
    void scale_shape(const cursor::Image& image, std::int64_t width, std::int64_t height);
    HDC memory_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ original_ = nullptr;
    std::uint8_t* pixels_ = nullptr;
    int dib_width_ = 0, dib_height_ = 0;
    cursor::Image cached_;
    std::int64_t scaled_width_ = 0, scaled_height_ = 0;
    cursor::Bytes scaled_;
};

} // namespace win_cursor
