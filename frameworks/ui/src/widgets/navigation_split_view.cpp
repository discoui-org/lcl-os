#include "lcl-ui/widgets/navigation_split_view.hpp"

#include "lcl-ui/widgets/navigation_bar.hpp"

#include <algorithm>
#include <cmath>

namespace lcl::ui {
namespace {

constexpr float kPrimaryParallax = 0.5f;
constexpr float kEdgeWidth = 28.0f;
constexpr float kGestureSlop = 9.0f;
constexpr float kCommitProgress = 0.32f;

lcl::motion::Motion navigationSpring() {
    constexpr float speed = 1.5f;
    constexpr float stiffness = 150.0f * speed * speed;
    auto motion = lcl::motion::Motion::spring(
        1.0f, stiffness, 2.0f * std::sqrt(stiffness));
    motion.springParams.settlePosEpsilon = 0.05f;
    motion.springParams.settleVelEpsilon = 0.05f * speed;
    return motion;
}

bool supportsEdgeDrag(const PointerEvent& event) {
    return event.source == PointerSource::Touch;
}

} // namespace

NavigationSplitView::NavigationSplitView() {
    setClipsToBounds(true);

    auto sidebarPane = std::make_unique<Container>();
    m_sidebarPane = sidebarPane.get();
    sidebarPane->setPositionType(layout::PositionType::Absolute);
    sidebarPane->setPosition(layout::Edge::Left, 0.0f);
    sidebarPane->setPosition(layout::Edge::Top, 0.0f);
    sidebarPane->setPosition(layout::Edge::Right, 0.0f);
    sidebarPane->setPosition(layout::Edge::Bottom, 0.0f);
    sidebarPane->setDirection(layout::Direction::Column);

    auto primaryBar = std::make_unique<NavigationBar>();
    m_primaryBar = primaryBar.get();
    primaryBar->setTitle("Settings");
    sidebarPane->addChild(std::move(primaryBar));

    auto sidebarHost = std::make_unique<Container>();
    m_sidebarHost = sidebarHost.get();
    sidebarHost->setFlexGrow(1.0f);
    sidebarHost->setDirection(layout::Direction::Column);
    sidebarPane->addChild(std::move(sidebarHost));
    addChild(std::move(sidebarPane));

    auto detail = std::make_unique<NavigationStack>();
    m_detail = detail.get();
    detail->setPositionType(layout::PositionType::Absolute);
    detail->setPosition(layout::Edge::Left, 0.0f);
    detail->setPosition(layout::Edge::Top, 0.0f);
    detail->setPosition(layout::Edge::Right, 0.0f);
    detail->setPosition(layout::Edge::Bottom, 0.0f);
    // Compact primary/detail navigation keeps both visual panes as retained
    // presentation layers.  The two layers fit the bounded navigation pool
    // and permit the primary page's -0.5W parallax without a client raster
    // frame on every spring sample.
    m_sidebarPane->setRetainedPresentationHint(true);
    detail->setRetainedPresentationHint(true);
    addChild(std::move(detail));

    applyMode();
}

NavigationSplitView::~NavigationSplitView() {
    if (m_motionCoordinator) {
        m_motionCoordinator->unregisterPresentation(getObjectId());
    }
}

void NavigationSplitView::setSidebar(std::unique_ptr<Widget> sidebar) {
    if (!sidebar) return;
    while (!m_sidebarHost->getChildren().empty()) {
        m_sidebarHost->removeChild(m_sidebarHost->getChildren().back().get());
    }
    sidebar->setFlexGrow(1.0f);
    m_sidebarHost->addChild(std::move(sidebar));
}

void NavigationSplitView::setSidebarWidth(float width) {
    const float next = std::max(160.0f, width);
    if (m_sidebarWidth == next) return;
    m_sidebarWidth = next;
    applyMode(true);
}

void NavigationSplitView::setPrimaryTitle(std::string title) {
    m_primaryBar->setTitle(std::move(title));
}

void NavigationSplitView::setSizeClass(LayoutSizeClass sizeClass) {
    if (m_sizeClass == sizeClass) return;
    finishTransition();
    resetEdgeDrag();
    m_sizeClass = sizeClass;
    applyMode();
}

bool NavigationSplitView::setDetailPage(NavigationPage page,
                                        PageTransition transition) {
    const bool wasEmpty = m_detail->pageCount() == 0;
    const bool changed = wasEmpty
        ? m_detail->setRootPage(std::move(page))
        : m_detail->reset(std::move(page),
            m_sizeClass == LayoutSizeClass::Expanded
                ? transition : PageTransition::None);
    if (!changed) return false;
    if (m_sizeClass == LayoutSizeClass::Compact) {
        presentDetail();
    }
    return true;
}

void NavigationSplitView::presentDetail() {
    if (m_sizeClass == LayoutSizeClass::Expanded ||
        m_compactDetailPresented || m_detail->pageCount() == 0) return;
    m_compactDetailPresented = true;
    m_detail->setRetainedPresentationHint(true);
    m_detail->setParentBack(m_primaryBar->title(),
                            [this] { presentPrimary(); });
    // This also covers selecting the already-current Settings route. That
    // path does not call setDetailPage(), and the offscreen compact pane has
    // therefore never been guaranteed to own an imported layer.
    animateCompactPresentation(true, true);
}

void NavigationSplitView::presentPrimary() {
    if (m_sizeClass == LayoutSizeClass::Expanded ||
        !m_compactDetailPresented) return;
    if (m_detail->pageCount() > 1) {
        m_detail->pop();
        return;
    }
    m_compactDetailPresented = false;
    // A nested NavigationStack transition temporarily gives ownership to its
    // page layers. Re-form the single detail layer before sliding the whole
    // pane back to the sidebar.
    m_detail->setRetainedPresentationHint(true);
    animateCompactPresentation(false, true);
}

void NavigationSplitView::syncLayout(float parentAbsX, float parentAbsY) {
    Container::syncLayout(parentAbsX, parentAbsY);
    if (!m_transitionActive && m_edgeState != EdgeState::Dragging) {
        applyMode(true);
    }
}

void NavigationSplitView::applyMode(bool preservePresentation) {
    const float width = std::max(1.0f, getBounds().width);
    if (m_sizeClass == LayoutSizeClass::Expanded) {
        // Expanded NavigationStack transitions retain their individual pages;
        // the split container itself is only a compact presentation boundary.
        m_sidebarPane->setRetainedPresentationHint(false);
        m_detail->setRetainedPresentationHint(false);
        m_sidebarPane->setWidth(m_sidebarWidth);
        m_detail->setPosition(layout::Edge::Left, m_sidebarWidth);
        m_primaryBar->setLayoutVisibility(LayoutVisibility::Collapsed);
        m_sidebarPane->setTranslationX(0.0f);
        m_detail->setTranslationX(0.0f);
        m_detail->setOpacity(1.0f);
        m_sidebarPane->setInteractionEnabled(true);
        m_detail->setInteractionEnabled(true);
        m_detail->clearParentBack();
        return;
    }

    // Page push/pop needs outgoing and incoming page layers independently.
    // Outside such a transition, a one-page stack can be retained as the
    // single pane used by the compact primary/detail slide.
    m_sidebarPane->setRetainedPresentationHint(true);
    m_detail->setRetainedPresentationHint(
        !m_detail->isTransitioning() && m_detail->pageCount() <= 1);
    m_sidebarPane->setWidthAuto();
    m_detail->setPosition(layout::Edge::Left, 0.0f);
    m_primaryBar->setLayoutVisibility(LayoutVisibility::Visible);
    m_detail->setParentBack(m_primaryBar->title(),
                            [this] { presentPrimary(); });
    if (preservePresentation) {
        m_sidebarPane->setTranslationX(
            m_compactDetailPresented ? -kPrimaryParallax * width : 0.0f);
        m_detail->setTranslationX(
            m_compactDetailPresented ? 0.0f : width);
        m_detail->setOpacity(1.0f);
    }
    m_sidebarPane->setInteractionEnabled(!m_compactDetailPresented);
    m_detail->setInteractionEnabled(m_compactDetailPresented);
}

void NavigationSplitView::animateCompactPresentation(bool detailPresented,
                                                      bool prepareDetail) {
    finishTransition();
    const float width = std::max(1.0f, getBounds().width);
    auto* coordinator = getMotionCoordinator();
    if (!coordinator) {
        m_sidebarPane->setTranslationX(
            detailPresented ? -kPrimaryParallax * width : 0.0f);
        m_detail->setTranslationX(detailPresented ? 0.0f : width);
        m_detail->setOpacity(1.0f);
        applyMode(true);
        return;
    }
    m_transitionActive = true;
    m_transitionTargetDetail = detailPresented;
    m_sidebarPane->setInteractionEnabled(false);
    m_detail->setInteractionEnabled(false);
    const auto lifetime = getLifetimeToken();
    coordinator->registerPresentation(*this, [lifetime, this](float) {
        if (!lifetime.expired()) tickTransition();
    });

    if (prepareDetail) {
        // Record/import the detail once at identity. On entrance it remains
        // hidden so the sidebar is still the complete visible snapshot; on
        // exit it stays opaque and replaces the independent page layers at
        // the exact same visual position.
        m_preparingDetail = true;
        m_preparationTicks = 1;
        m_detail->setTranslationX(0.0f);
        m_detail->setOpacity(detailPresented ? 0.0f : 1.0f);
        return;
    }

    startCompactAnimation();
}

void NavigationSplitView::startCompactAnimation() {
    auto* coordinator = getMotionCoordinator();
    if (!coordinator) {
        finishTransition();
        return;
    }
    const float width = std::max(1.0f, getBounds().width);
    const auto motion = navigationSpring();
    const auto sidebarLifetime = m_sidebarPane->getLifetimeToken();
    const auto detailLifetime = m_detail->getLifetimeToken();
    coordinator->animateFloat(
        *m_sidebarPane, AnimatableProperty::TranslationX,
        m_sidebarPane->getPresentationState().translationX,
        m_transitionTargetDetail ? -kPrimaryParallax * width : 0.0f, motion,
        [sidebarLifetime, sidebar = m_sidebarPane](float value) {
            if (!sidebarLifetime.expired()) sidebar->applyPresentationValue(
                AnimatableProperty::TranslationX, value);
        });
    coordinator->animateFloat(
        *m_detail, AnimatableProperty::TranslationX,
        m_detail->getPresentationState().translationX,
        m_transitionTargetDetail ? 0.0f : width, motion,
        [detailLifetime, detail = m_detail](float value) {
            if (!detailLifetime.expired()) detail->applyPresentationValue(
                AnimatableProperty::TranslationX, value);
        });
    // An unavailable/rejected retained cache is handled by the compositor
    // delegate as a one-shot final-state commit. In that path there will be no
    // PresentationAnimationResult callback, so complete ownership now rather
    // than leaving both panes input-disabled indefinitely.
    if (!coordinator->isObjectAnimating(m_sidebarPane->getObjectId()) &&
        !coordinator->isObjectAnimating(m_detail->getObjectId())) {
        finishTransition();
    }
}

void NavigationSplitView::tickTransition() {
    if (!m_transitionActive) return;
    if (m_preparingDetail) {
        if (m_preparationTicks > 0) {
            --m_preparationTicks;
            return;
        }
        m_preparingDetail = false;
        if (m_transitionTargetDetail) {
            const float width = std::max(1.0f, getBounds().width);
            // Frame A recorded the detail cache at identity and opacity zero.
            // Publish Frame B at the offscreen visible starting position; the
            // following FramePresented is the first safe point to animate it.
            m_detail->setTranslationX(width);
            m_detail->setOpacity(1.0f);
            m_positioningDetail = true;
            return;
        }
        startCompactAnimation();
        return;
    }
    if (m_positioningDetail) {
        m_positioningDetail = false;
        startCompactAnimation();
        return;
    }
    auto* coordinator = getMotionCoordinator();
    if (!coordinator ||
        (!coordinator->isObjectAnimating(m_sidebarPane->getObjectId()) &&
         !coordinator->isObjectAnimating(m_detail->getObjectId()))) {
        finishTransition();
    }
}

void NavigationSplitView::finishTransition() {
    if (!m_transitionActive) return;
    m_transitionActive = false;
    m_preparingDetail = false;
    m_positioningDetail = false;
    m_preparationTicks = 0;
    if (m_motionCoordinator) {
        m_motionCoordinator->unregisterPresentation(getObjectId());
    }
    m_compactDetailPresented = m_transitionTargetDetail;
    m_detail->setOpacity(1.0f);
    applyMode(true);
}

void NavigationSplitView::onPointerEventPreview(const PointerEvent& event) {
    if (!supportsEdgeDrag(event) ||
        m_sizeClass != LayoutSizeClass::Compact) return;
    if (event.type == PointerEventType::Down) {
        if (!m_compactDetailPresented || m_transitionActive ||
            m_detail->pageCount() > 1 ||
            event.x - getAbsoluteBounds().x > kEdgeWidth) return;
        m_edgeState = EdgeState::Pending;
        m_edgePointerId = event.pointerId;
        m_edgeStartX = event.x;
        m_edgeStartY = event.y;
        m_edgeProgress = 0.0f;
        return;
    }
    if (m_edgeState == EdgeState::Idle ||
        event.pointerId != m_edgePointerId) return;
    if (event.type == PointerEventType::Move) {
        const float dx = event.x - m_edgeStartX;
        const float dy = event.y - m_edgeStartY;
        if (m_edgeState == EdgeState::Pending) {
            if (dx < kGestureSlop || dx <= std::abs(dy)) return;
            if (!event.capturePointer(*this)) {
                resetEdgeDrag();
                return;
            }
            event.cancelPointerDownTarget(*this);
            m_edgeState = EdgeState::Dragging;
            m_sidebarPane->setInteractionEnabled(false);
            m_detail->setInteractionEnabled(false);
        }
        if (m_edgeState == EdgeState::Dragging) updateEdgeDrag(event.x);
        return;
    }
    if (event.type == PointerEventType::Up ||
        event.type == PointerEventType::Cancel) {
        if (m_edgeState == EdgeState::Dragging) {
            event.releasePointerCapture(*this);
            finishEdgeDrag(event.type == PointerEventType::Up &&
                           m_edgeProgress >= kCommitProgress);
        } else {
            resetEdgeDrag();
        }
    }
}

bool NavigationSplitView::onPointerMove(const PointerEvent& event) {
    return supportsEdgeDrag(event) && m_edgeState == EdgeState::Dragging &&
        event.pointerId == m_edgePointerId;
}

bool NavigationSplitView::onPointerUp(const PointerEvent& event) {
    return supportsEdgeDrag(event) && event.pointerId == m_edgePointerId;
}

bool NavigationSplitView::onPointerCancel(const PointerEvent& event) {
    if (!supportsEdgeDrag(event) || event.pointerId != m_edgePointerId) {
        return false;
    }
    if (m_edgeState == EdgeState::Dragging) finishEdgeDrag(false);
    else resetEdgeDrag();
    return true;
}

void NavigationSplitView::updateEdgeDrag(float x) {
    const float width = std::max(1.0f, getBounds().width);
    const float dx = std::clamp(x - m_edgeStartX, 0.0f, width);
    m_edgeProgress = dx / width;
    auto* coordinator = getMotionCoordinator();
    const float current = m_detail->getPresentationState().translationX;
    const auto lifetime = m_detail->getLifetimeToken();
    const bool submitted = coordinator && coordinator->updateCompositorFloat(
        *m_detail, AnimatableProperty::TranslationX, current, dx,
        [lifetime, detail = m_detail](float value) {
            if (!lifetime.expired()) detail->applyPresentationValue(
                AnimatableProperty::TranslationX, value);
        });
    if (!submitted) m_detail->setTranslationX(dx);
    const float primaryTarget =
        -kPrimaryParallax * width * (1.0f - m_edgeProgress);
    const float primaryCurrent =
        m_sidebarPane->getPresentationState().translationX;
    const auto primaryLifetime = m_sidebarPane->getLifetimeToken();
    const bool primarySubmitted = coordinator && coordinator->updateCompositorFloat(
        *m_sidebarPane, AnimatableProperty::TranslationX,
        primaryCurrent, primaryTarget,
        [primaryLifetime, sidebar = m_sidebarPane](float value) {
            if (!primaryLifetime.expired()) sidebar->applyPresentationValue(
                AnimatableProperty::TranslationX, value);
        });
    if (!primarySubmitted) m_sidebarPane->setTranslationX(primaryTarget);
}

void NavigationSplitView::finishEdgeDrag(bool commit) {
    resetEdgeDrag();
    if (commit) m_compactDetailPresented = false;
    animateCompactPresentation(!commit);
}

void NavigationSplitView::resetEdgeDrag() noexcept {
    m_edgeState = EdgeState::Idle;
    m_edgePointerId = 0;
    m_edgeStartX = 0.0f;
    m_edgeStartY = 0.0f;
    m_edgeProgress = 0.0f;
}

} // namespace lcl::ui
