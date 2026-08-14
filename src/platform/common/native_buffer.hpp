#pragma once

#include <cstdint>

namespace lcl::platform {

/**
 * @brief Opaque polymorphic representation of a hardware-allocated graphic buffer.
 *
 * Common and Core layers know only this interface. Concrete implementations
 * (e.g. DmaBufNativeBuffer on Desktop, AHardwareNativeBuffer on Android) live
 * exclusively within their respective platform modules.
 */
class INativeBuffer {
public:
    virtual ~INativeBuffer() = default;

    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;
};

} // namespace lcl::platform
