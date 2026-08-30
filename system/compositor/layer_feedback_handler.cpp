#include "system/compositor/layer_feedback_handler.hpp"

namespace lcl::core {

bool LayerFeedbackHandler::discard(
        int clientFd, const raster_protocol::LayerReady& layer,
        protocol::LCLFrameDiscardReason reason) noexcept {
    if (clientFd < 0 || layer.grant.surfaceId == 0 ||
        layer.configureSerial == 0 || layer.frameSerial == 0) {
        return false;
    }
    protocol::LCLMsgFrameDiscarded discarded{};
    discarded.surfaceId = layer.grant.surfaceId;
    discarded.configureSerial = layer.configureSerial;
    discarded.frameSerial = layer.frameSerial;
    discarded.geometryGeneration = layer.geometryGeneration;
    discarded.reason = reason;
    protocol::LCLHeader header{};
    header.opcode = protocol::LCLOpcode::FrameDiscarded;
    header.payloadSize = sizeof(discarded);
    return protocol::sendMsgWithFd(clientFd, header, &discarded);
}

} // namespace lcl::core
