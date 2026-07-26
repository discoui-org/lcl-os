#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace lcl::ui {

enum class PointerEventType {
    Move,
    Down,
    Up,
    Enter,
    Leave,
    Scroll
};

struct PointerEvent {
    float x{0.0f};
    float y{0.0f};
    int button{0}; // 0: Left, 1: Right, 2: Middle
    float deltaX{0.0f};
    float deltaY{0.0f};
    PointerEventType type{PointerEventType::Move};
};

enum class KeyEventType {
    KeyDown,
    KeyUp
};

struct KeyEvent {
    int keyCode{0};
    char32_t codepoint{0};
    uint8_t modifiers{0}; // Bitfield for Shift, Ctrl, Alt
    KeyEventType type{KeyEventType::KeyDown};
};

struct TextInputEvent {
    std::string text;
};

enum class FocusEventType {
    Gained,
    Lost
};

struct FocusEvent {
    FocusEventType type{FocusEventType::Gained};
};

using Event = std::variant<PointerEvent, KeyEvent, TextInputEvent, FocusEvent>;

} // namespace lcl::ui
