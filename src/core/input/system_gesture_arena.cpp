#include "core/input/system_gesture_arena.hpp"

#include <algorithm>
#include <cmath>

namespace lcl::core {

SystemGestureDecision SystemGestureArena::process(const InputEvent& event,
                                                  float outputHeight,
                                                  SystemGestureProgress* progress) {
    if (event.source != lcl::platform::PointerSource::Touch) {
        return SystemGestureDecision::PassThrough;
    }

    const bool down = event.type == InputEventType::PointerButton && event.pressed;
    const bool up = event.type == InputEventType::PointerButton && !event.pressed;
    if (m_state == State::Idle) {
        const float edgeTop = std::max(0.0f, outputHeight - m_config.bottomEdgeInset);
        if (!down || !std::isfinite(event.absoluteX) ||
            !std::isfinite(event.absoluteY) || event.absoluteY < edgeTop) {
            return SystemGestureDecision::PassThrough;
        }
        m_state = State::Tracking;
        m_pointerId = event.pointerId;
        m_startX = static_cast<float>(event.absoluteX);
        m_startY = static_cast<float>(event.absoluteY);
        return SystemGestureDecision::Tracking;
    }

    if (event.pointerId != m_pointerId) {
        return SystemGestureDecision::PassThrough;
    }
    if (progress) {
        progress->pointerId = m_pointerId;
        progress->startX = m_startX;
        progress->startY = m_startY;
        progress->x = static_cast<float>(event.absoluteX);
        progress->y = static_cast<float>(event.absoluteY);
    }
    if (event.type == InputEventType::PointerCancel) {
        const bool claimed = m_state == State::Claimed;
        reset();
        return claimed ? SystemGestureDecision::Cancel
                       : SystemGestureDecision::PassThrough;
    }
    if (event.type == InputEventType::PointerMotion) {
        if (m_state == State::Claimed) return SystemGestureDecision::Update;
        const float deltaX = static_cast<float>(event.absoluteX) - m_startX;
        const float deltaY = static_cast<float>(event.absoluteY) - m_startY;
        if (std::abs(deltaX) >= m_config.claimDistance &&
            std::abs(deltaX) > std::abs(deltaY)) {
            reset();
            return SystemGestureDecision::PassThrough;
        }
        if (-deltaY >= m_config.claimDistance &&
            -deltaY >= std::abs(deltaX)) {
            m_state = State::Claimed;
            return SystemGestureDecision::Claim;
        }
        return SystemGestureDecision::Tracking;
    }
    if (up) {
        const bool claimed = m_state == State::Claimed;
        reset();
        return claimed ? SystemGestureDecision::Home
                       : SystemGestureDecision::PassThrough;
    }
    return m_state == State::Claimed ? SystemGestureDecision::Consume
                                     : SystemGestureDecision::Tracking;
}

void SystemGestureArena::reset() noexcept {
    m_state = State::Idle;
    m_pointerId = 0;
    m_startX = 0.0f;
    m_startY = 0.0f;
}

bool SystemGestureArena::isTracking(uint32_t pointerId) const noexcept {
    return m_state != State::Idle && m_pointerId == pointerId;
}

bool SystemGestureArena::hasClaimed(uint32_t pointerId) const noexcept {
    return m_state == State::Claimed && m_pointerId == pointerId;
}

} // namespace lcl::core
