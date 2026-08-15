#pragma once

#include <cstdint>
#include <string>

namespace lcl::probe {

struct MagentaFrameResult {
    bool surfaceFlingerStopped{false};
    bool composerClientCreated{false};
    bool callbackRegistered{false};
    bool hotplugReceived{false};
    int64_t displayId{0};
    bool layerCreated{false};
    int64_t layerId{0};
    bool ahbRendered{false};
    bool commandsExecuted{false};
    bool presentSuccess{false};
    int presentFenceFd{-1};

    // Pixel Verification
    bool screenshotCaptured{false};
    uint8_t pixelR{0};
    uint8_t pixelG{0};
    uint8_t pixelB{0};
    uint8_t pixelA{0};
    bool isMagentaVisible{false};

    std::string details;
};

MagentaFrameResult runMagentaFramePresentation();

} // namespace lcl::probe
