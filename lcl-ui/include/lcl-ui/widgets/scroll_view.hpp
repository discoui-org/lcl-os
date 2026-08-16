#pragma once

#include "lcl-ui/widgets/widget.hpp"
#include <algorithm>
#include <memory>

namespace lcl::ui {

class ScrollView : public Widget {
public:
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

    void syncLayout(float parentAbsX = 0.0f, float parentAbsY = 0.0f) override;
    bool onScroll(const PointerEvent& event) override;

private:
    void clampScrollOffset();

    Widget* m_contentWidget{nullptr};
    float m_scrollY{0.0f};
    float m_contentHeight{0.0f};
    float m_scrollSpeed{24.0f};
};

} // namespace lcl::ui
