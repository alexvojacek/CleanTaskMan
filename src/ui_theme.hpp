#pragma once
#include <cstdint>

namespace ui {
// Plain values, resolved once per theme change. No stylesheet parsing or theme work per row.
struct Palette {
    uint32_t window, surface, hover, selected, border, text, muted, disabled;
    uint32_t accent, heat, search, searchLine, grid;
};
inline constexpr Palette dark{0x202020, 0x191919, 0x2D2D2D, 0x303030, 0x303030, 0xFFFFFF, 0xCECECE,
                              0x777777, 0x60CDFF, 0x263F49, 0x2D2D2D, 0x454545, 0x29404B};
inline constexpr Palette light{0xF3F3F3, 0xFFFFFF, 0xEAEAEA, 0xE8E8E8, 0xE5E5E5, 0x191919, 0x5D5D5D,
                               0x9C9C9C, 0x0067C0, 0xE0F3FC, 0xFBFBFB, 0x8A8A8A, 0xD9E8EE};
inline constexpr float titleHeight = 48, commandHeight = 52;
inline constexpr float compactNav = 48, expandedNav = 200;
inline constexpr float rowHeight = 32, tableHeaderHeight = 60, groupHeight = 36;
inline constexpr float bodyFont = 12, commandFont = 12, pageFont = 14;
inline constexpr const wchar_t* font = L"Segoe UI Variable Text";
inline constexpr const wchar_t* iconFont = L"Segoe Fluent Icons";
} // namespace ui
