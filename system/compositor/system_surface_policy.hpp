#pragma once

#include <cstdint>
#include <sys/types.h>

#include "system/ipc/lcl_protocol.hpp"

namespace lcl::core {

class SurfaceRegistry;

enum class SystemSurfacePlacement : uint8_t {
    ClientBounds,
    OutputBounds,
    OutputTopEdge,
    OutputBottomEdge,
};

/** Compositor-enforced policy for one trusted system surface. */
struct SystemSurfacePolicy {
    protocol::LCLWindowLayer layer{protocol::LCLWindowLayer::Normal};
    bool unfocusable{false};
    bool insetBorderEnabled{true};
    bool suppressInitialTransition{false};
    bool reservesWorkArea{false};
    bool isSystemSurface{false};
    SystemSurfacePlacement placement{SystemSurfacePlacement::ClientBounds};
};

/** Logical work-area insets contributed by mapped system surfaces. */
struct SystemReservedZone {
    float top{0.0f};
    float bottom{0.0f};
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
    /** Apply compositor-owned initial placement for system surfaces. */
    static void applyInitialPlacement(const SystemSurfacePolicy& policy,
                                      float outputWidth, float outputHeight,
                                      float& x, float& y, float& width, float& height) noexcept;
    /** Derive work-area reservations from logical surface geometry only. */
    static SystemReservedZone computeReservedZone(const SurfaceRegistry& surfaces) noexcept;
    static bool isTrustedShellPeer(pid_t pid, uid_t uid, gid_t gid) noexcept;
};

} // namespace lcl::core
