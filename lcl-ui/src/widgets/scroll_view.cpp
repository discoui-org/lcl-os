#include "lcl-ui/widgets/scroll_view.hpp"
#include <cmath>

namespace lcl::ui {

ScrollView::ScrollView() {
    setClipsToBounds(true);
    m_yogaNode.setAlignItems(YGAlignStretch);
}

void ScrollView::setContent(std::unique_ptr<Widget> content) {
    if (m_contentWidget) {
        removeChild(m_contentWidget);
        m_contentWidget = nullptr;
    }
    if (!content) return;
    m_contentWidget = content.get();
    m_contentWidget->getYogaNode().setFlexShrink(0.0f);
    m_contentWidget->getYogaNode().setHeightAuto();
    addChild(std::move(content));
    clampScrollOffset();
}

float ScrollView::getMaxScrollY() const noexcept {
    return std::max(0.0f, m_contentHeight - m_bounds.height);
}

void ScrollView::setScrollY(float offset) {
    const float maxScroll = getMaxScrollY();
    m_scrollY = std::clamp(offset, 0.0f, maxScroll);
    if (m_contentWidget) {
        m_contentWidget->setTranslationY(-m_scrollY);
    }
    markDirty();
}

void ScrollView::clampScrollOffset() {
    const float maxScroll = getMaxScrollY();
    const float clamped = std::clamp(m_scrollY, 0.0f, maxScroll);
    if (clamped != m_scrollY || (m_contentWidget && m_contentWidget->getPresentationState().translationY != -clamped)) {
        setScrollY(clamped);
    }
}

void ScrollView::syncLayout(float parentAbsX, float parentAbsY) {
    if (!m_contentWidget && !m_children.empty()) {
        m_contentWidget = m_children.front().get();
    }

    if (m_contentWidget) {
        m_contentWidget->getYogaNode().setFlexShrink(0.0f);
        m_contentWidget->getYogaNode().setHeightAuto();
    }

    Widget::syncLayout(parentAbsX, parentAbsY);

    if (m_contentWidget) {
        m_contentHeight = m_contentWidget->getBounds().height;
    } else {
        m_contentHeight = 0.0f;
    }

    clampScrollOffset();
}

bool ScrollView::onScroll(const PointerEvent& event) {
    if (m_contentHeight <= m_bounds.height) {
        return false;
    }

    if (std::abs(event.deltaY) < 0.001f) {
        return false;
    }

    const float prevScroll = m_scrollY;
    setScrollY(m_scrollY + event.deltaY * m_scrollSpeed);
    return m_scrollY != prevScroll || true;
}

} // namespace lcl::ui
