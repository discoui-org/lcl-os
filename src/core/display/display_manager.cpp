#include "core/display/display_manager.hpp"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <thread>
#include <chrono>

namespace lcl::core {

DisplayManager::DisplayManager() = default;

DisplayManager::~DisplayManager() {
    shutdown();
}

DisplayManager::DisplayManager(DisplayManager&& other) noexcept
    : m_devicePath(std::move(other.m_devicePath)),
      m_drmDevice(other.m_drmDevice),
      m_fbDevice(other.m_fbDevice),
      m_activeMode(other.m_activeMode),
      m_backendType(other.m_backendType),
      m_initialized(other.m_initialized) {
    other.m_drmDevice = DRMDevice{};
    other.m_fbDevice = FBDevice{};
    other.m_initialized = false;
    other.m_backendType = DisplayBackendType::None;
}

DisplayManager& DisplayManager::operator=(DisplayManager&& other) noexcept {
    if (this != &other) {
        shutdown();
        m_devicePath = std::move(other.m_devicePath);
        m_drmDevice = other.m_drmDevice;
        m_fbDevice = other.m_fbDevice;
        m_activeMode = other.m_activeMode;
        m_backendType = other.m_backendType;
        m_initialized = other.m_initialized;

        other.m_drmDevice = DRMDevice{};
        other.m_fbDevice = FBDevice{};
        other.m_initialized = false;
        other.m_backendType = DisplayBackendType::None;
    }
    return *this;
}

bool DisplayManager::initialize(const std::string& devicePath) {
    if (m_initialized) {
        std::cout << "[LCL Display] DisplayManager already initialized.\n";
        return true;
    }

    m_devicePath = devicePath;
    std::cout << "[LCL Display] Initializing Display Subsystem...\n";

    // 1. Try DRM/KMS initialization with retry loop
    if (probeDRMWithRetry(m_devicePath)) {
        m_backendType = DisplayBackendType::DRM_KMS;
        m_initialized = true;
        std::cout << "[LCL Display] DRM/KMS Backend successfully initialized!\n";
        std::cout << "  - Device: " << m_drmDevice.path << "\n";
        std::cout << "  - Driver: " << m_drmDevice.driverName << " v" << m_drmDevice.driverVersion << "\n";
        std::cout << "  - Hardware Acceleration: " << (m_drmDevice.hasHardwareAcceleration ? "ENABLED (" + m_drmDevice.driverName + ")" : "DISABLED (Software FB)") << "\n";
        if (!m_drmDevice.renderNodePath.empty()) {
            std::cout << "  - Render Node: " << m_drmDevice.renderNodePath << "\n";
        }
        std::cout << "  - Resolution: " << m_activeMode.width << "x" << m_activeMode.height
                  << " @ " << m_activeMode.refreshRate << "Hz (" << m_activeMode.name << ")\n";
        return true;
    }

    // 2. Try Linux Framebuffer (/dev/fb0) fallback
    std::cout << "[LCL Display] Probing Linux Framebuffer (/dev/fb0) fallback...\n";
    if (probeLinuxFramebuffer()) {
        m_backendType = DisplayBackendType::LinuxFB;
        m_initialized = true;
        std::cout << "[LCL Display] Linux Framebuffer Backend (/dev/fb0) successfully initialized!\n";
        std::cout << "  - Device: /dev/fb0\n";
        std::cout << "  - Resolution: " << m_activeMode.width << "x" << m_activeMode.height << "\n";
        return true;
    }

    std::cout << "[LCL Display WARNING] Neither DRM/KMS nor /dev/fb0 available. Running in fallback mode.\n";
    return false;
}

bool DisplayManager::probeDRMWithRetry(const std::string& devicePath) {
    std::vector<std::string> probePaths = {devicePath, "/dev/dri/card0", "/dev/dri/card1"};

    // Retry loop up to 10 attempts (1 second total) for devtmpfs node creation in QEMU
    for (int attempt = 0; attempt < 10; ++attempt) {
        for (const auto& path : probePaths) {
            int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                m_drmDevice.path = path;
                m_drmDevice.fd = fd;

                // Enable client capabilities
                drmSetClientCap(m_drmDevice.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
                drmSetClientCap(m_drmDevice.fd, DRM_CLIENT_CAP_ATOMIC, 1);

                if (probeDRMResources()) {
                    return true;
                }
                cleanupDRMDevice();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

bool DisplayManager::probeDRMResources() {
    // Query DRM Driver Version Information
    drmVersionPtr ver = drmGetVersion(m_drmDevice.fd);
    if (ver) {
        m_drmDevice.driverName = ver->name ? ver->name : "unknown";
        m_drmDevice.driverVersion = std::to_string(ver->version_major) + "." +
                                    std::to_string(ver->version_minor) + "." +
                                    std::to_string(ver->version_patchlevel);
        drmFreeVersion(ver);
    } else {
        m_drmDevice.driverName = "generic";
        m_drmDevice.driverVersion = "0.0.0";
    }

    m_drmDevice.resources = drmModeGetResources(m_drmDevice.fd);
    if (!m_drmDevice.resources) return false;

    for (int i = 0; i < m_drmDevice.resources->count_connectors; ++i) {
        drmModeConnectorPtr conn = drmModeGetConnector(m_drmDevice.fd, m_drmDevice.resources->connectors[i]);
        if (!conn) continue;

        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            m_drmDevice.connector = conn;
            break;
        }
        drmModeFreeConnector(conn);
    }

    if (!m_drmDevice.connector) return false;

    drmModeModeInfo bestMode = m_drmDevice.connector->modes[0];
    for (int m = 0; m < m_drmDevice.connector->count_modes; ++m) {
        const auto& mode = m_drmDevice.connector->modes[m];
        if (mode.vrefresh > bestMode.vrefresh ||
           (mode.vrefresh == bestMode.vrefresh && (mode.hdisplay * mode.vdisplay > bestMode.hdisplay * bestMode.vdisplay))) {
            bestMode = mode;
        }
    }
    m_drmDevice.currentMode = bestMode;
    m_activeMode.width = m_drmDevice.currentMode.hdisplay;
    m_activeMode.height = m_drmDevice.currentMode.vdisplay;
    m_activeMode.refreshRate = m_drmDevice.currentMode.vrefresh;
    m_activeMode.name = m_drmDevice.currentMode.name;

    if (m_drmDevice.connector->encoder_id) {
        m_drmDevice.encoder = drmModeGetEncoder(m_drmDevice.fd, m_drmDevice.connector->encoder_id);
    }
    if (!m_drmDevice.encoder && m_drmDevice.connector->count_encoders > 0) {
        m_drmDevice.encoder = drmModeGetEncoder(m_drmDevice.fd, m_drmDevice.connector->encoders[0]);
    }
    if (!m_drmDevice.encoder) return false;

    if (m_drmDevice.encoder->crtc_id) {
        m_drmDevice.crtc = drmModeGetCrtc(m_drmDevice.fd, m_drmDevice.encoder->crtc_id);
    } else if (m_drmDevice.resources->count_crtcs > 0) {
        m_drmDevice.crtc = drmModeGetCrtc(m_drmDevice.fd, m_drmDevice.resources->crtcs[0]);
    }

    if (m_drmDevice.crtc != nullptr) {
        probeRenderNode();
        return true;
    }

    return false;
}

bool DisplayManager::probeRenderNode() {
    std::vector<std::string> renderNodes = {"/dev/dri/renderD128", "/dev/dri/renderD129"};
    for (const auto& path : renderNodes) {
        int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            drmVersionPtr ver = drmGetVersion(fd);
            if (ver) {
                m_drmDevice.renderNodePath = path;
                m_drmDevice.renderNodeFd = fd;
                m_drmDevice.hasHardwareAcceleration = true;
                std::cout << "[LCL Display] DRM Render Node probed: " << path
                          << " (Driver: " << (ver->name ? ver->name : "unknown") << ")\n";
                drmFreeVersion(ver);
                return true;
            }
            close(fd);
        }
    }

    // Check if primary DRM driver itself supports 3D hardware acceleration (e.g. virtio_gpu)
    if (m_drmDevice.driverName == "virtio_gpu" || m_drmDevice.driverName == "virtio-gpu" ||
        m_drmDevice.driverName == "i915" || m_drmDevice.driverName == "amdgpu" || m_drmDevice.driverName == "nouveau") {
        m_drmDevice.hasHardwareAcceleration = true;
    }

    return m_drmDevice.hasHardwareAcceleration;
}

bool DisplayManager::probeLinuxFramebuffer() {
    m_fbDevice.path = "/dev/fb0";
    m_fbDevice.fd = open(m_fbDevice.path.c_str(), O_RDWR);
    if (m_fbDevice.fd < 0) {
        return false;
    }

    if (ioctl(m_fbDevice.fd, FBIOGET_VSCREENINFO, &m_fbDevice.vinfo) < 0 ||
        ioctl(m_fbDevice.fd, FBIOGET_FSCREENINFO, &m_fbDevice.finfo) < 0) {
        close(m_fbDevice.fd);
        m_fbDevice.fd = -1;
        return false;
    }

    m_fbDevice.width = m_fbDevice.vinfo.xres;
    m_fbDevice.height = m_fbDevice.vinfo.yres;
    m_fbDevice.bpp = m_fbDevice.vinfo.bits_per_pixel;
    m_fbDevice.pitch = m_fbDevice.finfo.line_length;
    m_fbDevice.size = m_fbDevice.finfo.smem_len;

    void* ptr = mmap(nullptr, m_fbDevice.size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fbDevice.fd, 0);
    if (ptr == MAP_FAILED) {
        close(m_fbDevice.fd);
        m_fbDevice.fd = -1;
        return false;
    }

    m_fbDevice.pixelData = static_cast<uint32_t*>(ptr);
    m_activeMode.width = m_fbDevice.width;
    m_activeMode.height = m_fbDevice.height;
    m_activeMode.refreshRate = 60;
    m_activeMode.name = "LinuxFB";

    return true;
}

void DisplayManager::shutdown() {
    if (!m_initialized) return;
    std::cout << "[LCL Display] Shutting down Display Subsystem...\n";
    cleanupDRMDevice();
    cleanupFBDevice();
    m_backendType = DisplayBackendType::None;
    m_initialized = false;
}

void DisplayManager::cleanupDRMDevice() {
    if (m_drmDevice.renderNodeFd >= 0) { close(m_drmDevice.renderNodeFd); m_drmDevice.renderNodeFd = -1; }
    if (m_drmDevice.crtc) { drmModeFreeCrtc(m_drmDevice.crtc); m_drmDevice.crtc = nullptr; }
    if (m_drmDevice.encoder) { drmModeFreeEncoder(m_drmDevice.encoder); m_drmDevice.encoder = nullptr; }
    if (m_drmDevice.connector) { drmModeFreeConnector(m_drmDevice.connector); m_drmDevice.connector = nullptr; }
    if (m_drmDevice.resources) { drmModeFreeResources(m_drmDevice.resources); m_drmDevice.resources = nullptr; }
    if (m_drmDevice.fd >= 0) { close(m_drmDevice.fd); m_drmDevice.fd = -1; }
}

void DisplayManager::cleanupFBDevice() {
    if (m_fbDevice.pixelData && m_fbDevice.size > 0) {
        munmap(m_fbDevice.pixelData, m_fbDevice.size);
        m_fbDevice.pixelData = nullptr;
    }
    if (m_fbDevice.fd >= 0) {
        close(m_fbDevice.fd);
        m_fbDevice.fd = -1;
    }
}

} // namespace lcl::core
