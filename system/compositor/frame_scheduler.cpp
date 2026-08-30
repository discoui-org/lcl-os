#include "system/compositor/frame_scheduler.hpp"

#include <algorithm>
#include <cmath>
#include <thread>
#include "lcl-motion/motion.hpp"
#include "lcl-theme/theme.hpp"

namespace lcl::core {

namespace {

using TransitionPhase = SurfaceRegistry::SurfaceEntry::TransitionPhase;

// Time-compress mobile launch morph springs without changing their damping
// ratio. For x(speed * t), stiffness scales by speed^2 and damping/velocity by
// speed.
constexpr float kLaunchSpringSpeed = 1.5f;
constexpr float kLaunchSpringStiffnessScale =
    kLaunchSpringSpeed * kLaunchSpringSpeed;
constexpr float kSpringyMinimizeDampingRatio = 0.72f;
constexpr float kSlowFlingScreenLengthsPerSecond = 0.5f;
constexpr float kFastFlingScreenLengthsPerSecond = 4.0f;
constexpr float kInteractiveHomeEffectProgress = 0.25f;
constexpr float kHomeEffectOpeningStiffness = 140.0f;
constexpr float kHomeEffectClosingStiffness = 45.0f;

bool isLaunchMorphPhase(TransitionPhase phase) {
    return phase == TransitionPhase::Entering ||
           phase == TransitionPhase::Closing ||
           phase == TransitionPhase::Minimizing ||
           phase == TransitionPhase::Restoring ||
           phase == TransitionPhase::Interactive;
}

lcl::motion::Motion launchSpring(float stiffness,
                                 float dampingRatio,
                                 float settlePosition,
                                 float settleVelocity) {
    const float scaledStiffness = stiffness * kLaunchSpringStiffnessScale;
    auto motion = lcl::motion::Motion::spring(
        1.0f, scaledStiffness,
        2.0f * dampingRatio * std::sqrt(scaledStiffness));
    motion.springParams.settlePosEpsilon = settlePosition;
    motion.springParams.settleVelEpsilon =
        settleVelocity * kLaunchSpringSpeed;
    return motion;
}

float minimizeDampingRatio(float velocityY, float targetHeight) {
    const float screenLengthsPerSecond =
        std::abs(velocityY) / std::max(1.0f, targetHeight);
    float progress = std::clamp(
        (screenLengthsPerSecond - kSlowFlingScreenLengthsPerSecond) /
            (kFastFlingScreenLengthsPerSecond -
             kSlowFlingScreenLengthsPerSecond),
        0.0f, 1.0f);
    progress = progress * progress * (3.0f - 2.0f * progress);
    return 1.0f +
        (kSpringyMinimizeDampingRatio - 1.0f) * progress;
}

struct LaunchFlingVelocity {
    float positionX{0.0f};
    float positionY{0.0f};
    float expand{0.0f};
    float perspective{0.0f};
};

LaunchFlingVelocity resolveLaunchFlingVelocity(
        const SurfaceRegistry::SurfaceEntry& entry,
        float targetX, float targetWidth, float targetHeight) {
    const float velocityX =
        entry.launchGestureVelocityX * kLaunchSpringSpeed;
    const float velocityY =
        entry.launchGestureVelocityY * kLaunchSpringSpeed;
    const float deltaY = std::max(
        0.0f, entry.launchGestureStartY - entry.launchGestureY);
    const float normalizedStartX = std::clamp(
        (entry.launchGestureStartX - targetX) / targetWidth, 0.0f, 1.0f);
    const float horizontalCoupling =
        (normalizedStartX - 0.5f) * targetWidth * 0.00125f;
    // Continue along the same non-linear path used while the finger is down.
    // This turns finger px/s into window-center px/s without letting a fast
    // fling shoot the morph past the screen's upper-third region.
    const float verticalDenominator = 2.0f +
        std::pow(deltaY, 1.5f) / (10.0f * targetHeight);
    const float verticalDerivative = deltaY > 0.0f
        ? 0.15f * std::sqrt(deltaY) /
            (verticalDenominator * verticalDenominator)
        : 0.0f;
    return {
        velocityX - velocityY * horizontalCoupling,
        velocityY * verticalDerivative,
        velocityY / targetHeight,
        velocityY / (targetHeight * (2.0f / 3.0f)),
    };
}

constexpr float kLaunchMorphMaxDurationSec = 1.5f / kLaunchSpringSpeed;

SurfaceRegistry::Key launchMotionKey(
        SurfaceRegistry::Key surfaceKey,
        const SurfaceRegistry::SurfaceEntry& entry) {
    constexpr uint64_t kLaunchMotionKeyBit = uint64_t{1} << 63;
    return entry.launchToken != 0
        ? kLaunchMotionKeyBit | entry.launchToken
        : surfaceKey;
}

} // namespace

void FrameScheduler::reset(std::chrono::steady_clock::time_point now) noexcept {
    m_lastTransitionTick = now;
}

void FrameScheduler::setDisplayCornerStyle(float radius,
                                           float roundness) noexcept {
    m_displayCornerRadius = std::max(0.0f, radius);
    m_displayCornerRoundness = std::clamp(roundness, 2.0f, 8.0f);
}

void FrameScheduler::prepareLaunchMorph(
        SurfaceRegistry::Key key, SurfaceRegistry::SurfaceEntry& entry) {
    if (!entry.hasLaunchOrigin || !isLaunchMorphPhase(entry.transitionPhase)) {
        return;
    }

    const float targetX = entry.configuredWidth > 0.0f
        ? entry.configuredX : entry.initialX;
    const float targetY = entry.configuredHeight > 0.0f
        ? entry.configuredY : entry.initialY;
    const float targetWidth = std::max(1.0f, entry.configuredWidth > 0.0f
        ? entry.configuredWidth : entry.initialWidth);
    const float targetHeight = std::max(1.0f, entry.configuredHeight > 0.0f
        ? entry.configuredHeight : entry.initialHeight);
    const float originCenterX = entry.launchOriginX + entry.launchOriginWidth * 0.5f;
    const float originCenterY = entry.launchOriginY + entry.launchOriginHeight * 0.5f;
    const float targetCenterX = targetX + targetWidth * 0.5f;
    const float targetCenterY = targetY + targetHeight * 0.5f;
    const bool opening = entry.transitionPhase == TransitionPhase::Entering ||
                         entry.transitionPhase == TransitionPhase::Restoring;
    const bool interactive =
        entry.transitionPhase == TransitionPhase::Interactive;

    key = launchMotionKey(key, entry);
    auto [found, inserted] = m_launchMorphs.try_emplace(key);
    auto& state = found->second;
    state.targetX = targetX;
    state.targetY = targetY;
    state.targetWidth = targetWidth;
    state.targetHeight = targetHeight;
    if (inserted) {
        const float initialX = opening ? originCenterX : targetCenterX;
        const float initialY = opening ? originCenterY : targetCenterY;
        const float initialProgress = opening ? 0.0f : 1.0f;
        state.positionX = m_launchMotion.createChannel({key, 1}, initialX);
        state.positionY = m_launchMotion.createChannel({key, 2}, initialY);
        state.expand = m_launchMotion.createChannel({key, 3}, initialProgress);
        state.perspective = m_launchMotion.createChannel({key, 4}, initialProgress);
        state.homeEffect = m_launchMotion.createChannel({key, 5}, initialProgress);
    }
    const bool phaseChanged = !inserted && state.phase != entry.transitionPhase;
    if (phaseChanged) {
        entry.transitionElapsedSec = 0.0f;
    }
    const bool flingToHome = entry.launchGestureFlingPending &&
        entry.transitionPhase == TransitionPhase::Minimizing;
    if (!inserted && state.phase == entry.transitionPhase && !interactive) return;

    constexpr float kDraggingStiffness =
        320.0f * kLaunchSpringStiffnessScale;
    const auto draggingMotion = lcl::motion::Motion::spring(
        1.0f, kDraggingStiffness,
        1.25f * std::sqrt(kDraggingStiffness));
    const float dampingRatio = flingToHome
        ? minimizeDampingRatio(
              entry.launchGestureVelocityY, targetHeight)
        : 1.0f;
    const auto positionMotion = interactive
        ? draggingMotion
        : launchSpring(opening ? 150.0f : 120.0f,
                       dampingRatio, 0.05f, 0.05f);
    const auto expandMotion = launchSpring(
        opening ? 200.0f : 70.0f,
        dampingRatio, 0.001f, 0.01f);
    const auto perspectiveMotion = interactive
        ? draggingMotion
        : launchSpring(100.0f, dampingRatio, 0.001f, 0.01f);
    // Launcher scale and brightness have their own critically damped spring. Fling
    // velocity remains owned by the app thumbnail trajectory.
    const auto homeEffectMotion = interactive
        ? draggingMotion
        : launchSpring(
              opening
                  ? kHomeEffectOpeningStiffness
                  : kHomeEffectClosingStiffness,
              1.0f, 0.001f, 0.01f);
    float positionTargetX = opening ? targetCenterX : originCenterX;
    float positionTargetY = opening ? targetCenterY : originCenterY;
    float expandTarget = opening ? 1.0f : 0.0f;
    float perspectiveTarget = opening ? 1.0f : 0.0f;
    float homeEffectTarget = opening ? 1.0f : 0.0f;
    if (interactive) {
        const float deltaX = entry.launchGestureX - entry.launchGestureStartX;
        const float deltaY = entry.launchGestureStartY - entry.launchGestureY;
        const float normalizedStartX = std::clamp(
            (entry.launchGestureStartX - targetX) / targetWidth, 0.0f, 1.0f);
        positionTargetX = targetCenterX + deltaX +
            deltaY * (normalizedStartX - 0.5f) * targetWidth * 0.00125f;
        positionTargetY = targetY + targetHeight /
            (2.0f + std::pow(std::abs(deltaY), 1.5f) /
                        (10.0f * targetHeight));
        expandTarget = 1.0f;
        perspectiveTarget = 1.0f - deltaY / (targetHeight * 2.0f / 3.0f);
        const float thumbnailProgress =
            std::clamp(perspectiveTarget, 0.0f, 1.0f);
        homeEffectTarget = 1.0f -
            (1.0f - thumbnailProgress) *
                kInteractiveHomeEffectProgress;
    }
    m_launchMotion.setSpec(state.positionX, positionMotion);
    m_launchMotion.setSpec(state.positionY, positionMotion);
    m_launchMotion.setSpec(state.expand, expandMotion);
    m_launchMotion.setSpec(state.perspective, perspectiveMotion);
    m_launchMotion.setSpec(
        state.homeEffect, homeEffectMotion,
        !(phaseChanged && !interactive));
    m_launchMotion.animateTo(state.positionX, positionTargetX, positionMotion);
    m_launchMotion.animateTo(state.positionY, positionTargetY, positionMotion);
    m_launchMotion.animateTo(state.expand, expandTarget, expandMotion);
    m_launchMotion.animateTo(
        state.perspective, perspectiveTarget, perspectiveMotion);
    m_launchMotion.animateTo(
        state.homeEffect, homeEffectTarget, homeEffectMotion);
    if (flingToHome) {
        const auto fling = resolveLaunchFlingVelocity(
            entry, targetX, targetWidth, targetHeight);
        m_launchMotion.setVelocity(state.positionX, fling.positionX);
        m_launchMotion.setVelocity(state.positionY, fling.positionY);
        m_launchMotion.setVelocity(state.expand, fling.expand);
        m_launchMotion.setVelocity(state.perspective, fling.perspective);
        entry.launchGestureFlingPending = false;
    }
    state.phase = entry.transitionPhase;
    entry.launchMorphActive = true;
}

bool FrameScheduler::updateLaunchMorph(
        SurfaceRegistry::Key key, SurfaceRegistry::SurfaceEntry& entry) {
    key = launchMotionKey(key, entry);
    const auto found = m_launchMorphs.find(key);
    if (found == m_launchMorphs.end() ||
        found->second.phase != entry.transitionPhase) {
        return false;
    }
    const auto& state = found->second;
    const bool timedOut =
        entry.transitionPhase != TransitionPhase::Interactive &&
        entry.transitionElapsedSec >= kLaunchMorphMaxDurationSec;
    if (timedOut) {
        const bool opening =
            entry.transitionPhase == TransitionPhase::Entering ||
            entry.transitionPhase == TransitionPhase::Restoring;
        const float targetCenterX = state.targetX + state.targetWidth * 0.5f;
        const float targetCenterY = state.targetY + state.targetHeight * 0.5f;
        const float originCenterX =
            entry.launchOriginX + entry.launchOriginWidth * 0.5f;
        const float originCenterY =
            entry.launchOriginY + entry.launchOriginHeight * 0.5f;
        m_launchMotion.setValue(
            state.positionX, opening ? targetCenterX : originCenterX);
        m_launchMotion.setValue(
            state.positionY, opening ? targetCenterY : originCenterY);
        m_launchMotion.setValue(state.expand, opening ? 1.0f : 0.0f);
        m_launchMotion.setValue(state.perspective, opening ? 1.0f : 0.0f);
        m_launchMotion.setValue(state.homeEffect, opening ? 1.0f : 0.0f);
    }
    const auto positionX = m_launchMotion.sample(state.positionX);
    const auto positionY = m_launchMotion.sample(state.positionY);
    const auto expandSample = m_launchMotion.sample(state.expand);
    const auto perspectiveSample = m_launchMotion.sample(state.perspective);
    const auto homeEffectSample = m_launchMotion.sample(state.homeEffect);
    const float expand = std::max(0.0f, expandSample.value);
    const float perspective = perspectiveSample.value;
    entry.launchHomeTransitionProgress =
        std::clamp(homeEffectSample.value, 0.0f, 1.0f);
    const float iconWidth = std::max(1.0f, entry.launchOriginWidth);
    const float iconHeight = std::max(1.0f, entry.launchOriginHeight);
    const float screenWidth = state.targetWidth;
    const float screenHeight = state.targetHeight;
    const float iconRatio = iconWidth / iconHeight;
    const float screenRatio = screenWidth / screenHeight;
    const float targetRatio = iconRatio + (screenRatio - iconRatio) * expand;

    float rawWidth = iconWidth;
    float rawHeight = iconHeight;
    if (screenRatio > iconRatio) {
        rawWidth = iconWidth * (targetRatio / iconRatio);
    } else {
        rawHeight = iconHeight * (iconRatio / std::max(0.0001f, targetRatio));
    }
    const float rawPerspective = screenRatio < iconRatio
        ? 1.0f + ((screenWidth - iconWidth) / iconWidth) * perspective
        : 1.0f + ((screenHeight - iconHeight) / iconHeight) * perspective;
    const float perspectiveScale = std::max(rawPerspective, 0.05f);
    const float presentedWidth = std::max(1.0f, rawWidth * perspectiveScale);
    const float presentedHeight = std::max(1.0f, rawHeight * perspectiveScale);

    // Preserve the aspect-aware interpolation while converging on the actual
    // physical display silhouette supplied by Gestalt. Scale the display
    // radius with the thumbnail itself so shrinking the window does not make
    // its corners progressively rounder.
    const float shapeProgress =
        std::clamp(std::pow(expand, 1.5f), 0.0f, 1.0f);
    const float displaySilhouetteScale = std::clamp(
        std::min(presentedWidth / screenWidth,
                 presentedHeight / screenHeight),
        0.0f, 1.0f);
    const float scaledDisplayRadius =
        m_displayCornerRadius * displaySilhouetteScale;
    const float radiusScale = perspectiveScale *
        (screenRatio < iconRatio
            ? rawHeight / iconHeight
            : rawWidth / iconWidth);
    const float scaledIconRadius =
        entry.launchOriginCornerRadius * radiusScale;
    const float cornerRadius = scaledIconRadius +
        (scaledDisplayRadius - scaledIconRadius) * shapeProgress;

    entry.launchMorphX = positionX.value - presentedWidth * 0.5f;
    entry.launchMorphY = positionY.value - presentedHeight * 0.5f;
    entry.launchMorphWidth = presentedWidth;
    entry.launchMorphHeight = presentedHeight;
    entry.launchMorphCornerRadius = std::max(0.0f, cornerRadius);
    entry.launchMorphCornerRoundness =
        lcl::theme::mobile::kAppIconCornerRoundness +
        (m_displayCornerRoundness -
         lcl::theme::mobile::kAppIconCornerRoundness) * shapeProgress;
    entry.transitionOpacity = std::clamp(expand * 2.0f - 0.25f, 0.0f, 1.0f);
    entry.transitionScale = 1.0f;
    entry.launchMorphActive = true;

    const bool moving = positionX.active || positionY.active ||
                        expandSample.active || perspectiveSample.active ||
                        homeEffectSample.active;
    if (entry.transitionPhase == TransitionPhase::Interactive) {
        return moving;
    }
    if (moving) return true;

    const auto phase = entry.transitionPhase;
    entry.transitionOpacity = phase == TransitionPhase::Entering ||
                              phase == TransitionPhase::Restoring
        ? 1.0f : 0.0f;
    if (phase == TransitionPhase::Entering || phase == TransitionPhase::Restoring) {
        entry.launchMorphCornerRadius = m_displayCornerRadius;
        entry.launchMorphCornerRoundness = m_displayCornerRoundness;
        entry.launchMorphActive = false;
        entry.launchIconHandoffActive = false;
        entry.launchIconHandoffDeadline = {};
        entry.transitionPhase = TransitionPhase::None;
    } else if (phase == TransitionPhase::Minimizing) {
        const bool handoff = entry.launchToken != 0 && entry.launchOwnerFd >= 0;
        entry.launchMorphActive = handoff;
        entry.launchIconHandoffActive = handoff;
        entry.launchIconHandoffDeadline = {};
        entry.pendingMinimize = true;
        entry.launchIconRevealPending = handoff;
        entry.transitionPhase = TransitionPhase::None;
    } else if (phase == TransitionPhase::Closing) {
        const bool handoff = entry.launchToken != 0 && entry.launchOwnerFd >= 0;
        entry.launchMorphActive = handoff;
        entry.launchIconHandoffActive = handoff;
        entry.launchIconHandoffDeadline = {};
        entry.launchIconRevealPending = handoff;
        entry.pendingDestroy = true;
        entry.transitionPhase = TransitionPhase::None;
    }
    return false;
}

bool FrameScheduler::advanceTransitions(SurfaceRegistry& surfaces,
                                        std::chrono::steady_clock::time_point now) {
    float elapsed = std::chrono::duration<float>(now - m_lastTransitionTick).count();
    m_lastTransitionTick = now;
    elapsed = std::clamp(elapsed, 1.0f / 240.0f, 1.0f / 20.0f);

    bool active = false;
    for (auto& [key, entry] : surfaces) {
        prepareLaunchMorph(key, entry);
        if (entry.launchContentFadeActive) {
            active = true;
            entry.launchContentFadeElapsedSec += elapsed;
            constexpr float kLaunchContentFadeDurationSec = 0.16f;
            const float progress = std::clamp(
                entry.launchContentFadeElapsedSec /
                    kLaunchContentFadeDurationSec,
                0.0f, 1.0f);
            entry.launchContentOpacity = lcl::motion::Easing(
                lcl::motion::EasingName::EaseOutCubic).evaluate(progress);
            if (progress >= 1.0f) {
                entry.launchContentFadeActive = false;
                entry.launchPlaceholderActive = false;
                entry.launchContentOpacity = 1.0f;
            }
        }
        if (entry.transitionPhase == SurfaceRegistry::SurfaceEntry::TransitionPhase::None) {
            continue;
        }

        if (entry.hasLaunchOrigin && isLaunchMorphPhase(entry.transitionPhase)) {
            if (entry.transitionPhase !=
                SurfaceRegistry::SurfaceEntry::TransitionPhase::Interactive) {
                entry.transitionElapsedSec += elapsed;
            }
            continue;
        }

        active = true;
        entry.transitionElapsedSec += elapsed;
        const float duration = std::max(0.001f, entry.transitionDurationSec);
        const float progress = std::clamp(entry.transitionElapsedSec / duration, 0.0f, 1.0f);

        if (entry.transitionPhase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Entering) {
            const float eased = lcl::motion::tokens::windowOpen().tweenParams.easing.evaluate(progress);
            entry.transitionOpacity = eased;
            entry.transitionScale = 0.96f + (0.04f * eased);
            if (progress >= 1.0f) {
                entry.transitionPhase = SurfaceRegistry::SurfaceEntry::TransitionPhase::None;
                entry.transitionOpacity = 1.0f;
                entry.transitionScale = 1.0f;
            }
        } else if (entry.transitionPhase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Closing) {
            const float eased = lcl::motion::tokens::windowClose().tweenParams.easing.evaluate(progress);
            entry.transitionOpacity = 1.0f - eased;
            entry.transitionScale = 1.0f - (0.04f * eased);
            if (progress >= 1.0f) {
                entry.transitionOpacity = 0.0f;
                entry.transitionScale = 0.96f;
                entry.launchIconRevealPending = entry.launchToken != 0;
                entry.pendingDestroy = true;
            }
        } else if (entry.transitionPhase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Minimizing) {
            const float eased = lcl::motion::tokens::minimize().tweenParams.easing.evaluate(progress);
            entry.transitionOpacity = 1.0f - eased;
            entry.transitionScale = 1.0f - 0.08f * eased;
            if (progress >= 1.0f) {
                entry.launchIconRevealPending = entry.launchToken != 0;
                entry.pendingMinimize = true;
                entry.transitionPhase = SurfaceRegistry::SurfaceEntry::TransitionPhase::None;
            }
        } else if (entry.transitionPhase == SurfaceRegistry::SurfaceEntry::TransitionPhase::Restoring) {
            const float eased = lcl::motion::tokens::restore().tweenParams.easing.evaluate(progress);
            entry.transitionOpacity = eased;
            entry.transitionScale = 0.92f + 0.08f * eased;
            if (progress >= 1.0f) {
                entry.transitionPhase = SurfaceRegistry::SurfaceEntry::TransitionPhase::None;
                entry.transitionOpacity = 1.0f;
                entry.transitionScale = 1.0f;
            }
        }
    }

    m_launchMotion.tick(elapsed);
    for (auto& [key, entry] : surfaces) {
        if (entry.hasLaunchOrigin && isLaunchMorphPhase(entry.transitionPhase)) {
            active = updateLaunchMorph(key, entry) || active;
        }
    }

    for (auto it = m_launchMorphs.begin(); it != m_launchMorphs.end();) {
        const bool stillOwned = std::any_of(
            surfaces.begin(), surfaces.end(), [&](const auto& item) {
                return launchMotionKey(item.first, item.second) == it->first;
            });
        if (stillOwned) {
            ++it;
            continue;
        }
        m_launchMotion.clearObjectChannels(it->first);
        it = m_launchMorphs.erase(it);
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
