#pragma once

#include "platform/common/native_buffer.hpp"
#include <android/hardware_buffer.h>

namespace lcl::platform::android {

/**
 * @brief Android AHardwareBuffer native buffer implementation.
 *
 * Encapsulates AHardwareBuffer pointer and reference count management.
 * Kept strictly inside src/platform/android/ so platform/common, core, and render
 * remain 100% free of AHardwareBuffer / Android details.
 */
class AHardwareNativeBuffer final : public lcl::platform::INativeBuffer {
public:
    explicit AHardwareNativeBuffer(AHardwareBuffer* buffer, bool owns = true)
        : m_buffer(buffer), m_owns(owns) {
        if (m_buffer) {
            AHardwareBuffer_Desc desc = {};
            AHardwareBuffer_describe(m_buffer, &desc);
            m_width = desc.width;
            m_height = desc.height;
            if (m_owns) {
                AHardwareBuffer_acquire(m_buffer);
            }
        }
    }

    ~AHardwareNativeBuffer() override {
        if (m_buffer && m_owns) {
            AHardwareBuffer_release(m_buffer);
            m_buffer = nullptr;
        }
    }

    // Non-copyable
    AHardwareNativeBuffer(const AHardwareNativeBuffer&) = delete;
    AHardwareNativeBuffer& operator=(const AHardwareNativeBuffer&) = delete;

    // Moveable
    AHardwareNativeBuffer(AHardwareNativeBuffer&& other) noexcept
        : m_buffer(other.m_buffer),
          m_width(other.m_width),
          m_height(other.m_height),
          m_owns(other.m_owns) {
        other.m_buffer = nullptr;
        other.m_owns = false;
    }

    AHardwareNativeBuffer& operator=(AHardwareNativeBuffer&& other) noexcept {
        if (this != &other) {
            if (m_buffer && m_owns) {
                AHardwareBuffer_release(m_buffer);
            }
            m_buffer = other.m_buffer;
            m_width = other.m_width;
            m_height = other.m_height;
            m_owns = other.m_owns;
            other.m_buffer = nullptr;
            other.m_owns = false;
        }
        return *this;
    }

    uint32_t width() const override { return m_width; }
    uint32_t height() const override { return m_height; }

    AHardwareBuffer* getHandle() const { return m_buffer; }

private:
    AHardwareBuffer* m_buffer{nullptr};
    uint32_t m_width{0};
    uint32_t m_height{0};
    bool m_owns{true};
};

} // namespace lcl::platform::android
