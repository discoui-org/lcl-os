#include "platform/android/android_platform_services.hpp"
#include <iostream>

namespace lcl::platform::android {

AndroidPlatformServices::AndroidPlatformServices() = default;

AndroidPlatformServices::~AndroidPlatformServices() {
    shutdown();
}

bool AndroidPlatformServices::initialize() {
    if (m_initialized) return true;

    // 1. Initialize Android Display Backend (Composer3 AIDL, then Composer 2.4/2.2 HIDL)
    if (!m_displayBackend.initialize()) {
        std::cerr << "[AndroidPlatformServices] Display backend initialization failed or running in fallback.\n";
    }

    // 2. Initialize Android Graphics Context (EGL/GLES 3) with active display dimensions
    const auto& mode = m_displayBackend.activeMode();
    uint32_t width = mode.width > 0 ? mode.width : 320;
    uint32_t height = mode.height > 0 ? mode.height : 640;

    if (!m_graphicsContext.initialize(width, height, &m_displayBackend)) {
        std::cerr << "[AndroidPlatformServices] Graphics context initialization failed.\n";
        return false;
    }

    // 3. Initialize Input Backend
    m_inputBackend.initialize(nullptr);

    m_initialized = true;
    return true;
}

void AndroidPlatformServices::shutdown() {
    if (!m_initialized) return;

    // Shutdown in reverse dependency order
    m_inputBackend.shutdown();
    m_graphicsContext.shutdown();
    m_displayBackend.shutdown();

    m_initialized = false;
}

} // namespace lcl::platform::android
