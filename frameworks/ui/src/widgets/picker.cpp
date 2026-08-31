#include "lcl-ui/widgets/picker.hpp"

#include "lcl-graphics/canvas.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace lcl::ui {
namespace {

constexpr float kDefaultWidth = 240.0f;
constexpr float kRadioRowHeight = 28.0f;
constexpr float kLabelHeight = 24.0f;
constexpr float kTextSize = 14.0f;
constexpr float kHorizontalPadding = 12.0f;
constexpr float kFocusInset = 3.0f;

} // namespace

Picker::Picker(WindowApp& window, PopupCanvasFactory popupCanvasFactory,
               std::string label, std::vector<PickerOption> options,
               size_t selectedIndex, PopupConnector popupConnector)
    : m_menu(window, std::move(popupCanvasFactory), std::move(popupConnector)),
      m_label(std::move(label)), m_options(std::move(options)),
      m_selectedIndex(selectedIndex) {
    normalizeSelection();
    setDefaultWidth(kDefaultWidth);
    updateDefaultSize();
    setFocusable(true);
    styleDidChange();
}

void Picker::setLabel(std::string label) {
    if (m_label == label) return;
    m_label = std::move(label);
    updateDefaultSize();
    invalidatePaint();
}

void Picker::setOptions(std::vector<PickerOption> options) {
    m_options = std::move(options);
    normalizeSelection();
    m_hoveredIndex.reset();
    updateDefaultSize();
    invalidatePaint();
}

void Picker::setSelectedIndex(size_t index) {
    if (index >= m_options.size() || !m_options[index].enabled ||
        index == m_selectedIndex) return;
    m_selectedIndex = index;
    invalidatePaint();
    auto callback = m_onChange;
    if (callback) callback(index);
}

void Picker::setPickerStyle(PickerStyle style) {
    if (m_style == style) return;
    m_style = style;
    m_hoveredIndex.reset();
    updateDefaultSize();
    invalidatePaint();
}

void Picker::setEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (!enabled) {
        m_hovered = false;
        m_pressed = false;
        m_pointerArmed = false;
        m_hoveredIndex.reset();
    }
    setInteractionEnabled(enabled);
    invalidatePaint();
}

PickerStyle Picker::resolvedPickerStyle() const noexcept {
    return m_style == PickerStyle::Automatic ? PickerStyle::Menu : m_style;
}

void Picker::updateDefaultSize() {
    if (m_hasHeight) return;
    if (resolvedPickerStyle() == PickerStyle::RadioGroup) {
        const float labelHeight = m_label.empty() ? 0.0f : kLabelHeight;
        setDefaultHeight(labelHeight +
            static_cast<float>(m_options.size()) * kRadioRowHeight);
    } else {
        setDefaultHeight(getTheme().metrics.regularControlHeight);
    }
}

std::optional<size_t> Picker::optionIndexAt(float x, float y) const noexcept {
    if (resolvedPickerStyle() != PickerStyle::RadioGroup ||
        x < m_absoluteBounds.x || x >= m_absoluteBounds.x + m_absoluteBounds.width) {
        return std::nullopt;
    }
    const float startY = m_absoluteBounds.y +
        (m_label.empty() ? 0.0f : kLabelHeight);
    if (y < startY) return std::nullopt;
    const size_t index = static_cast<size_t>((y - startY) / kRadioRowHeight);
    return index < m_options.size() ? std::optional<size_t>(index)
                                    : std::nullopt;
}

bool Picker::onPointerEnter(const PointerEvent& event) {
    if (!m_enabled) return false;
    m_hovered = event.source == PointerSource::Mouse;
    m_hoveredIndex = optionIndexAt(event.x, event.y);
    invalidatePaint();
    return true;
}

bool Picker::onPointerLeave(const PointerEvent&) {
    if (!m_pointerArmed) {
        m_hovered = false;
        m_pressed = false;
        m_hoveredIndex.reset();
    }
    invalidatePaint();
    return m_enabled;
}

bool Picker::onPointerDown(const PointerEvent& event) {
    if (!m_enabled || (event.source == PointerSource::Mouse && event.button != 0)) {
        return false;
    }
    m_pressed = true;
    m_pointerArmed = true;
    m_hoveredIndex = optionIndexAt(event.x, event.y);
    invalidatePaint();
    return true;
}

bool Picker::onPointerMove(const PointerEvent& event) {
    if (!m_enabled) return false;
    const auto next = optionIndexAt(event.x, event.y);
    if (next != m_hoveredIndex) {
        m_hoveredIndex = next;
        invalidatePaint();
    }
    return m_pointerArmed;
}

bool Picker::onPointerUp(const PointerEvent& event) {
    if (!m_enabled) return false;
    const bool wasArmed = m_pointerArmed;
    const bool activate = wasArmed &&
        (event.source == PointerSource::Mouse
            ? event.button == 0 && containsPresentationPoint(event.x, event.y)
            : event.isTouchTapCompletion());
    m_pointerArmed = false;
    m_pressed = false;
    m_hovered = event.source == PointerSource::Mouse;
    const auto index = optionIndexAt(event.x, event.y);
    m_hoveredIndex = index;
    invalidatePaint();
    if (activate) {
        if (resolvedPickerStyle() == PickerStyle::Menu) showMenu();
        else if (index) setSelectedIndex(*index);
    }
    return wasArmed;
}

bool Picker::onPointerCancel(const PointerEvent&) {
    m_hovered = false;
    m_pressed = false;
    m_pointerArmed = false;
    m_hoveredIndex.reset();
    invalidatePaint();
    return m_enabled;
}

bool Picker::moveSelection(int direction) {
    if (m_options.empty()) return true;
    const size_t count = m_options.size();
    for (size_t step = 1; step <= count; ++step) {
        const size_t index = direction < 0
            ? (m_selectedIndex + count - (step % count)) % count
            : (m_selectedIndex + step) % count;
        if (!m_options[index].enabled) continue;
        setSelectedIndex(index);
        return true;
    }
    return true;
}

void Picker::normalizeSelection() {
    if (m_options.empty()) {
        m_selectedIndex = 0;
        return;
    }
    m_selectedIndex = std::min(m_selectedIndex, m_options.size() - 1);
    if (m_options[m_selectedIndex].enabled) return;
    const auto firstEnabled = std::find_if(
        m_options.begin(), m_options.end(),
        [](const PickerOption& option) { return option.enabled; });
    if (firstEnabled != m_options.end()) {
        m_selectedIndex = static_cast<size_t>(
            std::distance(m_options.begin(), firstEnabled));
    }
}

bool Picker::onKeyDown(const KeyEvent& event) {
    if (!m_enabled) return false;
    switch (event.key) {
        case lcl::platform::PhysicalKey::ArrowLeft:
        case lcl::platform::PhysicalKey::ArrowUp:
            return moveSelection(-1);
        case lcl::platform::PhysicalKey::ArrowRight:
        case lcl::platform::PhysicalKey::ArrowDown:
            return moveSelection(1);
        case lcl::platform::PhysicalKey::Enter:
        case lcl::platform::PhysicalKey::Space:
            if (resolvedPickerStyle() == PickerStyle::Menu) {
                showMenu();
                return true;
            }
            return false;
        default:
            return false;
    }
}

bool Picker::onFocusGained(const FocusEvent&) {
    m_focused = true;
    invalidatePaint();
    return false;
}

bool Picker::onFocusLost(const FocusEvent&) {
    m_focused = false;
    m_pressed = false;
    m_pointerArmed = false;
    invalidatePaint();
    return false;
}

void Picker::showMenu() {
    if (m_options.empty()) return;
    std::vector<MenuItem> items;
    items.reserve(m_options.size());
    const auto lifetime = getLifetimeToken();
    for (size_t index = 0; index < m_options.size(); ++index) {
        items.push_back(MenuItem{
            m_options[index].label,
            m_options[index].enabled,
            [this, lifetime, index] {
                if (!lifetime.expired()) setSelectedIndex(index);
            },
            index == m_selectedIndex,
        });
    }
    m_menu.show(*this, std::move(items), MenuOptions{
        .width = std::max(160.0f, m_absoluteBounds.width),
        .itemHeight = getTheme().metrics.regularControlHeight,
        .onDismissed = {},
    });
}

lcl::theme::StyleState Picker::visualStyleState() const noexcept {
    if (!m_enabled) return lcl::theme::StyleState::Disabled;
    if (m_pressed) return lcl::theme::StyleState::Pressed;
    if (m_hovered) return lcl::theme::StyleState::Hover;
    if (m_focused) return lcl::theme::StyleState::Focused;
    return lcl::theme::StyleState::Normal;
}

const lcl::theme::WidgetStyle* Picker::defaultStyle() const noexcept {
    return &getTheme().picker;
}

void Picker::styleDidChange() {
    updateDefaultSize();
    invalidatePaint();
}

void Picker::draw(graphics::Canvas& canvas,
                  const graphics::RectF& damageRect) {
    if (!m_visible || isCollapsed() || !getPresentationPaintBounds().intersects(damageRect)) return;
    beginPresentation(canvas, damageRect);
    const auto visual = lcl::theme::resolveStyle(*resolvedStyle(),
                                                 visualStyleState());

    if (resolvedPickerStyle() == PickerStyle::Menu) {
        if (m_focused && m_enabled) {
            const graphics::RectF focus{
                m_absoluteBounds.x - kFocusInset,
                m_absoluteBounds.y - kFocusInset,
                m_absoluteBounds.width + kFocusInset * 2.0f,
                m_absoluteBounds.height + kFocusInset * 2.0f};
            canvas.drawRoundedRect(focus, visual.cornerRadius + kFocusInset,
                                   {}, getTheme().colors.focusRing, 2.0f, 1.0f);
        }
        canvas.drawRoundedRect(m_absoluteBounds, visual.cornerRadius,
                               visual.background, visual.border,
                               visual.borderWidth, 1.0f);
        if (!m_label.empty()) {
            canvas.drawText(m_absoluteBounds.x + kHorizontalPadding,
                            m_absoluteBounds.y +
                                (m_absoluteBounds.height - kTextSize * 1.2f) * 0.5f,
                            m_label, visual.secondaryForeground, kTextSize);
        }
        const std::string selected = m_options.empty()
            ? std::string{}
            : m_options[m_selectedIndex].label;
        const float selectedWidth = canvas.measureText(selected, kTextSize);
        const float chevronX = m_absoluteBounds.x + m_absoluteBounds.width -
            kHorizontalPadding - 8.0f;
        canvas.drawText(std::max(m_absoluteBounds.x + kHorizontalPadding,
                                 chevronX - 10.0f - selectedWidth),
                        m_absoluteBounds.y +
                            (m_absoluteBounds.height - kTextSize * 1.2f) * 0.5f,
                        selected, visual.foreground, kTextSize);
        graphics::Path chevron;
        const float cy = m_absoluteBounds.y + m_absoluteBounds.height * 0.5f;
        chevron.moveTo(chevronX - 3.0f, cy - 2.0f)
               .lineTo(chevronX, cy + 1.0f)
               .lineTo(chevronX + 3.0f, cy - 2.0f);
        graphics::Paint chevronPaint;
        chevronPaint.color = visual.secondaryForeground;
        chevronPaint.style = graphics::PaintStyle::Stroke;
        chevronPaint.stroke.width = 1.5f;
        chevronPaint.stroke.cap = graphics::StrokeCap::Round;
        chevronPaint.stroke.join = graphics::StrokeJoin::Round;
        canvas.drawPath(chevron, chevronPaint);
        endPresentation(canvas);
        return;
    }

    float y = m_absoluteBounds.y;
    if (!m_label.empty()) {
        canvas.drawText(m_absoluteBounds.x, y, m_label,
                        visual.secondaryForeground, kTextSize);
        y += kLabelHeight;
    }
    const float radioSize = getTheme().metrics.radioSize;
    for (size_t index = 0; index < m_options.size(); ++index) {
        const graphics::RectF row{m_absoluteBounds.x, y,
                                  m_absoluteBounds.width, kRadioRowHeight};
        if (m_hoveredIndex == index && m_options[index].enabled) {
            canvas.drawRoundedRect(row, getTheme().metrics.compactCornerRadius,
                                   getTheme().colors.selectionFill,
                                   {}, 0.0f, 1.0f);
        }
        const graphics::RectF radio{
            row.x + 2.0f,
            row.y + (row.height - radioSize) * 0.5f,
            radioSize, radioSize};
        graphics::Paint outer;
        outer.color = index == m_selectedIndex ? visual.accent : visual.border;
        outer.style = graphics::PaintStyle::Stroke;
        outer.stroke.width = index == m_selectedIndex ? 2.0f : 1.0f;
        canvas.drawEllipse(radio, outer);
        if (index == m_selectedIndex) {
            const float inset = radioSize * 0.28f;
            canvas.drawEllipse({radio.x + inset, radio.y + inset,
                                radio.width - inset * 2.0f,
                                radio.height - inset * 2.0f},
                               graphics::Paint{.color = visual.accent});
        }
        graphics::Color labelColor = m_options[index].enabled
            ? visual.foreground
            : getTheme().colors.tertiaryLabel;
        canvas.drawText(radio.x + radio.width + 8.0f,
                        row.y + (row.height - kTextSize * 1.2f) * 0.5f,
                        m_options[index].label, labelColor, kTextSize);
        y += kRadioRowHeight;
    }
    if (m_focused && m_enabled) {
        canvas.drawRoundedRect(m_absoluteBounds,
                               getTheme().metrics.compactCornerRadius,
                               {}, getTheme().colors.focusRing, 1.5f, 1.0f);
    }
    endPresentation(canvas);
}

} // namespace lcl::ui
