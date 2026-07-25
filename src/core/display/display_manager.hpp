#pragma once

#include <string>
#include <memory>
#include <vector>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <linux/fb.h>

namespace lcl::core {

enum class DisplayBackendType {
    None,
    DRM_KMS,
    LinuxFB
};

struct DisplayMode {
    uint32_t width{0};
    uint32_t height{0};
    uint32_t refreshRate{0};
    std::string name;
};

struct DRMDevice {
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

struct FBDevice {
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
    DisplayBackendType getBackendType() const { return m_backendType; }
    const DisplayMode& getActiveDisplayMode() const { return m_activeMode; }
    const std::string& getDevicePath() const { return m_devicePath; }

    // DRM / FB Data accessors
    int getDRMFd() const { return m_drmDevice.fd; }
    const DRMDevice& getDRMDevice() const { return m_drmDevice; }
    const FBDevice& getFBDevice() const { return m_fbDevice; }
    uint32_t* getFBPixelData() const { return m_fbDevice.pixelData; }

    // Hardware Acceleration & Driver Metadata
    const std::string& getDriverName() const { return m_drmDevice.driverName; }
    const std::string& getDriverVersion() const { return m_drmDevice.driverVersion; }
    const std::string& getRenderNodePath() const { return m_drmDevice.renderNodePath; }
    bool isHardwareAccelerated() const { return m_drmDevice.hasHardwareAcceleration; }

    // DRM Hardware Cursor Plane (Zero-Latency GPU Cursor)
    bool initHardwareCursor(uint32_t width = 64, uint32_t height = 64);
    bool moveHardwareCursor(int x, int y);
    bool isHardwareCursorActive() const { return m_drmDevice.hasHardwareCursor; }

private:
    bool probeDRMWithRetry(const std::string& devicePath);
    bool probeDRMResources();
    bool probeRenderNode();
    bool probeLinuxFramebuffer();
    void cleanupDRMDevice();
    void cleanupFBDevice();

    std::string m_devicePath;
    DRMDevice m_drmDevice;
    FBDevice m_fbDevice;
    DisplayMode m_activeMode;
    DisplayBackendType m_backendType{DisplayBackendType::None};
    bool m_initialized{false};
};

} // namespace lcl::core
