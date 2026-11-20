#include "win/waiting_screen.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Surface {
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP dib{};
    HGDIOBJ previous{};
    std::uint32_t* pixels{};
    int width, height;

    Surface(int w, int h) : width(w), height(h) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* bits{};
        dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dc || !dib || !(previous = SelectObject(dc, dib)))
            throw std::runtime_error("Could not create waiting screen test surface");
        pixels = static_cast<std::uint32_t*>(bits);
    }
    ~Surface() {
        SelectObject(dc, previous);
        DeleteObject(dib);
        DeleteDC(dc);
    }
};

struct TextPixels {
    int top;
    std::vector<std::uint32_t> rows;
};

TextPixels render(Surface& s, const std::wstring& text, UINT dpi, unsigned ms) {
    win_waiting::paint(s.dc, {0, 0, s.width, s.height}, text, dpi, ms);
    GdiFlush();
    int top = s.height, bottom = -1;
    for (int y = 0; y < s.height; ++y) {
        for (int x = 0; x < s.width; ++x) {
            auto& p = s.pixels[y * s.width + x];
            p &= 0xffffff; // Ignore the unused high byte of a 32-bit BI_RGB DIB.
            if (!p) continue;
            if (x == 0 || x == s.width - 1 || y == 0 || y == s.height - 1)
                throw std::runtime_error("Waiting text reached the edge of the client area");
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
    }
    if (bottom < top) throw std::runtime_error("Missing waiting screen text");
    return {top, {s.pixels + top * s.width, s.pixels + (bottom + 1) * s.width}};
}

void check(Surface& s, UINT dpi) {
    const std::wstring text = L"Waiting for connection 1\nDisplay: 1920 x 1080\n\n"
                              L"Local IP addresses (TCP and UDP 49301)\n"
                              L"2001:db8:8888:8888:8888:8888:8888:8881    192.168.100.101";
    const auto first = render(s, text, dpi, 0);
    // Changing the last digit on each line must change the visible pixels, including
    // the end of the long IPv6 token and the last address in a wrapped list.
    for (auto pos : {text.find(L'1'), text.find(L"1080") + 3, text.find(L"49301") + 4,
                     text.find(L"8881") + 3, text.size() - 1}) {
        auto changed = text;
        changed[pos] = L'7';
        if (render(s, changed, dpi, 0).rows == first.rows)
            throw std::runtime_error("Waiting text was truncated");
    }
    bool up = false, down = false;
    int previous = first.top;
    for (unsigned seconds = 1; seconds <= 120; ++seconds) {
        const auto next = render(s, text, dpi, seconds * 1000);
        if (next.rows != first.rows) throw std::runtime_error("Moving the text changed or cropped its pixels");
        const int delta = next.top - previous;
        if (std::abs(delta) > MulDiv(10, dpi, 96)) throw std::runtime_error("Waiting text moved too quickly");
        up |= delta < 0;
        down |= delta > 0;
        previous = next.top;
    }
    if (!up || !down) throw std::runtime_error("Waiting text did not move in both directions");
}
}

int main() {
    try {
        Surface normal(600, 240), small(240, 150);
        check(normal, 96);
        check(small, 192);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
