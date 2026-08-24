#pragma once

#include <memory>
#include "platform/common/display_backend.hpp"
#include "platform/common/graphics_context.hpp"
#include "platform/common/input_backend.hpp"
#include "platform/common/runtime_paths.hpp"
#include "platform/common/gestalt.hpp"

namespace lcl::platform {

/**
 * @brief Unified platform services container.
 *
 * Instantiated by the platform-specific composition root and injected
 * into the Compositor core.
 */
class IPlatformServices {
public:
    virtual ~IPlatformServices() = default;

    virtual bool initialize() = 0;
    virtual void shutdown() = 0;
    virtual bool isInitialized() const = 0;

    virtual IDisplayBackend& display() = 0;
    virtual IGraphicsContext& graphics() = 0;
    virtual IInputBackend& input() = 0;
    virtual const IRuntimePaths& paths() const = 0;
    virtual const DeviceGestalt& gestalt() const = 0;
};

} // namespace lcl::platform
