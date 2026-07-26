#pragma once

#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/text.hpp"
#include <functional>
#include <string>

namespace lcl::ui {

enum class ButtonState {
    Normal,
    Hover,
    Active
};

class Button : public Container {
public:
    explicit Button(const std::string& label = "");
    ~Button() override = default;

    void setLabel(const std::string& label);
    std::string getLabel() const;

    Text* getTextWidget() const { return m_textWidget; }

    void setOnClick(std::function<void()> callback) { m_onClick = callback; }

    ButtonState getState() const { return m_state; }

    bool onPointerMove(float px, float py) override;
    bool onPointerDown(float px, float py) override;
    bool onPointerUp(float px, float py) override;

    void draw(SkCanvas* canvas, const Rect& damageRect) override;

private:
    void setState(ButtonState newState);

    ButtonState m_state{ButtonState::Normal};
    Text* m_textWidget{nullptr};
    std::function<void()> m_onClick{nullptr};
};

} // namespace lcl::ui
