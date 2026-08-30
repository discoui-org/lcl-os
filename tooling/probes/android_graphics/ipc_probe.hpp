#pragma once

#include <cstdint>

namespace lcl::probe {

struct IpcProbeResult {
    bool socketTransferSuccess{false};
    bool handleReceivedValid{false};
    bool childImportSuccess{false};
    bool childPixelMatch{false};
    uint32_t receivedPixelHex{0};
};

IpcProbeResult runAhbIpcProbe();

} // namespace lcl::probe
