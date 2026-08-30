#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <linux/fb.h>

#include "platforms/common/display_backend.hpp"
#include "platforms/common/gestalt.hpp"

namespace lcl::platform::desktop {

enum class DesktopDisplayType {
    None,
    DRM_KMS,
    LinuxFB
};

struct DRMDeviceInfo {
    int fd{-1};
    std::string path;
    std::string driverName;
    std::string driverVersion;
    std::string renderNodePath;
    int renderNodeFd{-1};
    bool hasHardwareAcceleration{false};
    uint32_t cursorHandle{0};
    uint32_t* cursorPixels{nullptr};
    uint64_t cursorSize{0};
    uint32_t cursorWidth{64};
    uint32_t cursorHeight{64};
    bool hasHardwareCursor{false};
    drmModeResPtr resources{nullptr};
    drmModeConnectorPtr connector{nullptr};
    drmModeEncoderPtr encoder{nullptr};
    drmModeCrtcPtr crtc{nullptr};
    drmModeModeInfo currentMode{};
};

struct FBDeviceInfo {
    int fd{-1};
    std::string path;
    uint32_t width{0};
    uint32_t height{0};
    uint32_t bpp{32};
    uint32_t pitch{0};
    uint64_t size{0};
    uint32_t* pixelData{nullptr};
    struct fb_var_screeninfo vinfo{};
    struct fb_fix_screeninfo finfo{};
};

/**
 * @brief Desktop Linux DRM/KMS display backend with LinuxFB fallback.
 *
 * Implements IDisplayBackend for bare-metal Linux desktop environments.
 * Manages mode-setting, CRTC/Connector configuration, and hardware cursor planes.
 */
class DrmDisplayBackend final : public lcl::platform::IDisplayBackend {
public:
    DrmDisplayBackend();
    ~DrmDisplayBackend() override;

    // Non-copyable
    DrmDisplayBackend(const DrmDisplayBackend&) = delete;
    DrmDisplayBackend& operator=(const DrmDisplayBackend&) = delete;

    // Moveable
    DrmDisplayBackend(DrmDisplayBackend&&) noexcept;
    DrmDisplayBackend& operator=(DrmDisplayBackend&&) noexcept;

    /**
     * @brief Probe and initialize DRM/KMS graphics device or Linux FB fallback.
     * @return true if graphics backend initialized successfully, false otherwise
     */
    bool initialize() override;
    bool initialize(const std::string& devicePath);
    void setGestalt(const lcl::platform::DeviceGestalt& gestalt) { m_gestalt = gestalt; }

    /**
     * @brief Release graphics resources.
     */
    void shutdown() override;

    bool isInitialized() const override { return m_initialized; }
    const DisplayMode& activeMode() const override { return m_activeMode; }

    // Hardware Cursor Plane
    bool initHardwareCursor(uint32_t width = 64, uint32_t height = 64,
                            float deviceScale = 1.0f) override;
    bool moveHardwareCursor(int x, int y) override;
    bool isHardwareCursorActive() const override { return m_drmDevice.hasHardwareCursor; }

    // Desktop/DRM-specific accessors
    DesktopDisplayType getDisplayType() const { return m_displayType; }
    const std::string& getDevicePath() const { return m_devicePath; }
    int getDrmFd() const { return m_drmDevice.fd; }
    uint32_t getCrtcId() const { return m_drmDevice.crtc ? m_drmDevice.crtc->crtc_id : 0; }
    uint32_t getConnectorId() const { return m_drmDevice.connector ? m_drmDevice.connector->connector_id : 0; }
    const DRMDeviceInfo& getDrmDevice() const { return m_drmDevice; }
    const FBDeviceInfo& getFbDevice() const { return m_fbDevice; }
    uint32_t* getFbPixelData() const { return m_fbDevice.pixelData; }

    const std::string& getDriverName() const { return m_drmDevice.driverName; }
    const std::string& getDriverVersion() const { return m_drmDevice.driverVersion; }
    const std::string& getRenderNodePath() const { return m_drmDevice.renderNodePath; }
    bool isHardwareAccelerated() const { return m_drmDevice.hasHardwareAcceleration; }

    // Fallback DRM Dumb Scanout Buffer
    bool createDumbScanout(uint32_t width, uint32_t height);
    void destroyDumbScanout();
    uint32_t* getDumbScanoutPixels() const { return m_dumbScanout.pixelData; }
    uint64_t getDumbScanoutSize() const { return m_dumbScanout.size; }
    uint32_t getDumbScanoutFbId() const { return m_dumbScanout.fbId; }
    void flushDumbScanout();

private:
    struct DumbScanoutBuffer {
        uint32_t width{0};
        uint32_t height{0};
        uint32_t pitch{0};
        uint32_t handle{0};
        uint32_t fbId{0};
        uint64_t size{0};
        uint32_t* pixelData{nullptr};
    };

    bool probeDRMWithRetry(const std::string& devicePath);
    bool probeDRMResources();
    bool probeRenderNode();
    bool probeLinuxFramebuffer();
    void cleanupDRMDevice();
    void cleanupFBDevice();

    std::string m_devicePath{"/dev/dri/card0"};
    DRMDeviceInfo m_drmDevice;
    FBDeviceInfo m_fbDevice;
    DumbScanoutBuffer m_dumbScanout;
    DisplayMode m_activeMode;
    lcl::platform::DeviceGestalt m_gestalt;
    DesktopDisplayType m_displayType{DesktopDisplayType::None};
    bool m_initialized{false};
};

} // namespace lcl::platform::desktop
