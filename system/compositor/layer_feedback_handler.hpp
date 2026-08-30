#pragma once

#include "system/ipc/lcl_protocol.hpp"
#include "system/ipc/raster_protocol.hpp"

namespace lcl::core {

/** Returns presentation credit for a raster layer the compositor cannot use. */
class LayerFeedbackHandler {
public:
    static bool discard(
        int clientFd, const raster_protocol::LayerReady& layer,
        protocol::LCLFrameDiscardReason reason) noexcept;
};

} // namespace lcl::core
