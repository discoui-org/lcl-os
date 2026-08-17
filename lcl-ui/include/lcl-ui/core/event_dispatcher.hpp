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
    bool cancelPointerDownTarget(uint32_t pointerId, Widget* newOwner,
                                 const PointerEvent& sourceEvent);
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

    struct PointerDownTarget {
        Widget* target{nullptr};
        std::weak_ptr<uint8_t> lifetime;
        PointerSource source{PointerSource::Mouse};
        float downX{0.0f};
        float downY{0.0f};
        // Touch-only, one-way state. It starts true and becomes false only
        // after total displacement reaches the shared touch slop or ownership
        // leaves the original down-target sequence.
        bool tapEligible{true};
    };

    static bool isEventCapableInTree(Widget* root, const Widget* target,
                                     bool ancestorsVisible = true);
    static bool dispatchToTarget(Widget* target, const PointerEvent& event);
    static void dispatchPreviewToTarget(Widget* target, const PointerEvent& event);
    static bool dispatchCancelUntil(Widget* target, Widget* stopBefore,
                                    const PointerEvent& event);
    static Widget* findFocusableTarget(Widget* target);
    Widget* getPointerDownTarget(uint32_t pointerId, Widget* root);
    void updateTouchTapEligibility(uint32_t pointerId, const PointerEvent& event);
    void invalidateTouchTapForCapture(uint32_t pointerId);
    bool isTouchTapEligible(uint32_t pointerId) const;
    void applyPointerDownFocus(Widget* target, const PointerEvent& event);
    void applyTouchTapFocus(Widget* downTarget, Widget* upTarget,
                            const PointerEvent& event);
    Widget* validatePointerCapture(Widget* root, const PointerEvent& event);
    void clearPointerCapture(uint32_t pointerId);

    Widget* m_hoveredWidget{nullptr};
    Widget* m_focusedWidget{nullptr};
    std::unordered_map<uint32_t, PointerCapture> m_pointerCaptures;
    std::unordered_map<uint32_t, PointerDownTarget> m_pointerDownTargets;
};

} // namespace lcl::ui
