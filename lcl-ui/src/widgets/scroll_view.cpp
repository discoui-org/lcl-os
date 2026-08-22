#include "lcl-ui/widgets/scroll_view.hpp"

namespace lcl::ui {

ScrollView::ScrollView() {
    setClipsToBounds(true);
    m_yogaNode.setAlignItems(YGAlignStretch);
}

void ScrollView::setContent(std::unique_ptr<Widget> content) {
    m_cacheValid = false;
    if (m_contentWidget) {
        removeChild(m_contentWidget);
        m_contentWidget = nullptr;
    }
    if (!content) return;
    m_contentWidget = content.get();
    m_contentWidget->getYogaNode().setFlexShrink(0.0f);
    if (!m_contentWidget->m_hasHeight) {
        m_contentWidget->getYogaNode().setHeightAuto();
    }
    addChild(std::move(content));
    clampScrollOffset();
}

void ScrollView::draw(graphics::Canvas& canvas, const graphics::RectF& damageRect) {
    if (!m_visible || !getPresentationBounds().intersects(damageRect)) return;

    beginPresentation(canvas);
    if (!m_contentWidget) {
        endPresentation(canvas);
        return;
    }

    const graphics::RectF contentBounds = m_contentWidget->getAbsoluteBounds();
    const graphics::RectF presentedContentBounds{
        contentBounds.x,
        contentBounds.y - m_scrollY,
        contentBounds.width,
        contentBounds.height,
    };
    const uint64_t contentRevision = m_contentWidget->getPaintRevision();
    const uint64_t contentPresentationRevision =
        m_contentWidget->getPresentationRevision();
    const bool geometryChanged =
        m_cachedContentWidth != contentBounds.width ||
        m_cachedContentHeight != contentBounds.height ||
        m_cachedViewportWidth != m_absoluteBounds.width ||
        m_cachedViewportHeight != m_absoluteBounds.height;
    const bool subtreeAnimating = m_contentWidget->hasActiveAnimationInSubtree();
    const bool needsRaster = !m_cacheValid || geometryChanged ||
        m_cachedContentPaintRevision != contentRevision ||
        m_cachedContentPresentationRevision != contentPresentationRevision;

    // A cached ScrollView layer is a retained snapshot. Rebuilding the entire
    // long content texture for every descendant animation tick defeats that
    // model. Paint only the damaged visible subtree while motion is active,
    // then refresh the stable cache once after the last presentation frame.
    if (subtreeAnimating) {
        m_cacheValid = false;
        m_contentWidget->draw(canvas, damageRect);
        endPresentation(canvas);
        return;
    }

    if (needsRaster) {
        if (canvas.beginCachedLayer(getObjectId(), presentedContentBounds)) {
            // The cached target origin cancels the ScrollView-owned content
            // translation. Scroll offset is applied only when the texture is drawn.
            m_contentWidget->draw(canvas, m_contentWidget->getPresentationBounds());
            canvas.endCachedLayer();
            m_cacheValid = true;
            m_cachedContentPaintRevision = contentRevision;
            m_cachedContentPresentationRevision = contentPresentationRevision;
            m_cachedContentWidth = contentBounds.width;
            m_cachedContentHeight = contentBounds.height;
            m_cachedViewportWidth = m_absoluteBounds.width;
            m_cachedViewportHeight = m_absoluteBounds.height;
        } else {
            m_cacheValid = false;
        }
    }

    if (!m_cacheValid ||
        !canvas.drawCachedLayer(getObjectId(), presentedContentBounds)) {
        m_cacheValid = false;
        m_contentWidget->draw(canvas, damageRect);
    }
    endPresentation(canvas);
}

float ScrollView::getMaxScrollY() const noexcept {
    return std::max(0.0f, m_contentHeight - m_bounds.height);
}

void ScrollView::setScrollY(float offset) {
    const float maxScroll = getMaxScrollY();
    const float clamped = std::clamp(offset, 0.0f, maxScroll);
    if (clamped == m_scrollY) return;
    m_scrollY = clamped;
    if (m_contentWidget) {
        m_contentWidget->setParentControlledTranslationY(-m_scrollY);
    }
    markDirty();
}

void ScrollView::clampScrollOffset() {
    const float maxScroll = getMaxScrollY();
    const float clamped = std::clamp(m_scrollY, 0.0f, maxScroll);
    if (clamped != m_scrollY) {
        setScrollY(clamped);
    } else if (m_contentWidget &&
               m_contentWidget->getPresentationState().translationY != -clamped) {
        // A replacement content widget starts with an identity presentation
        // transform even when the preserved offset is already clamped.
        m_contentWidget->setParentControlledTranslationY(-clamped);
    }
}

void ScrollView::syncLayout(float parentAbsX, float parentAbsY) {
    if (!m_contentWidget && !m_children.empty()) {
        m_contentWidget = m_children.front().get();
    }

    if (m_contentWidget) {
        m_contentWidget->getYogaNode().setFlexShrink(0.0f);
        if (!m_contentWidget->m_hasHeight) {
            m_contentWidget->getYogaNode().setHeightAuto();
        }
    }

    Widget::syncLayout(parentAbsX, parentAbsY);

    if (m_contentWidget) {
        m_contentHeight = m_contentWidget->getBounds().height;
    } else {
        m_contentHeight = 0.0f;
    }

    clampScrollOffset();
}

void ScrollView::onPointerEventPreview(const PointerEvent& event) {
    if (event.source != PointerSource::Touch) return;

    if (event.type == PointerEventType::Down) {
        if (m_touchPanState != TouchPanState::Idle || getMaxScrollY() <= 0.0f) return;
        m_touchPanState = TouchPanState::Pending;
        m_touchPointerId = event.pointerId;
        m_touchStartY = event.y;
        m_touchStartScrollY = m_scrollY;
        return;
    }

    if (m_touchPanState == TouchPanState::Idle ||
        event.pointerId != m_touchPointerId) {
        return;
    }

    if (event.type == PointerEventType::Move) {
        const float dragDistance = event.y - m_touchStartY;
        if (m_touchPanState == TouchPanState::Pending &&
            touch_interaction::exceedsSlop(0.0f, dragDistance)) {
            if (!event.capturePointer(*this)) {
                resetTouchPan();
                return;
            }
            m_touchPanState = TouchPanState::Dragging;
            event.cancelPointerDownTarget(*this);
        }
        if (m_touchPanState == TouchPanState::Dragging) {
            setScrollY(m_touchStartScrollY - dragDistance);
        }
        return;
    }

    if (event.type == PointerEventType::Up ||
        event.type == PointerEventType::Cancel) {
        if (m_touchPanState == TouchPanState::Pending) resetTouchPan();
    }
}

bool ScrollView::onPointerMove(const PointerEvent& event) {
    return event.source == PointerSource::Touch &&
        event.pointerId == m_touchPointerId &&
        m_touchPanState == TouchPanState::Dragging;
}

bool ScrollView::onPointerUp(const PointerEvent& event) {
    if (event.source != PointerSource::Touch ||
        m_touchPanState != TouchPanState::Dragging ||
        event.pointerId != m_touchPointerId) {
        return Widget::onPointerUp(event);
    }
    resetTouchPan();
    return true;
}

bool ScrollView::onPointerCancel(const PointerEvent& event) {
    if (event.source != PointerSource::Touch ||
        m_touchPanState == TouchPanState::Idle ||
        event.pointerId != m_touchPointerId) {
        return Widget::onPointerCancel(event);
    }
    resetTouchPan();
    return true;
}

void ScrollView::resetTouchPan() noexcept {
    m_touchPanState = TouchPanState::Idle;
    m_touchPointerId = 0;
    m_touchStartY = 0.0f;
    m_touchStartScrollY = 0.0f;
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
