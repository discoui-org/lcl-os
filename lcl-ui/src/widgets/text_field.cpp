#include "lcl-ui/widgets/text_field.hpp"

#include "lcl-ui/core/canvas.hpp"
#include "render/text_metrics.hpp"

#include <algorithm>
#include <utility>

namespace lcl::ui {
namespace {

bool isUtf8Continuation(unsigned char value) {
    return (value & 0xC0u) == 0x80u;
}

} // namespace

TextField::TextField(const std::string& text)
    : m_text(sanitizeSingleLine(text)),
      m_caretIndex(characterCount(m_text)) {
    setFocusable(true);
    m_yogaNode.setMinWidth(80.0f);
    m_yogaNode.setHeight(36.0f);
}

void TextField::setText(const std::string& text) {
    std::string sanitized = sanitizeSingleLine(text);
    if (sanitized == m_text) {
        m_caretIndex = std::min(m_caretIndex, characterCount(m_text));
        ensureCaretVisible();
        return;
    }

    m_text = std::move(sanitized);
    m_caretIndex = std::min(m_caretIndex, characterCount(m_text));
    ensureCaretVisible();
    valueChanged();
}

void TextField::setPlaceholder(const std::string& placeholder) {
    std::string sanitized = sanitizeSingleLine(placeholder);
    if (sanitized == m_placeholder) return;
    m_placeholder = std::move(sanitized);
    markDirty();
}

void TextField::syncLayout(float parentAbsX, float parentAbsY) {
    Widget::syncLayout(parentAbsX, parentAbsY);
    ensureCaretVisible();
}

void TextField::draw(Canvas& canvas, const Rect& damageRect) {
    if (!m_visible || !getPresentationBounds().intersects(damageRect)) return;

    ensureCaretVisible();
    beginPresentation(canvas);

    const Rect field = m_absoluteBounds;
    const Color border = m_focused
        ? Color{96, 165, 250, 255}
        : Color{100, 116, 139, 220};
    canvas.drawRoundedRect(field, 6.0f, {24, 31, 42, 255}, border,
                           1.0f, 3.2f);

    const Rect viewport{
        field.x + kHorizontalPadding,
        field.y + 1.0f,
        std::max(0.0f, field.width - kHorizontalPadding * 2.0f),
        std::max(0.0f, field.height - 2.0f),
    };
    canvas.clipRect(viewport);

    const float textY = field.y + std::max(0.0f, (field.height - kFontSize * 1.2f) * 0.5f);
    if (m_text.empty()) {
        if (!m_placeholder.empty()) {
            canvas.drawText(viewport.x, textY, m_placeholder,
                            {148, 163, 184, 255}, kFontSize, kFontFamily);
        }
    } else {
        canvas.drawText(viewport.x - m_horizontalScroll, textY, m_text,
                        {241, 245, 249, 255}, kFontSize, kFontFamily);
    }

    if (m_focused) {
        const float caretX = viewport.x + measurePrefix(m_caretIndex) - m_horizontalScroll;
        const float caretHeight = kFontSize * 1.2f;
        canvas.drawRect({caretX, textY, kCaretWidth, caretHeight},
                        {226, 232, 240, 255});
    }

    endPresentation(canvas);
}

bool TextField::onPointerDown(const PointerEvent& event) {
    if (event.source == PointerSource::Touch) {
        m_touchTapPending = true;
        m_touchTapPointerId = event.pointerId;
        return true;
    }
    if (event.button != 0) return false;
    m_touchTapPending = false;
    const float contentX = event.x - (m_absoluteBounds.x + kHorizontalPadding) +
        m_horizontalScroll;
    m_caretIndex = characterIndexForX(std::max(0.0f, contentX));
    ensureCaretVisible();
    markDirty();
    return true;
}

bool TextField::onPointerUp(const PointerEvent& event) {
    if (event.source != PointerSource::Touch || !m_touchTapPending ||
        event.pointerId != m_touchTapPointerId) return false;

    m_touchTapPending = false;
    const float contentX = event.x - (m_absoluteBounds.x + kHorizontalPadding) +
        m_horizontalScroll;
    m_caretIndex = characterIndexForX(std::max(0.0f, contentX));
    ensureCaretVisible();
    markDirty();
    return event.requestFocus(*this);
}

bool TextField::onPointerCancel(const PointerEvent& event) {
    if (event.source != PointerSource::Touch || !m_touchTapPending ||
        event.pointerId != m_touchTapPointerId) return false;
    m_touchTapPending = false;
    return true;
}

bool TextField::shouldFocusOnPointerDown(const PointerEvent& event) const {
    return event.source == PointerSource::Mouse;
}

bool TextField::onKeyDown(const KeyEvent& event) {
    using lcl::platform::PhysicalKey;
    const size_t count = characterCount(m_text);
    bool caretChanged = false;

    switch (event.key) {
        case PhysicalKey::Backspace:
            if (m_caretIndex > 0) {
                const size_t begin = byteOffsetForCharacter(m_text, m_caretIndex - 1);
                const size_t end = byteOffsetForCharacter(m_text, m_caretIndex);
                m_text.erase(begin, end - begin);
                --m_caretIndex;
                ensureCaretVisible();
                valueChanged();
            }
            return true;
        case PhysicalKey::Delete:
            if (m_caretIndex < count) {
                const size_t begin = byteOffsetForCharacter(m_text, m_caretIndex);
                const size_t end = byteOffsetForCharacter(m_text, m_caretIndex + 1);
                m_text.erase(begin, end - begin);
                ensureCaretVisible();
                valueChanged();
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
        markDirty();
    }
    return true;
}

bool TextField::onTextInput(const TextInputEvent& event) {
    const std::string inserted = sanitizeSingleLine(event.text);
    if (inserted.empty()) return true;

    const size_t byteOffset = byteOffsetForCharacter(m_text, m_caretIndex);
    m_text.insert(byteOffset, inserted);
    m_caretIndex += characterCount(inserted);
    ensureCaretVisible();
    valueChanged();
    return true;
}

bool TextField::onFocusGained(const FocusEvent& event) {
    (void)event;
    if (!m_focused) {
        m_focused = true;
        ensureCaretVisible();
        markDirty();
    }
    return false;
}

bool TextField::onFocusLost(const FocusEvent& event) {
    (void)event;
    if (m_focused) {
        m_focused = false;
        markDirty();
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

size_t TextField::characterCount(const std::string& text) {
    size_t count = 0;
    for (unsigned char value : text) {
        if (!isUtf8Continuation(value)) ++count;
    }
    return count;
}

size_t TextField::byteOffsetForCharacter(const std::string& text, size_t index) {
    size_t character = 0;
    for (size_t offset = 0; offset < text.size(); ++offset) {
        if (!isUtf8Continuation(static_cast<unsigned char>(text[offset]))) {
            if (character == index) return offset;
            ++character;
        }
    }
    return text.size();
}

float TextField::measurePrefix(size_t characterIndex) const {
    const size_t byteOffset = byteOffsetForCharacter(m_text, characterIndex);
    return lcl::render::text_metrics::measureText(
        m_text.substr(0, byteOffset), kFontSize, kFontFamily);
}

size_t TextField::characterIndexForX(float x) const {
    const size_t count = characterCount(m_text);
    float previous = 0.0f;
    for (size_t index = 1; index <= count; ++index) {
        const float next = measurePrefix(index);
        if (x < (previous + next) * 0.5f) return index - 1;
        previous = next;
    }
    return count;
}

void TextField::ensureCaretVisible() {
    const float viewportWidth = m_absoluteBounds.width - kHorizontalPadding * 2.0f;
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

    const float textWidth = lcl::render::text_metrics::measureText(
        m_text, kFontSize, kFontFamily);
    const float maxScroll = std::max(0.0f, textWidth - usableWidth);
    m_horizontalScroll = std::clamp(m_horizontalScroll, 0.0f, maxScroll);
}

void TextField::valueChanged() {
    markDirty();
    if (!m_onChange) return;
    ChangeCallback callback = m_onChange;
    const std::string value = m_text;
    callback(value);
}

} // namespace lcl::ui
