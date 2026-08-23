#pragma once

#include "lcl-graphics/font.hpp"
#include "lcl-ui/widgets/widget.hpp"
#include "lcl-ui/widgets/container.hpp"
#include <string>

namespace lcl::ui {

enum class TextAlign {
    Start,
    Center,
    End,
};

class Text : public Widget {
public:
    explicit Text(const std::string& content = "");
    ~Text() override = default;

    void setText(const std::string& text);
    const std::string& getText() const { return m_text; }

    void setFontSize(float size);
    void resetFontSize();
    float getFontSize() const { return m_fontSize; }

    void setTextRole(lcl::theme::TextRole role);
    lcl::theme::TextRole getTextRole() const noexcept { return m_textRole; }

    void setFontFamily(graphics::FontFamily family);
    graphics::FontFamily getFontFamily() const { return m_fontFamily; }

    void setTextColor(const graphics::Color& color);
    void resetTextColor();
    graphics::Color getTextColor() const { return m_textColor; }

    void setTextAlign(TextAlign align) { m_textAlign = align; markDirty(); }
    TextAlign getTextAlign() const { return m_textAlign; }

    void draw(graphics::Canvas& canvas, const graphics::RectF& damageRect) override;

protected:
    const lcl::theme::WidgetStyle* defaultStyle() const noexcept override;
    void styleDidChange() override;

private:
    void updateMeasureFunc();

    std::string m_text;
    float m_fontSize{14.0f};
    graphics::FontFamily m_fontFamily{graphics::FontFamily::Interface};
    graphics::Color m_textColor{255, 255, 255, 255};
    bool m_hasExplicitFontSize{false};
    bool m_hasExplicitTextColor{false};
    lcl::theme::TextRole m_textRole{lcl::theme::TextRole::Body};
    TextAlign m_textAlign{TextAlign::Start};
};

} // namespace lcl::ui
