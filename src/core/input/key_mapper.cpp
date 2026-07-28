#include "core/input/key_mapper.hpp"
#include <linux/input-event-codes.h>

namespace lcl::core {

char32_t KeyMapper::toCodepoint(uint32_t keycode, uint8_t modifiers) {
    bool shift = (modifiers & LCL_MOD_SHIFT) != 0;
    bool ctrl  = (modifiers & LCL_MOD_CTRL) != 0;
    bool caps  = (modifiers & LCL_MOD_CAPSLOCK) != 0;
    bool isUpper = shift ^ caps;

    switch (keycode) {
        case KEY_ENTER:     return '\n';
        case KEY_KPENTER:   return '\n';
        case KEY_BACKSPACE: return '\x7F';
        case KEY_TAB:       return '\t';
        case KEY_SPACE:     return ' ';

        // Letter keys (KEY_A..KEY_Z) — evdev keycodes follow physical keyboard row order
        case KEY_A: return ctrl ? '\x01' : (isUpper ? 'A' : 'a');
        case KEY_B: return ctrl ? '\x02' : (isUpper ? 'B' : 'b');
        case KEY_C: return ctrl ? '\x03' : (isUpper ? 'C' : 'c');
        case KEY_D: return ctrl ? '\x04' : (isUpper ? 'D' : 'd');
        case KEY_E: return ctrl ? '\x05' : (isUpper ? 'E' : 'e');
        case KEY_F: return ctrl ? '\x06' : (isUpper ? 'F' : 'f');
        case KEY_G: return ctrl ? '\x07' : (isUpper ? 'G' : 'g');
        case KEY_H: return ctrl ? '\x08' : (isUpper ? 'H' : 'h');
        case KEY_I: return ctrl ? '\x09' : (isUpper ? 'I' : 'i');
        case KEY_J: return ctrl ? '\x0A' : (isUpper ? 'J' : 'j');
        case KEY_K: return ctrl ? '\x0B' : (isUpper ? 'K' : 'k');
        case KEY_L: return ctrl ? '\x0C' : (isUpper ? 'L' : 'l');
        case KEY_M: return ctrl ? '\x0D' : (isUpper ? 'M' : 'm');
        case KEY_N: return ctrl ? '\x0E' : (isUpper ? 'N' : 'n');
        case KEY_O: return ctrl ? '\x0F' : (isUpper ? 'O' : 'o');
        case KEY_P: return ctrl ? '\x10' : (isUpper ? 'P' : 'p');
        case KEY_Q: return ctrl ? '\x11' : (isUpper ? 'Q' : 'q');
        case KEY_R: return ctrl ? '\x12' : (isUpper ? 'R' : 'r');
        case KEY_S: return ctrl ? '\x13' : (isUpper ? 'S' : 's');
        case KEY_T: return ctrl ? '\x14' : (isUpper ? 'T' : 't');
        case KEY_U: return ctrl ? '\x15' : (isUpper ? 'U' : 'u');
        case KEY_V: return ctrl ? '\x16' : (isUpper ? 'V' : 'v');
        case KEY_W: return ctrl ? '\x17' : (isUpper ? 'W' : 'w');
        case KEY_X: return ctrl ? '\x18' : (isUpper ? 'X' : 'x');
        case KEY_Y: return ctrl ? '\x19' : (isUpper ? 'Y' : 'y');
        case KEY_Z: return ctrl ? '\x1A' : (isUpper ? 'Z' : 'z');

        // Digits (KEY_1..KEY_0)
        case KEY_1: return shift ? '!' : '1';
        case KEY_2: return shift ? '@' : '2';
        case KEY_3: return shift ? '#' : '3';
        case KEY_4: return shift ? '$' : '4';
        case KEY_5: return shift ? '%' : '5';
        case KEY_6: return shift ? '^' : '6';
        case KEY_7: return shift ? '&' : '7';
        case KEY_8: return shift ? '*' : '8';
        case KEY_9: return shift ? '(' : '9';
        case KEY_0: return shift ? ')' : '0';

        // Punctuation & Symbols
        case KEY_MINUS:      return shift ? '_' : '-';
        case KEY_EQUAL:      return shift ? '+' : '=';
        case KEY_LEFTBRACE:  return shift ? '{' : '[';
        case KEY_RIGHTBRACE: return shift ? '}' : ']';
        case KEY_BACKSLASH:  return shift ? '|' : '\\';
        case KEY_SEMICOLON:  return shift ? ':' : ';';
        case KEY_APOSTROPHE: return shift ? '"' : '\'';
        case KEY_GRAVE:      return shift ? '~' : '`';
        case KEY_COMMA:      return shift ? '<' : ',';
        case KEY_DOT:        return shift ? '>' : '.';
        case KEY_SLASH:      return shift ? '?' : '/';

        default:
            return 0;
    }
}

std::string KeyMapper::toUTF8(uint32_t keycode, uint8_t modifiers) {
    char32_t cp = toCodepoint(keycode, modifiers);
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

} // namespace lcl::core
