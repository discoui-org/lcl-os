#pragma once

#include <chrono>
#include <cstdint>

#include "core/compositor/surface_registry.hpp"

namespace lcl::core {

/** Owns frame pacing plus compositor-owned transition and cursor timing. */
class FrameScheduler {
public:
    void reset(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;

    bool cursorBlinkDue(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;
    bool advanceTransitions(SurfaceRegistry& surfaces,
                            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;

    std::chrono::microseconds frameBudgetForHz(uint32_t refreshHz) const noexcept;
    void waitForFrame(std::chrono::high_resolution_clock::time_point frameStart,
                      std::chrono::microseconds budget) const;
    void waitIdle() const;

private:
    std::chrono::steady_clock::time_point m_lastBlinkCheck{};
    std::chrono::steady_clock::time_point m_lastTransitionTick{};
};

} // namespace lcl::core
