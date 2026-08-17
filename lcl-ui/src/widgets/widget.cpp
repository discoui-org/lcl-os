#include "lcl-ui/widgets/widget.hpp"
#include <algorithm>
#include <cmath>

namespace lcl::ui {

std::atomic<uint64_t> Widget::s_nextObjectId{1};

Widget::Widget() : m_objectId(s_nextObjectId.fetch_add(1, std::memory_order_relaxed)) {}

Widget::~Widget() {
    if (m_destructionCallback) m_destructionCallback();
    m_lifetimeToken.reset();
    if (m_motionCoordinator) m_motionCoordinator->unregisterObject(m_objectId);
}

void Widget::addChild(std::unique_ptr<Widget> child) {
    if (!child) return;
    child->m_parent = this;
    child->setRenderPass(m_renderPass);
    child->setMotionCoordinator(m_motionCoordinator);
    m_yogaNode.appendChild(&child->getYogaNode());
    m_children.push_back(std::move(child));
    markDirty();
}

void Widget::setRenderPass(RenderPass* pass) {
    m_renderPass = pass;
    for (auto& child : m_children) {
        child->setRenderPass(pass);
    }
}

void Widget::setMotionCoordinator(MotionCoordinator* coordinator) {
    if (m_motionCoordinator == coordinator) return;
    if (m_motionCoordinator) m_motionCoordinator->unregisterObject(m_objectId);
    m_motionCoordinator = coordinator;
    for (auto& child : m_children) child->setMotionCoordinator(coordinator);
}

const InteractionMotionTheme& Widget::interactionMotionTheme() const {
    static const InteractionMotionTheme fallback{};
    if (m_interactionTheme) return *m_interactionTheme;
    return m_motionCoordinator ? m_motionCoordinator->interactionTheme() : fallback;
}

void Widget::setInteractionStyle(InteractionState state, InteractionStyle style) {
    if (style.scale) *style.scale = std::max(0.0f, *style.scale);
    if (style.opacity) *style.opacity = std::clamp(*style.opacity, 0.0f, 1.0f);
    m_interactionStyles[static_cast<size_t>(state)] = std::move(style);
    applyDeclarativeInteractionState();
}

void Widget::clearInteractionStyle(InteractionState state) {
    m_interactionStyles[static_cast<size_t>(state)].reset();
    applyDeclarativeInteractionState();
}

void Widget::setInteractionEnabled(bool enabled) {
    if (m_interactionEnabled == enabled) return;
    m_interactionEnabled = enabled;
    if (!enabled) {
        m_declarativeHovered = false;
        m_declarativePressed = false;
    }
    applyDeclarativeInteractionState();
}

bool Widget::hasDeclarativeInteraction() const {
    if (m_onClick) return true;
    return std::any_of(m_interactionStyles.begin(), m_interactionStyles.end(),
                       [](const auto& style) { return style.has_value(); });
}

void Widget::applyDeclarativeInteractionState() {
    if (!hasDeclarativeInteraction()) return;
    InteractionState state = InteractionState::Normal;
    if (!m_interactionEnabled) state = InteractionState::Disabled;
    else if (m_declarativePressed) state = InteractionState::Pressed;
    else if (m_declarativeHovered) state = InteractionState::Hover;
    else if (m_declarativeFocused) state = InteractionState::Focused;

    const auto& selected = m_interactionStyles[static_cast<size_t>(state)];
    const auto& normal = m_interactionStyles[static_cast<size_t>(InteractionState::Normal)];
    const auto resolve = [&](auto member, float fallback) {
        if (selected && ((*selected).*member)) return *((*selected).*member);
        if (normal && ((*normal).*member)) return *((*normal).*member);
        return fallback;
    };
    const float scale = resolve(&InteractionStyle::scale, m_modelTransform.scaleX);
    const float opacity = resolve(&InteractionStyle::opacity, m_opacity);

    lcl::motion::Motion motion = interactionMotionTheme().hover;
    if (state == InteractionState::Pressed) motion = interactionMotionTheme().pressed;
    else if (state == InteractionState::Normal) motion = interactionMotionTheme().release;
    else if (state == InteractionState::Focused || state == InteractionState::Disabled)
        motion = interactionMotionTheme().focusTransition;
    if (selected && selected->motion) motion = *selected->motion;

    if (!interactionMotionTheme().enabled || !m_motionCoordinator) {
        applyPresentationValue(AnimatableProperty::ScaleX, scale);
        applyPresentationValue(AnimatableProperty::ScaleY, scale);
        applyPresentationValue(AnimatableProperty::Opacity, opacity);
        return;
    }
    m_motionCoordinator->animateFloat(*this, AnimatableProperty::ScaleX,
        m_presentation.scaleX, scale, motion,
        [this](float value) { applyPresentationValue(AnimatableProperty::ScaleX, value); });
    m_motionCoordinator->animateFloat(*this, AnimatableProperty::ScaleY,
        m_presentation.scaleY, scale, motion,
        [this](float value) { applyPresentationValue(AnimatableProperty::ScaleY, value); });
    m_motionCoordinator->animateFloat(*this, AnimatableProperty::Opacity,
        m_presentation.opacity, opacity, motion,
        [this](float value) { applyPresentationValue(AnimatableProperty::Opacity, value); });
}

bool Widget::onPointerEnter(const PointerEvent& event) {
    (void)event;
    if (!m_interactionEnabled || !hasDeclarativeInteraction()) return false;
    m_declarativeHovered = true;
    applyDeclarativeInteractionState();
    return true;
}

bool Widget::onPointerLeave(const PointerEvent& event) {
    (void)event;
    if (!hasDeclarativeInteraction()) return false;
    m_declarativeHovered = false;
    m_declarativePressed = false;
    applyDeclarativeInteractionState();
    return true;
}

bool Widget::onPointerDown(const PointerEvent& event) {
    (void)event;
    if (!m_interactionEnabled || !hasDeclarativeInteraction()) return false;
    m_declarativePressed = true;
    applyDeclarativeInteractionState();
    return true;
}

bool Widget::onPointerUp(const PointerEvent& event) {
    (void)event;
    if (!m_interactionEnabled || !hasDeclarativeInteraction()) return false;
    const bool activate = m_declarativePressed;
    m_declarativePressed = false;
    m_declarativeHovered = true;
    applyDeclarativeInteractionState();
    if (activate && m_onClick) m_onClick();
    return true;
}

bool Widget::onPointerCancel(const PointerEvent& event) {
    (void)event;
    if (!hasDeclarativeInteraction()) return false;
    m_declarativePressed = false;
    m_declarativeHovered = false;
    applyDeclarativeInteractionState();
    return true;
}

bool Widget::onFocusGained(const FocusEvent& event) {
    (void)event;
    if (!hasDeclarativeInteraction()) return false;
    m_declarativeFocused = true;
    applyDeclarativeInteractionState();
    return false;
}

bool Widget::onFocusLost(const FocusEvent& event) {
    (void)event;
    if (!hasDeclarativeInteraction()) return false;
    m_declarativeFocused = false;
    m_declarativePressed = false;
    applyDeclarativeInteractionState();
    return false;
}

void Widget::removeChild(Widget* child) {
    if (!child) return;
    auto it = std::find_if(m_children.begin(), m_children.end(),
        [child](const std::unique_ptr<Widget>& ptr) { return ptr.get() == child; });
    if (it != m_children.end()) {
        m_yogaNode.removeChild(&child->getYogaNode());
        (*it)->m_parent = nullptr;
        m_children.erase(it);
        markDirty();
    }
}

void Widget::markDirty() {
    if (m_renderPass && m_visible) {
        m_renderPass->addDirtyRect(m_absoluteBounds.unionWith(getPresentationBounds()));
    }
    if (m_parent) {
        m_parent->markDirty();
    }
}

void Widget::setOpacity(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    m_opacity = value;
    if (m_motionCoordinator) {
        m_motionCoordinator->setFloat(*this, AnimatableProperty::Opacity, m_presentation.opacity, value,
            [this](float next) { m_presentation.opacity = next; markDirty(); });
    } else {
        m_presentation.opacity = value;
        markDirty();
    }
}

void Widget::setTranslation(float x, float y) { setTranslationX(x); setTranslationY(y); }
void Widget::setTranslationX(float value) {
    m_modelTransform.translationX = value;
    if (m_motionCoordinator) m_motionCoordinator->setFloat(*this, AnimatableProperty::TranslationX,
        m_presentation.translationX, value, [this](float next) { m_presentation.translationX = next; markDirty(); });
    else { m_presentation.translationX = value; markDirty(); }
}
void Widget::setTranslationY(float value) {
    m_modelTransform.translationY = value;
    if (m_motionCoordinator) m_motionCoordinator->setFloat(*this, AnimatableProperty::TranslationY,
        m_presentation.translationY, value, [this](float next) { m_presentation.translationY = next; markDirty(); });
    else { m_presentation.translationY = value; markDirty(); }
}
void Widget::setScale(float value) { setScale(value, value); }
void Widget::setScale(float x, float y) {
    m_modelTransform.scaleX = x;
    m_modelTransform.scaleY = y;
    if (m_motionCoordinator) {
        m_motionCoordinator->setFloat(*this, AnimatableProperty::ScaleX, m_presentation.scaleX, x,
            [this](float next) { m_presentation.scaleX = next; markDirty(); });
        m_motionCoordinator->setFloat(*this, AnimatableProperty::ScaleY, m_presentation.scaleY, y,
            [this](float next) { m_presentation.scaleY = next; markDirty(); });
    } else { m_presentation.scaleX = x; m_presentation.scaleY = y; markDirty(); }
}
void Widget::setRotation(float value) {
    m_modelTransform.rotationRadians = value;
    if (m_motionCoordinator) m_motionCoordinator->setFloat(*this, AnimatableProperty::Rotation,
        m_presentation.rotationRadians, value, [this](float next) { m_presentation.rotationRadians = next; markDirty(); });
    else { m_presentation.rotationRadians = value; markDirty(); }
}
void Widget::setTransformOrigin(float x, float y) {
    x = std::clamp(x, 0.0f, 1.0f); y = std::clamp(y, 0.0f, 1.0f);
    m_modelTransform.originX = x;
    m_modelTransform.originY = y;
    if (m_motionCoordinator) {
        m_motionCoordinator->setFloat(*this, AnimatableProperty::TransformOriginX, m_presentation.originX, x,
            [this](float next) { m_presentation.originX = next; markDirty(); });
        m_motionCoordinator->setFloat(*this, AnimatableProperty::TransformOriginY, m_presentation.originY, y,
            [this](float next) { m_presentation.originY = next; markDirty(); });
    } else { m_presentation.originX = x; m_presentation.originY = y; markDirty(); }
}

void Widget::setWidth(float value) {
    value = std::max(0.0f, value);
    if (!m_hasWidth) m_presentWidth = m_bounds.width;
    m_hasWidth = true; m_modelWidth = value;
    const auto apply = [this](float next) { m_presentWidth = next; m_yogaNode.setWidth(next); markDirty(); };
    if (m_motionCoordinator) m_motionCoordinator->setFloat(*this, AnimatableProperty::Width, m_presentWidth, value, apply, true);
    else apply(value);
}
void Widget::setHeight(float value) {
    value = std::max(0.0f, value);
    if (!m_hasHeight) m_presentHeight = m_bounds.height;
    m_hasHeight = true; m_modelHeight = value;
    const auto apply = [this](float next) { m_presentHeight = next; m_yogaNode.setHeight(next); markDirty(); };
    if (m_motionCoordinator) m_motionCoordinator->setFloat(*this, AnimatableProperty::Height, m_presentHeight, value, apply, true);
    else apply(value);
}

namespace {
int edgeIndex(YGEdge edge) {
    switch (edge) { case YGEdgeLeft: return 0; case YGEdgeTop: return 1; case YGEdgeRight: return 2; case YGEdgeBottom: return 3; default: return -1; }
}
AnimatableProperty paddingProperty(int index) { return static_cast<AnimatableProperty>(static_cast<uint32_t>(AnimatableProperty::PaddingLeft) + index); }
AnimatableProperty positionProperty(int index) { return static_cast<AnimatableProperty>(static_cast<uint32_t>(AnimatableProperty::PositionLeft) + index); }
}

void Widget::setPadding(YGEdge edge, float value) {
    const auto applyOne = [this, value](int index, YGEdge yogaEdge) {
        m_modelPadding[index] = value;
        const auto apply = [this, index, yogaEdge](float next) { m_presentPadding[index] = next; m_yogaNode.setPadding(yogaEdge, next); markDirty(); };
        if (m_motionCoordinator) m_motionCoordinator->setFloat(*this, paddingProperty(index), m_presentPadding[index], value, apply, true);
        else apply(value);
    };
    const int index = edgeIndex(edge);
    if (index >= 0) applyOne(index, edge);
    else if (edge == YGEdgeHorizontal) { applyOne(0, YGEdgeLeft); applyOne(2, YGEdgeRight); }
    else if (edge == YGEdgeVertical) { applyOne(1, YGEdgeTop); applyOne(3, YGEdgeBottom); }
    else if (edge == YGEdgeAll) for (int i = 0; i < 4; ++i) applyOne(i, static_cast<YGEdge>(i));
}

void Widget::setGap(YGGutter gutter, float value) {
    const auto applyOne = [this, value](int index, YGGutter yogaGutter) {
        m_modelGap[index] = value;
        const auto apply = [this, index, yogaGutter](float next) { m_presentGap[index] = next; m_yogaNode.setGap(yogaGutter, next); markDirty(); };
        const auto property = index == 0 ? AnimatableProperty::GapColumn : AnimatableProperty::GapRow;
        if (m_motionCoordinator) m_motionCoordinator->setFloat(*this, property, m_presentGap[index], value, apply, true);
        else apply(value);
    };
    if (gutter == YGGutterColumn) applyOne(0, gutter);
    else if (gutter == YGGutterRow) applyOne(1, gutter);
    else { applyOne(0, YGGutterColumn); applyOne(1, YGGutterRow); }
}

void Widget::setPosition(YGEdge edge, float value) {
    const int index = edgeIndex(edge);
    if (index < 0) return;
    m_modelPosition[index] = value;
    const auto apply = [this, index, edge](float next) { m_presentPosition[index] = next; m_yogaNode.setPosition(edge, next); markDirty(); };
    if (m_motionCoordinator) m_motionCoordinator->setFloat(*this, positionProperty(index), m_presentPosition[index], value, apply, true);
    else apply(value);
}

float Widget::getPresentationValue(AnimatableProperty property) const {
    switch (property) {
        case AnimatableProperty::Opacity: return m_presentation.opacity;
        case AnimatableProperty::TranslationX: return m_presentation.translationX;
        case AnimatableProperty::TranslationY: return m_presentation.translationY;
        case AnimatableProperty::ScaleX: return m_presentation.scaleX;
        case AnimatableProperty::ScaleY: return m_presentation.scaleY;
        case AnimatableProperty::Rotation: return m_presentation.rotationRadians;
        case AnimatableProperty::TransformOriginX: return m_presentation.originX;
        case AnimatableProperty::TransformOriginY: return m_presentation.originY;
        case AnimatableProperty::Width: return m_hasWidth ? m_presentWidth : m_bounds.width;
        case AnimatableProperty::Height: return m_hasHeight ? m_presentHeight : m_bounds.height;
        default: return 0.0f;
    }
}

void Widget::applyPresentationValue(AnimatableProperty property, float value) {
    switch (property) {
        case AnimatableProperty::Opacity: m_presentation.opacity = std::clamp(value, 0.0f, 1.0f); break;
        case AnimatableProperty::TranslationX: m_presentation.translationX = value; break;
        case AnimatableProperty::TranslationY: m_presentation.translationY = value; break;
        case AnimatableProperty::ScaleX: m_presentation.scaleX = value; break;
        case AnimatableProperty::ScaleY: m_presentation.scaleY = value; break;
        case AnimatableProperty::Rotation: m_presentation.rotationRadians = value; break;
        case AnimatableProperty::TransformOriginX: m_presentation.originX = std::clamp(value, 0.0f, 1.0f); break;
        case AnimatableProperty::TransformOriginY: m_presentation.originY = std::clamp(value, 0.0f, 1.0f); break;
        case AnimatableProperty::Width:
            m_presentation.scaleX = value / std::max(0.001f, m_absoluteBounds.width); break;
        case AnimatableProperty::Height:
            m_presentation.scaleY = value / std::max(0.001f, m_absoluteBounds.height); break;
        default: break;
    }
    markDirty();
}

void Widget::commitModelValue(AnimatableProperty property, float value) {
    switch (property) {
        case AnimatableProperty::Opacity: setOpacity(value); break;
        case AnimatableProperty::TranslationX: setTranslationX(value); break;
        case AnimatableProperty::TranslationY: setTranslationY(value); break;
        case AnimatableProperty::ScaleX: setScale(value, m_modelTransform.scaleY); break;
        case AnimatableProperty::ScaleY: setScale(m_modelTransform.scaleX, value); break;
        case AnimatableProperty::Rotation: setRotation(value); break;
        case AnimatableProperty::TransformOriginX: setTransformOrigin(value, m_modelTransform.originY); break;
        case AnimatableProperty::TransformOriginY: setTransformOrigin(m_modelTransform.originX, value); break;
        case AnimatableProperty::Width: setWidth(value); break;
        case AnimatableProperty::Height: setHeight(value); break;
        default: break;
    }
}

lcl::motion::AnimationHandle Widget::animate(
        AnimatableProperty property,
        std::vector<lcl::motion::Keyframe> keyframes,
        const lcl::motion::AnimationOptions& options) {
    if (!m_motionCoordinator) return {};
    return m_motionCoordinator->animate(*this, property, std::move(keyframes), options);
}

void Widget::syncLayout(float parentAbsX, float parentAbsY) {
    m_bounds = Rect{
        m_yogaNode.getLayoutX(),
        m_yogaNode.getLayoutY(),
        m_yogaNode.getLayoutWidth(),
        m_yogaNode.getLayoutHeight()
    };

    m_absoluteBounds = Rect{
        parentAbsX + m_bounds.x,
        parentAbsY + m_bounds.y,
        m_bounds.width,
        m_bounds.height
    };

    for (auto& child : m_children) {
        child->syncLayout(m_absoluteBounds.x, m_absoluteBounds.y);
    }
}

Rect Widget::getPresentationBounds() const {
    Rect result = m_absoluteBounds;
    const Widget* current = this;
    while (current) {
        const auto& p = current->m_presentation;
        const Rect basis = current->m_absoluteBounds;
        const float ox = basis.x + basis.width * p.originX;
        const float oy = basis.y + basis.height * p.originY;
        const float cosine = std::cos(p.rotationRadians);
        const float sine = std::sin(p.rotationRadians);
        const auto map = [&](float x, float y) {
            x -= ox; y -= oy;
            return std::pair<float, float>{ox + p.translationX + cosine * p.scaleX * x - sine * p.scaleY * y,
                                           oy + p.translationY + sine * p.scaleX * x + cosine * p.scaleY * y};
        };
        const auto a = map(result.x, result.y);
        const auto b = map(result.x + result.width, result.y);
        const auto c = map(result.x, result.y + result.height);
        const auto d = map(result.x + result.width, result.y + result.height);
        const float left = std::min({a.first, b.first, c.first, d.first});
        const float right = std::max({a.first, b.first, c.first, d.first});
        const float top = std::min({a.second, b.second, c.second, d.second});
        const float bottom = std::max({a.second, b.second, c.second, d.second});
        result = {left, top, right - left, bottom - top};
        current = current->m_parent;
    }
    return result;
}

bool Widget::containsPresentationPoint(float x, float y) const {
    std::vector<const Widget*> chain;
    for (const Widget* current = this; current; current = current->m_parent) chain.push_back(current);
    for (auto iterator = chain.rbegin(); iterator != chain.rend(); ++iterator) {
        const Widget* current = *iterator;
        const auto& p = current->m_presentation;
        const float ox = current->m_absoluteBounds.x + current->m_absoluteBounds.width * p.originX;
        const float oy = current->m_absoluteBounds.y + current->m_absoluteBounds.height * p.originY;
        x -= ox + p.translationX;
        y -= oy + p.translationY;
        const float cosine = std::cos(p.rotationRadians);
        const float sine = std::sin(p.rotationRadians);
        const float rotatedX = cosine * x + sine * y;
        const float rotatedY = -sine * x + cosine * y;
        if (std::fabs(p.scaleX) < 0.000001f || std::fabs(p.scaleY) < 0.000001f) return false;
        x = rotatedX / p.scaleX + ox;
        y = rotatedY / p.scaleY + oy;
    }
    return m_absoluteBounds.containsPoint(x, y);
}

bool Widget::hasActiveAnimationInHierarchy() const {
    for (const Widget* current = this; current; current = current->m_parent) {
        if (current->m_motionCoordinator &&
            current->m_motionCoordinator->isObjectAnimating(current->m_objectId)) {
            return true;
        }
    }
    return false;
}

void Widget::beginPresentation(Canvas& canvas) const {
    canvas.saveState();
    const float ox = m_absoluteBounds.x + m_absoluteBounds.width * m_presentation.originX;
    const float oy = m_absoluteBounds.y + m_absoluteBounds.height * m_presentation.originY;
    const float cosine = std::cos(m_presentation.rotationRadians);
    const float sine = std::sin(m_presentation.rotationRadians);
    canvas.concatTransform({
        cosine * m_presentation.scaleX,
        sine * m_presentation.scaleX,
        -sine * m_presentation.scaleY,
        cosine * m_presentation.scaleY,
        ox + m_presentation.translationX - cosine * m_presentation.scaleX * ox + sine * m_presentation.scaleY * oy,
        oy + m_presentation.translationY - sine * m_presentation.scaleX * ox - cosine * m_presentation.scaleY * oy,
    });
    if (m_clipsToBounds) canvas.clipRect(m_absoluteBounds);
    canvas.beginLayer(m_presentation.opacity);
}

void Widget::endPresentation(Canvas& canvas) const {
    canvas.endLayer();
    canvas.restoreState();
}

void Widget::drawChildren(Canvas& canvas, const Rect& damageRect) {
    for (auto& child : m_children) {
        if (child->isVisible() && child->getPresentationBounds().intersects(damageRect)) child->draw(canvas, damageRect);
    }
}

void Widget::draw(Canvas& canvas, const Rect& damageRect) {
    if (!m_visible) return;
    beginPresentation(canvas);
    drawChildren(canvas, damageRect);
    endPresentation(canvas);
}

void Widget::collectEffects(std::vector<EffectRegion>& outEffects) const {
    if (!m_visible) return;
    for (const auto& child : m_children) {
        child->collectEffects(outEffects);
    }
    (void)outEffects;
}

} // namespace lcl::ui
