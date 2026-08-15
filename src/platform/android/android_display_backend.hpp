#pragma once

#include "platform/common/display_backend.hpp"
#include <memory>
#include <string>
#include <cstdint>

struct AHardwareBuffer;

namespace lcl::platform::android {

/**
 * @brief Android Display Backend using AIDL Hardware Composer 3 (Composer3).
 *
 * Manages Composer3 client session, display configuration discovery, primary
 * presentation layer lifecycle (createLayer/destroyLayer), and frame presentation
 * from AHardwareBuffer via strongly-typed AIDL DisplayCommand.
 */
class AndroidDisplayBackend final : public lcl::platform::IDisplayBackend {
public:
    struct Impl;

    AndroidDisplayBackend();
    ~AndroidDisplayBackend() override;

    bool initialize() override;
    void shutdown() override;

    bool isInitialized() const override { return m_initialized; }
    const lcl::platform::DisplayMode& activeMode() const override { return m_activeMode; }

    bool initHardwareCursor(uint32_t width = 64, uint32_t height = 64) override;
    bool moveHardwareCursor(int x, int y) override;
    bool isHardwareCursorActive() const override { return false; }

    int64_t displayId() const { return m_displayId; }
    int64_t layerId() const { return m_layerId; }
    bool isDisplayConnected() const { return m_displayConnected; }

    /**
     * @brief Present an AHardwareBuffer directly to the primary Android display via Composer3.
     *
     * Submits layer state, validates display composition, accepts composition changes if requested,
     * presents display, and waits for present fence signal.
     *
     * @param buffer Hardware buffer containing the rendered frame.
     * @param acquireFenceFd Optional acquire fence file descriptor (-1 if none).
     * @return true if validate and present succeeded with 0 command errors.
     */
    bool presentBuffer(AHardwareBuffer* buffer, int acquireFenceFd = -1);

private:
    std::unique_ptr<Impl> m_impl;

    bool m_initialized{false};
    int64_t m_displayId{0};
    int64_t m_layerId{-1};
    bool m_displayConnected{false};
    lcl::platform::DisplayMode m_activeMode;
};

} // namespace lcl::platform::android
