#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "platforms/common/input_backend.hpp"

namespace lcl::platform::android {

/**
 * @brief Android platform input backend with direct evdev touchscreen support.
 *
 * Discovers /dev/input/event* nodes, identifies direct touchscreens via
 * EV_ABS + (ABS_X/Y or ABS_MT_POSITION_X/Y) + INPUT_PROP_DIRECT, normalizes axis ranges
 * to dynamic display dimensions, and emits unified PointerEvents (source = Touch).
 */
class AndroidInputBackend final : public lcl::platform::IInputBackend {
public:
    AndroidInputBackend();
    ~AndroidInputBackend() override;

    // Non-copyable
    AndroidInputBackend(const AndroidInputBackend&) = delete;
    AndroidInputBackend& operator=(const AndroidInputBackend&) = delete;

    // Moveable
    AndroidInputBackend(AndroidInputBackend&&) noexcept;
    AndroidInputBackend& operator=(AndroidInputBackend&&) noexcept;

    bool initialize(lcl::platform::InputEventCallback callback) override;
    void shutdown() override;
    bool isInitialized() const override { return m_initialized; }

    size_t pollEvents(int screenWidth = 1080, int screenHeight = 1920) override;

    size_t getDeviceCount() const { return m_devices.size(); }

private:
    struct Device {
        int fd{-1};
        std::string path;
        std::string name;
        bool isDirectTouchscreen{false};
        bool hasAbsX{false};
        bool hasAbsY{false};
        bool hasMTAbsX{false};
        bool hasMTAbsY{false};

        int absXMin{0}, absXMax{1};
        int absYMin{0}, absYMax{1};
        int currentAbsX{-1};
        int currentAbsY{-1};

        bool absXUpdated{false};
        bool absYUpdated{false};
        bool isTouching{false};
        bool touchPressed{false};
        bool touchReleased{false};

        double lastTouchX{-1.0};
        double lastTouchY{-1.0};
    };

    size_t scanInputDevices();
    void cleanup();

    std::vector<Device> m_devices;
    lcl::platform::InputEventCallback m_callback;
    bool m_initialized{false};
};

} // namespace lcl::platform::android

