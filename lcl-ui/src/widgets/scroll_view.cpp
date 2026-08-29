#include "lcl-ui/widgets/scroll_view.hpp"

namespace lcl::ui {

namespace {

std::optional<graphics::RectF> activeAnimationPaintBounds(const Widget& widget) {
    const MotionCoordinator* coordinator = widget.getMotionCoordinator();
    if (coordinator && coordinator->isObjectAnimating(widget.getObjectId())) {
        const graphics::RectF bounds = widget.getPresentationSubtreePaintBounds();
        if (!bounds.isEmpty()) return bounds;
        return std::nullopt;
    }

    std::optional<graphics::RectF> result;
    for (const auto& child : widget.getChildren()) {
        if (!child->isVisible()) continue;
        const auto childBounds = activeAnimationPaintBounds(*child);
        if (!childBounds) continue;
        result = result ? result->unionWith(*childBounds) : childBounds;
    }
    return result;
}

graphics::RectF relativeTo(const graphics::RectF& rect,
                           const graphics::RectF& origin) {
    return {
        rect.x - origin.x,
        rect.y - origin.y,
        rect.width,
        rect.height,
    };
}

graphics::RectF positionedAt(const graphics::RectF& rect,
                             const graphics::RectF& origin) {
    return {
        rect.x + origin.x,
        rect.y + origin.y,
        rect.width,
        rect.height,
    };
}

} // namespace

ScrollView::ScrollView() {
    setClipsToBounds(true);
    setAlignItems(layout::Align::Stretch);
}

void ScrollView::setContent(std::unique_ptr<Widget> content) {
    m_cacheValid = false;
    m_cachedSubtreeAnimating = false;
    m_cachedAnimationBounds.reset();
    if (m_contentWidget) {
        removeChild(m_contentWidget);
        m_contentWidget = nullptr;
    }
    if (!content) return;
    m_contentWidget = content.get();
    m_contentWidget->setFlexShrink(0.0f);
    if (!m_contentWidget->m_hasHeight) {
        m_contentWidget->setHeightAuto();
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
    const bool paintChanged =
        m_cachedContentPaintRevision != contentRevision;
    const bool presentationChanged =
        m_cachedContentPresentationRevision != contentPresentationRevision;
    const bool geometryChanged =
        m_cachedContentWidth != contentBounds.width ||
        m_cachedContentHeight != contentBounds.height ||
        m_cachedViewportWidth != m_absoluteBounds.width ||
        m_cachedViewportHeight != m_absoluteBounds.height;
    const bool subtreeAnimating = m_contentWidget->hasActiveAnimationInSubtree();
    const bool needsRaster = !m_cacheValid || geometryChanged ||
        paintChanged || presentationChanged;

    const bool wasSubtreeAnimating = m_cachedSubtreeAnimating;
    const bool animationUpdate = subtreeAnimating ||
        (wasSubtreeAnimating && needsRaster);
    const auto activeBounds = activeAnimationPaintBounds(*m_contentWidget);
    const auto activeCacheBounds = activeBounds
        ? std::optional<graphics::RectF>{relativeTo(*activeBounds,
                                                   presentedContentBounds)}
        : std::nullopt;
    std::optional<graphics::RectF> animationCacheDamage = activeCacheBounds;
    if (m_cachedAnimationBounds) {
        animationCacheDamage = animationCacheDamage
            ? animationCacheDamage->unionWith(*m_cachedAnimationBounds)
            : m_cachedAnimationBounds;
    }
    m_cachedSubtreeAnimating = subtreeAnimating;
    m_cachedAnimationBounds = activeCacheBounds;
    bool deferredOffscreenPresentationUpdate = false;
    if (m_cacheValid && !geometryChanged && animationUpdate) {
        const float rasterOutset = 1.0f /
            std::max(0.001f, canvas.renderTarget().deviceScale);
        graphics::RectF updateBounds{};
        if (animationCacheDamage) {
            const graphics::RectF positioned = positionedAt(
                *animationCacheDamage, presentedContentBounds);
            updateBounds = graphics::RectF{
                positioned.x - rasterOutset,
                positioned.y - rasterOutset,
                positioned.width + rasterOutset * 2.0f,
                positioned.height + rasterOutset * 2.0f,
            };
        }
        updateBounds = updateBounds
            .intersection(getPresentationBounds())
            .intersection(presentedContentBounds);
        if (updateBounds.isEmpty() && subtreeAnimating &&
            presentationChanged && !paintChanged) {
            // The procedural presentation is outside the viewport. Keep the
            // stable cached pixels and leave its presentation revision stale;
            // once it becomes visible, the normal partial-update path catches
            // up only that widget's bounds.
            deferredOffscreenPresentationUpdate = true;
        } else if (!updateBounds.isEmpty() &&
            canvas.beginCachedLayerUpdate(
                getObjectId(), presentedContentBounds, updateBounds)) {
            m_contentWidget->draw(canvas, updateBounds);
            canvas.endCachedLayer();
            m_cachedContentPaintRevision = contentRevision;
            m_cachedContentPresentationRevision = contentPresentationRevision;
        } else if (!updateBounds.isEmpty()) {
            // Preserve the old direct-paint fallback for Canvas backends that
            // do not implement retained partial layer updates.
            m_cacheValid = false;
            m_contentWidget->draw(canvas, damageRect);
            endPresentation(canvas);
            return;
        }
    } else if (subtreeAnimating && !m_cacheValid) {
        m_contentWidget->draw(canvas, damageRect);
        endPresentation(canvas);
        return;
    }

    const bool needsFullRaster = !deferredOffscreenPresentationUpdate &&
        (!m_cacheValid || geometryChanged || paintChanged || presentationChanged);
    if (needsFullRaster) {
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
    const graphics::RectF previousViewport = getVisiblePresentationPaintBounds();
    m_scrollY = clamped;
    if (m_contentWidget) {
        m_contentWidget->setParentControlledTranslationY(-m_scrollY);
    }
    invalidatePresentation(previousViewport);
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
        m_contentWidget->setFlexShrink(0.0f);
        if (!m_contentWidget->m_hasHeight) {
            m_contentWidget->setHeightAuto();
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
