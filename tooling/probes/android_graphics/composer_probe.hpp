#pragma once

#include <string>

namespace lcl::probe {

struct ComposerProbeResult {
    bool binderNdkLoaded{false};
    bool serviceReachable{false};
    std::string serviceName;
    int statusOrErrorCode{0};
    std::string details;
};

ComposerProbeResult runComposerProbe();

} // namespace lcl::probe
