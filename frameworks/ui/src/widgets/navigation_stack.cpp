#include "lcl-ui/widgets/navigation_stack.hpp"

#include "lcl-ui/widgets/external_buffer.hpp"
#include "lcl-ui/widgets/scroll_view.hpp"

#include <algorithm>
#include <cmath>

namespace lcl::ui {
namespace {

lcl::motion::Motion pageSpring() {
    // Match the critically damped position spring used by the compositor's
    // opening app-launch morph (including its 1.5x time compression).
    constexpr float speed = 1.5f;
    constexpr float stiffness = 150.0f * speed * speed;
    auto motion = lcl::motion::Motion::spring(
        1.0f, stiffness, 2.0f * std::sqrt(stiffness));
    motion.springParams.settlePosEpsilon = 0.05f;
    motion.springParams.settleVelEpsilon = 0.05f * speed;
    return motion;
}

constexpr float kBackParallax = 0.5f;
constexpr float kEdgeActivationWidth = 24.0f;
constexpr float kGestureSlop = 9.0f;
constexpr float kCommitProgress = 0.32f;

bool supportsEdgeDrag(const PointerEvent& event) {
    return event.source == PointerSource::Touch;
}

bool supportsRetainedPagePresentation(const Widget& widget) {
    if (dynamic_cast<const ScrollView*>(&widget) ||
        dynamic_cast<const ExternalBufferView*>(&widget)) {
        return false;
    }
    return std::all_of(
        widget.getChildren().begin(), widget.getChildren().end(),
        [](const auto& child) {
            return child && supportsRetainedPagePresentation(*child);
        });
}

} // namespace

NavigationStack::NavigationStack() {
    setDirection(layout::Direction::Column);
    setClipsToBounds(true);

    auto bar = std::make_unique<NavigationBar>();
    m_navigationBar = bar.get();
    bar->setOnBack([this] {
        if (m_entries.size() > 1) {
            pop();
        } else if (m_onParentBack) {
            m_onParentBack();
        }
    });
    addChild(std::move(bar));

    auto viewport = std::make_unique<Container>();
    m_viewport = viewport.get();
    viewport->setFlexGrow(1.0f);
    viewport->setClipsToBounds(true);
    addChild(std::move(viewport));
}

NavigationStack::~NavigationStack() {
    if (m_motionCoordinator) {
        m_motionCoordinator->unregisterPresentation(getObjectId());
    }
}

Widget* NavigationStack::mountPage(NavigationPage& page) {
    if (!page.content) return nullptr;
    Widget* content = page.content.get();
    content->setRetainedPresentationHint(
        supportsRetainedPagePresentation(*content));
    content->setPositionType(layout::PositionType::Absolute);
    content->setPosition(layout::Edge::Left, 0.0f);
    content->setPosition(layout::Edge::Top, 0.0f);
    content->setPosition(layout::Edge::Right, 0.0f);
    content->setPosition(layout::Edge::Bottom, 0.0f);
    m_viewport->addChild(std::move(page.content));
    return content;
}

bool NavigationStack::setRootPage(NavigationPage page) {
    if (!m_entries.empty() || !page.content || page.route.value.empty()) {
        return false;
    }
    Widget* content = mountPage(page);
    if (!content) return false;
    m_entries.push_back({std::move(page.route), std::move(page.title), content});
    updateRetainedPageHints();
    updateBar();
    notifyRouteChanged();
    return true;
}

bool NavigationStack::push(NavigationPage page, PageTransition transition) {
    if (m_entries.empty() || !page.content || page.route.value.empty()) return false;
    finishActiveTransition();
    Widget* outgoing = m_entries.back().content;
    Widget* incoming = mountPage(page);
    if (!incoming) return false;
    m_entries.push_back({std::move(page.route), std::move(page.title), incoming});
    updateRetainedPageHints();
    updateBar();
    notifyRouteChanged();
    beginTransition(transition, outgoing, incoming);
    return true;
}

bool NavigationStack::pop(PageTransition transition) {
    if (m_entries.size() < 2) return false;
    finishActiveTransition();
    Widget* outgoing = m_entries.back().content;
    m_entries.pop_back();
    Widget* incoming = m_entries.back().content;
    updateRetainedPageHints();
    incoming->setVisible(true);
    incoming->setInteractionEnabled(true);
    updateBar();
    notifyRouteChanged();
    beginTransition(transition, outgoing, incoming);
    return true;
}

bool NavigationStack::replace(NavigationPage page, PageTransition transition) {
    if (m_entries.empty() || !page.content || page.route.value.empty()) return false;
    finishActiveTransition();
    Widget* outgoing = m_entries.back().content;
    Widget* incoming = mountPage(page);
    if (!incoming) return false;
    m_entries.pop_back();
    m_entries.push_back({std::move(page.route), std::move(page.title), incoming});
    updateRetainedPageHints();
    updateBar();
    notifyRouteChanged();
    beginTransition(transition, outgoing, incoming);
    return true;
}

bool NavigationStack::reset(NavigationPage page, PageTransition transition) {
    if (m_entries.empty()) return setRootPage(std::move(page));
    if (!page.content || page.route.value.empty()) return false;
    finishActiveTransition();
    resetEdgeSwipe();

    Widget* outgoing = m_entries.back().content;
    for (size_t index = 0; index + 1 < m_entries.size(); ++index) {
        m_viewport->removeChild(m_entries[index].content);
    }
    m_entries.clear();
    Widget* incoming = mountPage(page);
    if (!incoming) return false;
    m_entries.push_back({std::move(page.route), std::move(page.title), incoming});
    updateRetainedPageHints();
    updateBar();
    notifyRouteChanged();

    if (transition == PageTransition::None) {
        m_viewport->removeChild(outgoing);
        incoming->setVisible(true);
        incoming->setInteractionEnabled(true);
        incoming->setTranslationX(0.0f);
        incoming->setOpacity(1.0f);
    } else {
        beginTransition(PageTransition::Replace, outgoing, incoming);
    }
    return true;
}

const NavigationRoute* NavigationStack::currentRoute() const noexcept {
    return m_entries.empty() ? nullptr : &m_entries.back().route;
}

void NavigationStack::setParentBack(std::string title,
                                    std::function<void()> callback) {
    m_parentBackTitle = std::move(title);
    m_onParentBack = std::move(callback);
    updateBar();
}

void NavigationStack::clearParentBack() {
    m_parentBackTitle.clear();
    m_onParentBack = {};
    updateBar();
}

void NavigationStack::updateBar() {
    if (m_entries.empty()) {
        m_navigationBar->setTitle({});
        m_navigationBar->setBackTitle({});
        m_navigationBar->setCanGoBack(false);
        return;
    }
    m_navigationBar->setTitle(m_entries.back().title);
    const bool canGoBack = m_entries.size() > 1 ||
        static_cast<bool>(m_onParentBack);
    m_navigationBar->setCanGoBack(canGoBack);
    m_navigationBar->setBackTitle(
        m_entries.size() > 1
            ? m_entries[m_entries.size() - 2].title
            : m_parentBackTitle);
}

void NavigationStack::notifyRouteChanged() {
    if (m_onRouteChanged && !m_entries.empty()) {
        m_onRouteChanged(m_entries.back().route);
    }
}

void NavigationStack::beginTransition(PageTransition kind, Widget* outgoing,
                                      Widget* incoming) {
    if (!outgoing || !incoming || kind == PageTransition::None) {
        if (outgoing && outgoing != incoming) {
            if (kind == PageTransition::Pop || kind == PageTransition::Replace)
                m_viewport->removeChild(outgoing);
            else {
                // Keep stacked pages mounted and painted behind the active
                // page. Revealing a previously hidden retained/scroll subtree
                // can otherwise reference raster cache content that was
                // discarded while it was absent from the render tree.
                outgoing->setVisible(true);
                outgoing->setInteractionEnabled(false);
            }
        }
        if (incoming) {
            incoming->setVisible(true);
            incoming->setInteractionEnabled(true);
            incoming->setTranslationX(0.0f);
            incoming->setOpacity(1.0f);
        }
        return;
    }

    const float width = std::max(1.0f, m_viewport->getBounds().width);
    outgoing->setInteractionEnabled(false);
    incoming->setInteractionEnabled(true);
    incoming->setVisible(true);

    if (kind == PageTransition::Push) {
        if (incoming->retainedPresentationHint()) {
            // Record the new page into its retained layer while it is
            // invisible but still within the viewport. The spring starts
            // after that frame is presented, avoiding a full page raster
            // during the visible slide.
            incoming->setTranslationX(0.0f);
            incoming->setOpacity(0.0f);
            beginSpringTransition(kind, outgoing, incoming, incoming, false,
                                  -kBackParallax * width, 0.0f, true, width);
        } else {
            incoming->setTranslationX(width);
            beginSpringTransition(kind, outgoing, incoming, incoming, false,
                                  -kBackParallax * width, 0.0f);
        }
    } else if (kind == PageTransition::Pop) {
        incoming->setTranslationX(-kBackParallax * width);
        beginSpringTransition(kind, outgoing, incoming, incoming, true,
                              width, 0.0f);
    } else {
        incoming->setOpacity(0.0f);
        const auto motion = pageSpring();
        auto* coordinator = getMotionCoordinator();
        const auto outgoingLifetime = outgoing->getLifetimeToken();
        const auto incomingLifetime = incoming->getLifetimeToken();
        if (!coordinator) {
            outgoing->setOpacity(0.0f);
            incoming->setOpacity(1.0f);
            m_viewport->removeChild(outgoing);
            return;
        }
        coordinator->animateFloat(
            *outgoing, AnimatableProperty::Opacity,
            outgoing->getPresentationState().opacity, 0.0f, motion,
            [outgoingLifetime, outgoing](float value) {
                if (!outgoingLifetime.expired())
                    outgoing->applyPresentationValue(
                        AnimatableProperty::Opacity, value);
            });
        coordinator->animateFloat(
            *incoming, AnimatableProperty::Opacity,
            incoming->getPresentationState().opacity, 1.0f, motion,
            [incomingLifetime, incoming](float value) {
                if (!incomingLifetime.expired())
                    incoming->applyPresentationValue(
                        AnimatableProperty::Opacity, value);
            });
        beginSpringTransition(kind, outgoing, incoming, incoming, true,
                              outgoing->getPresentationState().translationX,
                              incoming->getPresentationState().translationX);
    }
}

void NavigationStack::beginSpringTransition(
        PageTransition kind, Widget* outgoing, Widget* incoming,
        Widget* activeAfterCompletion, bool removeOutgoing,
        float outgoingTarget, float incomingTarget,
        bool preparePush, float preparedIncomingStart) {
    auto* coordinator = getMotionCoordinator();
    if (!coordinator) {
        outgoing->setTranslationX(outgoingTarget);
        incoming->setTranslationX(incomingTarget);
        incoming->setOpacity(1.0f);
        if (removeOutgoing) m_viewport->removeChild(outgoing);
        activeAfterCompletion->setInteractionEnabled(true);
        return;
    }
    const uint64_t generation = m_nextTransitionGeneration++;
    m_transition = {
        .active = true,
        .kind = kind,
        .outgoing = outgoing,
        .incoming = incoming,
        .activeAfterCompletion = activeAfterCompletion,
        .removeOutgoing = removeOutgoing,
        .preparingPush = preparePush,
        .preparationTicks = preparePush ? 1u : 0u,
        .outgoingTarget = outgoingTarget,
        .incomingTarget = incomingTarget,
        .preparedIncomingStart = preparedIncomingStart,
        .generation = generation,
    };
    const auto lifetime = getLifetimeToken();
    coordinator->registerPresentation(*this, [lifetime, this, generation](float) {
        if (!lifetime.expired()) tickTransition(generation);
    });
    if (preparePush) return;
    startSpringAnimations(generation);
}

void NavigationStack::startSpringAnimations(uint64_t generation) {
    if (!m_transition.active || m_transition.generation != generation) return;
    auto* coordinator = getMotionCoordinator();
    if (!coordinator) {
        completeTransition(generation);
        return;
    }
    Widget* outgoing = m_transition.outgoing;
    Widget* incoming = m_transition.incoming;
    const auto motion = pageSpring();
    const auto outgoingLifetime = outgoing->getLifetimeToken();
    const auto incomingLifetime = incoming->getLifetimeToken();
    coordinator->animateFloat(
        *outgoing, AnimatableProperty::TranslationX,
        outgoing->getPresentationState().translationX,
        m_transition.outgoingTarget, motion,
        [outgoingLifetime, outgoing](float value) {
            if (!outgoingLifetime.expired())
                outgoing->applyPresentationValue(
                    AnimatableProperty::TranslationX, value);
        });
    coordinator->animateFloat(
        *incoming, AnimatableProperty::TranslationX,
        incoming->getPresentationState().translationX,
        m_transition.incomingTarget, motion,
        [incomingLifetime, incoming](float value) {
            if (!incomingLifetime.expired())
                incoming->applyPresentationValue(
                    AnimatableProperty::TranslationX, value);
        });
}

void NavigationStack::tickTransition(uint64_t generation) {
    if (!m_transition.active || m_transition.generation != generation) return;
    if (m_transition.preparingPush) {
        if (m_transition.preparationTicks > 0) {
            --m_transition.preparationTicks;
            return;
        }
        m_transition.preparingPush = false;
        if (m_transition.incoming) {
            m_transition.incoming->setTranslationX(
                m_transition.preparedIncomingStart);
            m_transition.incoming->setOpacity(1.0f);
        }
        startSpringAnimations(generation);
        return;
    }
    auto* coordinator = getMotionCoordinator();
    if (!coordinator ||
        (!coordinator->isObjectAnimating(m_transition.outgoing->getObjectId()) &&
         !coordinator->isObjectAnimating(m_transition.incoming->getObjectId()))) {
        completeTransition(generation);
    }
}

void NavigationStack::completeTransition(uint64_t generation) {
    if (!m_transition.active || m_transition.generation != generation) return;
    Widget* outgoing = m_transition.outgoing;
    Widget* incoming = m_transition.incoming;
    Widget* active = m_transition.activeAfterCompletion;
    const bool removeOutgoing = m_transition.removeOutgoing;
    m_transition.active = false;
    if (m_motionCoordinator) {
        m_motionCoordinator->unregisterPresentation(getObjectId());
    }
    if (incoming) {
        incoming->setTranslationX(0.0f);
        incoming->setOpacity(1.0f);
        incoming->setInteractionEnabled(incoming == active);
    }
    if (!outgoing || outgoing == incoming) {
        updateRetainedPageHints();
        return;
    }
    outgoing->setTranslationX(0.0f);
    outgoing->setOpacity(1.0f);
    if (!removeOutgoing) {
        outgoing->setVisible(true);
        outgoing->setInteractionEnabled(outgoing == active);
    } else {
        m_viewport->removeChild(outgoing);
    }
    updateRetainedPageHints();
}

void NavigationStack::updateRetainedPageHints() {
    const size_t firstRetained = m_entries.size() > 2
        ? m_entries.size() - 2 : 0;
    for (size_t index = 0; index < m_entries.size(); ++index) {
        if (m_entries[index].content) {
            m_entries[index].content->setRetainedPresentationHint(
                index >= firstRetained && supportsRetainedPagePresentation(
                    *m_entries[index].content));
        }
    }
}

void NavigationStack::finishActiveTransition() {
    if (!m_transition.active) return;
    const uint64_t generation = m_transition.generation;
    if (m_transition.outgoing) m_transition.outgoing->setTranslationX(0.0f);
    if (m_transition.incoming) m_transition.incoming->setTranslationX(0.0f);
    completeTransition(generation);
}

void NavigationStack::onPointerEventPreview(const PointerEvent& event) {
    if (!supportsEdgeDrag(event)) return;
    if (event.type == PointerEventType::Down) {
        if (m_entries.size() < 2 || m_transition.active ||
            event.x - getAbsoluteBounds().x > kEdgeActivationWidth) {
            return;
        }
        m_edgeSwipe = {
            .state = EdgeSwipeState::Pending,
            .pointerId = event.pointerId,
            .startX = event.x,
            .startY = event.y,
        };
        return;
    }
    if (m_edgeSwipe.state == EdgeSwipeState::Idle ||
        event.pointerId != m_edgeSwipe.pointerId) return;
    if (event.type == PointerEventType::Move) {
        const float dx = event.x - m_edgeSwipe.startX;
        const float dy = event.y - m_edgeSwipe.startY;
        if (m_edgeSwipe.state == EdgeSwipeState::Pending) {
            if (dx < kGestureSlop || dx <= std::abs(dy)) return;
            if (!event.capturePointer(*this)) {
                resetEdgeSwipe();
                return;
            }
            event.cancelPointerDownTarget(*this);
            m_edgeSwipe.state = EdgeSwipeState::Dragging;
            m_edgeSwipe.outgoing = m_entries.back().content;
            m_edgeSwipe.incoming = m_entries[m_entries.size() - 2].content;
            m_edgeSwipe.incoming->setVisible(true);
            m_edgeSwipe.incoming->setInteractionEnabled(false);
            m_edgeSwipe.outgoing->setInteractionEnabled(false);
        }
        if (m_edgeSwipe.state == EdgeSwipeState::Dragging) {
            updateInteractivePop(event.x);
        }
        return;
    }
    if (event.type == PointerEventType::Up ||
        event.type == PointerEventType::Cancel) {
        if (m_edgeSwipe.state == EdgeSwipeState::Dragging) {
            event.releasePointerCapture(*this);
            finishInteractivePop(
                event.type == PointerEventType::Up &&
                m_edgeSwipe.progress >= kCommitProgress);
        } else {
            resetEdgeSwipe();
        }
    }
}

bool NavigationStack::onPointerMove(const PointerEvent& event) {
    return supportsEdgeDrag(event) &&
        m_edgeSwipe.state == EdgeSwipeState::Dragging &&
        event.pointerId == m_edgeSwipe.pointerId;
}

bool NavigationStack::onPointerUp(const PointerEvent& event) {
    return supportsEdgeDrag(event) &&
        event.pointerId == m_edgeSwipe.pointerId;
}

bool NavigationStack::onPointerCancel(const PointerEvent& event) {
    if (!supportsEdgeDrag(event) ||
        event.pointerId != m_edgeSwipe.pointerId) return false;
    if (m_edgeSwipe.state == EdgeSwipeState::Dragging) {
        finishInteractivePop(false);
    } else {
        resetEdgeSwipe();
    }
    return true;
}

void NavigationStack::updateInteractivePop(float x) {
    if (!m_edgeSwipe.outgoing || !m_edgeSwipe.incoming) return;
    const float width = std::max(1.0f, m_viewport->getBounds().width);
    const float dx = std::clamp(x - m_edgeSwipe.startX, 0.0f, width);
    m_edgeSwipe.progress = dx / width;
    m_edgeSwipe.outgoing->setTranslationX(dx);
    m_edgeSwipe.incoming->setTranslationX(
        -kBackParallax * width * (1.0f - m_edgeSwipe.progress));
}

void NavigationStack::finishInteractivePop(bool commit) {
    Widget* outgoing = m_edgeSwipe.outgoing;
    Widget* incoming = m_edgeSwipe.incoming;
    const float width = std::max(1.0f, m_viewport->getBounds().width);
    resetEdgeSwipe();
    if (!outgoing || !incoming) return;
    if (commit) {
        m_entries.pop_back();
        updateBar();
        notifyRouteChanged();
        beginSpringTransition(PageTransition::Pop, outgoing, incoming, incoming,
                              true, width, 0.0f);
    } else {
        beginSpringTransition(PageTransition::Push, outgoing, incoming, outgoing,
                              false, 0.0f, 0.0f);
    }
}

void NavigationStack::resetEdgeSwipe() noexcept {
    m_edgeSwipe = {};
}

} // namespace lcl::ui
