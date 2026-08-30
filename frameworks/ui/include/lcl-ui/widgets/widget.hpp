#pragma once

#include "lcl-graphics/canvas.hpp"
#include "lcl-ui/core/effects.hpp"
#include "lcl-ui/core/render_pass.hpp"
#include "lcl-ui/core/events.hpp"
#include "lcl-ui/core/motion.hpp"
#include "lcl-ui/layout/layout.hpp"
#include "lcl-theme/theme.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <vector>
#include <memory>
#include <optional>
#include <string>

namespace lcl::ui {

namespace detail {
class LayoutNode;
struct RetainedRenderAccess;
}

class WindowApp;
class ScrollView;
class MeasuredWidget;

class Widget {
public:
    Widget();
    virtual ~Widget();

    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    void addChild(std::unique_ptr<Widget> child);
    void removeChild(Widget* child);
    const std::vector<std::unique_ptr<Widget>>& getChildren() const { return m_children; }

    Widget* getParent() const { return m_parent; }

    graphics::RectF getBounds() const { return m_bounds; }
    graphics::RectF getAbsoluteBounds() const { return m_absoluteBounds; }
    graphics::RectF getPresentationBounds() const;
    /** Unclipped pixels painted by this widget, mapped through presentation transforms. */
    graphics::RectF getPresentationPaintBounds() const;
    /** Paint bounds intersected with every clipping ancestor. */
    graphics::RectF getVisiblePresentationPaintBounds() const;
    /** Painted pixels owned by this widget and its visible descendants. */
    graphics::RectF getPresentationSubtreePaintBounds() const;
    bool containsPresentationPoint(float x, float y) const;
    bool hasActiveAnimationInHierarchy() const;
    /** True when this widget or any descendant owns an active motion channel. */
    bool hasActiveAnimationInSubtree() const;
    uint64_t getObjectId() const noexcept { return m_objectId; }
    const PresentationState& getPresentationState() const noexcept { return m_presentation; }
    std::weak_ptr<uint8_t> getLifetimeToken() const noexcept { return m_lifetimeToken; }
    virtual float getPresentationValue(AnimatableProperty property) const;
    virtual void applyPresentationValue(AnimatableProperty property, float value);
    virtual void commitModelValue(AnimatableProperty property, float value);
    lcl::motion::AnimationHandle animate(
        AnimatableProperty property,
        std::vector<lcl::motion::Keyframe> keyframes,
        const lcl::motion::AnimationOptions& options = {});

    void setOpacity(float opacity);
    float getOpacity() const noexcept { return m_opacity; }
    const PresentationState& getModelTransform() const noexcept { return m_modelTransform; }
    void setTranslation(float x, float y);
    void setTranslationX(float x);
    void setTranslationY(float y);
    void setScale(float scale);
    void setScale(float x, float y);
    void setRotation(float radians);
    void setTransformOrigin(float normalizedX, float normalizedY);
    void setClipsToBounds(bool enabled);
    bool clipsToBounds() const noexcept { return m_clipsToBounds; }

    void setWidth(float width);
    void setWidthAuto();
    void setHeight(float height);
    void setHeightAuto();
    void setMinWidth(float width);
    void setMinHeight(float height);
    void setMaxWidth(float width);
    void setMaxHeight(float height);
    void setDirection(layout::Direction direction);
    void setJustifyContent(layout::Justify justify);
    void setAlignItems(layout::Align align);
    void setAlignSelf(layout::Align align);
    void setPositionType(layout::PositionType type);
    void setWrap(layout::Wrap wrap);
    void setFlexGrow(float value);
    void setFlexShrink(float value);
    void setFlexBasis(float value);
    void setFlexBasisAuto();
    void setPadding(layout::Edge edge, float value);
    void setPadding(float value) { setPadding(layout::Edge::All, value); }
    void setMargin(layout::Edge edge, float value);
    void setMargin(float value) { setMargin(layout::Edge::All, value); }
    void setGap(layout::Gutter gutter, float value);
    void setGap(float value) { setGap(layout::Gutter::All, value); }
    void setPosition(layout::Edge edge, float value);

    /** Calculate this tree without exposing the private layout engine. */
    void calculateLayout();
    void calculateLayout(float availableWidth, float availableHeight);

    void setVisible(bool visible);
    bool isVisible() const { return m_visible; }

    void setFocusable(bool focusable) { m_focusable = focusable; }
    bool isFocusable() const { return m_focusable; }
    /** Marks a traversal boundary without making the widget a focus target. */
    virtual bool isFocusScope() const noexcept { return false; }
    void invalidatePaint();
    /** Mark one global logical paint region without invalidating the full widget. */
    void invalidatePaint(const graphics::RectF& damageRect);
    uint64_t getLayoutRevision() const noexcept { return m_layoutRevision; }
    uint64_t getPaintRevision() const noexcept { return m_paintRevision; }
    uint64_t getPresentationRevision() const noexcept {
        return m_presentationRevision;
    }
    bool isLayoutDirty() const noexcept { return m_layoutDirty; }
    void setRenderPass(RenderPass* pass);
    void setMotionCoordinator(MotionCoordinator* coordinator);
    MotionCoordinator* getMotionCoordinator() const noexcept { return m_motionCoordinator; }
    void setInteractionMotionTheme(InteractionMotionTheme theme) { m_interactionTheme = std::move(theme); }
    void clearInteractionMotionTheme() { m_interactionTheme.reset(); }
    const InteractionMotionTheme& interactionMotionTheme() const;
    void setInteractionStyle(InteractionState state, InteractionStyle style);
    void clearInteractionStyle(InteractionState state);
    /** Apply one explicit native style; no selector or cascade is involved. */
    void useStyle(lcl::theme::WidgetStyle style);
    /** Bind to a semantic role that is resolved again whenever Theme changes. */
    void useThemeStyle(lcl::theme::WidgetStyleRole role);
    void clearStyle();
    const lcl::theme::Theme& getTheme() const noexcept;
    void addEffect(EffectSource source, const Effect& effect);
    void setEffects(EffectSource source, std::vector<Effect> effects);
    void clearEffects(EffectSource source);
    const std::vector<Effect>& getEffects(EffectSource source) const;
    void setInteractionEnabled(bool enabled);
    bool isInteractionEnabled() const noexcept { return m_interactionEnabled; }
    virtual void setOnClick(std::function<void()> callback) {
        m_onClick = std::move(callback);
        if (m_onClick) {
            setFocusable(true);
        }
    }

    using GcMarkCallback = std::function<void(void* rt, void* mark_func)>;
    void setGcMarkCallback(GcMarkCallback cb) { m_gcMarkCallback = std::move(cb); }
    const GcMarkCallback& getGcMarkCallback() const { return m_gcMarkCallback; }
    void setDestructionCallback(std::function<void()> callback) {
        m_destructionCallback = std::move(callback);
    }

    virtual void syncLayout(float parentAbsX = 0.0f, float parentAbsY = 0.0f);
    virtual void draw(graphics::Canvas& canvas, const graphics::RectF& damageRect);
    virtual void collectEffects(std::vector<EffectRegion>& outEffects) const;

    // Ancestor observation phase. It cannot consume normal target/bubble dispatch.
    virtual void onPointerEventPreview(const PointerEvent& event) { (void)event; }

    // Polymorphic Event Handlers (Return true if handled, false to bubble to parent)
    virtual bool onPointerEnter(const PointerEvent& event);
    virtual bool onPointerLeave(const PointerEvent& event);
    virtual bool onPointerDown(const PointerEvent& event);
    virtual bool onPointerUp(const PointerEvent& event);
    virtual bool onPointerCancel(const PointerEvent& event);
    virtual bool onPointerMove(const PointerEvent& event) { (void)event; return false; }
    virtual bool onScroll(const PointerEvent& event) { (void)event; return false; }
    virtual bool shouldFocusOnPointerDown(const PointerEvent& event) const {
        (void)event;
        return true;
    }
    // Touch focus is committed by EventDispatcher only after a matching tap
    // completes. Widgets may opt out without implementing pointer tracking.
    virtual bool shouldFocusOnTouchTap(const PointerEvent& event) const {
        (void)event;
        return true;
    }
    virtual bool onKeyDown(const KeyEvent& event) { (void)event; return false; }
    virtual bool onKeyUp(const KeyEvent& event) { (void)event; return false; }
    virtual bool onTextInput(const TextInputEvent& event) { (void)event; return false; }
    virtual bool onFocusGained(const FocusEvent& event);
    virtual bool onFocusLost(const FocusEvent& event);

protected:
    /** Schedule old/new presentation pixels without invalidating paint caches. */
    void invalidatePresentation();
    /** Schedule a previous paint extent before a style change shrinks it. */
    void invalidatePaintFrom(const graphics::RectF& previousPaintBounds);
    void beginPresentation(graphics::Canvas& canvas,
                           const graphics::RectF& damageRect) const;
    void endPresentation(graphics::Canvas& canvas) const;
    void drawChildren(graphics::Canvas& canvas, const graphics::RectF& damageRect);
    const lcl::theme::WidgetStyle* resolvedStyle() const noexcept;
    bool hasStyleOverride() const noexcept {
        return m_explicitStyle.has_value() || m_themeStyleRole.has_value();
    }
    virtual const lcl::theme::WidgetStyle* defaultStyle() const noexcept {
        return nullptr;
    }
    virtual graphics::RectF getUntransformedPaintBounds() const noexcept {
        return m_absoluteBounds;
    }
    /** Shape used to mask effects. Plain widgets are rectangular. */
    virtual float effectCornerRadius() const noexcept { return 0.0f; }
    virtual float effectCornerRoundness() const noexcept { return 2.0f; }
    virtual void styleDidChange() {}
    /** Apply a control-owned default without marking it as an app override. */
    void setDefaultWidth(float width);
    void setDefaultHeight(float height);

private:
    // Declared before child ownership so children release their layout nodes
    // before the parent node during reverse-order member destruction.
    std::unique_ptr<detail::LayoutNode> m_layoutNode;

protected:
    Widget* m_parent{nullptr};
    std::vector<std::unique_ptr<Widget>> m_children;

    graphics::RectF m_bounds{0.0f, 0.0f, 0.0f, 0.0f};
    graphics::RectF m_absoluteBounds{0.0f, 0.0f, 0.0f, 0.0f};
    bool m_visible{true};
    bool m_focusable{false};
    bool m_clipsToBounds{false};
    RenderPass* m_renderPass{nullptr};
    MotionCoordinator* m_motionCoordinator{nullptr};
    GcMarkCallback m_gcMarkCallback{nullptr};
    std::function<void()> m_destructionCallback{nullptr};

    uint64_t m_objectId{0};
    float m_opacity{1.0f};
    PresentationState m_presentation{};
    PresentationState m_modelTransform{};
    float m_modelWidth{0.0f};
    float m_modelHeight{0.0f};
    float m_presentWidth{0.0f};
    float m_presentHeight{0.0f};
    bool m_hasWidth{false};
    bool m_hasHeight{false};
    std::array<float, 4> m_modelPadding{};
    std::array<float, 4> m_presentPadding{};
    std::array<float, 2> m_modelGap{};
    std::array<float, 2> m_presentGap{};
    std::array<float, 4> m_modelPosition{};
    std::array<float, 4> m_presentPosition{};
    std::shared_ptr<uint8_t> m_lifetimeToken{std::make_shared<uint8_t>(0)};
    std::optional<InteractionMotionTheme> m_interactionTheme;
    std::array<std::optional<InteractionStyle>, 5> m_interactionStyles;
    const lcl::theme::ThemeContext* m_themeContext{nullptr};
    std::optional<lcl::theme::WidgetStyle> m_explicitStyle;
    std::optional<lcl::theme::WidgetStyleRole> m_themeStyleRole;
    std::function<void()> m_onClick{nullptr};
    bool m_interactionEnabled{true};
    bool m_declarativeHovered{false};
    bool m_declarativePressed{false};
    bool m_declarativeFocused{false};
    std::array<std::vector<Effect>, 3> m_effects;

private:
    friend class WindowApp;
    friend class ScrollView;
    friend class MeasuredWidget;
    friend struct detail::RetainedRenderAccess;
    void setMeasureCallback(
        std::function<layout::Size(const layout::Constraints&)> callback);
    void invalidateMeasurement();
    layout::PositionType positionType() const;
    void invalidatePresentation(const graphics::RectF& previousBounds);
    void invalidatePresentationForCompositing(
        const graphics::RectF& previousBounds);
    graphics::RectF mapPresentationRect(
        const graphics::RectF& rect, const Widget* firstTransform) const;
    void invalidateLayout();
    void advancePaintRevision();
    void propagateDescendantPaintRevision();
    void propagateDescendantPresentationRevision();
    void markLayoutDirty();
    void clearLayoutDirty();
    void setParentControlledTranslationY(float value);
    void setRetainedPresentationBoundary(bool enabled) noexcept;
    void invalidateRetainedPresentationCache() noexcept;
    graphics::Matrix3 presentationMatrix() const noexcept;
    graphics::RectF retainedPresentationSourceBounds() const;
    void collectRetainedPresentationBounds(
        const Widget& root, graphics::RectF& bounds,
        bool& initialized) const;
    void setThemeContext(const lcl::theme::ThemeContext* context);
    bool hasDeclarativeInteraction() const;
    void applyDeclarativeInteractionState();
    static std::atomic<uint64_t> s_nextObjectId;
    // Local counters exclude descendant propagation. The retained compiler
    // uses them to assign flattened content to its nearest retained owner.
    uint64_t m_localPaintRevision{0};
    uint64_t m_localPresentationRevision{0};
    uint64_t m_paintRevision{0};
    uint64_t m_presentationRevision{0};
    uint64_t m_layoutRevision{1};
    bool m_layoutDirty{true};
    bool m_retainedPresentationBoundary{false};
    mutable bool m_recordingRetainedPresentationCache{false};
    mutable bool m_retainedPresentationCacheValid{false};
    mutable graphics::RectF m_retainedPresentationSourceBounds{};
    mutable float m_retainedPresentationDeviceScale{0.0f};
};

} // namespace lcl::ui
