#pragma once

#include "platform/common/display_backend.hpp"

#include <cstdint>
#include <memory>

struct AHardwareBuffer;

namespace lcl::platform::android {

/** Android Hardware Composer 2.4 HIDL presentation path. */
class AndroidHidlDisplayBackend final {
public:
    struct Impl;

    AndroidHidlDisplayBackend();
    ~AndroidHidlDisplayBackend();

    bool initialize(float outputScale);
    void shutdown();
    bool presentBuffer(AHardwareBuffer* buffer, int acquireFenceFd = -1);

    bool isInitialized() const { return m_initialized; }
    int64_t displayId() const { return static_cast<int64_t>(m_displayId); }
    int64_t layerId() const { return static_cast<int64_t>(m_layerId); }
    bool isDisplayConnected() const { return m_displayConnected; }
    const lcl::platform::DisplayMode& activeMode() const { return m_activeMode; }

private:
    std::unique_ptr<Impl> m_impl;
    bool m_initialized{false};
    uint64_t m_displayId{0};
    uint64_t m_layerId{0};
    bool m_hasLayer{false};
    bool m_displayConnected{false};
    lcl::platform::DisplayMode m_activeMode;
};

} // namespace lcl::platform::android
