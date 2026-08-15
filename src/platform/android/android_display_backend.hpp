#pragma once

#include "platform/common/display_backend.hpp"
#include <memory>
#include <string>
#include <cstdint>

namespace lcl::platform::android {

/**
 * @brief Android Display Backend using AIDL Composer3 (HWC3).
 *
 * Implements IDisplayBackend for Android devices and emulators.
 * Connects directly to android.hardware.graphics.composer3.IComposer/default,
 * receives onHotplug events, and manages display metadata.
 */
class AndroidDisplayBackend final : public lcl::platform::IDisplayBackend {
public:
    AndroidDisplayBackend();
    ~AndroidDisplayBackend() override;

    // Non-copyable, non-moveable
    AndroidDisplayBackend(const AndroidDisplayBackend&) = delete;
    AndroidDisplayBackend& operator=(const AndroidDisplayBackend&) = delete;

    bool initialize() override;
    void shutdown() override;
    bool isInitialized() const override { return m_initialized; }

    const lcl::platform::DisplayMode& activeMode() const override { return m_activeMode; }

    // Hardware Cursor Plane (stub on mobile/touch platforms)
    bool initHardwareCursor(uint32_t width = 64, uint32_t height = 64) override;
    bool moveHardwareCursor(int x, int y) override;
    bool isHardwareCursorActive() const override { return false; }

    // Android-specific accessors
    int64_t getDisplayId() const { return m_displayId; }
    bool isDisplayConnected() const { return m_displayConnected; }

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;

    lcl::platform::DisplayMode m_activeMode;
    int64_t m_displayId{0};
    bool m_displayConnected{false};
    bool m_initialized{false};
};

} // namespace lcl::platform::android
