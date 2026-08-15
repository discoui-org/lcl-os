#pragma once

#include "platform/common/input_backend.hpp"

namespace lcl::platform::android {

/**
 * @brief Android platform input backend skeleton.
 *
 * Implements IInputBackend. Full evdev/touch dispatch will be wired in later stage.
 */
class AndroidInputBackend final : public lcl::platform::IInputBackend {
public:
    AndroidInputBackend();
    ~AndroidInputBackend() override;

    bool initialize(lcl::platform::InputEventCallback callback) override;
    void shutdown() override;
    bool isInitialized() const override { return m_initialized; }

    size_t pollEvents(int screenWidth, int screenHeight) override;

private:
    lcl::platform::InputEventCallback m_callback;
    bool m_initialized{false};
};

} // namespace lcl::platform::android
