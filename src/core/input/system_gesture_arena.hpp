#pragma once

#include <cstdint>

#include "core/input/input_manager.hpp"

namespace lcl::core {

enum class SystemGestureDecision {
    PassThrough,
    Tracking,
    Claim,
    Consume,
    Home
};

struct SystemGestureConfig {
    float bottomEdgeInset{24.0f};
    float claimDistance{18.0f};
};

/**
 * Arbitrates the single-pointer bottom-edge stream reserved by the mobile shell.
 * It recognizes ownership only; rendering and shell policy remain outside it.
 */
class SystemGestureArena {
public:
    explicit SystemGestureArena(SystemGestureConfig config = {})
        : m_config(config) {}

    SystemGestureDecision process(const InputEvent& event, float outputHeight);
    void reset() noexcept;
    bool isTracking(uint32_t pointerId) const noexcept;
    bool hasClaimed(uint32_t pointerId) const noexcept;

private:
    enum class State {
        Idle,
        Tracking,
        Claimed
    };

    SystemGestureConfig m_config;
    State m_state{State::Idle};
    uint32_t m_pointerId{0};
    float m_startX{0.0f};
    float m_startY{0.0f};
};

} // namespace lcl::core
