#include "platform/desktop/drm_display_backend.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <thread>
#include <chrono>

namespace lcl::platform::desktop {
namespace {

struct PreferredMode {
    int width{0};
    int height{0};
    int refreshHz{0};
};

PreferredMode parsePreferredModeFromBoot() {
    PreferredMode pref{};

    auto applyVideoToken = [&](const std::string& token) {
        // video=1280x800-32@60  or  video=1280x800@60  or  video=1280x800
        if (token.rfind("video=", 0) != 0) {
            return;
        }
        std::string spec = token.substr(6);
        // strip connector prefix like "Virtual-1:" if present
        auto colon = spec.find(':');
        if (colon != std::string::npos) {
            spec = spec.substr(colon + 1);
        }
        int w = 0, h = 0, hz = 0;
        if (std::sscanf(spec.c_str(), "%dx%d-%*d@%d", &w, &h, &hz) >= 2 ||
            std::sscanf(spec.c_str(), "%dx%d@%d", &w, &h, &hz) >= 2 ||
            std::sscanf(spec.c_str(), "%dx%d", &w, &h) >= 2) {
            if (w > 0 && h > 0) {
                pref.width = w;
                pref.height = h;
                if (hz > 0) {
                    pref.refreshHz = hz;
                }
            }
        }
    };

    auto applyKv = [&](const std::string& token) {
        auto eq = token.find('=');
        if (eq == std::string::npos) {
            return;
        }
        std::string key = token.substr(0, eq);
        std::string val = token.substr(eq + 1);
        try {
            if (key == "lcl.width") {
                pref.width = std::stoi(val);
            } else if (key == "lcl.height") {
                pref.height = std::stoi(val);
            } else if (key == "lcl.refresh" || key == "lcl.hz") {
                pref.refreshHz = std::stoi(val);
            }
        } catch (...) {
        }
    };

    std::ifstream cmdline("/proc/cmdline");
    if (cmdline) {
        std::string line;
        std::getline(cmdline, line);
        std::istringstream iss(line);
        std::string token;
        while (iss >> token) {
            applyVideoToken(token);
            applyKv(token);
        }
    }

    if (const char* ew = std::getenv("LCL_WIDTH")) {
        try { pref.width = std::stoi(ew); } catch (...) {}
    }
    if (const char* eh = std::getenv("LCL_HEIGHT")) {
        try { pref.height = std::stoi(eh); } catch (...) {}
    }

    return pref;
}

drmModeModeInfo pickMode(drmModeConnectorPtr conn, const PreferredMode& pref) {
    drmModeModeInfo best = conn->modes[0];

    if (pref.width > 0 && pref.height > 0) {
        int bestScore = -1;
        for (int m = 0; m < conn->count_modes; ++m) {
            const auto& mode = conn->modes[m];
            const int dw = static_cast<int>(mode.hdisplay) - pref.width;
            const int dh = static_cast<int>(mode.vdisplay) - pref.height;
            // Prefer exact match, then closest area; refresh as tie-breaker
            int score = 0;
            if (mode.hdisplay == static_cast<uint16_t>(pref.width) &&
                mode.vdisplay == static_cast<uint16_t>(pref.height)) {
                score = 1'000'000;
                if (pref.refreshHz > 0) {
                    score -= std::abs(static_cast<int>(mode.vrefresh) - pref.refreshHz) * 100;
                } else {
                    score += static_cast<int>(mode.vrefresh);
                }
            } else {
                // Lower is better distance — invert into score
                const int dist = dw * dw + dh * dh;
                score = 500'000 - dist;
                if (pref.refreshHz > 0) {
                    score -= std::abs(static_cast<int>(mode.vrefresh) - pref.refreshHz);
                }
            }
            if (score > bestScore) {
                bestScore = score;
                best = mode;
            }
        }
        return best;
    }

    // No preference: highest refresh, then largest resolution
    for (int m = 0; m < conn->count_modes; ++m) {
        const auto& mode = conn->modes[m];
        if (mode.vrefresh > best.vrefresh ||
            (mode.vrefresh == best.vrefresh &&
             (mode.hdisplay * mode.vdisplay > best.hdisplay * best.vdisplay))) {
            best = mode;
        }
    }
    return best;
}

} // namespace

DrmDisplayBackend::DrmDisplayBackend() = default;

DrmDisplayBackend::~DrmDisplayBackend() {
    shutdown();
}

DrmDisplayBackend::DrmDisplayBackend(DrmDisplayBackend&& other) noexcept
    : m_devicePath(std::move(other.m_devicePath)),
      m_drmDevice(other.m_drmDevice),
      m_fbDevice(other.m_fbDevice),
      m_activeMode(other.m_activeMode),
      m_displayType(other.m_displayType),
      m_initialized(other.m_initialized) {
    other.m_drmDevice = DRMDeviceInfo{};
    other.m_fbDevice = FBDeviceInfo{};
    other.m_initialized = false;
    other.m_displayType = DesktopDisplayType::None;
}

DrmDisplayBackend& DrmDisplayBackend::operator=(DrmDisplayBackend&& other) noexcept {
    if (this != &other) {
        shutdown();
        m_devicePath = std::move(other.m_devicePath);
        m_drmDevice = other.m_drmDevice;
        m_fbDevice = other.m_fbDevice;
        m_activeMode = other.m_activeMode;
        m_displayType = other.m_displayType;
        m_initialized = other.m_initialized;

        other.m_drmDevice = DRMDeviceInfo{};
        other.m_fbDevice = FBDeviceInfo{};
        other.m_initialized = false;
        other.m_displayType = DesktopDisplayType::None;
    }
    return *this;
}

bool DrmDisplayBackend::initialize() {
    return initialize(m_devicePath);
}

bool DrmDisplayBackend::initialize(const std::string& devicePath) {
    if (m_initialized) {
        std::cout << "[LCL Display] DrmDisplayBackend already initialized.\n";
        return true;
    }

    m_devicePath = devicePath;
    std::cout << "[LCL Display] Initializing Desktop Display Subsystem...\n";

    // 1. Try DRM/KMS initialization with retry loop
    if (probeDRMWithRetry(m_devicePath)) {
        m_displayType = DesktopDisplayType::DRM_KMS;
        m_initialized = true;
        std::cout << "[LCL Display] DRM/KMS Backend successfully initialized!\n";
        std::cout << "  - Device: " << m_drmDevice.path << "\n";
        std::cout << "  - Driver: " << m_drmDevice.driverName << " v" << m_drmDevice.driverVersion << "\n";
        std::cout << "  - Hardware Acceleration: " << (m_drmDevice.hasHardwareAcceleration ? "ENABLED (" + m_drmDevice.driverName + ")" : "DISABLED (Software FB)") << "\n";
        if (!m_drmDevice.renderNodePath.empty()) {
            std::cout << "  - Render Node: " << m_drmDevice.renderNodePath << "\n";
        }
        std::cout << "  - Resolution: " << m_activeMode.width << "x" << m_activeMode.height
                  << " @ " << m_activeMode.refreshRateHz << "Hz (" << m_activeMode.name << ")\n";
        return true;
    }

    // 2. Try Linux Framebuffer (/dev/fb0) fallback
    std::cout << "[LCL Display] Probing Linux Framebuffer (/dev/fb0) fallback...\n";
    if (probeLinuxFramebuffer()) {
        m_displayType = DesktopDisplayType::LinuxFB;
        m_initialized = true;
        std::cout << "[LCL Display] Linux Framebuffer Backend (/dev/fb0) successfully initialized!\n";
        std::cout << "  - Device: /dev/fb0\n";
        std::cout << "  - Resolution: " << m_activeMode.width << "x" << m_activeMode.height << "\n";
        return true;
    }

    std::cout << "[LCL Display WARNING] Neither DRM/KMS nor /dev/fb0 available. Running in fallback mode.\n";
    return false;
}

bool DrmDisplayBackend::probeDRMWithRetry(const std::string& devicePath) {
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

bool DrmDisplayBackend::probeDRMResources() {
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

    const PreferredMode pref = parsePreferredModeFromBoot();
    if (pref.width > 0 && pref.height > 0) {
        std::cout << "[LCL Display] Preferred mode from boot: "
                  << pref.width << "x" << pref.height;
        if (pref.refreshHz > 0) {
            std::cout << "@" << pref.refreshHz;
        }
        std::cout << "\n";
    }

    drmModeModeInfo bestMode = pickMode(m_drmDevice.connector, pref);
    m_drmDevice.currentMode = bestMode;
    m_activeMode.width = m_drmDevice.currentMode.hdisplay;
    m_activeMode.height = m_drmDevice.currentMode.vdisplay;
    m_activeMode.refreshRate = m_drmDevice.currentMode.vrefresh;
    m_activeMode.refreshRateHz = m_drmDevice.currentMode.vrefresh;
    m_activeMode.name = m_drmDevice.currentMode.name;

    if (pref.width > 0 && pref.height > 0 &&
        (static_cast<int>(m_activeMode.width) != pref.width ||
         static_cast<int>(m_activeMode.height) != pref.height)) {
        std::cout << "[LCL Display WARNING] Exact mode " << pref.width << "x" << pref.height
                  << " not in EDID list; using closest "
                  << m_activeMode.width << "x" << m_activeMode.height << "\n";
    }

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

bool DrmDisplayBackend::probeRenderNode() {
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

bool DrmDisplayBackend::probeLinuxFramebuffer() {
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
    m_activeMode.refreshRateHz = 60;
    m_activeMode.name = "LinuxFB";

    return true;
}

bool DrmDisplayBackend::initHardwareCursor(uint32_t width, uint32_t height,
                                           float deviceScale) {
    if (!m_initialized || m_displayType != DesktopDisplayType::DRM_KMS || m_drmDevice.fd < 0) return false;
    if (!m_drmDevice.crtc) return false;

    uint32_t crtcId = m_drmDevice.crtc->crtc_id;

    struct drm_mode_create_dumb creq{};
    creq.width = width;
    creq.height = height;
    creq.bpp = 32;

    if (ioctl(m_drmDevice.fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        std::cerr << "[LCL Display WARNING] Failed to create DRM Hardware Cursor dumb buffer.\n";
        return false;
    }

    m_drmDevice.cursorWidth = width;
    m_drmDevice.cursorHeight = height;
    m_drmDevice.cursorHandle = creq.handle;
    m_drmDevice.cursorSize = creq.size;

    struct drm_mode_map_dumb mreq{};
    mreq.handle = creq.handle;
    if (ioctl(m_drmDevice.fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        return false;
    }

    void* mapPtr = mmap(nullptr, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, m_drmDevice.fd, mreq.offset);
    if (mapPtr == MAP_FAILED) {
        return false;
    }

    m_drmDevice.cursorPixels = static_cast<uint32_t*>(mapPtr);
    std::memset(m_drmDevice.cursorPixels, 0, creq.size);

    // Rasterize the logical cursor at the output RenderTarget scale supplied by
    // the compositor. The backend never consults process-global UI scale.
    static const char* cursorShape[] = {
        "X           ",
        "XX          ",
        "X.X         ",
        "X..X        ",
        "X...X       ",
        "X....X      ",
        "X.....X     ",
        "X......X    ",
        "X.......X   ",
        "X........X  ",
        "X.....XXXXX ",
        "X..X..X     ",
        "X.X X..X    ",
        "XX   X..X   ",
        "X    X..X   ",
        "      XX    "
    };

    const float scale = std::clamp(
        std::isfinite(deviceScale) ? deviceScale : 1.0f, 0.5f, 4.0f);
    const int pitch = static_cast<int>(width);
    const int drawnWidth = std::min(
        static_cast<int>(width), static_cast<int>(std::ceil(12.0f * scale)));
    const int drawnHeight = std::min(
        static_cast<int>(height), static_cast<int>(std::ceil(16.0f * scale)));
    for (int py = 0; py < drawnHeight; ++py) {
        const int r = std::min(15, static_cast<int>(std::floor(py / scale)));
        for (int px = 0; px < drawnWidth; ++px) {
            const int c = std::min(11, static_cast<int>(std::floor(px / scale)));
            const char ch = cursorShape[r][c];
            uint32_t color = 0;
            if (ch == 'X') {
                color = 0xFF000000;
            } else if (ch == '.') {
                color = 0xFFFFFFFF;
            } else {
                continue;
            }
            m_drmDevice.cursorPixels[py * pitch + px] = color;
        }
    }

    // Register Hardware Cursor Plane on CRTC
    if (drmModeSetCursor(m_drmDevice.fd, crtcId, m_drmDevice.cursorHandle, width, height) != 0) {
        std::cerr << "[LCL Display WARNING] drmModeSetCursor failed.\n";
        return false;
    }

    m_drmDevice.hasHardwareCursor = true;
    std::cout << "[LCL Display] DRM Hardware Cursor Plane initialized (" << width << "x" << height << " ARGB, Zero-Latency)!\n";
    return true;
}

bool DrmDisplayBackend::moveHardwareCursor(int x, int y) {
    if (!m_drmDevice.hasHardwareCursor || m_drmDevice.fd < 0 || !m_drmDevice.crtc) return false;
    return drmModeMoveCursor(m_drmDevice.fd, m_drmDevice.crtc->crtc_id, x, y) == 0;
}

bool DrmDisplayBackend::createDumbScanout(uint32_t width, uint32_t height) {
    if (!m_initialized || m_displayType != DesktopDisplayType::DRM_KMS || m_drmDevice.fd < 0) return false;
    if (!m_drmDevice.crtc || !m_drmDevice.connector) return false;

    uint32_t crtcId = m_drmDevice.crtc->crtc_id;
    uint32_t connectorId = m_drmDevice.connector->connector_id;
    auto modeInfo = m_drmDevice.currentMode;

    struct drm_mode_create_dumb creq{};
    creq.width = width;
    creq.height = height;
    creq.bpp = 32;

    if (ioctl(m_drmDevice.fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) return false;

    m_dumbScanout.width = width;
    m_dumbScanout.height = height;
    m_dumbScanout.pitch = creq.pitch;
    m_dumbScanout.handle = creq.handle;
    m_dumbScanout.size = creq.size;

    if (drmModeAddFB(m_drmDevice.fd, width, height, 24, 32, creq.pitch, creq.handle, &m_dumbScanout.fbId) != 0) return false;

    struct drm_mode_map_dumb mreq{};
    mreq.handle = creq.handle;
    if (ioctl(m_drmDevice.fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) return false;

    void* mapPtr = mmap(nullptr, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, m_drmDevice.fd, mreq.offset);
    if (mapPtr == MAP_FAILED) return false;

    m_dumbScanout.pixelData = static_cast<uint32_t*>(mapPtr);

    if (drmModeSetCrtc(m_drmDevice.fd, crtcId, m_dumbScanout.fbId, 0, 0, &connectorId, 1, &modeInfo) != 0) {
        std::cerr << "[LCL Render WARNING] drmModeSetCrtc failed for dumb buffer.\n";
    } else {
        std::cout << "[LCL Render] DRM Modeset active on CRTC ID: " << crtcId << " (FB ID: " << m_dumbScanout.fbId << ")\n";
    }

    return true;
}

void DrmDisplayBackend::destroyDumbScanout() {
    if (m_dumbScanout.pixelData && m_dumbScanout.size > 0) {
        munmap(m_dumbScanout.pixelData, m_dumbScanout.size);
        m_dumbScanout.pixelData = nullptr;
    }
    if (m_dumbScanout.fbId > 0 && m_drmDevice.fd >= 0) {
        drmModeRmFB(m_drmDevice.fd, m_dumbScanout.fbId);
        m_dumbScanout.fbId = 0;
    }
    if (m_dumbScanout.handle > 0 && m_drmDevice.fd >= 0) {
        struct drm_mode_destroy_dumb dreq{};
        dreq.handle = m_dumbScanout.handle;
        ioctl(m_drmDevice.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        m_dumbScanout.handle = 0;
    }
}

void DrmDisplayBackend::flushDumbScanout() {
    if (m_drmDevice.fd >= 0 && m_dumbScanout.fbId > 0) {
        drmModeDirtyFB(m_drmDevice.fd, m_dumbScanout.fbId, nullptr, 0);
    }
}

void DrmDisplayBackend::shutdown() {
    if (!m_initialized) return;
    std::cout << "[LCL Display] Shutting down Desktop Display Subsystem...\n";
    destroyDumbScanout();
    cleanupDRMDevice();
    cleanupFBDevice();
    m_displayType = DesktopDisplayType::None;
    m_initialized = false;
}

void DrmDisplayBackend::cleanupDRMDevice() {
    destroyDumbScanout();
    if (m_drmDevice.cursorPixels && m_drmDevice.cursorSize > 0) {
        munmap(m_drmDevice.cursorPixels, m_drmDevice.cursorSize);
        m_drmDevice.cursorPixels = nullptr;
    }
    if (m_drmDevice.cursorHandle > 0 && m_drmDevice.fd >= 0) {
        struct drm_mode_destroy_dumb dreq{m_drmDevice.cursorHandle};
        ioctl(m_drmDevice.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        m_drmDevice.cursorHandle = 0;
    }
    if (m_drmDevice.renderNodeFd >= 0) { close(m_drmDevice.renderNodeFd); m_drmDevice.renderNodeFd = -1; }
    if (m_drmDevice.crtc) { drmModeFreeCrtc(m_drmDevice.crtc); m_drmDevice.crtc = nullptr; }
    if (m_drmDevice.encoder) { drmModeFreeEncoder(m_drmDevice.encoder); m_drmDevice.encoder = nullptr; }
    if (m_drmDevice.connector) { drmModeFreeConnector(m_drmDevice.connector); m_drmDevice.connector = nullptr; }
    if (m_drmDevice.resources) { drmModeFreeResources(m_drmDevice.resources); m_drmDevice.resources = nullptr; }
    if (m_drmDevice.fd >= 0) { close(m_drmDevice.fd); m_drmDevice.fd = -1; }
}

void DrmDisplayBackend::cleanupFBDevice() {
    if (m_fbDevice.pixelData && m_fbDevice.size > 0) {
        munmap(m_fbDevice.pixelData, m_fbDevice.size);
        m_fbDevice.pixelData = nullptr;
    }
    if (m_fbDevice.fd >= 0) {
        close(m_fbDevice.fd);
        m_fbDevice.fd = -1;
    }
}

} // namespace lcl::platform::desktop
