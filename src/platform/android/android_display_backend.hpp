#pragma once

#include "platform/common/display_backend.hpp"
#include <memory>
#include <string>
#include <cstdint>

struct AHardwareBuffer;

namespace lcl::platform::android {

class AndroidHidlDisplayBackend;

/**
 * @brief Android display facade selecting Composer3 AIDL or Composer 2.2/2.4 HIDL.
 *
 * Manages the selected Composer client session, display configuration discovery,
 * primary presentation layer lifecycle, and AHardwareBuffer presentation.
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

    bool initHardwareCursor(uint32_t width = 64, uint32_t height = 64,
                            float deviceScale = 1.0f) override;
    bool moveHardwareCursor(int x, int y) override;
    bool isHardwareCursorActive() const override { return false; }

    int64_t displayId() const { return m_displayId; }
    int64_t layerId() const { return m_layerId; }
    bool isDisplayConnected() const { return m_displayConnected; }
    const char* backendName() const;

    /**
     * @brief Present an AHardwareBuffer directly to the primary Android display.
     *
     * Submits layer state through the selected AIDL/HIDL backend, validates display
     * composition and presents the display. GPU completion is supplied as an
     * acquire fence so the renderer does not need a CPU-side glFinish().
     *
     * @param buffer Hardware buffer containing the rendered frame.
     * @param acquireFenceFd Optional acquire fence file descriptor (-1 if none).
     * @return true if validate and present succeeded with 0 command errors.
     */
    bool presentBuffer(AHardwareBuffer* buffer, int acquireFenceFd = -1);

private:
    enum class BackendKind {
        None,
        AidlComposer3,
        HidlComposer,
    };

    bool initializeAidl();
    void shutdownAidl();
    bool presentBufferAidl(AHardwareBuffer* buffer, int acquireFenceFd);

    std::unique_ptr<Impl> m_impl;
    std::unique_ptr<AndroidHidlDisplayBackend> m_hidlBackend;
    BackendKind m_backendKind{BackendKind::None};

    bool m_initialized{false};
    int64_t m_displayId{0};
    int64_t m_layerId{-1};
    bool m_displayConnected{false};
    lcl::platform::DisplayMode m_activeMode;
};

} // namespace lcl::platform::android
