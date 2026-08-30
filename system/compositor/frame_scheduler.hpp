#pragma once

#include <chrono>
#include <cstdint>
#include <unordered_map>

#include "system/compositor/surface_registry.hpp"
#include "lcl-motion/motion.hpp"

namespace lcl::core {

/** Owns frame pacing plus compositor-owned transition and cursor timing. */
class FrameScheduler {
public:
    void reset(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;

    /** Set the physical display silhouette in compositor logical units. */
    void setDisplayCornerStyle(float radius, float roundness) noexcept;

    bool advanceTransitions(SurfaceRegistry& surfaces,
                            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    std::chrono::microseconds frameBudgetForHz(uint32_t refreshHz) const noexcept;
    void waitForFrame(std::chrono::high_resolution_clock::time_point frameStart,
                      std::chrono::microseconds budget) const;
    void waitIdle() const;

private:
    struct LaunchMorphState {
        lcl::motion::ChannelId positionX{0};
        lcl::motion::ChannelId positionY{0};
        lcl::motion::ChannelId expand{0};
        lcl::motion::ChannelId perspective{0};
        lcl::motion::ChannelId homeEffect{0};
        SurfaceRegistry::SurfaceEntry::TransitionPhase phase{
            SurfaceRegistry::SurfaceEntry::TransitionPhase::None};
        float targetX{0.0f};
        float targetY{0.0f};
        float targetWidth{1.0f};
        float targetHeight{1.0f};
    };

    void prepareLaunchMorph(SurfaceRegistry::Key key,
                            SurfaceRegistry::SurfaceEntry& entry);
    bool updateLaunchMorph(SurfaceRegistry::Key key,
                           SurfaceRegistry::SurfaceEntry& entry);

    std::chrono::steady_clock::time_point m_lastTransitionTick{};
    float m_displayCornerRadius{0.0f};
    float m_displayCornerRoundness{2.0f};
    lcl::motion::AnimationEngine m_launchMotion;
    std::unordered_map<SurfaceRegistry::Key, LaunchMorphState> m_launchMorphs;
};

} // namespace lcl::core
