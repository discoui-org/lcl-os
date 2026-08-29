#pragma once

#include "lcl-ui/core/touch_interaction.hpp"
#include "lcl-ui/widgets/widget.hpp"
#include <algorithm>
#include <memory>

namespace lcl::ui {

class ScrollView : public Widget {
    enum class TouchPanState {
        Idle,
        Pending,
        Dragging,
    };

public:
    // Compatibility alias; the interaction policy is shared with dispatcher
    // tap validation.
    static constexpr float kTouchDragThreshold = touch_interaction::kTouchSlop;

    ScrollView();
    ~ScrollView() override = default;

    void setContent(std::unique_ptr<Widget> content);
    Widget* getContent() const { return m_contentWidget; }

    void setScrollY(float offset);
    float getScrollY() const noexcept { return m_scrollY; }
    float getMaxScrollY() const noexcept;
    float getContentHeight() const noexcept { return m_contentHeight; }

    void setScrollSpeed(float speed) { m_scrollSpeed = speed; }
    float getScrollSpeed() const noexcept { return m_scrollSpeed; }
    bool isTouchPanActive() const noexcept { return m_touchPanState != TouchPanState::Idle; }
    bool isTouchDragging() const noexcept { return m_touchPanState == TouchPanState::Dragging; }

    void syncLayout(float parentAbsX = 0.0f, float parentAbsY = 0.0f) override;
    void draw(graphics::Canvas& canvas, const graphics::RectF& damageRect) override;
    void onPointerEventPreview(const PointerEvent& event) override;
    bool onPointerMove(const PointerEvent& event) override;
    bool onPointerUp(const PointerEvent& event) override;
    bool onPointerCancel(const PointerEvent& event) override;
    bool onScroll(const PointerEvent& event) override;

private:
    void clampScrollOffset();
    void resetTouchPan() noexcept;

    Widget* m_contentWidget{nullptr};
    float m_scrollY{0.0f};
    float m_contentHeight{0.0f};
    float m_scrollSpeed{24.0f};
    TouchPanState m_touchPanState{TouchPanState::Idle};
    uint32_t m_touchPointerId{0};
    float m_touchStartY{0.0f};
    float m_touchStartScrollY{0.0f};
    bool m_cacheValid{false};
    bool m_cachedSubtreeAnimating{false};
    std::optional<graphics::RectF> m_cachedAnimationBounds{};
    uint64_t m_cachedContentPaintRevision{0};
    uint64_t m_cachedContentPresentationRevision{0};
    uint64_t m_lastCacheUpdatePassSerial{0};
    float m_cachedContentX{0.0f};
    float m_cachedContentY{0.0f};
    float m_cachedContentWidth{0.0f};
    float m_cachedContentHeight{0.0f};
    float m_cachedViewportWidth{0.0f};
    float m_cachedViewportHeight{0.0f};
    float m_cachedDeviceScale{0.0f};
};

} // namespace lcl::ui
