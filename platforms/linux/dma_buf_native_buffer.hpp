#pragma once

#include <cstdint>
#include "platforms/common/native_buffer.hpp"

namespace lcl::platform::desktop {

/**
 * @brief Desktop Linux DMA-BUF native buffer implementation.
 *
 * Encapsulates DMA-BUF file descriptor, stride, format, and DRM modifier.
 * Kept strictly inside platforms/linux/ so platform/common and render
 * remain 100% free of DMA-BUF / DRM details.
 */
class DmaBufNativeBuffer final : public lcl::platform::INativeBuffer {
public:
    DmaBufNativeBuffer(int fd, uint32_t width, uint32_t height,
                       uint32_t stride = 0, uint32_t format = 1,
                       uint64_t modifier = ~uint64_t{0})
        : m_fd(fd),
          m_width(width),
          m_height(height),
          m_stride(stride == 0 ? width * 4 : stride),
          m_format(format),
          m_modifier(modifier) {}

    ~DmaBufNativeBuffer() override = default;

    uint32_t width() const override { return m_width; }
    uint32_t height() const override { return m_height; }

    int fd() const { return m_fd; }
    uint32_t stride() const { return m_stride; }
    uint32_t format() const { return m_format; }
    uint64_t modifier() const { return m_modifier; }

private:
    int m_fd{-1};
    uint32_t m_width{0};
    uint32_t m_height{0};
    uint32_t m_stride{0};
    uint32_t m_format{1};
    uint64_t m_modifier{~uint64_t{0}};
};

} // namespace lcl::platform::desktop
