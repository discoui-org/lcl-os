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

void addUpdateRegion(std::vector<graphics::RectF>& regions,
                     const graphics::RectF& candidate) {
    if (candidate.isEmpty()) return;
    graphics::RectF merged = candidate;
    for (auto it = regions.begin(); it != regions.end();) {
        if (!merged.intersects(*it)) {
            ++it;
            continue;
        }
        merged = merged.unionWith(*it);
        it = regions.erase(it);
    }
    regions.push_back(merged);
}

} // namespace

ScrollView::ScrollView() {
    setClipsToBounds(true);
    setAlignItems(layout::Align::Stretch);
}

void ScrollView::invalidateRetainedCache() noexcept {
    m_cacheValid = false;
    m_cachedSubtreeAnimating = false;
    m_cachedAnimationBounds.reset();
    m_cachedContentPaintRevision = 0;
    m_cachedContentPresentationRevision = 0;
    m_lastCacheUpdatePassSerial = 0;
    m_cachedContentX = 0.0f;
    m_cachedContentY = 0.0f;
    m_cachedContentWidth = 0.0f;
    m_cachedContentHeight = 0.0f;
    m_cachedViewportWidth = 0.0f;
    m_cachedViewportHeight = 0.0f;
    m_cachedDeviceScale = 0.0f;
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

    beginPresentation(canvas, damageRect);
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
    const float deviceScale =
        std::max(0.001f, canvas.renderTarget().deviceScale);
    const bool geometryChanged =
        m_cachedContentX != contentBounds.x ||
        m_cachedContentY != contentBounds.y ||
        m_cachedContentWidth != contentBounds.width ||
        m_cachedContentHeight != contentBounds.height ||
        m_cachedViewportWidth != m_absoluteBounds.width ||
        m_cachedViewportHeight != m_absoluteBounds.height ||
        std::fabs(m_cachedDeviceScale - deviceScale) > 0.0001f;
    const bool subtreeAnimating = m_contentWidget->hasActiveAnimationInSubtree();
    const bool needsRaster = !m_cacheValid || geometryChanged ||
        paintChanged || presentationChanged;
    const bool framePassActive = m_renderPass && m_renderPass->isInPass();
    const uint64_t framePassSerial = framePassActive
        ? m_renderPass->passSerial()
        : 0;
    const bool cacheUpdateProcessedForPass = framePassActive &&
        m_lastCacheUpdatePassSerial == framePassSerial;

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
    if (m_cacheValid && !geometryChanged &&
        !cacheUpdateProcessedForPass &&
        (paintChanged || presentationChanged || animationUpdate)) {
        const float rasterOutset = 1.0f / deviceScale;

        std::vector<graphics::RectF> presentationUpdateRegions;
        const graphics::RectF visibleContentBounds = getPresentationBounds()
            .intersection(presentedContentBounds);

        // Paint and non-procedural presentation revisions can arrive as
        // several independent damage rects in one immutable frame (for
        // example a Slider plus its value Text). Process the complete pass
        // before acknowledging the shared content revision, otherwise later
        // regions would keep stale cache pixels.
        if (paintChanged || presentationChanged) {
            if (framePassActive && !m_renderPass->frameDamageRects().empty()) {
                for (const graphics::RectF& frameDamage :
                     m_renderPass->frameDamageRects()) {
                    addUpdateRegion(
                        presentationUpdateRegions,
                        frameDamage.intersection(visibleContentBounds));
                }
            } else {
                addUpdateRegion(
                    presentationUpdateRegions,
                    damageRect.intersection(visibleContentBounds));
            }
        }

        if (animationCacheDamage) {
            const graphics::RectF positioned = positionedAt(
                *animationCacheDamage, presentedContentBounds);
            addUpdateRegion(presentationUpdateRegions, graphics::RectF{
                positioned.x - rasterOutset,
                positioned.y - rasterOutset,
                positioned.width + rasterOutset * 2.0f,
                positioned.height + rasterOutset * 2.0f,
            }.intersection(visibleContentBounds));
        }

        if (presentationUpdateRegions.empty()) {
            // All changes are clipped outside this viewport. Preserve the
            // stable cache and leave its revisions stale; scrolling the region
            // into view supplies damage that catches it up without rebuilding
            // the complete long content layer.
        } else {
            bool allUpdatesSucceeded = true;
            for (const graphics::RectF& presentationUpdateBounds :
                 presentationUpdateRegions) {
                const graphics::RectF cacheUpdateBounds = positionedAt(
                    relativeTo(presentationUpdateBounds,
                               presentedContentBounds),
                    contentBounds);
                if (!canvas.beginCachedLayerUpdate(
                        getObjectId(), contentBounds, cacheUpdateBounds)) {
                    allUpdatesSucceeded = false;
                    break;
                }
                // The cache always uses stable unscrolled content coordinates.
                // Cancel the content widget's parent-controlled scroll
                // translation only while rasterizing into that cache.
                canvas.concatTransform(
                    graphics::Matrix3::translation(0.0f, m_scrollY));
                m_contentWidget->draw(canvas, presentationUpdateBounds);
                canvas.endCachedLayer();
            }

            if (allUpdatesSucceeded) {
                m_cachedContentPaintRevision = contentRevision;
                m_cachedContentPresentationRevision =
                    contentPresentationRevision;
            } else {
                // Preserve the old direct-paint fallback for Canvas backends
                // that do not implement retained partial layer updates.
                m_cacheValid = false;
                m_contentWidget->draw(canvas, damageRect);
                endPresentation(canvas);
                return;
            }
        }
        if (framePassActive) {
            m_lastCacheUpdatePassSerial = framePassSerial;
        }
    } else if (subtreeAnimating && !m_cacheValid) {
        m_contentWidget->draw(canvas, damageRect);
        endPresentation(canvas);
        return;
    }

    const bool needsFullRaster = !m_cacheValid || geometryChanged;
    if (needsFullRaster) {
        if (canvas.beginCachedLayer(getObjectId(), contentBounds)) {
            // Cache geometry is stable across scrolling. Cancel the content
            // presentation translation while populating it; scroll offset is
            // applied only by drawCachedLayer's destination.
            canvas.concatTransform(
                graphics::Matrix3::translation(0.0f, m_scrollY));
            m_contentWidget->draw(canvas, m_contentWidget->getPresentationBounds());
            canvas.endCachedLayer();
            m_cacheValid = true;
            m_cachedContentPaintRevision = contentRevision;
            m_cachedContentPresentationRevision = contentPresentationRevision;
            m_cachedContentX = contentBounds.x;
            m_cachedContentY = contentBounds.y;
            m_cachedContentWidth = contentBounds.width;
            m_cachedContentHeight = contentBounds.height;
            m_cachedViewportWidth = m_absoluteBounds.width;
            m_cachedViewportHeight = m_absoluteBounds.height;
            m_cachedDeviceScale = deviceScale;
            if (framePassActive) {
                m_lastCacheUpdatePassSerial = framePassSerial;
            }
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
    // Scrolling changes only the retained content-node transform. Keep the
    // viewport damaged for the current DisplayList path without advancing a
    // paint revision or invalidating the cached content raster.
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
