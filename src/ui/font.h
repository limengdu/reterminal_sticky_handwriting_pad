#pragma once

#include <cstdint>

struct FontGlyph {
    uint8_t columns[5];
};

constexpr uint8_t kFontWidth = 5;
constexpr uint8_t kFontHeight = 7;
constexpr uint8_t kFontSpacing = 1;

// Returns a compact 5x7 glyph. Unsupported characters use the '?' glyph.
// 返回一个5x7点阵字符；当前字体不支持的字符统一显示为问号。
const FontGlyph &font_get_glyph(char character);
