#pragma once

#include "platforms/common/platform_services.hpp"
#include "platforms/linux/drm_display_backend.hpp"
#include "platforms/linux/gbm_graphics_context.hpp"
#include "platforms/linux/evdev_input_backend.hpp"
#include "platforms/linux/desktop_runtime_paths.hpp"

namespace lcl::platform::desktop {

/**
 * @brief Desktop Linux platform services implementation.
 *
 * Owns and coordinates the lifecycle of DRM/KMS display, GBM/EGL graphics context,
 * evdev input backend, and desktop runtime paths.
 */
class DesktopPlatformServices final : public lcl::platform::IPlatformServices {
public:
    DesktopPlatformServices();
    ~DesktopPlatformServices() override;

    // Non-copyable, non-moveable
    DesktopPlatformServices(const DesktopPlatformServices&) = delete;
    DesktopPlatformServices& operator=(const DesktopPlatformServices&) = delete;

    bool initialize() override;
    void shutdown() override;
    bool isInitialized() const override { return m_initialized; }

    lcl::platform::IDisplayBackend& display() override { return m_displayBackend; }
    lcl::platform::IGraphicsContext& graphics() override { return m_graphicsContext; }
    lcl::platform::IInputBackend& input() override { return m_inputBackend; }
    const lcl::platform::IRuntimePaths& paths() const override { return m_paths; }
    const lcl::platform::DeviceGestalt& gestalt() const override { return m_gestalt; }

    // Desktop-specific accessors
    DrmDisplayBackend& getDrmDisplay() { return m_displayBackend; }
    const DrmDisplayBackend& getDrmDisplay() const { return m_displayBackend; }
    GbmGraphicsContext& getGbmGraphics() { return m_graphicsContext; }
    const GbmGraphicsContext& getGbmGraphics() const { return m_graphicsContext; }
    EvdevInputBackend& getEvdevInput() { return m_inputBackend; }
    const EvdevInputBackend& getEvdevInput() const { return m_inputBackend; }
    const DesktopRuntimePaths& getDesktopPaths() const { return m_paths; }

private:
    DrmDisplayBackend m_displayBackend;
    GbmGraphicsContext m_graphicsContext;
    EvdevInputBackend m_inputBackend;
    DesktopRuntimePaths m_paths;
    lcl::platform::DeviceGestalt m_gestalt;
    bool m_initialized{false};
};

} // namespace lcl::platform::desktop
