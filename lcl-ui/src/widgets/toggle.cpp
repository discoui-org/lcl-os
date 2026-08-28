#include "lcl-ui/widgets/toggle.hpp"

#include "lcl-graphics/canvas.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace lcl::ui {
namespace {

constexpr float kDefaultWidth = 60.0f;
constexpr float kTrackWidth = 52.0f;
constexpr float kTrackHeight = 32.0f;
constexpr float kThumbInset = 3.0f;
constexpr float kFocusInset = 3.0f;
constexpr float kLabelGap = 8.0f;
constexpr float kLabelFontSize = 14.0f;
constexpr float kLabeledWidth = 220.0f;
constexpr float kButtonWidth = 140.0f;

} // namespace

Toggle::Toggle(bool value) : Toggle(std::string{}, value) {}

Toggle::Toggle(std::string label, bool value)
    : m_value(value), m_thumbProgress(value ? 1.0f : 0.0f),
      m_label(std::move(label)) {
    setDefaultWidth(m_label.empty() ? kDefaultWidth : kLabeledWidth);
    setDefaultHeight(getTheme().metrics.largeControlHeight);
    setFocusable(true);
    styleDidChange();
}

void Toggle::setLabel(std::string label) {
    if (m_label == label) return;
    m_label = std::move(label);
    markDirty();
}

void Toggle::setToggleStyle(ToggleStyle style) {
    if (m_style == style) return;
    const graphics::RectF previous = getVisiblePresentationPaintBounds();
    m_style = style;
    if (m_label.empty() && !m_hasWidth) {
        setDefaultWidth(style == ToggleStyle::Button
                                ? kButtonWidth
                                : kDefaultWidth);
    }
    retargetThumb();
    markPaintDirty(previous);
}

void Toggle::setValue(bool value) {
    if (m_value == value && !m_mixed) return;

    m_value = value;
    m_mixed = false;
    markDirty();
    retargetThumb();

    // Keep notification last: callbacks may remove or destroy this Toggle.
    auto callback = m_onChange;
    if (callback) callback(m_value);
}

void Toggle::setMixed(bool mixed) {
    if (m_mixed == mixed) return;
    m_mixed = mixed;
    markDirty();
}

void Toggle::toggleValue() {
    const bool next = m_mixed ? true : !m_value;
    const bool notify = m_mixed && m_value == next;
    m_mixed = false;
    if (notify) {
        markDirty();
        auto callback = m_onChange;
        if (callback) callback(next);
        return;
    }
    setValue(next);
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
    if (activate) toggleValue();
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
    toggleValue();
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
        m_label.empty()
            ? m_absoluteBounds.x + (m_absoluteBounds.width - width) * 0.5f
            : m_absoluteBounds.x + m_absoluteBounds.width - width,
        m_absoluteBounds.y + (m_absoluteBounds.height - height) * 0.5f,
        width,
        height,
    };
}

graphics::RectF Toggle::checkboxRect() const noexcept {
    const float size = std::min(getTheme().metrics.checkboxSize,
                                std::max(0.0f, m_absoluteBounds.height));
    return {
        m_absoluteBounds.x,
        m_absoluteBounds.y + (m_absoluteBounds.height - size) * 0.5f,
        size,
        size,
    };
}

ToggleStyle Toggle::resolvedToggleStyle() const noexcept {
    return m_style == ToggleStyle::Automatic ? ToggleStyle::Switch : m_style;
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
        setDefaultHeight(getTheme().metrics.largeControlHeight);
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
    const VisualColors colors = visualColors();
    const ToggleStyle style = resolvedToggleStyle();

    if (style == ToggleStyle::Checkbox) {
        const graphics::RectF box = checkboxRect();
        const float radius = getTheme().metrics.compactCornerRadius * 0.55f;
        if (m_focused && m_enabled) {
            const graphics::RectF focus{box.x - kFocusInset, box.y - kFocusInset,
                                       box.width + kFocusInset * 2.0f,
                                       box.height + kFocusInset * 2.0f};
            canvas.drawRoundedRect(focus, radius + kFocusInset,
                                   graphics::Color{0, 0, 0, 0},
                                   getTheme().colors.focusRing, 2.0f, 1.0f);
        }
        canvas.drawRoundedRect(box, radius,
                               (m_value || m_mixed) ? colors.track
                                                    : graphics::Color{0, 0, 0, 0},
                               colors.trackBorder,
                               m_value || m_mixed ? 0.0f : colors.trackBorderWidth,
                               1.0f);
        if (m_value || m_mixed) {
            graphics::Path mark;
            if (m_mixed) {
                mark.moveTo(box.x + 4.0f, box.y + box.height * 0.5f)
                    .lineTo(box.x + box.width - 4.0f,
                            box.y + box.height * 0.5f);
            } else {
                mark.moveTo(box.x + 4.0f, box.y + box.height * 0.52f)
                    .lineTo(box.x + box.width * 0.43f, box.y + box.height - 4.0f)
                    .lineTo(box.x + box.width - 3.5f, box.y + 4.0f);
            }
            graphics::Paint paint;
            paint.color = colors.thumb;
            paint.style = graphics::PaintStyle::Stroke;
            paint.stroke.width = 2.0f;
            paint.stroke.cap = graphics::StrokeCap::Round;
            paint.stroke.join = graphics::StrokeJoin::Round;
            canvas.drawPath(mark, paint);
        }
        if (!m_label.empty()) {
            canvas.drawText(box.x + box.width + kLabelGap,
                            m_absoluteBounds.y +
                                (m_absoluteBounds.height - kLabelFontSize * 1.2f) * 0.5f,
                            m_label, colors.thumb, kLabelFontSize);
        }
        endPresentation(canvas);
        return;
    }

    if (style == ToggleStyle::Button) {
        const auto visual = lcl::theme::resolveStyle(
            *resolvedStyle(), visualStyleState());
        const graphics::Color fill = m_value ? visual.accent : visual.background;
        if (m_focused && m_enabled) {
            const graphics::RectF focus{
                m_absoluteBounds.x - kFocusInset,
                m_absoluteBounds.y - kFocusInset,
                m_absoluteBounds.width + kFocusInset * 2.0f,
                m_absoluteBounds.height + kFocusInset * 2.0f};
            canvas.drawRoundedRect(focus, visual.cornerRadius + kFocusInset,
                                   {}, getTheme().colors.focusRing,
                                   2.0f, 1.0f);
        }
        canvas.drawRoundedRect(m_absoluteBounds, visual.cornerRadius, fill,
                               visual.border, visual.borderWidth, 1.0f);
        if (!m_label.empty()) {
            const float width = canvas.measureText(m_label, kLabelFontSize);
            canvas.drawText(m_absoluteBounds.x +
                                std::max(0.0f, (m_absoluteBounds.width - width) * 0.5f),
                            m_absoluteBounds.y +
                                (m_absoluteBounds.height - kLabelFontSize * 1.2f) * 0.5f,
                            m_label, visual.foreground, kLabelFontSize);
        }
        endPresentation(canvas);
        return;
    }

    const graphics::RectF track = trackRect();
    const graphics::RectF thumb = thumbRect();

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

    if (!m_label.empty()) {
        canvas.drawText(m_absoluteBounds.x,
                        m_absoluteBounds.y +
                            (m_absoluteBounds.height - kLabelFontSize * 1.2f) * 0.5f,
                        m_label, colors.thumb, kLabelFontSize);
    }

    endPresentation(canvas);
}

} // namespace lcl::ui
