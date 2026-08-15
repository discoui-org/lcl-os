#pragma once

#include <cstdint>
#include <string>

namespace lcl::probe {

struct DirectPresentationResult {
    bool composerServiceReachable{false};
    bool clientCreated{false};
    int32_t clientCreateStatus{0};
    std::string clientCreateErrorMsg;

    bool callbackRegistered{false};
    bool hotplugReceived{false};
    int64_t primaryDisplayId{0};
    int32_t displayWidth{0};
    int32_t displayHeight{0};
    int32_t vsyncPeriodNanos{0};

    bool layerCreated{false};
    int64_t layerId{0};

    bool ahbAllocated{false};
    bool ahbRendered{false};
    bool bufferBoundToLayer{false};

    bool validateDisplaySuccess{false};
    bool presentDisplaySuccess{false};
    int presentFenceFd{-1};

    bool visualVerificationSuccess{false};
    bool multiFrameSuccess{false};

    std::string details;
};

DirectPresentationResult runDirectPresentationProbe(bool isSurfaceFlingerRunning);

} // namespace lcl::probe
