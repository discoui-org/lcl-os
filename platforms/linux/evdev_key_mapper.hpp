#pragma once

#include <cstdint>
#include "platforms/common/input_backend.hpp"
#include "platforms/common/keyboard_types.hpp"

namespace lcl::platform::desktop {

/**
 * @brief Translates Linux evdev raw keycodes & button codes to platform-neutral representations.
 *
 * Exists exclusively in platforms/linux/. Holds all dependencies on
 * linux/input-event-codes.h.
 */
class EvdevKeyMapper {
public:
    static lcl::platform::PhysicalKey toPhysicalKey(uint32_t linuxKeycode);
    static lcl::platform::PointerButton toPointerButton(uint32_t linuxButton);
};

} // namespace lcl::platform::desktop
