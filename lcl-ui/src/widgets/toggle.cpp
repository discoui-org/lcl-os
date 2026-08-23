#include "lcl-ui/widgets/toggle.hpp"

#include "lcl-graphics/canvas.hpp"

#include <algorithm>
#include <cmath>

namespace lcl::ui {
namespace {

constexpr float kDefaultWidth = 60.0f;
constexpr float kTrackWidth = 52.0f;
constexpr float kTrackHeight = 32.0f;
constexpr float kThumbInset = 3.0f;
constexpr float kFocusInset = 3.0f;

} // namespace

Toggle::Toggle(bool value)
    : m_value(value), m_thumbProgress(value ? 1.0f : 0.0f) {
    setWidth(kDefaultWidth);
    m_yogaNode.setHeight(getTheme().metrics.largeControlHeight);
    setFocusable(true);
    styleDidChange();
}

void Toggle::setValue(bool value) {
    if (m_value == value) return;

    m_value = value;
    markDirty();
    retargetThumb();

    // Keep notification last: callbacks may remove or destroy this Toggle.
    auto callback = m_onChange;
    if (callback) callback(m_value);
}

void Toggle::setEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (!enabled) {
        m_hovered = false;
        m_pressed = false;
        m_pointerArmed = false;
    }
    setInteractionEnabled(enabled);
    markDirty();
}

bool Toggle::onPointerEnter(const PointerEvent& event) {
    if (!m_enabled) return false;
    if (event.source == PointerSource::Mouse) m_hovered = true;
    Widget::onPointerEnter(event);
    markDirty();
    return true;
}

bool Toggle::onPointerLeave(const PointerEvent& event) {
    m_hovered = false;
    m_pressed = false;
    m_pointerArmed = false;
    Widget::onPointerLeave(event);
    markDirty();
    return m_enabled;
}

bool Toggle::onPointerDown(const PointerEvent& event) {
    if (!m_enabled || (event.source == PointerSource::Mouse && event.button != 0)) {
        return false;
    }
    m_pressed = true;
    m_pointerArmed = true;
    Widget::onPointerDown(event);
    markDirty();
    return true;
}

bool Toggle::onPointerUp(const PointerEvent& event) {
    if (!m_enabled) return false;

    const bool wasArmed = m_pointerArmed;
    const bool activate = wasArmed &&
        (event.source == PointerSource::Mouse
            ? event.button == 0
            : event.isTouchTapCompletion());
    m_pointerArmed = false;
    m_pressed = false;

    if (event.source == PointerSource::Touch) {
        // A validated tap supplies activation; touch never leaves hover behind.
        Widget::onPointerCancel(event);
        m_hovered = false;
    } else {
        Widget::onPointerUp(event);
        m_hovered = true;
    }
    markDirty();
    if (activate) setValue(!m_value);
    return wasArmed;
}

bool Toggle::onPointerCancel(const PointerEvent& event) {
    m_hovered = false;
    m_pressed = false;
    m_pointerArmed = false;
    Widget::onPointerCancel(event);
    markDirty();
    return m_enabled;
}

bool Toggle::onKeyDown(const KeyEvent& event) {
    if (!m_enabled || event.key != lcl::platform::PhysicalKey::Space) {
        return false;
    }
    setValue(!m_value);
    return true;
}

bool Toggle::onFocusGained(const FocusEvent& event) {
    m_focused = true;
    Widget::onFocusGained(event);
    markDirty();
    return false;
}

bool Toggle::onFocusLost(const FocusEvent& event) {
    m_focused = false;
    m_pressed = false;
    m_pointerArmed = false;
    Widget::onFocusLost(event);
    markDirty();
    return false;
}

graphics::RectF Toggle::trackRect() const noexcept {
    const float width = std::min(kTrackWidth, std::max(0.0f, m_absoluteBounds.width));
    const float height = std::min(kTrackHeight, std::max(0.0f, m_absoluteBounds.height));
    return {
        m_absoluteBounds.x + (m_absoluteBounds.width - width) * 0.5f,
        m_absoluteBounds.y + (m_absoluteBounds.height - height) * 0.5f,
        width,
        height,
    };
}

graphics::RectF Toggle::thumbRect() const noexcept {
    const graphics::RectF track = trackRect();
    const float diameter = std::max(0.0f, track.height - kThumbInset * 2.0f);
    const float offX = track.x + kThumbInset;
    const float onX = track.x + track.width - kThumbInset - diameter;
    return {
        offX + (onX - offX) * std::clamp(m_thumbProgress, 0.0f, 1.0f),
        track.y + (track.height - diameter) * 0.5f,
        diameter,
        diameter,
    };
}

Toggle::VisualColors Toggle::visualColors() const noexcept {
    const auto* style = resolvedStyle();
    const auto visual = style
        ? lcl::theme::resolveStyle(*style, visualStyleState())
        : lcl::theme::ResolvedStyle{};
    return {
        m_value ? visual.accent : visual.background,
        visual.border,
        visual.foreground,
        getTheme().colors.separator,
        visual.borderWidth,
    };
}

lcl::theme::StyleState Toggle::visualStyleState() const noexcept {
    if (!m_enabled) return lcl::theme::StyleState::Disabled;
    if (m_pressed) return lcl::theme::StyleState::Pressed;
    if (m_hovered) return lcl::theme::StyleState::Hover;
    if (m_focused) return lcl::theme::StyleState::Focused;
    return lcl::theme::StyleState::Normal;
}

const lcl::theme::WidgetStyle* Toggle::defaultStyle() const noexcept {
    return &getTheme().toggle;
}

void Toggle::styleDidChange() {
    const auto* style = resolvedStyle();
    if (!style) return;
    if (!m_hasHeight) {
        m_yogaNode.setHeight(getTheme().metrics.largeControlHeight);
    }
    const auto sync = [this, style](InteractionState interaction,
                                    lcl::theme::StyleState state) {
        const auto visual = lcl::theme::resolveStyle(*style, state);
        InteractionStyle interactionStyle;
        interactionStyle.scale = visual.scale;
        interactionStyle.opacity = visual.opacity;
        setInteractionStyle(interaction, std::move(interactionStyle));
    };
    sync(InteractionState::Normal, lcl::theme::StyleState::Normal);
    sync(InteractionState::Hover, lcl::theme::StyleState::Hover);
    sync(InteractionState::Pressed, lcl::theme::StyleState::Pressed);
    sync(InteractionState::Focused, lcl::theme::StyleState::Focused);
    sync(InteractionState::Disabled, lcl::theme::StyleState::Disabled);
    markDirty();
}

void Toggle::setThumbPresentation(float progress) {
    const float clamped = std::clamp(progress, 0.0f, 1.0f);
    if (std::fabs(m_thumbProgress - clamped) < 0.0001f) return;
    m_thumbProgress = clamped;
    markDirty();
}

void Toggle::retargetThumb() {
    const float target = m_value ? 1.0f : 0.0f;
    if (!m_motionCoordinator || !interactionMotionTheme().enabled) {
        setThumbPresentation(target);
        return;
    }

    m_motionCoordinator->animateFloat(
        *this, AnimatableProperty::SelectionProgress, m_thumbProgress, target,
        lcl::motion::Motion::spring(0.240f, 0.04f),
        [this](float progress) { setThumbPresentation(progress); });
}

void Toggle::draw(graphics::Canvas& canvas, const graphics::RectF& damageRect) {
    if (!m_visible || !getPresentationBounds().intersects(damageRect)) return;

    beginPresentation(canvas);
    const graphics::RectF track = trackRect();
    const graphics::RectF thumb = thumbRect();
    const VisualColors colors = visualColors();

    if (m_focused && m_enabled) {
        const graphics::RectF focus{
            track.x - kFocusInset, track.y - kFocusInset,
            track.width + kFocusInset * 2.0f,
            track.height + kFocusInset * 2.0f,
        };
        canvas.drawRoundedRect(focus, focus.height * 0.5f,
                               graphics::Color{0, 0, 0, 0}, getTheme().colors.focusRing,
                               2.0f, 1.0f);
    }

    canvas.drawRoundedRect(track, track.height * 0.5f, colors.track,
                           colors.trackBorder, colors.trackBorderWidth, 1.0f);

    const graphics::RectF shadow{thumb.x, thumb.y + 1.5f, thumb.width, thumb.height};
    canvas.drawRoundedRect(shadow, shadow.height * 0.5f,
                           getTheme().colors.controlShadow,
                           graphics::Color{0, 0, 0, 0}, 0.0f, 1.0f);
    canvas.drawRoundedRect(thumb, thumb.height * 0.5f, colors.thumb,
                           colors.thumbBorder, 0.75f, 1.0f);

    endPresentation(canvas);
}

} // namespace lcl::ui
