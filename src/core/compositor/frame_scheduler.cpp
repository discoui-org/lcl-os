#include "core/compositor/frame_scheduler.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

namespace lcl::core {

namespace {

float easeOutCubic(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return 1.0f - std::pow(1.0f - value, 3.0f);
}

float easeInCubic(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * value;
}

} // namespace

void FrameScheduler::reset(std::chrono::steady_clock::time_point now) noexcept {
    m_lastBlinkCheck = now;
    m_lastTransitionTick = now;
}

bool FrameScheduler::cursorBlinkDue(std::chrono::steady_clock::time_point now) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastBlinkCheck);
    if (elapsed.count() < 500) {
        return false;
    }
    m_lastBlinkCheck = now;
    return true;
}

bool FrameScheduler::advanceTransitions(SurfaceRegistry& surfaces,
                                        std::chrono::steady_clock::time_point now) noexcept {
    float elapsed = std::chrono::duration<float>(now - m_lastTransitionTick).count();
    m_lastTransitionTick = now;
    elapsed = std::clamp(elapsed, 1.0f / 240.0f, 1.0f / 20.0f);

    bool active = false;
    for (auto& [_, entry] : surfaces) {
        if (entry.transitionPhase == SurfaceRegistry::SurfaceEntry::TransitionPhase::None) {
            continue;
        }

        active = true;
        entry.transitionElapsedSec += elapsed;
        const float duration = std::max(0.001f, entry.transitionDurationSec);
        const float progress = std::clamp(entry.transitionElapsedSec / duration, 0.0f, 1.0f);

        if (entry.transitionPhase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Entering) {
            const float eased = easeOutCubic(progress);
            entry.transitionOpacity = eased;
            entry.transitionScale = 0.96f + (0.04f * eased);
            if (progress >= 1.0f) {
                entry.transitionPhase = SurfaceRegistry::SurfaceEntry::TransitionPhase::None;
                entry.transitionOpacity = 1.0f;
                entry.transitionScale = 1.0f;
            }
        } else if (entry.transitionPhase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Closing) {
            const float eased = easeInCubic(progress);
            entry.transitionOpacity = 1.0f - eased;
            entry.transitionScale = 1.0f - (0.04f * eased);
            if (progress >= 1.0f) {
                entry.transitionOpacity = 0.0f;
                entry.transitionScale = 0.96f;
                entry.pendingDestroy = true;
            }
        }
    }
    return active;
}

std::chrono::microseconds FrameScheduler::frameBudgetForHz(uint32_t refreshHz) const noexcept {
    const float periodMs = 1000.0f / static_cast<float>(refreshHz == 0 ? 60 : refreshHz);
    const float headroomMs = std::min(0.5f, periodMs * 0.08f);
    return std::chrono::microseconds(static_cast<int64_t>((periodMs - headroomMs) * 1000.0f));
}

void FrameScheduler::waitForFrame(std::chrono::high_resolution_clock::time_point frameStart,
                                  std::chrono::microseconds budget) const {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - frameStart);
    if (elapsed < budget) {
        std::this_thread::sleep_for(budget - elapsed);
    }
}

void FrameScheduler::waitIdle() const {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

} // namespace lcl::core
