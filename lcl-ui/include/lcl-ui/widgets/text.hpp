#pragma once

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
    float getFontSize() const { return m_fontSize; }

    void setTextColor(const Color& color) { m_textColor = color; markDirty(); }
    Color getTextColor() const { return m_textColor; }

    void setTextAlign(TextAlign align) { m_textAlign = align; markDirty(); }
    TextAlign getTextAlign() const { return m_textAlign; }

    void draw(Canvas& canvas, const Rect& damageRect) override;

private:
    void updateMeasureFunc();

    std::string m_text;
    float m_fontSize{14.0f};
    Color m_textColor{255, 255, 255, 255};
    TextAlign m_textAlign{TextAlign::Start};
};

} // namespace lcl::ui
