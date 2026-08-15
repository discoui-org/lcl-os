#pragma once

#include <android/hardware_buffer.h>
#include <cstdint>

namespace lcl::probe {

struct AhbProbeResult {
    bool allocationSuccess{false};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride{0};
    uint32_t format{0};
    uint64_t usage{0};

    bool eglImageImportSuccess{false};
    bool glRenderSuccess{false};
    bool pixelMatchGlReadPixels{false};
    bool pixelMatchCpuLock{false};
    uint32_t renderedPixelHex{0};
};

AhbProbeResult runAhbRenderProbe();

} // namespace lcl::probe
