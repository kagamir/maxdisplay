#include "waiting_screen.hpp"

#include <algorithm>
#include <cmath>

namespace win_waiting {

void paint(HDC dc, const RECT& client, const std::wstring& text, UINT dpi, std::uint64_t elapsed_ms) {
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    const int width = client.right - client.left, height = client.bottom - client.top;
    if (width <= 0 || height <= 0 || text.empty()) return;

    if (!dpi) dpi = 96;
    const int margin = MulDiv(16, dpi, 96);
    const int inset_x = (std::min)(margin, width / 12), inset_y = (std::min)(margin, height / 12);
    const int wrap_width = width - 2 * inset_x, track_height = height - 2 * inset_y;
    // Preserve some room for motion even when a large set of addresses needs a smaller font.
    const int max_height = track_height - (std::min)(margin, track_height / 8);
    const int saved = SaveDC(dc);
    if (!saved) return;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(224, 224, 224));
    constexpr UINT flags = DT_CENTER | DT_WORDBREAK | DT_NOPREFIX;
    int font_px = (std::max)(1, MulDiv(13, dpi, 72));
    HFONT font = nullptr;
    RECT block{};
    for (;;) {
        font = CreateFontW(-font_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
        const auto old = SelectObject(dc, font ? static_cast<HGDIOBJ>(font) : GetStockObject(DEFAULT_GUI_FONT));
        block = {0, 0, wrap_width, 0};
        DrawTextW(dc, text.c_str(), -1, &block, flags | DT_CALCRECT);
        if ((block.right <= wrap_width && block.bottom <= max_height) || !font || font_px == 1) break;
        // Reducing type size can reduce both the line height and the number of wrapped lines.
        const double ratio = (std::min)(double(wrap_width) / block.right,
                                        std::sqrt(double(max_height) / block.bottom));
        font_px = (std::max)(1, (std::min)(font_px - 1, static_cast<int>(font_px * ratio)));
        SelectObject(dc, old);
        DeleteObject(font);
    }

    if (block.right > 0 && block.bottom > 0) {
        // Only extremely small clients can still overflow with the smallest available GDI font.
        const double scale = (std::min)({1.0, double(wrap_width) / block.right, double(max_height) / block.bottom});
        const int travel = (std::max)(0, track_height - static_cast<int>(std::ceil(block.bottom * scale)));
        // Four logical pixels per second, reflected at both ends. Start in the middle.
        const double phase = travel > 0 ? std::fmod(travel * 0.5 + elapsed_ms * double(dpi) / 24000.0, 2.0 * travel) : 0;
        const int y = client.top + inset_y + static_cast<int>(std::round(travel - std::abs(travel - phase)));
        const int x = client.left + (width - static_cast<int>(std::ceil(block.right * scale))) / 2;
        if (scale == 1) {
            OffsetRect(&block, x, y);
        } else {
            SetGraphicsMode(dc, GM_ADVANCED);
            const auto factor = static_cast<FLOAT>(scale);
            const XFORM transform{factor, 0, 0, factor, static_cast<FLOAT>(x), static_cast<FLOAT>(y)};
            SetWorldTransform(dc, &transform);
        }
        DrawTextW(dc, text.c_str(), -1, &block, flags);
    }
    RestoreDC(dc, saved);
    if (font) DeleteObject(font);
}

} // namespace win_waiting
