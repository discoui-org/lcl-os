#include "lcl-ui/widgets/widget.hpp"
#include "layout/layout_node.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace lcl::ui {

std::atomic<uint64_t> Widget::s_nextObjectId{1};

Widget::Widget()
    : m_layoutNode(std::make_unique<detail::LayoutNode>()),
      m_objectId(s_nextObjectId.fetch_add(1, std::memory_order_relaxed)) {
    m_layoutNode->setInvalidationCallback([this] { invalidateLayout(); });
}

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
    child->setThemeContext(m_themeContext);
    m_layoutNode->appendChild(*child->m_layoutNode);
    m_children.push_back(std::move(child));
    // Tree membership changes the retained subtree identity, but the new
    // child has no synchronized geometry yet. syncLayout() will damage its
    // first real presentation bounds.
    ++m_localPaintRevision;
    advancePaintRevision();
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

void Widget::useStyle(lcl::theme::WidgetStyle style) {
    m_themeStyleRole.reset();
    m_explicitStyle = std::move(style);
    styleDidChange();
    applyDeclarativeInteractionState();
    invalidatePaint();
}

void Widget::useThemeStyle(lcl::theme::WidgetStyleRole role) {
    m_explicitStyle.reset();
    m_themeStyleRole = role;
    styleDidChange();
    applyDeclarativeInteractionState();
    invalidatePaint();
}

void Widget::clearStyle() {
    if (!m_explicitStyle && !m_themeStyleRole) return;
    m_explicitStyle.reset();
    m_themeStyleRole.reset();
    styleDidChange();
    applyDeclarativeInteractionState();
    invalidatePaint();
}

const lcl::theme::Theme& Widget::getTheme() const noexcept {
    return m_themeContext ? m_themeContext->value() : lcl::theme::defaultTheme();
}

const lcl::theme::WidgetStyle* Widget::resolvedStyle() const noexcept {
    if (m_explicitStyle) return &*m_explicitStyle;
    if (m_themeStyleRole) {
        return &lcl::theme::widgetStyleForRole(getTheme(), *m_themeStyleRole);
    }
    return defaultStyle();
}

void Widget::setThemeContext(const lcl::theme::ThemeContext* context) {
    if (m_themeContext == context) return;
    m_themeContext = context;
    styleDidChange();
    applyDeclarativeInteractionState();
    for (auto& child : m_children) child->setThemeContext(context);
    invalidatePaint();
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
    float styleScale = m_modelTransform.scaleX;
    float styleOpacity = m_opacity;
    if (const auto* style = resolvedStyle()) {
        lcl::theme::StyleState styleState = lcl::theme::StyleState::Normal;
        if (state == InteractionState::Hover) styleState = lcl::theme::StyleState::Hover;
        else if (state == InteractionState::Pressed) styleState = lcl::theme::StyleState::Pressed;
        else if (state == InteractionState::Focused) styleState = lcl::theme::StyleState::Focused;
        else if (state == InteractionState::Disabled) styleState = lcl::theme::StyleState::Disabled;
        const auto visual = lcl::theme::resolveStyle(*style, styleState);
        styleScale = visual.scale;
        styleOpacity = visual.opacity;
    }
    const float scale = resolve(&InteractionStyle::scale, styleScale);
    const float opacity = resolve(&InteractionStyle::opacity, styleOpacity);

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
        const graphics::RectF previousBounds =
            (*it)->getPresentationSubtreePaintBounds();
        m_layoutNode->removeChild(*child->m_layoutNode);
        (*it)->m_parent = nullptr;
        m_children.erase(it);
        if (m_renderPass) m_renderPass->addDirtyRect(previousBounds);
        ++m_localPaintRevision;
        advancePaintRevision();
    }
}

void Widget::invalidatePaint() {
    ++m_localPaintRevision;
    if (m_renderPass) {
        m_renderPass->addDirtyRect(getVisiblePresentationPaintBounds());
    }
    advancePaintRevision();
}

void Widget::invalidatePaint(const graphics::RectF& damageRect) {
    ++m_localPaintRevision;
    if (m_renderPass) {
        m_renderPass->addDirtyRect(
            damageRect.intersection(getVisiblePresentationPaintBounds()));
    }
    advancePaintRevision();
}

void Widget::advancePaintRevision() {
    ++m_paintRevision;
    if (m_parent) m_parent->propagateDescendantPaintRevision();
}

void Widget::invalidatePresentation() {
    invalidatePresentation(m_visible ? getPresentationSubtreePaintBounds()
                                     : graphics::RectF{});
}

void Widget::invalidatePresentation(const graphics::RectF& previousBounds) {
    ++m_localPresentationRevision;
    ++m_presentationRevision;
    if (m_renderPass) {
        m_renderPass->addDirtyRect(previousBounds);
        if (m_visible) {
            m_renderPass->addDirtyRect(getPresentationSubtreePaintBounds());
        }
    }
    if (m_parent) m_parent->propagateDescendantPresentationRevision();
}

void Widget::invalidatePaintFrom(const graphics::RectF& previousPaintBounds) {
    if (m_renderPass) m_renderPass->addDirtyRect(previousPaintBounds);
    invalidatePaint();
}

void Widget::propagateDescendantPaintRevision() {
    ++m_paintRevision;
    if (m_parent) m_parent->propagateDescendantPaintRevision();
}

void Widget::propagateDescendantPresentationRevision() {
    ++m_presentationRevision;
    if (m_parent) m_parent->propagateDescendantPresentationRevision();
}

void Widget::invalidateLayout() {
    markLayoutDirty();
}

void Widget::markLayoutDirty() {
    if (m_layoutDirty) return;
    m_layoutDirty = true;
    ++m_layoutRevision;
    if (m_parent) m_parent->markLayoutDirty();
}

void Widget::clearLayoutDirty() {
    m_layoutDirty = false;
    for (auto& child : m_children) child->clearLayoutDirty();
}

void Widget::setParentControlledTranslationY(float value) {
    if (m_modelTransform.translationY == value &&
        m_presentation.translationY == value) return;
    m_modelTransform.translationY = value;
    m_presentation.translationY = value;
}

void Widget::setVisible(bool visible) {
    if (m_visible == visible) return;
    const graphics::RectF previousBounds =
        m_visible ? getPresentationSubtreePaintBounds() : graphics::RectF{};
    m_visible = visible;
    invalidatePresentation(previousBounds);
}

void Widget::setClipsToBounds(bool enabled) {
    if (m_clipsToBounds == enabled) return;
    const graphics::RectF previousBounds =
        m_visible ? getPresentationSubtreePaintBounds() : graphics::RectF{};
    m_clipsToBounds = enabled;
    invalidatePresentation(previousBounds);
}

void Widget::setOpacity(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    m_opacity = value;
    if (m_motionCoordinator) {
        m_motionCoordinator->setFloat(*this, AnimatableProperty::Opacity, m_presentation.opacity, value,
            [this](float next) { applyPresentationValue(AnimatableProperty::Opacity, next); });
    } else {
        applyPresentationValue(AnimatableProperty::Opacity, value);
    }
}

void Widget::setTranslation(float x, float y) { setTranslationX(x); setTranslationY(y); }
void Widget::setTranslationX(float value) {
    m_modelTransform.translationX = value;
    if (m_motionCoordinator) m_motionCoordinator->setFloat(*this, AnimatableProperty::TranslationX,
        m_presentation.translationX, value,
        [this](float next) { applyPresentationValue(AnimatableProperty::TranslationX, next); });
    else applyPresentationValue(AnimatableProperty::TranslationX, value);
}
void Widget::setTranslationY(float value) {
    m_modelTransform.translationY = value;
    if (m_motionCoordinator) m_motionCoordinator->setFloat(*this, AnimatableProperty::TranslationY,
        m_presentation.translationY, value,
        [this](float next) { applyPresentationValue(AnimatableProperty::TranslationY, next); });
    else applyPresentationValue(AnimatableProperty::TranslationY, value);
}
void Widget::setScale(float value) { setScale(value, value); }
void Widget::setScale(float x, float y) {
    m_modelTransform.scaleX = x;
    m_modelTransform.scaleY = y;
    if (m_motionCoordinator) {
        m_motionCoordinator->setFloat(*this, AnimatableProperty::ScaleX, m_presentation.scaleX, x,
            [this](float next) { applyPresentationValue(AnimatableProperty::ScaleX, next); });
        m_motionCoordinator->setFloat(*this, AnimatableProperty::ScaleY, m_presentation.scaleY, y,
            [this](float next) { applyPresentationValue(AnimatableProperty::ScaleY, next); });
    } else {
        applyPresentationValue(AnimatableProperty::ScaleX, x);
        applyPresentationValue(AnimatableProperty::ScaleY, y);
    }
}
void Widget::setRotation(float value) {
    m_modelTransform.rotationRadians = value;
    if (m_motionCoordinator) m_motionCoordinator->setFloat(*this, AnimatableProperty::Rotation,
        m_presentation.rotationRadians, value,
        [this](float next) { applyPresentationValue(AnimatableProperty::Rotation, next); });
    else applyPresentationValue(AnimatableProperty::Rotation, value);
}
void Widget::setTransformOrigin(float x, float y) {
    x = std::clamp(x, 0.0f, 1.0f); y = std::clamp(y, 0.0f, 1.0f);
    m_modelTransform.originX = x;
    m_modelTransform.originY = y;
    if (m_motionCoordinator) {
        m_motionCoordinator->setFloat(*this, AnimatableProperty::TransformOriginX, m_presentation.originX, x,
            [this](float next) { applyPresentationValue(AnimatableProperty::TransformOriginX, next); });
        m_motionCoordinator->setFloat(*this, AnimatableProperty::TransformOriginY, m_presentation.originY, y,
            [this](float next) { applyPresentationValue(AnimatableProperty::TransformOriginY, next); });
    } else {
        applyPresentationValue(AnimatableProperty::TransformOriginX, x);
        applyPresentationValue(AnimatableProperty::TransformOriginY, y);
    }
}

void Widget::setWidth(float value) {
    value = std::max(0.0f, value);
    if (!m_hasWidth) m_presentWidth = m_bounds.width;
    m_hasWidth = true; m_modelWidth = value;
    const auto apply = [this](float next) { m_presentWidth = next; m_layoutNode->setWidth(next); };
    if (m_motionCoordinator) m_motionCoordinator->setFloat(*this, AnimatableProperty::Width, m_presentWidth, value, apply, true);
    else apply(value);
}
void Widget::setWidthAuto() {
    m_hasWidth = false;
    m_layoutNode->setWidthAuto();
}
void Widget::setHeight(float value) {
    value = std::max(0.0f, value);
    if (!m_hasHeight) m_presentHeight = m_bounds.height;
    m_hasHeight = true; m_modelHeight = value;
    const auto apply = [this](float next) { m_presentHeight = next; m_layoutNode->setHeight(next); };
    if (m_motionCoordinator) m_motionCoordinator->setFloat(*this, AnimatableProperty::Height, m_presentHeight, value, apply, true);
    else apply(value);
}
void Widget::setHeightAuto() {
    m_hasHeight = false;
    m_layoutNode->setHeightAuto();
}

void Widget::setDefaultWidth(float value) {
    m_layoutNode->setWidth(std::max(0.0f, value));
}

void Widget::setDefaultHeight(float value) {
    m_layoutNode->setHeight(std::max(0.0f, value));
}

void Widget::setMinWidth(float value) { m_layoutNode->setMinWidth(value); }
void Widget::setMinHeight(float value) { m_layoutNode->setMinHeight(value); }
void Widget::setMaxWidth(float value) { m_layoutNode->setMaxWidth(value); }
void Widget::setMaxHeight(float value) { m_layoutNode->setMaxHeight(value); }
void Widget::setDirection(layout::Direction value) { m_layoutNode->setDirection(value); }
void Widget::setJustifyContent(layout::Justify value) { m_layoutNode->setJustifyContent(value); }
void Widget::setAlignItems(layout::Align value) { m_layoutNode->setAlignItems(value); }
void Widget::setAlignSelf(layout::Align value) { m_layoutNode->setAlignSelf(value); }
void Widget::setPositionType(layout::PositionType value) { m_layoutNode->setPositionType(value); }
void Widget::setWrap(layout::Wrap value) { m_layoutNode->setWrap(value); }
void Widget::setFlexGrow(float value) { m_layoutNode->setFlexGrow(value); }
void Widget::setFlexShrink(float value) { m_layoutNode->setFlexShrink(value); }
void Widget::setFlexBasis(float value) { m_layoutNode->setFlexBasis(value); }
void Widget::setFlexBasisAuto() { m_layoutNode->setFlexBasisAuto(); }

namespace {
int edgeIndex(layout::Edge edge) {
    switch (edge) {
        case layout::Edge::Left: return 0;
        case layout::Edge::Top: return 1;
        case layout::Edge::Right: return 2;
        case layout::Edge::Bottom: return 3;
        default: return -1;
    }
}
AnimatableProperty paddingProperty(int index) { return static_cast<AnimatableProperty>(static_cast<uint32_t>(AnimatableProperty::PaddingLeft) + index); }
AnimatableProperty positionProperty(int index) { return static_cast<AnimatableProperty>(static_cast<uint32_t>(AnimatableProperty::PositionLeft) + index); }
}

void Widget::setPadding(layout::Edge edge, float value) {
    const auto applyOne = [this, value](int index, layout::Edge layoutEdge) {
        m_modelPadding[index] = value;
        const auto apply = [this, index, layoutEdge](float next) { m_presentPadding[index] = next; m_layoutNode->setPadding(layoutEdge, next); };
        if (m_motionCoordinator) m_motionCoordinator->setFloat(*this, paddingProperty(index), m_presentPadding[index], value, apply, true);
        else apply(value);
    };
    const int index = edgeIndex(edge);
    if (index >= 0) applyOne(index, edge);
    else if (edge == layout::Edge::Horizontal) { applyOne(0, layout::Edge::Left); applyOne(2, layout::Edge::Right); }
    else if (edge == layout::Edge::Vertical) { applyOne(1, layout::Edge::Top); applyOne(3, layout::Edge::Bottom); }
    else if (edge == layout::Edge::All) {
        applyOne(0, layout::Edge::Left);
        applyOne(1, layout::Edge::Top);
        applyOne(2, layout::Edge::Right);
        applyOne(3, layout::Edge::Bottom);
    } else {
        m_layoutNode->setPadding(edge, value);
    }
}

void Widget::setMargin(layout::Edge edge, float value) {
    m_layoutNode->setMargin(edge, value);
}

void Widget::setGap(layout::Gutter gutter, float value) {
    const auto applyOne = [this, value](int index, layout::Gutter layoutGutter) {
        m_modelGap[index] = value;
        const auto apply = [this, index, layoutGutter](float next) { m_presentGap[index] = next; m_layoutNode->setGap(layoutGutter, next); };
        const auto property = index == 0 ? AnimatableProperty::GapColumn : AnimatableProperty::GapRow;
        if (m_motionCoordinator) m_motionCoordinator->setFloat(*this, property, m_presentGap[index], value, apply, true);
        else apply(value);
    };
    if (gutter == layout::Gutter::Column) applyOne(0, gutter);
    else if (gutter == layout::Gutter::Row) applyOne(1, gutter);
    else { applyOne(0, layout::Gutter::Column); applyOne(1, layout::Gutter::Row); }
}

void Widget::setPosition(layout::Edge edge, float value) {
    const int index = edgeIndex(edge);
    if (index < 0) {
        m_layoutNode->setPosition(edge, value);
        return;
    }
    m_modelPosition[index] = value;
    const auto apply = [this, index, edge](float next) { m_presentPosition[index] = next; m_layoutNode->setPosition(edge, next); };
    if (m_motionCoordinator) m_motionCoordinator->setFloat(*this, positionProperty(index), m_presentPosition[index], value, apply, true);
    else apply(value);
}

void Widget::setMeasureCallback(
        std::function<layout::Size(const layout::Constraints&)> callback) {
    m_layoutNode->setMeasureCallback(std::move(callback));
}

void Widget::invalidateMeasurement() {
    m_layoutNode->invalidateMeasurement();
}

layout::PositionType Widget::positionType() const {
    return m_layoutNode->positionType();
}

void Widget::calculateLayout() {
    const float undefined = std::numeric_limits<float>::quiet_NaN();
    calculateLayout(undefined, undefined);
}

void Widget::calculateLayout(float availableWidth, float availableHeight) {
    // Service the current layout epoch before calculation. A genuine mutation
    // during measurement or sync starts a new epoch instead of being lost.
    clearLayoutDirty();
    m_layoutNode->calculateLayout(availableWidth, availableHeight);
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
    float next = value;
    switch (property) {
        case AnimatableProperty::Opacity:
            next = std::clamp(value, 0.0f, 1.0f);
            if (m_presentation.opacity == next) return;
            break;
        case AnimatableProperty::TranslationX:
            if (m_presentation.translationX == next) return;
            break;
        case AnimatableProperty::TranslationY:
            if (m_presentation.translationY == next) return;
            break;
        case AnimatableProperty::ScaleX:
            if (m_presentation.scaleX == next) return;
            break;
        case AnimatableProperty::ScaleY:
            if (m_presentation.scaleY == next) return;
            break;
        case AnimatableProperty::Rotation:
            if (m_presentation.rotationRadians == next) return;
            break;
        case AnimatableProperty::TransformOriginX:
            next = std::clamp(value, 0.0f, 1.0f);
            if (m_presentation.originX == next) return;
            break;
        case AnimatableProperty::TransformOriginY:
            next = std::clamp(value, 0.0f, 1.0f);
            if (m_presentation.originY == next) return;
            break;
        case AnimatableProperty::Width:
            next = value / std::max(0.001f, m_absoluteBounds.width);
            if (m_presentation.scaleX == next) return;
            break;
        case AnimatableProperty::Height:
            next = value / std::max(0.001f, m_absoluteBounds.height);
            if (m_presentation.scaleY == next) return;
            break;
        default:
            return;
    }
    const graphics::RectF previousBounds = getPresentationSubtreePaintBounds();
    switch (property) {
        case AnimatableProperty::Opacity: m_presentation.opacity = next; break;
        case AnimatableProperty::TranslationX: m_presentation.translationX = next; break;
        case AnimatableProperty::TranslationY: m_presentation.translationY = next; break;
        case AnimatableProperty::ScaleX: m_presentation.scaleX = next; break;
        case AnimatableProperty::ScaleY: m_presentation.scaleY = next; break;
        case AnimatableProperty::Rotation: m_presentation.rotationRadians = next; break;
        case AnimatableProperty::TransformOriginX: m_presentation.originX = next; break;
        case AnimatableProperty::TransformOriginY: m_presentation.originY = next; break;
        case AnimatableProperty::Width: m_presentation.scaleX = next; break;
        case AnimatableProperty::Height: m_presentation.scaleY = next; break;
        default: break;
    }
    invalidatePresentation(previousBounds);
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
    const graphics::RectF previousBounds =
        m_visible ? getPresentationSubtreePaintBounds() : graphics::RectF{};
    const graphics::RectF nextBounds{
        m_layoutNode->layoutX(),
        m_layoutNode->layoutY(),
        m_layoutNode->layoutWidth(),
        m_layoutNode->layoutHeight()
    };

    const graphics::RectF nextAbsoluteBounds{
        parentAbsX + nextBounds.x,
        parentAbsY + nextBounds.y,
        nextBounds.width,
        nextBounds.height
    };
    const bool geometryChanged =
        m_bounds.x != nextBounds.x || m_bounds.y != nextBounds.y ||
        m_bounds.width != nextBounds.width || m_bounds.height != nextBounds.height ||
        m_absoluteBounds.x != nextAbsoluteBounds.x ||
        m_absoluteBounds.y != nextAbsoluteBounds.y ||
        m_absoluteBounds.width != nextAbsoluteBounds.width ||
        m_absoluteBounds.height != nextAbsoluteBounds.height;
    m_bounds = nextBounds;
    m_absoluteBounds = nextAbsoluteBounds;

    for (auto& child : m_children) {
        child->syncLayout(m_absoluteBounds.x, m_absoluteBounds.y);
    }
    if (geometryChanged) invalidatePresentation(previousBounds);
}

graphics::RectF Widget::getPresentationBounds() const {
    return mapPresentationRect(m_absoluteBounds, this);
}

graphics::RectF Widget::mapPresentationRect(
        const graphics::RectF& rect, const Widget* firstTransform) const {
    graphics::RectF result = rect;
    const Widget* current = firstTransform;
    while (current) {
        const auto& p = current->m_presentation;
        const graphics::RectF basis = current->m_absoluteBounds;
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

graphics::RectF Widget::getPresentationPaintBounds() const {
    return mapPresentationRect(getUntransformedPaintBounds(), this);
}

graphics::RectF Widget::getVisiblePresentationPaintBounds() const {
    if (!m_visible) return {};
    graphics::RectF result = getPresentationPaintBounds();
    for (const Widget* current = this; current && !result.isEmpty();
         current = current->m_parent) {
        if (!current->m_visible) return {};
        if (!current->m_clipsToBounds) continue;
        const graphics::RectF clip = current->mapPresentationRect(
            current->m_absoluteBounds, current);
        result = result.intersection(clip);
    }
    return result;
}

graphics::RectF Widget::getPresentationSubtreePaintBounds() const {
    if (!m_visible) return {};
    graphics::RectF result = getVisiblePresentationPaintBounds();
    for (const auto& child : m_children) {
        if (!child->m_visible) continue;
        result = result.unionWith(child->getPresentationSubtreePaintBounds());
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

bool Widget::hasActiveAnimationInSubtree() const {
    if (m_motionCoordinator &&
        m_motionCoordinator->isObjectAnimating(m_objectId)) {
        return true;
    }
    for (const auto& child : m_children) {
        if (child->hasActiveAnimationInSubtree()) return true;
    }
    return false;
}

void Widget::beginPresentation(graphics::Canvas& canvas) const {
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

void Widget::endPresentation(graphics::Canvas& canvas) const {
    canvas.endLayer();
    canvas.restoreState();
}

void Widget::drawChildren(graphics::Canvas& canvas, const graphics::RectF& damageRect) {
    for (auto& child : m_children) {
        if (child->isVisible() && child->getPresentationBounds().intersects(damageRect)) {
            child->draw(canvas, damageRect);
        }
    }
}

void Widget::draw(graphics::Canvas& canvas, const graphics::RectF& damageRect) {
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
