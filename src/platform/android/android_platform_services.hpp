#pragma once

#include "platform/common/platform_services.hpp"
#include "platform/android/android_display_backend.hpp"
#include "platform/android/android_graphics_context.hpp"
#include "platform/android/android_input_backend.hpp"
#include "platform/android/android_runtime_paths.hpp"

namespace lcl::platform::android {

/**
 * @brief Android platform services implementation.
 *
 * Owns and coordinates the lifecycle of Composer3 display backend, EGL/GLES graphics context,
 * input backend, and runtime paths on Android targets.
 */
class AndroidPlatformServices final : public lcl::platform::IPlatformServices {
public:
    AndroidPlatformServices();
    ~AndroidPlatformServices() override;

    // Non-copyable, non-moveable
    AndroidPlatformServices(const AndroidPlatformServices&) = delete;
    AndroidPlatformServices& operator=(const AndroidPlatformServices&) = delete;

    bool initialize() override;
    void shutdown() override;
    bool isInitialized() const override { return m_initialized; }

    lcl::platform::IDisplayBackend& display() override { return m_displayBackend; }
    lcl::platform::IGraphicsContext& graphics() override { return m_graphicsContext; }
    lcl::platform::IInputBackend& input() override { return m_inputBackend; }
    const lcl::platform::IRuntimePaths& paths() const override { return m_paths; }

    // Android-specific accessors
    AndroidDisplayBackend& getAndroidDisplay() { return m_displayBackend; }
    const AndroidDisplayBackend& getAndroidDisplay() const { return m_displayBackend; }
    AndroidGraphicsContext& getAndroidGraphics() { return m_graphicsContext; }
    const AndroidGraphicsContext& getAndroidGraphics() const { return m_graphicsContext; }
    AndroidInputBackend& getAndroidInput() { return m_inputBackend; }
    const AndroidInputBackend& getAndroidInput() const { return m_inputBackend; }
    const AndroidRuntimePaths& getAndroidPaths() const { return m_paths; }

private:
    AndroidDisplayBackend m_displayBackend;
    AndroidGraphicsContext m_graphicsContext;
    AndroidInputBackend m_inputBackend;
    AndroidRuntimePaths m_paths;
    bool m_initialized{false};
};

} // namespace lcl::platform::android
