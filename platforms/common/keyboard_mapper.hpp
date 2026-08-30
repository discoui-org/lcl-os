#pragma once

#include <cstdint>
#include <string>
#include "platforms/common/keyboard_types.hpp"

namespace lcl::platform {

/**
 * @brief Platform-neutral keyboard layout and text mapping engine.
 *
 * Translates (PhysicalKey, modifiers) to Unicode codepoints and UTF-8 strings.
 * Free of any Linux evdev or Android headers.
 */
class KeyboardMapper {
public:
    /**
     * @brief Translate PhysicalKey + modifiers to Unicode codepoint.
     * @param key Platform-agnostic PhysicalKey
     * @param modifiers Bitmask of active modifier flags (kMod*)
     * @return Translated char32_t codepoint (0 if non-printable / control key without text output)
     */
    static char32_t toCodepoint(PhysicalKey key, uint8_t modifiers);

    /**
     * @brief Translate PhysicalKey + modifiers to UTF-8 encoded string.
     */
    static std::string toUTF8(PhysicalKey key, uint8_t modifiers);

    /**
     * @brief Check if key is a modifier key.
     */
    static bool isModifier(PhysicalKey key);
};

} // namespace lcl::platform

namespace lcl::core {
// Transitional alias for core/apps
using KeyboardMapper = lcl::platform::KeyboardMapper;
using KeyMapper = lcl::platform::KeyboardMapper;
using PhysicalKey = lcl::platform::PhysicalKey;
constexpr uint8_t LCL_MOD_SHIFT = lcl::platform::kModShift;
constexpr uint8_t LCL_MOD_CTRL = lcl::platform::kModCtrl;
constexpr uint8_t LCL_MOD_ALT = lcl::platform::kModAlt;
constexpr uint8_t LCL_MOD_CAPSLOCK = lcl::platform::kModCapsLock;
constexpr uint8_t LCL_MOD_SUPER = lcl::platform::kModSuper;
} // namespace lcl::core
