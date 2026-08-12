#pragma once

#include <cstdint>
#include <sys/types.h>

#include "core/ipc/lcl_protocol.hpp"

namespace lcl::core {

/** Compositor-enforced policy for one trusted system surface. */
struct SystemSurfacePolicy {
    protocol::LCLRole role{protocol::LCLRole::ClientApp};
    protocol::LCLWindowLayer layer{protocol::LCLWindowLayer::Normal};
    bool unfocusable{false};
    bool insetBorderEnabled{true};
    bool suppressInitialTransition{false};
    bool reservesWorkArea{false};
    bool isSystemSurface{false};
};

/**
 * Defines system-surface semantics in one place.  Client requests can name a
 * kind, but compositor policy—not client-selected layer/role commands—decides
 * its focus, decoration, transition and work-area behavior.
 */
class SystemSurfacePolicyRegistry {
public:
    static bool isValidKind(protocol::LCLSystemSurfaceKind kind) noexcept;
    static SystemSurfacePolicy policyFor(protocol::LCLSystemSurfaceKind kind) noexcept;
    static protocol::LCLSystemSurfaceKind inferLegacyKind(protocol::LCLRole role,
                                                           const char* title) noexcept;
    static bool isTrustedShellPeer(pid_t pid) noexcept;
};

} // namespace lcl::core
