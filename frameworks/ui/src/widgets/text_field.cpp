#include "lcl-ui/widgets/text_field.hpp"

#include "lcl-graphics/canvas.hpp"
#include "system/render/text_metrics.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

namespace lcl::ui {
namespace {

bool isUtf8Continuation(unsigned char value) noexcept {
    return (value & 0xC0u) == 0x80u;
}

size_t utf8SequenceLength(std::string_view text, size_t offset) noexcept {
    const unsigned char lead = static_cast<unsigned char>(text[offset]);
    size_t length = 1;
    if (lead >= 0xC2u && lead <= 0xDFu) length = 2;
    else if (lead >= 0xE0u && lead <= 0xEFu) length = 3;
    else if (lead >= 0xF0u && lead <= 0xF4u) length = 4;
    if (offset + length > text.size()) return 1;
    for (size_t index = 1; index < length; ++index) {
        if (!isUtf8Continuation(static_cast<unsigned char>(text[offset + index]))) return 1;
    }
    return length;
}

class Utf8CodepointMap {
public:
    explicit Utf8CodepointMap(std::string_view text) : m_text(text) {
        m_boundaries.push_back(0);
        size_t offset = 0;
        while (offset < m_text.size()) {
            offset += utf8SequenceLength(m_text, offset);
            m_boundaries.push_back(offset);
        }
    }

    size_t count() const noexcept { return m_boundaries.size() - 1; }

    size_t byteOffset(size_t codepointIndex) const noexcept {
        return m_boundaries[std::min(codepointIndex, count())];
    }

    size_t previousBoundary(size_t byteOffset) const noexcept {
        const auto found = std::lower_bound(
            m_boundaries.begin(), m_boundaries.end(), byteOffset);
        if (found == m_boundaries.begin()) return 0;
        return *(found - 1);
    }

    size_t nextBoundary(size_t byteOffset) const noexcept {
        const auto found = std::upper_bound(
            m_boundaries.begin(), m_boundaries.end(), byteOffset);
        return found == m_boundaries.end() ? m_text.size() : *found;
    }

    std::string prefix(size_t codepointCount) const {
        return std::string(m_text.substr(0, byteOffset(codepointCount)));
    }

private:
    std::string_view m_text;
    std::vector<size_t> m_boundaries;
};

} // namespace

TextField::TextField(const std::string& text)
    : m_text(sanitizeSingleLine(text)),
      m_caretIndex(Utf8CodepointMap(m_text).count()) {
    setFocusable(true);
    setMinWidth(80.0f);
    setDefaultHeight(getTheme().metrics.regularControlHeight);
}

void TextField::setText(const std::string& text) {
    std::string sanitized = sanitizeSingleLine(text);
    if (sanitized == m_text) {
        const size_t previousCaretIndex = m_caretIndex;
        m_caretIndex = std::min(m_caretIndex, Utf8CodepointMap(m_text).count());
        ensureCaretVisible();
        if (m_caretIndex != previousCaretIndex) resetCaretPresentation();
        return;
    }

    m_text = std::move(sanitized);
    invalidateCaretAdvances();
    m_caretIndex = std::min(m_caretIndex, Utf8CodepointMap(m_text).count());
    ensureCaretVisible();
    valueChanged();
}

void TextField::setPlaceholder(const std::string& placeholder) {
    std::string sanitized = sanitizeSingleLine(placeholder);
    if (sanitized == m_placeholder) return;
    m_placeholder = std::move(sanitized);
    invalidatePaint();
}

void TextField::syncLayout(float parentAbsX, float parentAbsY) {
    Widget::syncLayout(parentAbsX, parentAbsY);
    ensureCaretVisible();
}

void TextField::draw(graphics::Canvas& canvas, const graphics::RectF& damageRect) {
    if (!m_visible || !getPresentationBounds().intersects(damageRect)) return;

    rebuildCaretAdvances(canvas);
    ensureCaretVisible();
    beginPresentation(canvas, damageRect);

    const graphics::RectF field = m_absoluteBounds;
    const auto visual = visualStyle();
    const float padding = horizontalPadding();
    canvas.drawRoundedRect(field, visual.cornerRadius, visual.background,
                           visual.border, visual.borderWidth, 3.2f);

    const graphics::RectF viewport{
        field.x + padding,
        field.y + 1.0f,
        std::max(0.0f, field.width - padding * 2.0f),
        std::max(0.0f, field.height - 2.0f),
    };
    canvas.clipRect(viewport);

    const float textY = field.y + std::max(0.0f, (field.height - kFontSize * 1.2f) * 0.5f);
    if (m_text.empty()) {
        if (!m_placeholder.empty()) {
            canvas.drawText(viewport.x, textY, m_placeholder,
                            visual.secondaryForeground, kFontSize, kFontFamily);
        }
    } else {
        canvas.drawText(viewport.x - m_horizontalScroll, textY, m_text,
                        visual.foreground, kFontSize, kFontFamily);
    }

    const float caretOpacity = m_caretPresentation.opacity();
    if (m_focused && caretOpacity > 0.0f) {
        const float caretX = viewport.x + measurePrefix(m_caretIndex) - m_horizontalScroll;
        const float caretHeight = kFontSize * 1.2f;
        graphics::Color caret = visual.accent;
        caret.a = static_cast<uint8_t>(std::lround(
            static_cast<float>(caret.a) * caretOpacity));
        canvas.drawRect({caretX, textY, kCaretWidth, caretHeight}, caret);
    }

    endPresentation(canvas);
}

bool TextField::onPointerDown(const PointerEvent& event) {
    if (event.source == PointerSource::Touch) {
        return true;
    }
    if (event.button != 0) return false;
    const float contentX = event.x - (m_absoluteBounds.x + horizontalPadding()) +
        m_horizontalScroll;
    m_caretIndex = characterIndexForX(std::max(0.0f, contentX));
    ensureCaretVisible();
    resetCaretPresentation();
    invalidatePaint();
    return true;
}

bool TextField::onPointerUp(const PointerEvent& event) {
    if (event.source != PointerSource::Touch || !event.isTouchTapCompletion()) return false;
    const float contentX = event.x - (m_absoluteBounds.x + horizontalPadding()) +
        m_horizontalScroll;
    m_caretIndex = characterIndexForX(std::max(0.0f, contentX));
    ensureCaretVisible();
    resetCaretPresentation();
    invalidatePaint();
    return true;
}

bool TextField::onPointerCancel(const PointerEvent& event) {
    // Tap ownership belongs to EventDispatcher. Cancellation therefore has no
    // TextField-local focus state to clear; retaining this handler keeps the
    // control's pointer contract explicit.
    return event.source == PointerSource::Touch;
}

bool TextField::shouldFocusOnPointerDown(const PointerEvent& event) const {
    return event.source == PointerSource::Mouse;
}

bool TextField::onKeyDown(const KeyEvent& event) {
    using lcl::platform::PhysicalKey;
    const Utf8CodepointMap codepoints(m_text);
    const size_t count = codepoints.count();
    bool caretChanged = false;

    switch (event.key) {
        case PhysicalKey::Backspace:
            if (m_caretIndex > 0) {
                const size_t end = codepoints.byteOffset(m_caretIndex);
                const size_t begin = codepoints.previousBoundary(end);
                m_text.erase(begin, end - begin);
                --m_caretIndex;
                invalidateCaretAdvances();
                ensureCaretVisible();
                valueChanged();
            } else {
                resetCaretPresentation();
            }
            return true;
        case PhysicalKey::Delete:
            if (m_caretIndex < count) {
                const size_t begin = codepoints.byteOffset(m_caretIndex);
                const size_t end = codepoints.nextBoundary(begin);
                m_text.erase(begin, end - begin);
                invalidateCaretAdvances();
                ensureCaretVisible();
                valueChanged();
            } else {
                resetCaretPresentation();
            }
            return true;
        case PhysicalKey::ArrowLeft:
            if (m_caretIndex > 0) {
                --m_caretIndex;
                caretChanged = true;
            }
            break;
        case PhysicalKey::ArrowRight:
            if (m_caretIndex < count) {
                ++m_caretIndex;
                caretChanged = true;
            }
            break;
        case PhysicalKey::Home:
            caretChanged = m_caretIndex != 0;
            m_caretIndex = 0;
            break;
        case PhysicalKey::End:
            caretChanged = m_caretIndex != count;
            m_caretIndex = count;
            break;
        default:
            return false;
    }

    if (caretChanged) {
        ensureCaretVisible();
        invalidatePaint();
    }
    resetCaretPresentation();
    return true;
}

bool TextField::onTextInput(const TextInputEvent& event) {
    const std::string inserted = sanitizeSingleLine(event.text);
    if (inserted.empty()) return true;

    const size_t byteOffset = Utf8CodepointMap(m_text).byteOffset(m_caretIndex);
    m_text.insert(byteOffset, inserted);
    m_caretIndex += Utf8CodepointMap(inserted).count();
    invalidateCaretAdvances();
    ensureCaretVisible();
    valueChanged();
    return true;
}

bool TextField::onFocusGained(const FocusEvent& event) {
    (void)event;
    if (!m_focused) {
        m_focused = true;
        m_caretPresentation.setActive(true);
        registerCaretPresentation();
        ensureCaretVisible();
        invalidatePaint();
    }
    return false;
}

bool TextField::onFocusLost(const FocusEvent& event) {
    (void)event;
    if (m_focused) {
        m_focused = false;
        m_caretPresentation.setActive(false);
        unregisterCaretPresentation();
        invalidatePaint();
    }
    return false;
}

std::string TextField::sanitizeSingleLine(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    for (unsigned char value : text) {
        if (value == '\n' || value == '\r' || value < 0x20u || value == 0x7Fu) continue;
        result.push_back(static_cast<char>(value));
    }
    return result;
}

void TextField::rebuildCaretAdvances(graphics::Canvas& canvas) {
    const Utf8CodepointMap codepoints(m_text);
    m_caretAdvances.clear();
    m_caretAdvances.reserve(codepoints.count() + 1);
    for (size_t index = 0; index <= codepoints.count(); ++index) {
        m_caretAdvances.push_back(canvas.measureText(
            codepoints.prefix(index), kFontSize, kFontFamily));
    }
    m_caretAdvancesValid = true;
}

void TextField::invalidateCaretAdvances() {
    m_caretAdvancesValid = false;
    m_caretAdvances.clear();
}

float TextField::measurePrefix(size_t characterIndex) const {
    const Utf8CodepointMap codepoints(m_text);
    const size_t clampedIndex = std::min(characterIndex, codepoints.count());
    if (m_caretAdvancesValid && m_caretAdvances.size() == codepoints.count() + 1) {
        return m_caretAdvances[clampedIndex];
    }
    return lcl::render::text_metrics::measureText(
        codepoints.prefix(clampedIndex), kFontSize, kFontFamily);
}

float TextField::measureTextWidth() const {
    const Utf8CodepointMap codepoints(m_text);
    if (m_caretAdvancesValid && m_caretAdvances.size() == codepoints.count() + 1) {
        return m_caretAdvances.back();
    }
    return lcl::render::text_metrics::measureText(m_text, kFontSize, kFontFamily);
}

size_t TextField::characterIndexForX(float x) const {
    const size_t count = Utf8CodepointMap(m_text).count();
    float previous = 0.0f;
    for (size_t index = 1; index <= count; ++index) {
        const float next = measurePrefix(index);
        if (x < (previous + next) * 0.5f) return index - 1;
        previous = next;
    }
    return count;
}

void TextField::ensureCaretVisible() {
    const float viewportWidth = m_absoluteBounds.width - horizontalPadding() * 2.0f;
    if (viewportWidth <= 0.0f) {
        m_horizontalScroll = 0.0f;
        return;
    }

    const float caretX = measurePrefix(m_caretIndex);
    const float usableWidth = std::max(0.0f, viewportWidth - kCaretWidth);
    if (caretX < m_horizontalScroll) {
        m_horizontalScroll = caretX;
    } else if (caretX - m_horizontalScroll > usableWidth) {
        m_horizontalScroll = caretX - usableWidth;
    }

    const float textWidth = measureTextWidth();
    const float maxScroll = std::max(0.0f, textWidth - usableWidth);
    m_horizontalScroll = std::clamp(m_horizontalScroll, 0.0f, maxScroll);
}

lcl::theme::ResolvedStyle TextField::visualStyle() const noexcept {
    const auto* style = resolvedStyle();
    if (!style) return {};
    return lcl::theme::resolveStyle(
        *style, m_focused ? lcl::theme::StyleState::Focused
                          : lcl::theme::StyleState::Normal);
}

float TextField::horizontalPadding() const noexcept {
    const auto* style = resolvedStyle();
    return style ? std::max(0.0f, style->horizontalPadding.value_or(0.0f))
                 : 0.0f;
}

const lcl::theme::WidgetStyle* TextField::defaultStyle() const noexcept {
    return &getTheme().textField;
}

void TextField::styleDidChange() {
    if (!m_hasHeight) {
        setDefaultHeight(getTheme().metrics.regularControlHeight);
    }
    ensureCaretVisible();
    invalidatePaint();
}

void TextField::registerCaretPresentation() {
    if (!m_motionCoordinator || !m_caretPresentation.isActive()) return;
    m_motionCoordinator->registerPresentation(*this, [this](float deltaSec) {
        tickCaretPresentation(deltaSec);
    });
}

void TextField::unregisterCaretPresentation() {
    if (m_motionCoordinator) m_motionCoordinator->unregisterPresentation(getObjectId());
}

void TextField::tickCaretPresentation(float deltaSec) {
    if (m_caretPresentation.update(deltaSec)) invalidatePaint();
}

void TextField::resetCaretPresentation() {
    if (m_caretPresentation.resetActivity()) invalidatePaint();
}

void TextField::valueChanged() {
    resetCaretPresentation();
    invalidatePaint();
    if (!m_onChange) return;
    ChangeCallback callback = m_onChange;
    const std::string value = m_text;
    callback(value);
}

} // namespace lcl::ui
