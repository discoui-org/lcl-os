#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "platform/common/input_backend.hpp"

namespace lcl::core {

using InputEventType = lcl::platform::RawInputEventType;
using InputEvent = lcl::platform::RawInputEvent;

/**
 * @brief Platform-agnostic Compositor InputManager.
 *
 * Delegates all hardware event listening and dispatching to IInputBackend.
 * Completely free of any Linux evdev, libinput, udev, or netlink headers.
 */
class InputManager {
public:
    using EventCallback = lcl::platform::InputEventCallback;

    InputManager();
    explicit InputManager(std::unique_ptr<lcl::platform::IInputBackend> backend);
    ~InputManager();

    // Non-copyable
    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    // Moveable
    InputManager(InputManager&&) noexcept;
    InputManager& operator=(InputManager&&) noexcept;

    /**
     * @brief Initialize input subsystem with given backend.
     * @param backend Platform-specific input backend instance
     * @return true if backend initialized successfully
     */
    bool initialize(std::unique_ptr<lcl::platform::IInputBackend> backend);

    void setEventCallback(EventCallback cb);

    /**
     * @brief Poll and process pending input events.
     * @param screenWidth  Screen width for coordinate scaling
     * @param screenHeight Screen height for coordinate scaling
     * @return Number of events dispatched
     */
    size_t dispatchEvents(int screenWidth = 1024, int screenHeight = 768);

    void shutdown();

    bool isInitialized() const { return m_initialized && m_backend != nullptr; }
    int getFD() const { return -1; }

    lcl::platform::IInputBackend* getBackend() { return m_backend.get(); }

private:
    std::unique_ptr<lcl::platform::IInputBackend> m_backend;
    EventCallback m_eventCallback;
    bool m_initialized{false};
};

} // namespace lcl::core
