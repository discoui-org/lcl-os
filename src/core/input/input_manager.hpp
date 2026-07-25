#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <libinput.h>
#include <libudev.h>

namespace lcl::core {

enum class InputEventType {
    Unknown,
    PointerMotion,
    PointerButton,
    KeyboardKey
};

struct InputEvent {
    InputEventType type{InputEventType::Unknown};
    double dx{0.0};
    double dy{0.0};
    double absoluteX{-1.0};
    double absoluteY{-1.0};
    uint32_t button{0};
    bool pressed{false};
    uint32_t key{0};
    std::string deviceName;
};

class InputManager {
public:
    using EventCallback = std::function<void(const InputEvent&)>;

    InputManager();
    ~InputManager();

    // Non-copyable
    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    // Moveable
    InputManager(InputManager&&) noexcept;
    InputManager& operator=(InputManager&&) noexcept;

    /**
     * @brief Initialize libinput using udev seat (requires udevd) or fallback
     *        to libinput path backend which directly opens /dev/input/eventX nodes.
     * @param seatName Seat identifier (default: "seat0")
     * @return true if initialized successfully, false otherwise
     */
    bool initialize(const std::string& seatName = "seat0");

    /**
     * @brief Register callback for input event notifications.
     */
    void setEventCallback(EventCallback cb) { m_eventCallback = std::move(cb); }

    /**
     * @brief Poll and process pending input events from evdev.
     * @param screenWidth Current screen width for absolute coordinate transformation
     * @param screenHeight Current screen height for absolute coordinate transformation
     * @return Number of events dispatched
     */
    size_t dispatchEvents(int screenWidth = 1024, int screenHeight = 768);

    /**
     * @brief Release libinput and udev resources.
     */
    void shutdown();

    bool isInitialized() const { return m_initialized; }
    int getFD() const;

private:
    bool initWithUdev(const std::string& seatName);
    bool initWithPathBackend();
    void cleanup();

    struct udev* m_udev{nullptr};
    struct libinput* m_libinput{nullptr};
    std::string m_seatName;
    EventCallback m_eventCallback;
    bool m_initialized{false};
    bool m_usingPathBackend{false};
};

} // namespace lcl::core
