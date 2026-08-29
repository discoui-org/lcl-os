#include "lcl-ui/widgets/slider.hpp"

#include "lcl-graphics/canvas.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace lcl::ui {
namespace {

constexpr float kDefaultWidth = 180.0f;
constexpr float kDefaultHeight = 32.0f;
constexpr float kFocusInset = 3.0f;

} // namespace

Slider::Slider(float value, float minimum, float maximum, float step)
    : m_minimum(std::min(minimum, maximum)),
      m_maximum(std::max(minimum, maximum)), m_step(std::max(0.0f, step)) {
    setDefaultWidth(kDefaultWidth);
    setDefaultHeight(kDefaultHeight);
    m_value = normalizedAndStepped(value);
    setFocusable(true);
    styleDidChange();
}

float Slider::normalizedAndStepped(float value) const noexcept {
    const float clamped = std::clamp(value, m_minimum, m_maximum);
    if (m_step <= 0.0f) return clamped;
    const float steps = std::round((clamped - m_minimum) / m_step);
    return std::clamp(m_minimum + steps * m_step, m_minimum, m_maximum);
}

void Slider::setValue(float value) {
    const float next = normalizedAndStepped(value);
    if (std::fabs(next - m_value) <= 0.0001f) return;
    m_value = next;
    invalidatePaint();
    auto callback = m_onChange;
    if (callback) callback(m_value);
}

void Slider::setRange(float minimum, float maximum) {
    if (!std::isfinite(minimum) || !std::isfinite(maximum)) return;
    if (minimum > maximum) std::swap(minimum, maximum);
    if (std::fabs(m_minimum - minimum) <= 0.0001f &&
        std::fabs(m_maximum - maximum) <= 0.0001f) return;
    m_minimum = minimum;
    m_maximum = maximum;
    setValue(m_value);
    invalidatePaint();
}

void Slider::setStep(float step) {
    const float next = std::max(0.0f, std::isfinite(step) ? step : 0.0f);
    if (std::fabs(next - m_step) <= 0.0001f) return;
    m_step = next;
    setValue(m_value);
}

void Slider::setEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (!enabled) {
        m_hovered = false;
        m_pressed = false;
        if (m_dragging) setEditing(false);
    }
    setInteractionEnabled(enabled);
    invalidatePaint();
}

float Slider::normalizedValue() const noexcept {
    const float span = m_maximum - m_minimum;
    return span <= 0.0001f ? 0.0f : (m_value - m_minimum) / span;
}

void Slider::updateFromPointer(float x) {
    const float thumb = getTheme().metrics.sliderThumbSize;
    const float left = m_absoluteBounds.x + thumb * 0.5f;
    const float width = std::max(1.0f, m_absoluteBounds.width - thumb);
    const float progress = std::clamp((x - left) / width, 0.0f, 1.0f);
    setValue(m_minimum + progress * (m_maximum - m_minimum));
}

void Slider::setEditing(bool editing) {
    if (m_dragging == editing) return;
    m_dragging = editing;
    auto callback = m_onEditingChanged;
    if (callback) callback(editing);
}

bool Slider::onPointerEnter(const PointerEvent& event) {
    if (!m_enabled) return false;
    if (event.source == PointerSource::Mouse) m_hovered = true;
    invalidatePaint();
    return true;
}

bool Slider::onPointerLeave(const PointerEvent&) {
    if (!m_dragging) {
        m_hovered = false;
        m_pressed = false;
    }
    invalidatePaint();
    return m_enabled;
}

bool Slider::onPointerDown(const PointerEvent& event) {
    if (!m_enabled || (event.source == PointerSource::Mouse && event.button != 0)) {
        return false;
    }
    if (!event.capturePointer(*this)) return false;
    m_pressed = true;
    setEditing(true);
    updateFromPointer(event.x);
    invalidatePaint();
    return true;
}

bool Slider::onPointerMove(const PointerEvent& event) {
    if (!m_enabled || !m_dragging) return false;
    updateFromPointer(event.x);
    return true;
}

bool Slider::onPointerUp(const PointerEvent& event) {
    if (!m_enabled || !m_dragging) return false;
    updateFromPointer(event.x);
    event.releasePointerCapture(*this);
    m_pressed = false;
    m_hovered = event.source == PointerSource::Mouse &&
        containsPresentationPoint(event.x, event.y);
    setEditing(false);
    invalidatePaint();
    return true;
}

bool Slider::onPointerCancel(const PointerEvent& event) {
    if (m_dragging) event.releasePointerCapture(*this);
    m_pressed = false;
    m_hovered = false;
    setEditing(false);
    invalidatePaint();
    return m_enabled;
}

bool Slider::onKeyDown(const KeyEvent& event) {
    if (!m_enabled) return false;
    const float increment = m_step > 0.0f
        ? m_step
        : std::max((m_maximum - m_minimum) / 100.0f, 0.01f);
    switch (event.key) {
        case lcl::platform::PhysicalKey::ArrowLeft:
        case lcl::platform::PhysicalKey::ArrowDown:
            setValue(m_value - increment);
            return true;
        case lcl::platform::PhysicalKey::ArrowRight:
        case lcl::platform::PhysicalKey::ArrowUp:
            setValue(m_value + increment);
            return true;
        case lcl::platform::PhysicalKey::Home:
            setValue(m_minimum);
            return true;
        case lcl::platform::PhysicalKey::End:
            setValue(m_maximum);
            return true;
        default:
            return false;
    }
}

bool Slider::onFocusGained(const FocusEvent&) {
    m_focused = true;
    invalidatePaint();
    return false;
}

bool Slider::onFocusLost(const FocusEvent&) {
    m_focused = false;
    m_pressed = false;
    setEditing(false);
    invalidatePaint();
    return false;
}

lcl::theme::StyleState Slider::visualStyleState() const noexcept {
    if (!m_enabled) return lcl::theme::StyleState::Disabled;
    if (m_pressed) return lcl::theme::StyleState::Pressed;
    if (m_hovered) return lcl::theme::StyleState::Hover;
    if (m_focused) return lcl::theme::StyleState::Focused;
    return lcl::theme::StyleState::Normal;
}

const lcl::theme::WidgetStyle* Slider::defaultStyle() const noexcept {
    return &getTheme().slider;
}

void Slider::styleDidChange() {
    const auto* style = resolvedStyle();
    if (!style) return;
    const auto sync = [this, style](InteractionState interaction,
                                    lcl::theme::StyleState state) {
        const auto visual = lcl::theme::resolveStyle(*style, state);
        InteractionStyle interactionStyle;
        interactionStyle.opacity = visual.opacity;
        setInteractionStyle(interaction, std::move(interactionStyle));
    };
    sync(InteractionState::Normal, lcl::theme::StyleState::Normal);
    sync(InteractionState::Hover, lcl::theme::StyleState::Hover);
    sync(InteractionState::Pressed, lcl::theme::StyleState::Pressed);
    sync(InteractionState::Focused, lcl::theme::StyleState::Focused);
    sync(InteractionState::Disabled, lcl::theme::StyleState::Disabled);
    invalidatePaint();
}

void Slider::draw(graphics::Canvas& canvas,
                  const graphics::RectF& damageRect) {
    if (!m_visible || !getPresentationPaintBounds().intersects(damageRect)) return;
    beginPresentation(canvas, damageRect);

    const auto visual = lcl::theme::resolveStyle(*resolvedStyle(),
                                                 visualStyleState());
    const float thumbSize = std::min(getTheme().metrics.sliderThumbSize,
                                     m_absoluteBounds.height);
    const float trackHeight = std::min(getTheme().metrics.sliderTrackHeight,
                                       m_absoluteBounds.height);
    const graphics::RectF track{
        m_absoluteBounds.x + thumbSize * 0.5f,
        m_absoluteBounds.y + (m_absoluteBounds.height - trackHeight) * 0.5f,
        std::max(0.0f, m_absoluteBounds.width - thumbSize), trackHeight};
    const float progress = normalizedValue();
    const graphics::RectF fill{track.x, track.y, track.width * progress,
                               track.height};
    const graphics::RectF thumb{
        track.x + track.width * progress - thumbSize * 0.5f,
        m_absoluteBounds.y + (m_absoluteBounds.height - thumbSize) * 0.5f,
        thumbSize, thumbSize};

    if (m_focused && m_enabled) {
        const graphics::RectF focus{thumb.x - kFocusInset, thumb.y - kFocusInset,
                                   thumb.width + kFocusInset * 2.0f,
                                   thumb.height + kFocusInset * 2.0f};
        canvas.drawEllipse(focus, graphics::Paint{
            .color = getTheme().colors.focusRing,
            .style = graphics::PaintStyle::Stroke,
            .stroke = graphics::StrokeStyle{.width = 2.0f}});
    }
    canvas.drawRoundedRect(track, trackHeight * 0.5f, visual.background,
                           {}, 0.0f, 1.0f);
    if (fill.width > 0.0f) {
        canvas.drawRoundedRect(fill, trackHeight * 0.5f, visual.accent,
                               {}, 0.0f, 1.0f);
    }
    const graphics::RectF shadow{thumb.x, thumb.y + 1.0f,
                                 thumb.width, thumb.height};
    canvas.drawEllipse(shadow, graphics::Paint{
        .color = getTheme().colors.controlShadow});
    canvas.drawEllipse(thumb, graphics::Paint{
        .color = visual.foreground});

    endPresentation(canvas);
}

} // namespace lcl::ui
