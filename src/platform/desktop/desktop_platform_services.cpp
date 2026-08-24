#include "platform/desktop/desktop_platform_services.hpp"
#include <iostream>

namespace lcl::platform::desktop {

DesktopPlatformServices::DesktopPlatformServices() = default;

DesktopPlatformServices::~DesktopPlatformServices() {
    shutdown();
}

bool DesktopPlatformServices::initialize() {
    if (m_initialized) return true;

    const auto gestaltResult = lcl::platform::loadGestalt(m_paths.gestaltFilePath());
    if (!gestaltResult.ok()) {
        std::cerr << "[LCL Desktop] " << gestaltResult.error << "\n";
        return false;
    }
    m_gestalt = gestaltResult.gestalt;
    m_displayBackend.setGestalt(m_gestalt);
    if (gestaltResult.loadedFromFile) {
        std::cout << "[LCL Gestalt] Loaded " << gestaltResult.path
                  << " (" << m_gestalt.name << ")\n";
    } else {
        std::cout << "[LCL Gestalt] Using built-in default; no file at "
                  << gestaltResult.path << "\n";
    }

    // 1. Initialize Display Backend (DRM/KMS with LinuxFB fallback)
    if (!m_displayBackend.initialize("/dev/dri/card0")) {
        std::cerr << "[LCL Desktop] Display backend running in fallback mode.\n";
    }

    // 2. Initialize Graphics Context (GBM/EGL on DRM card)
    const auto& mode = m_displayBackend.activeMode();
    uint32_t renW = mode.width > 0 ? mode.width : 1024;
    uint32_t renH = mode.height > 0 ? mode.height : 768;

    if (m_displayBackend.getDrmFd() >= 0) {
        if (!m_graphicsContext.initialize(
                m_displayBackend.getDrmFd(),
                renW, renH,
                m_displayBackend.getCrtcId(),
                m_displayBackend.getConnectorId())) {
            std::cerr << "[LCL Desktop] GBM/EGL graphics context failed to initialize.\n";
        }
    }

    m_initialized = true;
    return true;
}

void DesktopPlatformServices::shutdown() {
    if (!m_initialized) return;

    // Shutdown in reverse dependency order
    m_inputBackend.shutdown();
    m_graphicsContext.shutdown();
    m_displayBackend.shutdown();

    m_initialized = false;
}

} // namespace lcl::platform::desktop
