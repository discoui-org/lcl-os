#pragma once

#include <string>
#include <memory>
#include <vector>

#include "core/display/egl_backend.hpp"
#include "platform/common/display_backend.hpp"
#include "platform/desktop/drm_display_backend.hpp"

namespace lcl::core {

using DisplayBackendType = platform::desktop::DesktopDisplayType;
using DisplayMode = platform::DisplayMode;
using DRMDevice = platform::desktop::DRMDeviceInfo;
using FBDevice = platform::desktop::FBDeviceInfo;

/**
 * @brief Transitional DisplayManager adapter wrapping DrmDisplayBackend and EGLBackend.
 *
 * Extracted in Stage 2A: All DRM/KMS mode-setting, CRTC/Connector configuration,
 * and hardware cursor plane logic is delegated to DrmDisplayBackend (IDisplayBackend).
 */
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
     * @brief Probe and initialize DRM/KMS graphics device or Linux FB fallback.
     * @param devicePath Primary DRM device node path (default: /dev/dri/card0)
     * @return true if graphics backend initialized successfully, false otherwise
     */
    bool initialize(const std::string& devicePath = "/dev/dri/card0");

    /**
     * @brief Release graphics resources.
     */
    void shutdown();

    bool isInitialized() const { return m_initialized; }
    DisplayBackendType getBackendType() const { return m_backend.getDisplayType(); }
    const DisplayMode& getActiveDisplayMode() const { return m_backend.activeMode(); }
    const std::string& getDevicePath() const { return m_backend.getDevicePath(); }

    // DRM / FB Data accessors
    int getDRMFd() const { return m_backend.getDrmFd(); }
    const DRMDevice& getDRMDevice() const { return m_backend.getDrmDevice(); }
    const FBDevice& getFBDevice() const { return m_backend.getFbDevice(); }
    uint32_t* getFBPixelData() const { return m_backend.getFbPixelData(); }

    // Hardware Acceleration & Driver Metadata
    const std::string& getDriverName() const { return m_backend.getDriverName(); }
    const std::string& getDriverVersion() const { return m_backend.getDriverVersion(); }
    const std::string& getRenderNodePath() const { return m_backend.getRenderNodePath(); }
    bool isHardwareAccelerated() const { return m_backend.isHardwareAccelerated(); }

    // DRM Hardware Cursor Plane (Zero-Latency GPU Cursor)
    bool initHardwareCursor(uint32_t width = 64, uint32_t height = 64) {
        return m_backend.initHardwareCursor(width, height);
    }
    bool moveHardwareCursor(int x, int y) {
        return m_backend.moveHardwareCursor(x, y);
    }
    bool isHardwareCursorActive() const {
        return m_backend.isHardwareCursorActive();
    }

    // Platform display backend accessor
    platform::IDisplayBackend* getDisplayBackend() { return &m_backend; }
    platform::desktop::DrmDisplayBackend* getDrmBackend() { return &m_backend; }

    // EGL Hardware Backend Accessor (Stage 2B will transition this to GbmGraphicsContext)
    EGLBackend* getEGLBackend() { return &m_eglBackend; }

private:
    platform::desktop::DrmDisplayBackend m_backend;
    EGLBackend m_eglBackend;
    bool m_initialized{false};
};

} // namespace lcl::core
