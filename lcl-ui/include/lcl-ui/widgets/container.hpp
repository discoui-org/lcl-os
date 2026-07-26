#pragma once

#include "lcl-ui/widgets/widget.hpp"

namespace lcl::ui {

struct Color {
    uint8_t r{0};
    uint8_t g{0};
    uint8_t b{0};
    uint8_t a{0}; // Default completely transparent (unstyled baseline)
};

class Container : public Widget {
public:
    Container();
    ~Container() override = default;

    void setBackgroundColor(const Color& color) { m_backgroundColor = color; markDirty(); }
    Color getBackgroundColor() const { return m_backgroundColor; }

    void setBorderColor(const Color& color) { m_borderColor = color; markDirty(); }
    Color getBorderColor() const { return m_borderColor; }

    void setBorderWidth(float width) { m_borderWidth = width; markDirty(); }
    float getBorderWidth() const { return m_borderWidth; }

    void draw(SkCanvas* canvas, const Rect& damageRect) override;

private:
    Color m_backgroundColor{0, 0, 0, 0};
    Color m_borderColor{0, 0, 0, 0};
    float m_borderWidth{0.0f};
};

} // namespace lcl::ui
