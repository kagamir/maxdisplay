#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

namespace win_waiting {
void paint(HDC dc, const RECT& client, const std::wstring& text, UINT dpi, std::uint64_t elapsed_ms);
}
