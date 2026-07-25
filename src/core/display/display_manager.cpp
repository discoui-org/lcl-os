#include "core/display/display_manager.hpp"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

namespace lcl::core {

DisplayManager::DisplayManager() = default;

DisplayManager::~DisplayManager() {
    shutdown();
}

DisplayManager::DisplayManager(DisplayManager&& other) noexcept
    : m_device(other.m_device),
      m_activeMode(other.m_activeMode),
      m_initialized(other.m_initialized) {
    other.m_device = DRMDevice{};
    other.m_initialized = false;
}

DisplayManager& DisplayManager::operator=(DisplayManager&& other) noexcept {
    if (this != &other) {
        shutdown();
        m_device = other.m_device;
        m_activeMode = other.m_activeMode;
        m_initialized = other.m_initialized;

        other.m_device = DRMDevice{};
        other.m_initialized = false;
    }
    return *this;
}

bool DisplayManager::initialize(const std::string& devicePath) {
    if (m_initialized) {
        std::cout << "[LCL DRM/KMS] DisplayManager already initialized.\n";
        return true;
    }

    std::cout << "[LCL DRM/KMS] Opening DRM device node: " << devicePath << "...\n";
    m_device.path = devicePath;
    m_device.fd = open(devicePath.c_str(), O_RDWR | O_CLOEXEC);

    if (m_device.fd < 0) {
        std::cerr << "[LCL DRM/KMS WARNING] Failed to open DRM device " << devicePath
                  << ": " << std::strerror(errno) << "\n";
        std::cerr << "[LCL DRM/KMS HINT] Device node unavailable or insufficient permission (user needs 'video' or 'render' group).\n";
        return false;
    }

    // Enable modern client capabilities
    if (drmSetClientCap(m_device.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0) {
        std::cout << "[LCL DRM/KMS] Universal planes capability enabled.\n";
    }
    if (drmSetClientCap(m_device.fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0) {
        std::cout << "[LCL DRM/KMS] Atomic KMS capability enabled.\n";
    }

    if (!probeDRMResources()) {
        std::cerr << "[LCL DRM/KMS WARNING] Failed to probe DRM/KMS connectors/CRTC on " << devicePath << ".\n";
        cleanupDRMDevice();
        return false;
    }

    m_initialized = true;
    std::cout << "[LCL DRM/KMS] Display Subsystem successfully initialized!\n";
    std::cout << "  - Device: " << m_device.path << "\n";
    std::cout << "  - Resolution: " << m_activeMode.width << "x" << m_activeMode.height
              << " @ " << m_activeMode.refreshRate << "Hz (" << m_activeMode.name << ")\n";

    return true;
}

bool DisplayManager::probeDRMResources() {
    m_device.resources = drmModeGetResources(m_device.fd);
    if (!m_device.resources) {
        std::cerr << "[LCL DRM/KMS ERROR] drmModeGetResources failed.\n";
        return false;
    }

    // Search for a connected display connector
    for (int i = 0; i < m_device.resources->count_connectors; ++i) {
        drmModeConnectorPtr conn = drmModeGetConnector(m_device.fd, m_device.resources->connectors[i]);
        if (!conn) continue;

        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            m_device.connector = conn;
            std::cout << "[LCL DRM/KMS] Found connected display connector (ID: "
                      << conn->connector_id << ", Modes: " << conn->count_modes << ")\n";
            break;
        }
        drmModeFreeConnector(conn);
    }

    if (!m_device.connector) {
        std::cerr << "[LCL DRM/KMS WARNING] No connected display connector found on " << m_device.path << ".\n";
        return false;
    }

    // Select preferred mode (first mode)
    m_device.currentMode = m_device.connector->modes[0];
    m_activeMode.width = m_device.currentMode.hdisplay;
    m_activeMode.height = m_device.currentMode.vdisplay;
    m_activeMode.refreshRate = m_device.currentMode.vrefresh;
    m_activeMode.name = m_device.currentMode.name;

    // Acquire encoder
    if (m_device.connector->encoder_id) {
        m_device.encoder = drmModeGetEncoder(m_device.fd, m_device.connector->encoder_id);
    }
    if (!m_device.encoder && m_device.connector->count_encoders > 0) {
        m_device.encoder = drmModeGetEncoder(m_device.fd, m_device.connector->encoders[0]);
    }

    if (!m_device.encoder) {
        std::cerr << "[LCL DRM/KMS WARNING] Could not find suitable encoder for connector.\n";
        return false;
    }

    // Acquire CRTC
    if (m_device.encoder->crtc_id) {
        m_device.crtc = drmModeGetCrtc(m_device.fd, m_device.encoder->crtc_id);
    } else if (m_device.resources->count_crtcs > 0) {
        m_device.crtc = drmModeGetCrtc(m_device.fd, m_device.resources->crtcs[0]);
    }

    if (!m_device.crtc) {
        std::cerr << "[LCL DRM/KMS WARNING] Could not acquire CRTC.\n";
        return false;
    }

    return true;
}

void DisplayManager::shutdown() {
    if (!m_initialized && m_device.fd < 0) return;

    std::cout << "[LCL DRM/KMS] Shutting down Display Subsystem...\n";
    cleanupDRMDevice();
    m_initialized = false;
}

void DisplayManager::cleanupDRMDevice() {
    if (m_device.crtc) {
        drmModeFreeCrtc(m_device.crtc);
        m_device.crtc = nullptr;
    }
    if (m_device.encoder) {
        drmModeFreeEncoder(m_device.encoder);
        m_device.encoder = nullptr;
    }
    if (m_device.connector) {
        drmModeFreeConnector(m_device.connector);
        m_device.connector = nullptr;
    }
    if (m_device.resources) {
        drmModeFreeResources(m_device.resources);
        m_device.resources = nullptr;
    }
    if (m_device.fd >= 0) {
        close(m_device.fd);
        m_device.fd = -1;
    }
}

} // namespace lcl::core
