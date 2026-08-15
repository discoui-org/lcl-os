#pragma once

#include <cstdint>
#include "platform/common/keyboard_types.hpp"

namespace lcl::platform::desktop {

/**
 * @brief Translates Linux evdev raw keycodes to platform-neutral PhysicalKey.
 *
 * Exists exclusively in src/platform/desktop/. Holds all dependencies on
 * linux/input-event-codes.h.
 */
class EvdevKeyMapper {
public:
    static lcl::platform::PhysicalKey toPhysicalKey(uint32_t linuxKeycode);
};

} // namespace lcl::platform::desktop
