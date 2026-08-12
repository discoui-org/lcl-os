#pragma once

#include <algorithm>
#include <limits>

// Compatibility facade for the original low-level lcl-ui channel API. New
// code should use lcl-motion directly; existing callers keep omega/zeta specs.
#include "lcl-motion/motion.hpp"

namespace lcl::ui {

using EasingName = lcl::motion::EasingName;
using Easing = lcl::motion::Easing;
using StepPosition = lcl::motion::StepPosition;
using MotionMode = lcl::motion::MotionMode;
using DampingMode = lcl::motion::DampingMode;
using InterruptBehavior = lcl::motion::InterruptBehavior;
using Motion = lcl::motion::Motion;
using ChannelKey = lcl::motion::ChannelKey;
using ChannelKeyHasher = lcl::motion::ChannelKeyHasher;
using ChannelId = lcl::motion::ChannelId;
using AnimatedSample = lcl::motion::AnimatedSample;
using Keyframe = lcl::motion::Keyframe;
using AnimationOptions = lcl::motion::AnimationOptions;
using AnimationHandle = lcl::motion::AnimationHandle;
using Timeline = lcl::motion::Timeline;
using PlaybackDirection = lcl::motion::PlaybackDirection;
using FillMode = lcl::motion::FillMode;
using PlayState = lcl::motion::PlayState;

struct SpringParams {
    float mass{1.0f};
    float omega{18.0f};
    float zeta{1.0f};
    float maxVelocity{std::numeric_limits<float>::infinity()};
    float settlePosEpsilon{0.001f};
    float settleVelEpsilon{0.001f};
};

struct TweenParams {
    float durationSec{0.18f};
    float delaySec{0.0f};
    EasingName easing{EasingName::EaseOutCubic};
};

struct MotionSpec {
    MotionMode mode{MotionMode::Spring};
    SpringParams spring{};
    TweenParams tween{};
    InterruptBehavior interruptBehavior{InterruptBehavior::PreserveVelocityAndRetarget};

    lcl::motion::Motion toMotion() const {
        lcl::motion::Motion result;
        result.mode = mode;
        result.interruptBehavior = interruptBehavior;
        result.springParams.mass = std::max(0.0001f, spring.mass);
        result.springParams.stiffness = result.springParams.mass * spring.omega * spring.omega;
        result.springParams.damping = 2.0f * result.springParams.mass * spring.zeta * spring.omega;
        result.springParams.maxVelocity = spring.maxVelocity;
        result.springParams.settlePosEpsilon = spring.settlePosEpsilon;
        result.springParams.settleVelEpsilon = spring.settleVelEpsilon;
        result.tweenParams.durationSec = tween.durationSec;
        result.tweenParams.delaySec = tween.delaySec;
        result.tweenParams.easing = Easing(tween.easing);
        return result;
    }
};

class AnimationEngine : public lcl::motion::AnimationEngine {
public:
    using lcl::motion::AnimationEngine::animateTo;
    using lcl::motion::AnimationEngine::setSpec;

    bool animateTo(ChannelId channel, float target, const MotionSpec& spec) {
        return lcl::motion::AnimationEngine::animateTo(channel, target, spec.toMotion());
    }
    bool setSpec(ChannelId channel, const MotionSpec& spec, bool keepVelocity = true) {
        return lcl::motion::AnimationEngine::setSpec(channel, spec.toMotion(), keepVelocity);
    }
};

inline float evaluateEasing(EasingName easing, float progress) {
    return lcl::motion::evaluateEasing(easing, progress);
}

inline SpringParams springFromSettling(float settleSec, float zeta = 1.0f,
                                       float mass = 1.0f) {
    SpringParams result;
    result.mass = std::max(0.0001f, mass);
    result.zeta = std::max(0.01f, zeta);
    result.omega = 6.0f / (result.zeta * std::max(0.01f, settleSec));
    return result;
}

inline float estimateSettlingTime(const SpringParams& spring) {
    return 6.0f / std::max(0.0001f, spring.zeta * spring.omega);
}

} // namespace lcl::ui
