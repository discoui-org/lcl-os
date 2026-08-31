#pragma once

#include <cstdint>

namespace lcl::graphics {

enum class FontFamily : uint8_t {
    Interface,
    Monospace,
    /** Repository-packaged symbol font used by the generic Icon widget. */
    Icons,
};

} // namespace lcl::graphics
