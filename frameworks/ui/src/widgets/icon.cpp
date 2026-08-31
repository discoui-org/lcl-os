#include "lcl-ui/widgets/icon.hpp"

#include "system/render/text_metrics.hpp"

#include <algorithm>

namespace lcl::ui {
namespace {

std::string encodeUtf8(char32_t codepoint) {
    if (codepoint == 0 || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
        return {};
    }
    std::string result;
    if (codepoint <= 0x7f) {
        result.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        result.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        result.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        result.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
    return result;
}

} // namespace

Icon::Icon(IconData icon) : m_icon(icon) { updateEncodedIcon(); }

void Icon::setIcon(IconData icon) {
    if (m_icon == icon) return;
    m_icon = icon;
    updateEncodedIcon();
    invalidateMeasurement();
    invalidatePaint();
}

void Icon::setIconSize(float size) {
    const float next = std::max(1.0f, size);
    if (m_size == next) return;
    m_size = next;
    invalidateMeasurement();
    invalidatePaint();
}

void Icon::setColor(graphics::Color color) {
    if (m_color.toARGB() == color.toARGB()) return;
    m_color = color;
    invalidatePaint();
}

void Icon::updateEncodedIcon() { m_utf8 = encodeUtf8(m_icon.codepoint); }

layout::Size Icon::measure(const layout::Constraints&) {
    const auto metrics = lcl::render::text_metrics::measure(
        m_utf8, m_size, graphics::FontFamily::Icons);
    return {std::max(m_size, metrics.advanceWidth),
            std::max(m_size, metrics.lineHeight)};
}

void Icon::draw(graphics::Canvas& canvas,
                const graphics::RectF& damageRect) {
    if (!m_visible || isCollapsed() || m_utf8.empty() ||
        !getPresentationBounds().intersects(damageRect)) {
        return;
    }
    beginPresentation(canvas, damageRect);
    const float width = canvas.measureText(
        m_utf8, m_size, graphics::FontFamily::Icons);
    const float x = m_absoluteBounds.x +
        std::max(0.0f, (m_absoluteBounds.width - width) * 0.5f);
    canvas.drawText(x, m_absoluteBounds.y, m_utf8, m_color, m_size,
                    graphics::FontFamily::Icons);
    drawChildren(canvas, damageRect);
    endPresentation(canvas);
}

} // namespace lcl::ui
