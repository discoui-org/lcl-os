#pragma once

#include <cstdint>
#include <memory>

namespace lcl::platform {

// DRM_FORMAT_ARGB8888 ("AR24"). DMA-BUF descriptors always carry a DRM
// fourcc here, never an internal pixel-format enum.
inline constexpr uint32_t kDmaBufFormatArgb8888 = 0x34325241u;

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

/**
 * @brief Transport descriptor for DMA-BUF memory sharing across IPC.
 */
struct DmaBufDescriptor {
    int fd{-1};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride{0};
    uint32_t format{0};
    uint64_t modifier{~uint64_t{0}};
};

/**
 * @brief Platform-agnostic factory for constructing native buffers from IPC descriptors.
 */
class INativeBufferFactory {
public:
    virtual ~INativeBufferFactory() = default;

    virtual std::unique_ptr<INativeBuffer> createDmaBufBuffer(const DmaBufDescriptor& descriptor) = 0;
};

} // namespace lcl::platform
