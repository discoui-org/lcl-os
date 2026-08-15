#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include "platform/common/keyboard_types.hpp"

namespace lcl::platform {

/**
 * @brief Platform-agnostic mouse/pointer button identifiers.
 */
enum class PointerButton : uint32_t {
    None = 0,
    Left = 1,     // Primary
    Right = 2,    // Secondary / Context
    Middle = 3    // Tertiary / Wheel click
};

enum class RawInputEventType {
    Unknown,
    PointerMotion,
    PointerButton,
    KeyboardKey,
    TouchDown,
    TouchMove,
    TouchUp
};

struct RawInputEvent {
    RawInputEventType type{RawInputEventType::Unknown};
    double dx{0.0};
    double dy{0.0};
    double absoluteX{-1.0};
    double absoluteY{-1.0};
    PointerButton button{PointerButton::None};
    bool pressed{false};
    bool isRepeat{false};
    PhysicalKey key{PhysicalKey::Unknown};
    bool superPressed{false};
    uint8_t modifiers{0};
    char32_t codepoint{0};
    std::string deviceName;
};

using InputEventCallback = std::function<void(const RawInputEvent&)>;

/**
 * @brief Platform-agnostic hardware input event listener.
 *
 * Dispatches raw events to the Compositor's event router.
 */
class IInputBackend {
public:
    virtual ~IInputBackend() = default;

    virtual bool initialize(InputEventCallback callback) = 0;
    virtual void shutdown() = 0;
    virtual bool isInitialized() const = 0;

    virtual size_t pollEvents(int screenWidth, int screenHeight) = 0;
};

} // namespace lcl::platform

namespace lcl::core {
// Transitional type alias for core
using PointerButton = lcl::platform::PointerButton;
} // namespace lcl::core
