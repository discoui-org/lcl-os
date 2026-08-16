#pragma once

#include <cstdint>

namespace lcl::ui {

/** Selects the text face without exposing a renderer implementation to widgets. */
enum class FontFamily : uint8_t {
    Interface,
    Monospace,
};

} // namespace lcl::ui
