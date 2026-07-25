#pragma once

#include <string>
#include <memory>
#include <vector>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace lcl::core {

struct DisplayMode {
    uint32_t width{0};
    uint32_t height{0};
    uint32_t refreshRate{0};
    std::string name;
};

struct DRMDevice {
    int fd{-1};
    std::string path;
    drmModeResPtr resources{nullptr};
    drmModeConnectorPtr connector{nullptr};
    drmModeEncoderPtr encoder{nullptr};
    drmModeCrtcPtr crtc{nullptr};
    drmModeModeInfo currentMode{};
};

class DisplayManager {
public:
    DisplayManager();
    ~DisplayManager();

    // Non-copyable
    DisplayManager(const DisplayManager&) = delete;
    DisplayManager& operator=(const DisplayManager&) = delete;

    // Moveable
    DisplayManager(DisplayManager&&) noexcept;
    DisplayManager& operator=(DisplayManager&&) noexcept;

    /**
     * @brief Probe and initialize DRM/KMS graphics device.
     * @param devicePath Path to DRM device node (default: /dev/dri/card0)
     * @return true if DRM/KMS device initialized successfully, false otherwise
     */
    bool initialize(const std::string& devicePath = "/dev/dri/card0");

    /**
     * @brief Release all DRM/KMS resources.
     */
    void shutdown();

    bool isInitialized() const { return m_initialized; }
    const DisplayMode& getActiveDisplayMode() const { return m_activeMode; }
    const std::string& getDevicePath() const { return m_device.path; }

private:
    bool probeDRMResources();
    void cleanupDRMDevice();

    DRMDevice m_device;
    DisplayMode m_activeMode;
    bool m_initialized{false};
};

} // namespace lcl::core
