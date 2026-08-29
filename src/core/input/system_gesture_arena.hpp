#pragma once

#include <chrono>
#include <cstdint>
#include <deque>

#include "core/input/input_manager.hpp"

namespace lcl::core {

enum class SystemGestureDecision {
    PassThrough,
    Tracking,
    Claim,
    Update,
    Consume,
    Cancel,
    Home
};

struct SystemGestureProgress {
    uint32_t pointerId{0};
    float startX{0.0f};
    float startY{0.0f};
    float x{0.0f};
    float y{0.0f};
    // Least-squares fling estimate in output-logical units per second.
    float velocityX{0.0f};
    float velocityY{0.0f};
};

struct SystemGestureConfig {
    float bottomEdgeInset{24.0f};
    float claimDistance{18.0f};
    float flingWindowSec{0.08f};
    float maxFlingVelocity{8000.0f};
};

/**
 * Arbitrates the single-pointer bottom-edge stream reserved by the mobile shell.
 * Touch and primary-button mouse drags share this recognizer. It recognizes
 * ownership only; rendering and shell policy remain outside it.
 */
class SystemGestureArena {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit SystemGestureArena(SystemGestureConfig config = {})
        : m_config(config) {}

    SystemGestureDecision process(const InputEvent& event, float outputHeight,
                                  SystemGestureProgress* progress = nullptr,
                                  TimePoint now = Clock::now());
    void reset() noexcept;
    bool isTracking(uint32_t pointerId) const noexcept;
    bool hasClaimed(uint32_t pointerId) const noexcept;

private:
    enum class State {
        Idle,
        Tracking,
        Claimed
    };

    struct VelocitySample {
        float x{0.0f};
        float y{0.0f};
        TimePoint time{};
    };

    void recordVelocitySample(float x, float y, TimePoint now);
    void resolveFlingVelocity(float& velocityX, float& velocityY) const;

    SystemGestureConfig m_config;
    State m_state{State::Idle};
    uint32_t m_pointerId{0};
    lcl::platform::PointerSource m_pointerSource{
        lcl::platform::PointerSource::Touch};
    float m_startX{0.0f};
    float m_startY{0.0f};
    std::deque<VelocitySample> m_velocitySamples;
};

} // namespace lcl::core
