#include "core/display/display_manager.hpp"
#include <iostream>

namespace lcl::core {

DisplayManager::DisplayManager() = default;

DisplayManager::~DisplayManager() {
    shutdown();
}

DisplayManager::DisplayManager(DisplayManager&& other) noexcept
    : m_backend(std::move(other.m_backend)),
      m_eglBackend(std::move(other.m_eglBackend)),
      m_initialized(other.m_initialized) {
    other.m_initialized = false;
}

DisplayManager& DisplayManager::operator=(DisplayManager&& other) noexcept {
    if (this != &other) {
        shutdown();
        m_backend = std::move(other.m_backend);
        m_eglBackend = std::move(other.m_eglBackend);
        m_initialized = other.m_initialized;
        other.m_initialized = false;
    }
    return *this;
}

bool DisplayManager::initialize(const std::string& devicePath) {
    if (m_initialized) {
        std::cout << "[LCL Display] DisplayManager already initialized.\n";
        return true;
    }

    if (!m_backend.initialize(devicePath)) {
        return false;
    }

    m_initialized = true;

    // Attempt EGL initialization if DRM backend has active CRTC and connector
    if (m_backend.getDisplayType() == platform::desktop::DesktopDisplayType::DRM_KMS &&
        m_backend.getCrtcId() != 0) {
        const auto& mode = m_backend.activeMode();
        if (m_eglBackend.initialize(m_backend.getDrmFd(), mode.width, mode.height,
                                    m_backend.getCrtcId(), m_backend.getConnectorId())) {
            // EGL backend initialized successfully
        }
    }

    return true;
}

void DisplayManager::shutdown() {
    if (!m_initialized) return;
    std::cout << "[LCL Display] Shutting down DisplayManager...\n";
    m_eglBackend.shutdown();
    m_backend.shutdown();
    m_initialized = false;
}

} // namespace lcl::core
