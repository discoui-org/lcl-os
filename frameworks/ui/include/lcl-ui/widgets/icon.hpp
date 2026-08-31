#pragma once

#include "lcl-ui/widgets/measured_widget.hpp"

#include <cstdint>
#include <string>

namespace lcl::ui {

/** A font-backed, backend-neutral symbol identifier. */
struct IconData {
    char32_t codepoint{0};
    constexpr bool operator==(const IconData&) const = default;
};

/**
 * Common symbols from the repository-packaged CupertinoIcons font.
 * Keeping codepoints here prevents applications from embedding private-use
 * UTF-8 literals or depending on a renderer-specific font API.
 */
namespace icons {
inline constexpr IconData Back{0xf3cf};
inline constexpr IconData Forward{0xf3d1};
inline constexpr IconData ChevronRight{0xf3d3};
inline constexpr IconData Search{0xf4a5};
inline constexpr IconData PersonCircle{0xf419};
inline constexpr IconData Bluetooth{0xf116};
inline constexpr IconData Gear{0xf43c};
inline constexpr IconData Paintbrush{0xf72e};
inline constexpr IconData Shield{0xf7cf};
inline constexpr IconData ShieldFill{0xf7d0};
inline constexpr IconData Wifi{0xf89b};
inline constexpr IconData InfoCircle{0xf44c};
} // namespace icons

class Icon final : public MeasuredWidget {
public:
    explicit Icon(IconData icon = {});

    void setIcon(IconData icon);
    IconData icon() const noexcept { return m_icon; }
    void setIconSize(float size);
    float iconSize() const noexcept { return m_size; }
    void setColor(graphics::Color color);
    graphics::Color color() const noexcept { return m_color; }

    void draw(graphics::Canvas& canvas,
              const graphics::RectF& damageRect) override;

protected:
    layout::Size measure(const layout::Constraints& constraints) override;

private:
    void updateEncodedIcon();

    IconData m_icon{};
    std::string m_utf8;
    float m_size{20.0f};
    graphics::Color m_color{30, 30, 32, 255};
};

} // namespace lcl::ui
