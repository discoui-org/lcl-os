#pragma once

#include "system/compositor/surface_registry.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace lcl::core {

/** Serializes and promotes complete same-generation WindowGroup transactions. */
class SurfaceTransactionCoordinator {
public:
    using ReadyHandler = std::function<bool(uint32_t windowId,
                                            uint64_t generation)>;
    /** Begins only when the WindowGroup has no generation already in flight. */
    static bool begin(SurfaceRegistry& surfaces, uint32_t windowId,
                      uint64_t generation,
                      const std::vector<SurfaceRegistry::Key>& participants) noexcept;
    static void cancel(SurfaceRegistry& surfaces, uint32_t windowId,
                       uint64_t generation) noexcept;
    /** Commits ready geometry, clears its barrier, and reports incomplete work. */
    static bool promoteReady(SurfaceRegistry& surfaces,
                             const ReadyHandler& readyHandler = {});
};

} // namespace lcl::core
