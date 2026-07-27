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
    bool isRepeat{false};
    uint32_t key{0};
    bool superPressed{false};
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
     * @brief Initialize input subsystem. Tries libinput (udev) first,
     *        then falls back to raw Linux evdev (no udev required).
     * @param seatName Seat identifier (default: "seat0")
     * @return true if any input backend initialized successfully
     */
    bool initialize(const std::string& seatName = "seat0");

    void setEventCallback(EventCallback cb) { m_eventCallback = std::move(cb); }

    /**
     * @brief Poll and process pending input events.
     * @param screenWidth  Screen width for absolute coordinate scaling
     * @param screenHeight Screen height for absolute coordinate scaling
     * @return Number of events dispatched
     */
    size_t dispatchEvents(int screenWidth = 1024, int screenHeight = 768);

    void shutdown();

    bool isInitialized() const { return m_initialized; }
    int getFD() const;

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
    std::string m_seatName;
    EventCallback m_eventCallback;
    bool m_initialized{false};
    bool m_usingEvdev{false};
    bool m_superPressed{false};
};

} // namespace lcl::core

