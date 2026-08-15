#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <libinput.h>
#include <libudev.h>

#include "platform/common/input_backend.hpp"
#include "platform/common/keyboard_mapper.hpp"
#include "platform/desktop/evdev_key_mapper.hpp"

namespace lcl::platform::desktop {

/**
 * @brief Desktop Linux input backend supporting libinput/udev with direct evdev fallback.
 *
 * Implements IInputBackend. Handles /dev/input enumeration, evdev ioctls,
 * multi-touch/touchpad tracking, netlink hotplug, and raw event dispatch.
 */
class EvdevInputBackend final : public lcl::platform::IInputBackend {
public:
    EvdevInputBackend();
    ~EvdevInputBackend() override;

    // Non-copyable
    EvdevInputBackend(const EvdevInputBackend&) = delete;
    EvdevInputBackend& operator=(const EvdevInputBackend&) = delete;

    // Moveable
    EvdevInputBackend(EvdevInputBackend&&) noexcept;
    EvdevInputBackend& operator=(EvdevInputBackend&&) noexcept;

    bool initialize(lcl::platform::InputEventCallback callback) override;
    bool initialize(const std::string& seatName, lcl::platform::InputEventCallback callback);
    void shutdown() override;
    bool isInitialized() const override { return m_initialized; }

    size_t pollEvents(int screenWidth = 1024, int screenHeight = 768) override;

    int getFd() const;
    bool isUsingEvdev() const { return m_usingEvdev; }

private:
    bool initWithLibinputUdev(const std::string& seatName);
    bool initWithEvdev();

    size_t dispatchLibinputEvents(int screenWidth, int screenHeight);
    size_t dispatchEvdevEvents(int screenWidth, int screenHeight);

    void cleanup();

    // libinput backend
    struct udev* m_udev{nullptr};
    struct libinput* m_libinput{nullptr};

    // Raw evdev backend
    struct EvdevDevice {
        int fd{-1};
        std::string path;
        std::string name;
        bool hasRelX{false};
        bool hasRelY{false};
        bool hasAbsX{false};
        bool hasAbsY{false};
        bool isTouchpad{false};

        int absXMin{0}, absXMax{1};
        int absYMin{0}, absYMax{1};
        int currentAbsX{-1};
        int currentAbsY{-1};

        // Touchpad tracking
        int lastTouchX{-1};
        int lastTouchY{-1};
        bool isTouching{false};

        bool absXUpdated{false};
        bool absYUpdated{false};
        double currentRelX{0.0};
        double currentRelY{0.0};
        bool relXUpdated{false};
        bool relYUpdated{false};
    };
    std::vector<EvdevDevice> m_evdevDevices;

    bool initUeventSocket();
    void processUeventHotplug();
    size_t rescanEvdevDevices();
    void performPeriodicRescan();

    int m_netlinkFd{-1};
    uint64_t m_dispatchCounter{0};
    std::string m_seatName{"seat0"};
    lcl::platform::InputEventCallback m_callback;
    bool m_initialized{false};
    bool m_usingEvdev{false};
    bool m_superPressed{false};
    bool m_shiftPressed{false};
    bool m_ctrlPressed{false};
    bool m_altPressed{false};
    bool m_capsLockActive{false};

    uint8_t getActiveModifiers() const;
};

} // namespace lcl::platform::desktop
