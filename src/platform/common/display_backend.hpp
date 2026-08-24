#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace lcl::platform {

struct DisplayMode {
    uint32_t width{0};
    uint32_t height{0};
    uint32_t refreshRate{60};
    uint32_t refreshRateHz{60};
    float scaleFactor{1.0f};
    std::string name;
};

/**
 * @brief Platform-agnostic display subsystem abstraction.
 *
 * Manages mode-setting, display dimensions, and hardware cursor planes.
 * Free of DRM/KMS/HWC headers.
 */
class IDisplayBackend {
public:
    virtual ~IDisplayBackend() = default;

    virtual bool initialize() = 0;
    virtual void shutdown() = 0;
    virtual bool isInitialized() const = 0;

    virtual const DisplayMode& activeMode() const = 0;

    // Hardware Cursor Plane
    virtual bool initHardwareCursor(uint32_t width = 64, uint32_t height = 64,
                                    float deviceScale = 1.0f) = 0;
    virtual bool moveHardwareCursor(int x, int y) = 0;
    virtual bool isHardwareCursorActive() const = 0;

    /** Wait for the next hardware display timing edge when the backend exposes it. */
    virtual bool waitForVsync(std::chrono::nanoseconds) { return false; }
};

} // namespace lcl::platform
