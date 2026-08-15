#include "platform/desktop/evdev_key_mapper.hpp"
#include <linux/input-event-codes.h>

namespace lcl::platform::desktop {

PhysicalKey EvdevKeyMapper::toPhysicalKey(uint32_t linuxKeycode) {
    switch (linuxKeycode) {
        // Letters
        case KEY_A: return PhysicalKey::A;
        case KEY_B: return PhysicalKey::B;
        case KEY_C: return PhysicalKey::C;
        case KEY_D: return PhysicalKey::D;
        case KEY_E: return PhysicalKey::E;
        case KEY_F: return PhysicalKey::F;
        case KEY_G: return PhysicalKey::G;
        case KEY_H: return PhysicalKey::H;
        case KEY_I: return PhysicalKey::I;
        case KEY_J: return PhysicalKey::J;
        case KEY_K: return PhysicalKey::K;
        case KEY_L: return PhysicalKey::L;
        case KEY_M: return PhysicalKey::M;
        case KEY_N: return PhysicalKey::N;
        case KEY_O: return PhysicalKey::O;
        case KEY_P: return PhysicalKey::P;
        case KEY_Q: return PhysicalKey::Q;
        case KEY_R: return PhysicalKey::R;
        case KEY_S: return PhysicalKey::S;
        case KEY_T: return PhysicalKey::T;
        case KEY_U: return PhysicalKey::U;
        case KEY_V: return PhysicalKey::V;
        case KEY_W: return PhysicalKey::W;
        case KEY_X: return PhysicalKey::X;
        case KEY_Y: return PhysicalKey::Y;
        case KEY_Z: return PhysicalKey::Z;

        // Digits
        case KEY_1: return PhysicalKey::Digit1;
        case KEY_2: return PhysicalKey::Digit2;
        case KEY_3: return PhysicalKey::Digit3;
        case KEY_4: return PhysicalKey::Digit4;
        case KEY_5: return PhysicalKey::Digit5;
        case KEY_6: return PhysicalKey::Digit6;
        case KEY_7: return PhysicalKey::Digit7;
        case KEY_8: return PhysicalKey::Digit8;
        case KEY_9: return PhysicalKey::Digit9;
        case KEY_0: return PhysicalKey::Digit0;

        // Function keys
        case KEY_F1:  return PhysicalKey::F1;
        case KEY_F2:  return PhysicalKey::F2;
        case KEY_F3:  return PhysicalKey::F3;
        case KEY_F4:  return PhysicalKey::F4;
        case KEY_F5:  return PhysicalKey::F5;
        case KEY_F6:  return PhysicalKey::F6;
        case KEY_F7:  return PhysicalKey::F7;
        case KEY_F8:  return PhysicalKey::F8;
        case KEY_F9:  return PhysicalKey::F9;
        case KEY_F10: return PhysicalKey::F10;
        case KEY_F11: return PhysicalKey::F11;
        case KEY_F12: return PhysicalKey::F12;

        // Common Controls
        case KEY_ENTER:     return PhysicalKey::Enter;
        case KEY_KPENTER:   return PhysicalKey::KpEnter;
        case KEY_ESC:       return PhysicalKey::Escape;
        case KEY_BACKSPACE: return PhysicalKey::Backspace;
        case KEY_TAB:       return PhysicalKey::Tab;
        case KEY_SPACE:     return PhysicalKey::Space;

        // Modifiers
        case KEY_LEFTSHIFT:  return PhysicalKey::LeftShift;
        case KEY_RIGHTSHIFT: return PhysicalKey::RightShift;
        case KEY_LEFTCTRL:   return PhysicalKey::LeftCtrl;
        case KEY_RIGHTCTRL:  return PhysicalKey::RightCtrl;
        case KEY_LEFTALT:    return PhysicalKey::LeftAlt;
        case KEY_RIGHTALT:   return PhysicalKey::RightAlt;
        case KEY_LEFTMETA:   return PhysicalKey::LeftMeta;
        case KEY_RIGHTMETA:  return PhysicalKey::RightMeta;
        case KEY_CAPSLOCK:   return PhysicalKey::CapsLock;

        // Navigation
        case KEY_UP:       return PhysicalKey::ArrowUp;
        case KEY_DOWN:     return PhysicalKey::ArrowDown;
        case KEY_LEFT:     return PhysicalKey::ArrowLeft;
        case KEY_RIGHT:    return PhysicalKey::ArrowRight;
        case KEY_HOME:     return PhysicalKey::Home;
        case KEY_END:      return PhysicalKey::End;
        case KEY_PAGEUP:   return PhysicalKey::PageUp;
        case KEY_PAGEDOWN: return PhysicalKey::PageDown;
        case KEY_INSERT:   return PhysicalKey::Insert;
        case KEY_DELETE:   return PhysicalKey::Delete;

        // Symbols & Punctuation
        case KEY_MINUS:      return PhysicalKey::Minus;
        case KEY_EQUAL:      return PhysicalKey::Equal;
        case KEY_LEFTBRACE:  return PhysicalKey::LeftBracket;
        case KEY_RIGHTBRACE: return PhysicalKey::RightBracket;
        case KEY_BACKSLASH:  return PhysicalKey::Backslash;
        case KEY_SEMICOLON:  return PhysicalKey::Semicolon;
        case KEY_APOSTROPHE: return PhysicalKey::Apostrophe;
        case KEY_GRAVE:      return PhysicalKey::Grave;
        case KEY_COMMA:      return PhysicalKey::Comma;
        case KEY_DOT:        return PhysicalKey::Period;
        case KEY_SLASH:      return PhysicalKey::Slash;

        default:
            return PhysicalKey::Unknown;
    }
}

PointerButton EvdevKeyMapper::toPointerButton(uint32_t linuxButton) {
    switch (linuxButton) {
        case BTN_LEFT:
        case BTN_TOUCH:
        case BTN_TOOL_FINGER:
            return PointerButton::Left;
        case BTN_RIGHT:
            return PointerButton::Right;
        case BTN_MIDDLE:
            return PointerButton::Middle;
        default:
            return PointerButton::None;
    }
}

} // namespace lcl::platform::desktop
