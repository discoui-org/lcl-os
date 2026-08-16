#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include "platform/common/keyboard_types.hpp"

namespace lcl::ui {

enum class PointerSource {
    Mouse,
    Touch
};

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
    PointerSource source{PointerSource::Mouse};
};

enum class KeyEventType {
    KeyDown,
    KeyUp
};

struct KeyEvent {
    lcl::platform::PhysicalKey key{lcl::platform::PhysicalKey::Unknown};
    int keyCode{0};
    char32_t codepoint{0};
    uint8_t modifiers{0}; // Bitfield for Shift, Ctrl, Alt
    KeyEventType type{KeyEventType::KeyDown};

    KeyEvent() = default;
    KeyEvent(int code, char32_t cp = 0, uint8_t mods = 0, KeyEventType t = KeyEventType::KeyDown)
        : key(static_cast<lcl::platform::PhysicalKey>(code)), keyCode(code), codepoint(cp), modifiers(mods), type(t) {}
    KeyEvent(lcl::platform::PhysicalKey k, int code, char32_t cp = 0, uint8_t mods = 0, KeyEventType t = KeyEventType::KeyDown)
        : key(k), keyCode(code), codepoint(cp), modifiers(mods), type(t) {}
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
