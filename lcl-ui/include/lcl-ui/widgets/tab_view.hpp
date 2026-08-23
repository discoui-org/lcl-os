#pragma once

#include "lcl-ui/widgets/widget.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lcl::ui {

enum class TabViewStyle {
    Automatic,
    TabBar,
};

struct Tab {
    std::string title;
    std::unique_ptr<Widget> content;
    bool enabled{true};
};

class TabView final : public Widget {
public:
    using ChangeCallback = std::function<void(size_t)>;

    TabView();
    void addTab(Tab tab);
    size_t tabCount() const noexcept { return m_tabs.size(); }
    void setSelectedIndex(size_t index);
    size_t selectedIndex() const noexcept { return m_selectedIndex; }
    void setOnChange(ChangeCallback callback) { m_onChange = std::move(callback); }
    void setTabViewStyle(TabViewStyle style);
    TabViewStyle tabViewStyle() const noexcept { return m_style; }

    bool onPointerEnter(const PointerEvent& event) override;
    bool onPointerLeave(const PointerEvent& event) override;
    bool onPointerDown(const PointerEvent& event) override;
    bool onPointerMove(const PointerEvent& event) override;
    bool onPointerUp(const PointerEvent& event) override;
    bool onPointerCancel(const PointerEvent& event) override;
    bool onKeyDown(const KeyEvent& event) override;
    bool onFocusGained(const FocusEvent& event) override;
    bool onFocusLost(const FocusEvent& event) override;

    void draw(graphics::Canvas& canvas,
              const graphics::RectF& damageRect) override;

private:
    struct TabState {
        std::string title;
        Widget* content{nullptr};
        bool enabled{true};
    };

    std::optional<size_t> tabIndexAt(float x, float y) const noexcept;
    bool moveSelection(int direction);
    void updateContentVisibility();
    void updateContentInsets();
    const lcl::theme::WidgetStyle* defaultStyle() const noexcept override;
    void styleDidChange() override;

    std::vector<TabState> m_tabs;
    size_t m_selectedIndex{0};
    TabViewStyle m_style{TabViewStyle::Automatic};
    std::optional<size_t> m_hoveredIndex;
    std::optional<size_t> m_pressedIndex;
    bool m_focused{false};
    ChangeCallback m_onChange;
};

} // namespace lcl::ui
