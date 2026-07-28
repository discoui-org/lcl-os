#pragma once

#include <cstdint>
#include <string>

namespace lcl::core {

// Modifier bitmask flags
constexpr uint8_t LCL_MOD_SHIFT    = 1 << 0; // 0x01
constexpr uint8_t LCL_MOD_CTRL     = 1 << 1; // 0x02
constexpr uint8_t LCL_MOD_ALT      = 1 << 2; // 0x04
constexpr uint8_t LCL_MOD_CAPSLOCK = 1 << 3; // 0x08
constexpr uint8_t LCL_MOD_SUPER    = 1 << 4; // 0x10

class KeyMapper {
public:
    /**
     * @brief Translate Linux evdev keycode + modifiers to Unicode codepoint.
     * @param keycode Linux evdev keycode (e.g. KEY_A, KEY_1, KEY_ENTER)
     * @param modifiers Bitmask of active modifier flags (LCL_MOD_*)
     * @return Translated char32_t codepoint (0 if non-printable / control key without text output)
     */
    static char32_t toCodepoint(uint32_t keycode, uint8_t modifiers);

    /**
     * @brief Translate Linux evdev keycode + modifiers to UTF-8 encoded string.
     */
    static std::string toUTF8(uint32_t keycode, uint8_t modifiers);
};

} // namespace lcl::core
