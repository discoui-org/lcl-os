#define STB_TRUETYPE_IMPLEMENTATION
#include "render/stb_truetype.h"
#include "render/font_renderer.hpp"
#include <fstream>
#include <iostream>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace lcl::render {

FontRenderer::FontRenderer() = default;

FontRenderer::~FontRenderer() {
    if (m_fontInfo) {
        delete static_cast<stbtt_fontinfo*>(m_fontInfo);
        m_fontInfo = nullptr;
    }
}

FontRenderer::FontRenderer(FontRenderer&& other) noexcept
    : m_ttfBuffer(std::move(other.m_ttfBuffer)),
      m_fontInfo(other.m_fontInfo),
      m_fontSize(other.m_fontSize),
      m_scale(other.m_scale),
      m_ascent(other.m_ascent),
      m_descent(other.m_descent),
      m_lineGap(other.m_lineGap),
      m_initialized(other.m_initialized),
      m_glyphCache(std::move(other.m_glyphCache)) {
    other.m_fontInfo = nullptr;
    other.m_initialized = false;
}

FontRenderer& FontRenderer::operator=(FontRenderer&& other) noexcept {
    if (this != &other) {
        if (m_fontInfo) {
            delete static_cast<stbtt_fontinfo*>(m_fontInfo);
        }
        m_ttfBuffer = std::move(other.m_ttfBuffer);
        m_fontInfo = other.m_fontInfo;
        m_fontSize = other.m_fontSize;
        m_scale = other.m_scale;
        m_ascent = other.m_ascent;
        m_descent = other.m_descent;
        m_lineGap = other.m_lineGap;
        m_initialized = other.m_initialized;
        m_glyphCache = std::move(other.m_glyphCache);

        other.m_fontInfo = nullptr;
        other.m_initialized = false;
    }
    return *this;
}

bool FontRenderer::loadFont(const std::string& fontPath, float fontSize) {
    std::ifstream file(fontPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    m_ttfBuffer.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(m_ttfBuffer.data()), size)) {
        m_ttfBuffer.clear();
        return false;
    }

    auto* info = new stbtt_fontinfo();
    if (!stbtt_InitFont(info, m_ttfBuffer.data(), 0)) {
        delete info;
        m_ttfBuffer.clear();
        return false;
    }

    m_fontInfo = info;
    m_fontSize = fontSize;
    m_scale = stbtt_ScaleForPixelHeight(info, m_fontSize);

    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &lineGap);
    m_ascent = static_cast<int>(std::round(ascent * m_scale));
    m_descent = static_cast<int>(std::round(descent * m_scale));
    m_lineGap = static_cast<int>(std::round(lineGap * m_scale));

    m_initialized = true;
    m_glyphCache.clear();

    precacheASCII();
    //comment out the following line if you don't want to see the font loading message
    // std::cout << "[LCL Font] TrueType Font loaded successfully: " << fontPath
    //           << " (" << fontSize << "px, macOS-style antialiasing active).\n";
    return true;
}

static char32_t decodeNextUTF8(const std::string& str, size_t& i) {
    if (i >= str.size()) return 0;
    uint8_t c1 = static_cast<uint8_t>(str[i]);
    if (c1 < 0x80) {
        i += 1;
        return c1;
    } else if ((c1 & 0xE0) == 0xC0) {
        if (i + 1 < str.size()) {
            uint8_t c2 = static_cast<uint8_t>(str[i + 1]);
            i += 2;
            return ((c1 & 0x1F) << 6) | (c2 & 0x3F);
        }
    } else if ((c1 & 0xF0) == 0xE0) {
        if (i + 2 < str.size()) {
            uint8_t c2 = static_cast<uint8_t>(str[i + 1]);
            uint8_t c3 = static_cast<uint8_t>(str[i + 2]);
            i += 3;
            return ((c1 & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
        }
    } else if ((c1 & 0xF8) == 0xF0) {
        if (i + 3 < str.size()) {
            uint8_t c2 = static_cast<uint8_t>(str[i + 1]);
            uint8_t c3 = static_cast<uint8_t>(str[i + 2]);
            uint8_t c4 = static_cast<uint8_t>(str[i + 3]);
            i += 4;
            return ((c1 & 0x07) << 18) | ((c2 & 0x3F) << 12) | ((c3 & 0x3F) << 6) | (c4 & 0x3F);
        }
    }
    i += 1;
    return c1;
}

void FontRenderer::precacheASCII() {
    for (char32_t c = 32; c <= 126; ++c) {
        getGlyph(c);
    }
    // Precache Chevron symbol (❯ U+276F) and arrows
    getGlyph(0x276F); // Heavy Right-Pointing Angle Quotation Mark Mark (Chevron ❯)
    getGlyph(0x276E); // Chevron ❮
    getGlyph(0x279C); // Heavy Round-Headed Rightwards Arrow ➜
    getGlyph(0x2192); // Rightwards Arrow →
}

const GlyphInfo* FontRenderer::getGlyph(char32_t codepoint) {
    if (!m_initialized || !m_fontInfo) return nullptr;

    auto it = m_glyphCache.find(codepoint);
    if (it != m_glyphCache.end()) {
        return &it->second;
    }

    auto* info = static_cast<stbtt_fontinfo*>(m_fontInfo);
    int advance = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(info, codepoint, &advance, &lsb);

    int w = 0, h = 0, xoff = 0, yoff = 0;
    unsigned char* bmp = stbtt_GetCodepointBitmap(info, 0, m_scale, codepoint, &w, &h, &xoff, &yoff);

    GlyphInfo gInfo{};
    gInfo.width = w;
    gInfo.height = h;
    gInfo.xOffset = xoff;
    gInfo.yOffset = yoff;
    gInfo.advanceWidth = static_cast<int>(std::round(advance * m_scale));
    gInfo.leftSideBearing = static_cast<int>(std::round(lsb * m_scale));

    if (bmp && w > 0 && h > 0) {
        gInfo.bitmap.assign(bmp, bmp + (w * h));
        stbtt_FreeBitmap(bmp, nullptr);
    }

    auto [insertedIt, success] = m_glyphCache.emplace(codepoint, std::move(gInfo));
    return &insertedIt->second;
}

int FontRenderer::getCellWidth() {
    if (!m_initialized) return 8;
    const GlyphInfo* g = getGlyph('M');
    if (g && g->advanceWidth > 0) {
        return g->advanceWidth;
    }
    return 8;
}

int FontRenderer::getTextWidth(const std::string& text) {
    if (!m_initialized || text.empty()) return 0;
    int totalWidth = 0;
    size_t i = 0;
    while (i < text.size()) {
        char32_t codepoint = decodeNextUTF8(text, i);
        if (codepoint == '\n') break;
        const GlyphInfo* g = getGlyph(codepoint);
        if (g) {
            totalWidth += (g->advanceWidth > 0 ? g->advanceWidth : 9);
        } else {
            totalWidth += 9;
        }
    }
    return totalWidth;
}

void FontRenderer::renderString(uint32_t* backBuffer, int screenWidth, int screenHeight,
                                int x, int y, const std::string& text, uint32_t fgColor) {
    renderStringClipped(backBuffer, screenWidth, screenHeight, x, y, text, fgColor, 0, 0, screenWidth, screenHeight);
}

void FontRenderer::renderStringClipped(uint32_t* backBuffer, int screenWidth, int screenHeight,
                                       int x, int y, const std::string& text, uint32_t fgColor,
                                       int minX, int minY, int maxX, int maxY) {
    if (!m_initialized || !backBuffer) return;

    uint8_t fgA = static_cast<uint8_t>((fgColor >> 24) & 0xFF);
    uint8_t fgR = static_cast<uint8_t>((fgColor >> 16) & 0xFF);
    uint8_t fgG = static_cast<uint8_t>((fgColor >> 8) & 0xFF);
    uint8_t fgB = static_cast<uint8_t>(fgColor & 0xFF);
    if (fgA == 0) fgA = 255;

    int curX = x;
    int curY = y + m_ascent;

    size_t i = 0;
    while (i < text.size()) {
        char32_t codepoint = decodeNextUTF8(text, i);
        if (codepoint == '\n') {
            curX = x;
            curY += m_ascent - m_descent + m_lineGap;
            continue;
        }

        const GlyphInfo* g = getGlyph(codepoint);
        if (!g) continue;

        if (g->width > 0 && g->height > 0 && !g->bitmap.empty()) {
            int glyphStartX = curX + g->xOffset;
            int glyphStartY = curY + g->yOffset;

            for (int r = 0; r < g->height; ++r) {
                int py = glyphStartY + r;
                if (py < minY || py >= maxY || py < 0 || py >= screenHeight) continue;

                for (int col = 0; col < g->width; ++col) {
                    int px = glyphStartX + col;
                    if (px < minX || px >= maxX || px < 0 || px >= screenWidth) continue;

                    uint8_t cov = g->bitmap[r * g->width + col];
                    if (cov == 0) continue;

                    // macOS-Style Antialiasing & Subpixel Alpha Blending
                    float coverage = (cov / 255.0f) * (fgA / 255.0f);
                    float alpha = std::pow(coverage, 0.85f); // Gamma adjustment for crisp font rendering

                    size_t pixelIndex = static_cast<size_t>(py * screenWidth + px);
                    uint32_t bgPixel = backBuffer[pixelIndex];

                    uint8_t bgA = static_cast<uint8_t>((bgPixel >> 24) & 0xFF);
                    uint8_t bgR = static_cast<uint8_t>((bgPixel >> 16) & 0xFF);
                    uint8_t bgG = static_cast<uint8_t>((bgPixel >> 8) & 0xFF);
                    uint8_t bgB = static_cast<uint8_t>(bgPixel & 0xFF);

                    float aText = alpha; // glyph coverage alpha
                    float aBg = bgA / 255.0f;
                    float aOut = aText + aBg * (1.0f - aText);

                    if (aOut > 0.001f) {
                        float outR = (aText * fgR + (1.0f - aText) * aBg * bgR) / aOut;
                        float outG = (aText * fgG + (1.0f - aText) * aBg * bgG) / aOut;
                        float outB = (aText * fgB + (1.0f - aText) * aBg * bgB) / aOut;

                        uint8_t rByte = static_cast<uint8_t>(std::clamp(outR, 0.0f, 255.0f));
                        uint8_t gByte = static_cast<uint8_t>(std::clamp(outG, 0.0f, 255.0f));
                        uint8_t bByte = static_cast<uint8_t>(std::clamp(outB, 0.0f, 255.0f));
                        uint8_t aByte = static_cast<uint8_t>(std::clamp(aOut * 255.0f, 0.0f, 255.0f));

                        backBuffer[pixelIndex] = (static_cast<uint32_t>(aByte) << 24) |
                                                 (static_cast<uint32_t>(rByte) << 16) |
                                                 (static_cast<uint32_t>(gByte) << 8)  |
                                                  static_cast<uint32_t>(bByte);
                    }
                }
            }
        }

        curX += (g->advanceWidth > 0 ? g->advanceWidth : 9);
    }
}

} // namespace lcl::render
