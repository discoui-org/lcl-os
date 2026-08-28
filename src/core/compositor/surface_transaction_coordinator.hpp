#pragma once

#include "core/compositor/surface_registry.hpp"

#include <cstdint>
#include <vector>

namespace lcl::core {

/** Promotes only complete same-generation WindowGroup layer transactions. */
class SurfaceTransactionCoordinator {
public:
    static bool begin(SurfaceRegistry& surfaces, uint32_t windowId,
                      uint64_t generation,
                      const std::vector<SurfaceRegistry::Key>& participants) noexcept;
    static void cancel(SurfaceRegistry& surfaces, uint32_t windowId,
                       uint64_t generation) noexcept;
    /** Clears ready barriers and reports whether any incomplete group remains. */
    static bool promoteReady(SurfaceRegistry& surfaces) noexcept;
};

} // namespace lcl::core
