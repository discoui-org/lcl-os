#include "core/input/system_gesture_arena.hpp"

#include <algorithm>
#include <cmath>

namespace lcl::core {

SystemGestureDecision SystemGestureArena::process(const InputEvent& event,
                                                  float outputHeight,
                                                  SystemGestureProgress* progress,
                                                  TimePoint now) {
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
        recordVelocitySample(m_startX, m_startY, now);
        return SystemGestureDecision::Tracking;
    }

    if (event.pointerId != m_pointerId) {
        return SystemGestureDecision::PassThrough;
    }
    const bool motion = event.type == InputEventType::PointerMotion;
    if ((motion || up) && std::isfinite(event.absoluteX) &&
        std::isfinite(event.absoluteY)) {
        recordVelocitySample(
            static_cast<float>(event.absoluteX),
            static_cast<float>(event.absoluteY), now);
    }
    if (progress) {
        progress->pointerId = m_pointerId;
        progress->startX = m_startX;
        progress->startY = m_startY;
        progress->x = static_cast<float>(event.absoluteX);
        progress->y = static_cast<float>(event.absoluteY);
        resolveFlingVelocity(progress->velocityX, progress->velocityY);
    }
    if (event.type == InputEventType::PointerCancel) {
        const bool claimed = m_state == State::Claimed;
        reset();
        return claimed ? SystemGestureDecision::Cancel
                       : SystemGestureDecision::PassThrough;
    }
    if (motion) {
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
    m_velocitySamples.clear();
}

void SystemGestureArena::recordVelocitySample(float x, float y,
                                              TimePoint now) {
    const auto window = std::chrono::duration<float>(
        std::max(0.01f, m_config.flingWindowSec));
    while (!m_velocitySamples.empty() &&
           now - m_velocitySamples.front().time > window) {
        m_velocitySamples.pop_front();
    }
    m_velocitySamples.push_back({x, y, now});
    constexpr size_t kMaximumVelocitySamples = 16;
    while (m_velocitySamples.size() > kMaximumVelocitySamples) {
        m_velocitySamples.pop_front();
    }
}

void SystemGestureArena::resolveFlingVelocity(float& velocityX,
                                              float& velocityY) const {
    velocityX = 0.0f;
    velocityY = 0.0f;
    if (m_velocitySamples.size() < 2) return;

    // Least-squares slope across the recent pointer history is stable against
    // one noisy final sample while still dropping to zero after a release
    // pause longer than the fling window.
    const auto origin = m_velocitySamples.back().time;
    double meanTime = 0.0;
    double meanX = 0.0;
    double meanY = 0.0;
    for (const auto& sample : m_velocitySamples) {
        meanTime += std::chrono::duration<double>(sample.time - origin).count();
        meanX += sample.x;
        meanY += sample.y;
    }
    const double count = static_cast<double>(m_velocitySamples.size());
    meanTime /= count;
    meanX /= count;
    meanY /= count;

    double timeVariance = 0.0;
    double covarianceX = 0.0;
    double covarianceY = 0.0;
    for (const auto& sample : m_velocitySamples) {
        const double time =
            std::chrono::duration<double>(sample.time - origin).count();
        const double centeredTime = time - meanTime;
        timeVariance += centeredTime * centeredTime;
        covarianceX += centeredTime * (sample.x - meanX);
        covarianceY += centeredTime * (sample.y - meanY);
    }
    if (timeVariance <= 1e-8) return;

    const float limit = std::max(0.0f, m_config.maxFlingVelocity);
    velocityX = std::clamp(
        static_cast<float>(covarianceX / timeVariance), -limit, limit);
    velocityY = std::clamp(
        static_cast<float>(covarianceY / timeVariance), -limit, limit);
}

bool SystemGestureArena::isTracking(uint32_t pointerId) const noexcept {
    return m_state != State::Idle && m_pointerId == pointerId;
}

bool SystemGestureArena::hasClaimed(uint32_t pointerId) const noexcept {
    return m_state == State::Claimed && m_pointerId == pointerId;
}

} // namespace lcl::core
