#pragma once

#include "lcl-ui/core/events.hpp"
#include "lcl-ui/widgets/widget.hpp"
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace lcl::ui {

class EventDispatcher {
public:
    EventDispatcher() = default;
    ~EventDispatcher() = default;

    Widget* hitTest(Widget* root, float x, float y);

    bool dispatchPointerEvent(Widget* root, const PointerEvent& event);
    bool dispatchKeyEvent(const KeyEvent& event);
    bool dispatchTextInputEvent(const TextInputEvent& event);

    bool capturePointer(uint32_t pointerId, Widget* owner,
                        PointerSource source = PointerSource::Mouse);
    bool releasePointerCapture(uint32_t pointerId, const Widget* owner = nullptr);
    bool hasPointerCapture(uint32_t pointerId, const Widget* owner = nullptr) const;
    Widget* getPointerCapture(uint32_t pointerId) const;
    void cancelPointerCaptures();

    void setFocus(Widget* widget);
    Widget* getFocusedWidget() const { return m_focusedWidget; }
    Widget* getHoveredWidget() const { return m_hoveredWidget; }

private:
    struct PointerCapture {
        Widget* owner{nullptr};
        std::weak_ptr<uint8_t> lifetime;
        PointerSource source{PointerSource::Mouse};
    };

    static bool isEventCapableInTree(Widget* root, const Widget* target,
                                     bool ancestorsVisible = true);
    static bool dispatchToTarget(Widget* target, const PointerEvent& event);
    Widget* validatePointerCapture(Widget* root, const PointerEvent& event);
    void clearPointerCapture(uint32_t pointerId);

    Widget* m_hoveredWidget{nullptr};
    Widget* m_focusedWidget{nullptr};
    std::unordered_map<uint32_t, PointerCapture> m_pointerCaptures;
};

} // namespace lcl::ui
