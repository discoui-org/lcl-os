#pragma once

#include "lcl-ui/widgets/menu.hpp"
#include "lcl-ui/widgets/widget.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lcl::ui {

enum class PickerStyle {
    Automatic,
    Menu,
    RadioGroup,
};

struct PickerOption {
    std::string label;
    bool enabled{true};
};

class Picker final : public Widget {
public:
    using ChangeCallback = std::function<void(size_t)>;
    using PopupCanvasFactory = Menu::PopupCanvasFactory;
    using PopupConnector = Menu::PopupConnector;

    Picker(WindowApp& window, PopupCanvasFactory popupCanvasFactory,
           std::string label = {}, std::vector<PickerOption> options = {},
           size_t selectedIndex = 0, PopupConnector popupConnector = {});

    void setLabel(std::string label);
    const std::string& label() const noexcept { return m_label; }
    void setOptions(std::vector<PickerOption> options);
    const std::vector<PickerOption>& options() const noexcept { return m_options; }
    void setSelectedIndex(size_t index);
    size_t selectedIndex() const noexcept { return m_selectedIndex; }
    void setOnChange(ChangeCallback callback) { m_onChange = std::move(callback); }
    void setPickerStyle(PickerStyle style);
    PickerStyle pickerStyle() const noexcept { return m_style; }
    void setEnabled(bool enabled);
    bool isEnabled() const noexcept { return m_enabled; }

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
    PickerStyle resolvedPickerStyle() const noexcept;
    std::optional<size_t> optionIndexAt(float x, float y) const noexcept;
    bool moveSelection(int direction);
    void normalizeSelection();
    void showMenu();
    void updateDefaultSize();
    lcl::theme::StyleState visualStyleState() const noexcept;
    const lcl::theme::WidgetStyle* defaultStyle() const noexcept override;
    void styleDidChange() override;

    Menu m_menu;
    std::string m_label;
    std::vector<PickerOption> m_options;
    size_t m_selectedIndex{0};
    PickerStyle m_style{PickerStyle::Automatic};
    std::optional<size_t> m_hoveredIndex;
    bool m_enabled{true};
    bool m_hovered{false};
    bool m_pressed{false};
    bool m_focused{false};
    bool m_pointerArmed{false};
    ChangeCallback m_onChange;
};

} // namespace lcl::ui
