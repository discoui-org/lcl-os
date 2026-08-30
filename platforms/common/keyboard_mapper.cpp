#include "platforms/common/keyboard_mapper.hpp"

namespace lcl::platform {

char32_t KeyboardMapper::toCodepoint(PhysicalKey key, uint8_t modifiers) {
    bool shift = (modifiers & kModShift) != 0;
    bool ctrl  = (modifiers & kModCtrl) != 0;
    bool caps  = (modifiers & kModCapsLock) != 0;
    bool isUpper = shift ^ caps;

    switch (key) {
        case PhysicalKey::Enter:     return '\n';
        case PhysicalKey::KpEnter:   return '\n';
        case PhysicalKey::Backspace: return '\x7F';
        case PhysicalKey::Tab:       return '\t';
        case PhysicalKey::Space:     return ' ';

        // Letter keys (A..Z)
        case PhysicalKey::A: return ctrl ? '\x01' : (isUpper ? 'A' : 'a');
        case PhysicalKey::B: return ctrl ? '\x02' : (isUpper ? 'B' : 'b');
        case PhysicalKey::C: return ctrl ? '\x03' : (isUpper ? 'C' : 'c');
        case PhysicalKey::D: return ctrl ? '\x04' : (isUpper ? 'D' : 'd');
        case PhysicalKey::E: return ctrl ? '\x05' : (isUpper ? 'E' : 'e');
        case PhysicalKey::F: return ctrl ? '\x06' : (isUpper ? 'F' : 'f');
        case PhysicalKey::G: return ctrl ? '\x07' : (isUpper ? 'G' : 'g');
        case PhysicalKey::H: return ctrl ? '\x08' : (isUpper ? 'H' : 'h');
        case PhysicalKey::I: return ctrl ? '\x09' : (isUpper ? 'I' : 'i');
        case PhysicalKey::J: return ctrl ? '\x0A' : (isUpper ? 'J' : 'j');
        case PhysicalKey::K: return ctrl ? '\x0B' : (isUpper ? 'K' : 'k');
        case PhysicalKey::L: return ctrl ? '\x0C' : (isUpper ? 'L' : 'l');
        case PhysicalKey::M: return ctrl ? '\x0D' : (isUpper ? 'M' : 'm');
        case PhysicalKey::N: return ctrl ? '\x0E' : (isUpper ? 'N' : 'n');
        case PhysicalKey::O: return ctrl ? '\x0F' : (isUpper ? 'O' : 'o');
        case PhysicalKey::P: return ctrl ? '\x10' : (isUpper ? 'P' : 'p');
        case PhysicalKey::Q: return ctrl ? '\x11' : (isUpper ? 'Q' : 'q');
        case PhysicalKey::R: return ctrl ? '\x12' : (isUpper ? 'R' : 'r');
        case PhysicalKey::S: return ctrl ? '\x13' : (isUpper ? 'S' : 's');
        case PhysicalKey::T: return ctrl ? '\x14' : (isUpper ? 'T' : 't');
        case PhysicalKey::U: return ctrl ? '\x15' : (isUpper ? 'U' : 'u');
        case PhysicalKey::V: return ctrl ? '\x16' : (isUpper ? 'V' : 'v');
        case PhysicalKey::W: return ctrl ? '\x17' : (isUpper ? 'W' : 'w');
        case PhysicalKey::X: return ctrl ? '\x18' : (isUpper ? 'X' : 'x');
        case PhysicalKey::Y: return ctrl ? '\x19' : (isUpper ? 'Y' : 'y');
        case PhysicalKey::Z: return ctrl ? '\x1A' : (isUpper ? 'Z' : 'z');

        // Digits (Digit1..Digit0)
        case PhysicalKey::Digit1: return shift ? '!' : '1';
        case PhysicalKey::Digit2: return shift ? '@' : '2';
        case PhysicalKey::Digit3: return shift ? '#' : '3';
        case PhysicalKey::Digit4: return shift ? '$' : '4';
        case PhysicalKey::Digit5: return shift ? '%' : '5';
        case PhysicalKey::Digit6: return shift ? '^' : '6';
        case PhysicalKey::Digit7: return shift ? '&' : '7';
        case PhysicalKey::Digit8: return shift ? '*' : '8';
        case PhysicalKey::Digit9: return shift ? '(' : '9';
        case PhysicalKey::Digit0: return shift ? ')' : '0';

        // Punctuation & Symbols
        case PhysicalKey::Minus:        return shift ? '_' : '-';
        case PhysicalKey::Equal:        return shift ? '+' : '=';
        case PhysicalKey::LeftBracket:  return shift ? '{' : '[';
        case PhysicalKey::RightBracket: return shift ? '}' : ']';
        case PhysicalKey::Backslash:    return shift ? '|' : '\\';
        case PhysicalKey::Semicolon:    return shift ? ':' : ';';
        case PhysicalKey::Apostrophe:   return shift ? '"' : '\'';
        case PhysicalKey::Grave:        return shift ? '~' : '`';
        case PhysicalKey::Comma:        return shift ? '<' : ',';
        case PhysicalKey::Period:       return shift ? '>' : '.';
        case PhysicalKey::Slash:        return shift ? '?' : '/';

        default:
            return 0;
    }
}

std::string KeyboardMapper::toUTF8(PhysicalKey key, uint8_t modifiers) {
    char32_t cp = toCodepoint(key, modifiers);
    if (cp == 0) return "";

    std::string out;
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

bool KeyboardMapper::isModifier(PhysicalKey key) {
    switch (key) {
        case PhysicalKey::LeftShift:
        case PhysicalKey::RightShift:
        case PhysicalKey::LeftCtrl:
        case PhysicalKey::RightCtrl:
        case PhysicalKey::LeftAlt:
        case PhysicalKey::RightAlt:
        case PhysicalKey::LeftMeta:
        case PhysicalKey::RightMeta:
        case PhysicalKey::CapsLock:
            return true;
        default:
            return false;
    }
}

} // namespace lcl::platform
