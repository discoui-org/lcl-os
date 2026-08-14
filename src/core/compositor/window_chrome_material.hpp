#pragma once

#include <cstdint>

namespace lcl::core {

struct WindowChromeMaterial {
    uint8_t r{0};
    uint8_t g{0};
    uint8_t b{0};
    uint8_t a{0};
};

inline constexpr WindowChromeMaterial kOpaqueSsdTitlebarMaterial{17, 19, 23, 255};

inline constexpr bool paintsOpaqueSsdTitlebar(bool edgeToEdge) {
    return !edgeToEdge;
}

} // namespace lcl::core
