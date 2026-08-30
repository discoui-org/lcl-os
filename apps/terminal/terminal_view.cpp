#include "apps/terminal/terminal_view.hpp"

#include <algorithm>
#include <cmath>

#include "lcl-graphics/canvas.hpp"

namespace lcl::apps {

namespace {

constexpr float kFontSize = 15.0f;
constexpr float kLineHeight = 18.0f;
constexpr float kPadding = 8.0f;
constexpr lcl::graphics::Color kTextColor{236, 239, 244, 255};
constexpr lcl::graphics::Color kCursorColor{196, 202, 211, 255};

size_t utf8CodepointLength(unsigned char firstByte) {
    if ((firstByte & 0x80) == 0) return 1;
    if ((firstByte & 0xE0) == 0xC0) return 2;
    if ((firstByte & 0xF0) == 0xE0) return 3;
    if ((firstByte & 0xF8) == 0xF0) return 4;
    return 1;
}

} // namespace

TerminalView::TerminalView(TerminalApp& terminal) : m_terminal(terminal) {}

bool TerminalView::isCursorVisible() const {
    if (m_terminal.shouldDrawSolidCursor()) return true;
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return ((milliseconds / 500) % 2) == 0;
}

bool TerminalView::updateCursorBlink() {
    const bool visible = isCursorVisible();
    if (!m_cursorStateInitialized || visible != m_cursorVisible) {
        m_cursorStateInitialized = true;
        m_cursorVisible = visible;
        // Cursor visibility changes paint only the last caret cell. Terminal
        // output and resize still invalidate the full widget through their
        // existing paths and refresh this cached geometry during draw().
        invalidatePaint(m_cursorPaintBounds);
        return true;
    }
    return false;
}

std::string TerminalView::clippedLine(const std::string& line, float availableWidth,
                                      float cellWidth) const {
    if (availableWidth <= 0.0f || line.empty()) return {};

    const size_t maxColumns = static_cast<size_t>(std::max(1.0f, std::floor(availableWidth /
        std::max(1.0f, cellWidth))));
    size_t column = 0;
    size_t byte = 0;
    while (byte < line.size() && column < maxColumns) {
        const size_t sequenceLength = std::min(utf8CodepointLength(
            static_cast<unsigned char>(line[byte])), line.size() - byte);
        byte += sequenceLength;
        ++column;
    }
    return line.substr(0, byte);
}

void TerminalView::draw(lcl::graphics::Canvas& canvas, const lcl::graphics::RectF& damageRect) {
    if (!m_visible || !m_absoluteBounds.intersects(damageRect)) return;

    const auto& lines = m_terminal.getLines();
    const float contentLeft = m_absoluteBounds.x + kPadding;
    const float contentTop = m_absoluteBounds.y + kPadding;
    const float contentRight = m_absoluteBounds.x + m_absoluteBounds.width - kPadding;
    const float contentBottom = m_absoluteBounds.y + m_absoluteBounds.height - kPadding;
    if (contentRight <= contentLeft || contentBottom <= contentTop) return;

    const int maxRows = std::max(1, static_cast<int>(std::floor(
        (contentBottom - contentTop) / kLineHeight)));
    const int startLine = std::max(0, static_cast<int>(lines.size()) - maxRows);
    const float cellWidth = std::max(1.0f, canvas.measureText(
        "M", kFontSize, lcl::graphics::FontFamily::Monospace));
    const float availableWidth = contentRight - contentLeft;

    float y = contentTop;
    for (size_t index = static_cast<size_t>(startLine);
         index < lines.size() && y + kLineHeight <= contentBottom;
         ++index, y += kLineHeight) {
        const std::string text = clippedLine(lines[index], availableWidth, cellWidth);
        if (!text.empty()) {
            canvas.drawText(contentLeft, y, text, kTextColor, kFontSize,
                            lcl::graphics::FontFamily::Monospace);
        }
    }

    const float cursorY = (y > contentTop) ? (y - kLineHeight) : contentTop;
    const std::string& finalLine = lines.empty() ? std::string{} : lines.back();
    const size_t cursorByte = static_cast<size_t>(std::clamp(
        m_terminal.getCursorColumn(), 0, static_cast<int>(finalLine.size())));
    const std::string cursorPrefix = clippedLine(finalLine.substr(0, cursorByte),
                                                  availableWidth, cellWidth);
    const float cursorX = std::min(contentRight - cellWidth,
        contentLeft + canvas.measureText(cursorPrefix, kFontSize, lcl::graphics::FontFamily::Monospace));
    m_cursorPaintBounds = {cursorX, cursorY, cellWidth, kFontSize};
    if (m_cursorVisible && cursorX >= contentLeft &&
        cursorY + kFontSize <= contentBottom) {
        canvas.drawRect(m_cursorPaintBounds, kCursorColor);
    }
}

} // namespace lcl::apps
