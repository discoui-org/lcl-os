#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>

#include "lcl-motion/motion.hpp"
#include "lcl-graphics/canvas.hpp"

namespace lcl::ui {

class Widget;

using Motion = lcl::motion::Motion;
using Easing = lcl::motion::Easing;
using EasingName = lcl::motion::EasingName;
using Keyframe = lcl::motion::Keyframe;
using AnimationOptions = lcl::motion::AnimationOptions;
using AnimationHandle = lcl::motion::AnimationHandle;
using FillMode = lcl::motion::FillMode;
using PlaybackDirection = lcl::motion::PlaybackDirection;

enum class LayoutMode : uint8_t { Reflow, Morph };

struct AnimationTransactionOptions {
    LayoutMode layout{LayoutMode::Reflow};
};

enum class AnimatableProperty : uint32_t {
    Opacity = 1,
    TranslationX,
    TranslationY,
    ScaleX,
    ScaleY,
    Rotation,
    TransformOriginX,
    TransformOriginY,
    Width,
    Height,
    PaddingLeft,
    PaddingTop,
    PaddingRight,
    PaddingBottom,
    GapRow,
    GapColumn,
    PositionLeft,
    PositionTop,
    PositionRight,
    PositionBottom,
    BackgroundRed,
    BackgroundGreen,
    BackgroundBlue,
    BackgroundAlpha,
    BorderRed,
    BorderGreen,
    BorderBlue,
    BorderAlpha,
    BorderWidth,
    BorderRadius,
    /** Generic selected/unselected control presentation channel. */
    SelectionProgress,
};

struct PresentationState {
    float opacity{1.0f};
    float translationX{0.0f};
    float translationY{0.0f};
    float scaleX{1.0f};
    float scaleY{1.0f};
    float rotationRadians{0.0f};
    float originX{0.5f};
    float originY{0.5f};
};

struct InteractionMotionTheme {
    bool enabled{true};
    float hoverScale{1.015f};
    float pressedScale{0.965f};
    lcl::motion::Motion hover{lcl::motion::tokens::hover()};
    lcl::motion::Motion pressed{lcl::motion::tokens::pressed()};
    lcl::motion::Motion release{lcl::motion::tokens::release()};
    lcl::motion::Motion focusTransition{lcl::motion::tokens::focus()};
};

enum class InteractionState : uint8_t {
    Normal,
    Hover,
    Pressed,
    Focused,
    Disabled,
};

/** Presentation-only pseudo-state values for custom interactive widgets. */
struct InteractionStyle {
    std::optional<float> scale;
    std::optional<float> opacity;
    std::optional<lcl::motion::Motion> motion;
};

class MotionCoordinator {
public:
    using ApplyFloat = std::function<void(float)>;
    using LayoutCallback = std::function<void()>;
    using DamageCallback = std::function<void(const graphics::RectF&)>;
    /**
     * A presentation-only callback owned by an active widget. Unlike a
     * property animation, it controls its own discrete presentation state.
     */
    using PresentationCallback = std::function<void(float)>;

    MotionCoordinator() = default;

    void setCallbacks(LayoutCallback layout, DamageCallback damage);
    void setInteractionTheme(InteractionMotionTheme theme) { m_interactionTheme = std::move(theme); }
    const InteractionMotionTheme& interactionTheme() const noexcept { return m_interactionTheme; }
    void beginTransaction(const lcl::motion::Motion& motion,
                          AnimationTransactionOptions options);
    void endTransaction();
    bool inTransaction() const noexcept { return m_transactionActive; }
    LayoutMode transactionLayoutMode() const noexcept { return m_options.layout; }

    void setFloat(Widget& widget, AnimatableProperty property,
                  float presentationValue, float targetValue,
                  ApplyFloat applyPresentation, bool affectsLayout = false,
                  const lcl::motion::Motion* overrideMotion = nullptr);
    void animateFloat(Widget& widget, AnimatableProperty property,
                      float presentationValue, float targetValue,
                      const lcl::motion::Motion& motion,
                      ApplyFloat applyPresentation, bool affectsLayout = false);
    void setColor(Widget& widget, AnimatableProperty firstChannel,
                  graphics::Color presentation, graphics::Color target,
                  std::function<void(graphics::Color)> applyPresentation,
                  const lcl::motion::Motion* overrideMotion = nullptr);
    lcl::motion::AnimationHandle animate(Widget& widget, AnimatableProperty property,
                                         std::vector<lcl::motion::Keyframe> keyframes,
                                         const lcl::motion::AnimationOptions& options = {});

    bool tick(float dtSec);
    bool hasActiveAnimations() const noexcept;
    bool isObjectAnimating(uint64_t objectId) const;
    /** Registers an active presentation owner; Widget teardown removes it. */
    void registerPresentation(Widget& widget, PresentationCallback callback);
    void unregisterPresentation(uint64_t objectId);
    void unregisterObject(uint64_t objectId);
    void clear();

private:
    struct Binding {
        uint64_t objectId{0};
        Widget* widget{nullptr};
        ApplyFloat apply;
        bool affectsLayout{false};
    };

    struct PresentationBinding {
        std::weak_ptr<uint8_t> lifetime;
        PresentationCallback update;
    };

    void tickPresentations(float dtSec);

    lcl::motion::AnimationEngine m_engine;
    lcl::motion::Timeline m_timeline;
    std::unordered_map<lcl::motion::ChannelId, Binding> m_bindings;
    std::unordered_map<uint64_t, PresentationBinding> m_presentationBindings;
    lcl::motion::Motion m_motion{lcl::motion::Motion::spring(0.18f, 0.0f)};
    AnimationTransactionOptions m_options{};
    bool m_transactionActive{false};
    LayoutCallback m_layoutCallback;
    DamageCallback m_damageCallback;
    InteractionMotionTheme m_interactionTheme{};
};

} // namespace lcl::ui
