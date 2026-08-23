#pragma once

#include "lcl-ui/core/caret_presentation_controller.hpp"
#include "lcl-graphics/font.hpp"
#include "lcl-ui/widgets/widget.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace lcl::ui {

/** Minimal single-line editable text control. */
class TextField : public Widget {
public:
    using ChangeCallback = std::function<void(const std::string&)>;

    explicit TextField(const std::string& text = "");
    ~TextField() override = default;

    void setText(const std::string& text);
    const std::string& getText() const noexcept { return m_text; }

    void setPlaceholder(const std::string& placeholder);
    const std::string& getPlaceholder() const noexcept { return m_placeholder; }

    void setOnChange(ChangeCallback callback) { m_onChange = std::move(callback); }

    void syncLayout(float parentAbsX = 0.0f, float parentAbsY = 0.0f) override;
    void draw(graphics::Canvas& canvas, const graphics::RectF& damageRect) override;

    bool onPointerDown(const PointerEvent& event) override;
    bool onPointerUp(const PointerEvent& event) override;
    bool onPointerCancel(const PointerEvent& event) override;
    bool shouldFocusOnPointerDown(const PointerEvent& event) const override;
    bool onKeyDown(const KeyEvent& event) override;
    bool onTextInput(const TextInputEvent& event) override;
    bool onFocusGained(const FocusEvent& event) override;
    bool onFocusLost(const FocusEvent& event) override;

private:
    static std::string sanitizeSingleLine(const std::string& text);

    void rebuildCaretAdvances(graphics::Canvas& canvas);
    void invalidateCaretAdvances();
    float measurePrefix(size_t characterIndex) const;
    float measureTextWidth() const;
    size_t characterIndexForX(float x) const;
    void ensureCaretVisible();
    void registerCaretPresentation();
    void unregisterCaretPresentation();
    void tickCaretPresentation(float deltaSec);
    void resetCaretPresentation();
    void valueChanged();
    lcl::theme::ResolvedStyle visualStyle() const noexcept;
    float horizontalPadding() const noexcept;
    const lcl::theme::WidgetStyle* defaultStyle() const noexcept override;

    std::string m_text;
    std::string m_placeholder;
    ChangeCallback m_onChange;
    size_t m_caretIndex{0};
    float m_horizontalScroll{0.0f};
    bool m_focused{false};
    CaretPresentationController m_caretPresentation;
    std::vector<float> m_caretAdvances;
    bool m_caretAdvancesValid{false};

    static constexpr float kFontSize = 14.0f;
    static constexpr float kCaretWidth = 1.0f;
    static constexpr graphics::FontFamily kFontFamily = graphics::FontFamily::Interface;
};

} // namespace lcl::ui
