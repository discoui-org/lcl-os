#pragma once

#include "core/ipc/lcl_protocol.hpp"
#include "core/ipc/raster_protocol.hpp"

namespace lcl::core {

/** Returns presentation credit for a raster layer the compositor cannot use. */
class LayerFeedbackHandler {
public:
    static bool discard(
        int clientFd, const raster_protocol::LayerReady& layer,
        protocol::LCLFrameDiscardReason reason) noexcept;
};

} // namespace lcl::core
