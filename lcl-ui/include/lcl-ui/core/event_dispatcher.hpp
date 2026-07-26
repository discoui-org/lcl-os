#pragma once

#include "lcl-ui/core/events.hpp"
#include "lcl-ui/widgets/widget.hpp"

namespace lcl::ui {

class EventDispatcher {
public:
    EventDispatcher() = default;
    ~EventDispatcher() = default;

    Widget* hitTest(Widget* root, float x, float y);

    bool dispatchPointerEvent(Widget* root, const PointerEvent& event);
    bool dispatchKeyEvent(const KeyEvent& event);
    bool dispatchTextInputEvent(const TextInputEvent& event);

    void setFocus(Widget* widget);
    Widget* getFocusedWidget() const { return m_focusedWidget; }
    Widget* getHoveredWidget() const { return m_hoveredWidget; }

private:
    Widget* m_hoveredWidget{nullptr};
    Widget* m_focusedWidget{nullptr};
};

} // namespace lcl::ui
