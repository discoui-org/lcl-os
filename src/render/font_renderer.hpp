#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <unordered_map>

namespace lcl::render {

struct GlyphInfo {
    int width{0};
    int height{0};
    int xOffset{0};
    int yOffset{0};
    int advanceWidth{0};
    int leftSideBearing{0};
    std::vector<uint8_t> bitmap; // 8-bit alpha coverage mask (0..255)
};

class FontRenderer {
public:
    FontRenderer();
    ~FontRenderer();

    // Non-copyable
    FontRenderer(const FontRenderer&) = delete;
    FontRenderer& operator=(const FontRenderer&) = delete;

    // Moveable
    FontRenderer(FontRenderer&&) noexcept;
    FontRenderer& operator=(FontRenderer&&) noexcept;

    /**
     * @brief Load TTF font file and initialize TrueType rasterizer & glyph cache.
     * @param fontPath Absolute or relative path to TTF font file
     * @param fontSize Target pixel height (default: 16.0f)
     * @return true if font loaded and initialized successfully
     */
    bool loadFont(const std::string& fontPath, float fontSize = 16.0f);

    bool isInitialized() const { return m_initialized; }
    float getFontSize() const { return m_fontSize; }
    int getFontAscent() const { return m_ascent; }
    int getFontDescent() const { return m_descent; }
    int getLineGap() const { return m_lineGap; }

    /**
     * @brief Fetch or rasterize glyph info for a specific codepoint.
     */
    const GlyphInfo* getGlyph(char32_t codepoint);

    /**
     * @brief Calculate the exact pixel width of a UTF-8 text string.
     */
    int getTextWidth(const std::string& text);

    /**
     * @brief Render text onto a 32-bit ARGB software backbuffer with macOS-style antialiasing.
     */
    void renderString(uint32_t* backBuffer, int screenWidth, int screenHeight,
                      int x, int y, const std::string& text, uint32_t fgColor);

    void renderStringClipped(uint32_t* backBuffer, int screenWidth, int screenHeight,
                             int x, int y, const std::string& text, uint32_t fgColor,
                             int minX, int minY, int maxX, int maxY);

private:
    void precacheASCII();

    std::vector<uint8_t> m_ttfBuffer;
    void* m_fontInfo{nullptr}; // opaque pointer to stbtt_fontinfo
    float m_fontSize{16.0f};
    float m_scale{1.0f};
    int m_ascent{0};
    int m_descent{0};
    int m_lineGap{0};
    bool m_initialized{false};

    std::unordered_map<char32_t, GlyphInfo> m_glyphCache;
};

} // namespace lcl::render
