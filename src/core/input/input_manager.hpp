#pragma once

#include <string>
#include <memory>
#include <libinput.h>
#include <libudev.h>

namespace lcl::core {

enum class InputEventType {
    Unknown,
    KeyboardKey,
    PointerMotion,
    PointerButton,
    TouchDown
};

struct InputEvent {
    InputEventType type{InputEventType::Unknown};
    uint32_t keyOrButton{0};
    bool pressed{false};
    double dx{0.0};
    double dy{0.0};
    std::string deviceName;
};

class InputManager {
public:
    InputManager();
    ~InputManager();

    // Non-copyable
    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    // Moveable
    InputManager(InputManager&&) noexcept;
    InputManager& operator=(InputManager&&) noexcept;

    /**
     * @brief Initialize libinput / evdev context on specified seat.
     * @param seatName Seat identifier (default: "seat0")
     * @return true if initialized successfully, false otherwise
     */
    bool initialize(const std::string& seatName = "seat0");

    /**
     * @brief Poll and process pending input events from evdev.
     * @return Number of events dispatched
     */
    size_t dispatchEvents();

    /**
     * @brief Release libinput and udev resources.
     */
    void shutdown();

    bool isInitialized() const { return m_initialized; }
    int getFD() const;

private:
    void cleanup();

    struct udev* m_udev{nullptr};
    struct libinput* m_libinput{nullptr};
    std::string m_seatName;
    bool m_initialized{false};
};

} // namespace lcl::core
