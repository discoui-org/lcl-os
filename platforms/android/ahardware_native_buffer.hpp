#pragma once

#include "platforms/common/native_buffer.hpp"
#include <android/hardware_buffer.h>

namespace lcl::platform::android {

/**
 * @brief Android AHardwareBuffer native buffer implementation.
 *
 * Encapsulates AHardwareBuffer pointer and reference count management.
 * Kept strictly inside platforms/android/ so platform/common, core, and render
 * remain 100% free of AHardwareBuffer / Android details.
 */
class AHardwareNativeBuffer final : public lcl::platform::INativeBuffer {
public:
    enum class ReferenceMode {
        Adopt,
        Acquire,
    };

    explicit AHardwareNativeBuffer(
            AHardwareBuffer* buffer,
            ReferenceMode referenceMode = ReferenceMode::Adopt)
        : m_buffer(buffer) {
        if (m_buffer) {
            AHardwareBuffer_Desc desc = {};
            AHardwareBuffer_describe(m_buffer, &desc);
            m_width = desc.width;
            m_height = desc.height;
            if (referenceMode == ReferenceMode::Acquire) {
                AHardwareBuffer_acquire(m_buffer);
            }
        }
    }

    ~AHardwareNativeBuffer() override {
        if (m_buffer) {
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
          m_height(other.m_height) {
        other.m_buffer = nullptr;
    }

    AHardwareNativeBuffer& operator=(AHardwareNativeBuffer&& other) noexcept {
        if (this != &other) {
            if (m_buffer) {
                AHardwareBuffer_release(m_buffer);
            }
            m_buffer = other.m_buffer;
            m_width = other.m_width;
            m_height = other.m_height;
            other.m_buffer = nullptr;
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
};

} // namespace lcl::platform::android
