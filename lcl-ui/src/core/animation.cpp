#include "lcl-ui/core/animation.hpp"

#include <algorithm>
#include <cmath>

namespace lcl::ui {

namespace {

constexpr float kPi = 3.14159265358979323846f;

float clamp01(float x) {
    return std::clamp(x, 0.0f, 1.0f);
}

float easeOutBounceImpl(float x) {
    constexpr float n1 = 7.5625f;
    constexpr float d1 = 2.75f;

    if (x < 1.0f / d1) {
        return n1 * x * x;
    }
    if (x < 2.0f / d1) {
        x -= 1.5f / d1;
        return n1 * x * x + 0.75f;
    }
    if (x < 2.5f / d1) {
        x -= 2.25f / d1;
        return n1 * x * x + 0.9375f;
    }
    x -= 2.625f / d1;
    return n1 * x * x + 0.984375f;
}

float safeSqrt(float x) {
    return std::sqrt(std::max(0.0f, x));
}

} // namespace

float evaluateEasing(EasingName easing, float x) {
    x = clamp01(x);

    switch (easing) {
        case EasingName::Linear:
            return x;

        case EasingName::EaseInSine:
            return 1.0f - std::cos((x * kPi) / 2.0f);
        case EasingName::EaseOutSine:
            return std::sin((x * kPi) / 2.0f);
        case EasingName::EaseInOutSine:
            return -(std::cos(kPi * x) - 1.0f) / 2.0f;

        case EasingName::EaseInQuad:
            return x * x;
        case EasingName::EaseOutQuad:
            return 1.0f - (1.0f - x) * (1.0f - x);
        case EasingName::EaseInOutQuad:
            return x < 0.5f
                ? 2.0f * x * x
                : 1.0f - std::pow(-2.0f * x + 2.0f, 2.0f) / 2.0f;

        case EasingName::EaseInCubic:
            return x * x * x;
        case EasingName::EaseOutCubic:
            return 1.0f - std::pow(1.0f - x, 3.0f);
        case EasingName::EaseInOutCubic:
            return x < 0.5f
                ? 4.0f * x * x * x
                : 1.0f - std::pow(-2.0f * x + 2.0f, 3.0f) / 2.0f;

        case EasingName::EaseInQuart:
            return x * x * x * x;
        case EasingName::EaseOutQuart:
            return 1.0f - std::pow(1.0f - x, 4.0f);
        case EasingName::EaseInOutQuart:
            return x < 0.5f
                ? 8.0f * x * x * x * x
                : 1.0f - std::pow(-2.0f * x + 2.0f, 4.0f) / 2.0f;

        case EasingName::EaseInQuint:
            return x * x * x * x * x;
        case EasingName::EaseOutQuint:
            return 1.0f - std::pow(1.0f - x, 5.0f);
        case EasingName::EaseInOutQuint:
            return x < 0.5f
                ? 16.0f * x * x * x * x * x
                : 1.0f - std::pow(-2.0f * x + 2.0f, 5.0f) / 2.0f;

        case EasingName::EaseInExpo:
            return x == 0.0f ? 0.0f : std::pow(2.0f, 10.0f * x - 10.0f);
        case EasingName::EaseOutExpo:
            return x == 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * x);
        case EasingName::EaseInOutExpo:
            if (x == 0.0f) return 0.0f;
            if (x == 1.0f) return 1.0f;
            return x < 0.5f
                ? std::pow(2.0f, 20.0f * x - 10.0f) / 2.0f
                : (2.0f - std::pow(2.0f, -20.0f * x + 10.0f)) / 2.0f;

        case EasingName::EaseInCirc:
            return 1.0f - safeSqrt(1.0f - std::pow(x, 2.0f));
        case EasingName::EaseOutCirc:
            return safeSqrt(1.0f - std::pow(x - 1.0f, 2.0f));
        case EasingName::EaseInOutCirc:
            return x < 0.5f
                ? (1.0f - safeSqrt(1.0f - std::pow(2.0f * x, 2.0f))) / 2.0f
                : (safeSqrt(1.0f - std::pow(-2.0f * x + 2.0f, 2.0f)) + 1.0f) / 2.0f;

        case EasingName::EaseInBack: {
            constexpr float c1 = 1.70158f;
            constexpr float c3 = c1 + 1.0f;
            return c3 * x * x * x - c1 * x * x;
        }
        case EasingName::EaseOutBack: {
            constexpr float c1 = 1.70158f;
            constexpr float c3 = c1 + 1.0f;
            return 1.0f + c3 * std::pow(x - 1.0f, 3.0f) + c1 * std::pow(x - 1.0f, 2.0f);
        }
        case EasingName::EaseInOutBack: {
            constexpr float c1 = 1.70158f;
            constexpr float c2 = c1 * 1.525f;
            return x < 0.5f
                ? (std::pow(2.0f * x, 2.0f) * ((c2 + 1.0f) * 2.0f * x - c2)) / 2.0f
                : (std::pow(2.0f * x - 2.0f, 2.0f) * ((c2 + 1.0f) * (x * 2.0f - 2.0f) + c2) + 2.0f) / 2.0f;
        }

        case EasingName::EaseInElastic: {
            constexpr float c4 = (2.0f * kPi) / 3.0f;
            if (x == 0.0f) return 0.0f;
            if (x == 1.0f) return 1.0f;
            return -std::pow(2.0f, 10.0f * x - 10.0f) * std::sin((x * 10.0f - 10.75f) * c4);
        }
        case EasingName::EaseOutElastic: {
            constexpr float c4 = (2.0f * kPi) / 3.0f;
            if (x == 0.0f) return 0.0f;
            if (x == 1.0f) return 1.0f;
            return std::pow(2.0f, -10.0f * x) * std::sin((x * 10.0f - 0.75f) * c4) + 1.0f;
        }
        case EasingName::EaseInOutElastic: {
            constexpr float c5 = (2.0f * kPi) / 4.5f;
            if (x == 0.0f) return 0.0f;
            if (x == 1.0f) return 1.0f;
            return x < 0.5f
                ? -(std::pow(2.0f, 20.0f * x - 10.0f) * std::sin((20.0f * x - 11.125f) * c5)) / 2.0f
                : (std::pow(2.0f, -20.0f * x + 10.0f) * std::sin((20.0f * x - 11.125f) * c5)) / 2.0f + 1.0f;
        }

        case EasingName::EaseInBounce:
            return 1.0f - easeOutBounceImpl(1.0f - x);
        case EasingName::EaseOutBounce:
            return easeOutBounceImpl(x);
        case EasingName::EaseInOutBounce:
            return x < 0.5f
                ? (1.0f - easeOutBounceImpl(1.0f - 2.0f * x)) / 2.0f
                : (1.0f + easeOutBounceImpl(2.0f * x - 1.0f)) / 2.0f;
    }

    return x;
}

SpringParams springFromSettling(float settleSec, float zeta, float mass) {
    SpringParams p{};
    p.mass = std::max(0.0001f, mass);
    p.zeta = std::max(0.01f, zeta);
    const float safeSettle = std::max(0.01f, settleSec);
    p.omega = 4.0f / (p.zeta * safeSettle);
    return p;
}

float estimateSettlingTime(const SpringParams& spring) {
    const float safeZeta = std::max(0.01f, spring.zeta);
    const float safeOmega = std::max(0.01f, spring.omega);
    return 4.0f / (safeZeta * safeOmega);
}

ChannelId AnimationEngine::createChannel(const ChannelKey& key, float initialValue) {
    const ChannelId id = m_nextChannelId++;
    ChannelState st{};
    st.key = key;
    st.current = initialValue;
    st.target = initialValue;
    st.startValue = initialValue;
    m_channels[id] = st;
    m_index[key] = id;
    return id;
}

ChannelId AnimationEngine::ensureChannel(const ChannelKey& key, float initialValue) {
    if (auto it = m_index.find(key); it != m_index.end()) {
        return it->second;
    }
    return createChannel(key, initialValue);
}

bool AnimationEngine::animateTo(ChannelId channelId, float target, const MotionSpec& spec) {
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return false;

    ChannelState& ch = it->second;

    if (spec.interruptBehavior == InterruptBehavior::HardSnap) {
        ch.current = target;
        ch.target = target;
        ch.velocity = 0.0f;
        ch.startValue = target;
        ch.elapsedSec = 0.0f;
        ch.spec = spec;
        ch.active = false;
        return true;
    }

    ch.target = target;
    ch.spec = spec;

    if (spec.mode == MotionMode::Tween) {
        ch.startValue = ch.current;
        ch.elapsedSec = 0.0f;
    }

    if (!ch.active) {
        ch.elapsedSec = 0.0f;
        if (spec.mode == MotionMode::Tween) {
            ch.startValue = ch.current;
        }
    }

    ch.active = true;
    return true;
}

bool AnimationEngine::retarget(ChannelId channelId, float target) {
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return false;

    ChannelState& ch = it->second;
    ch.target = target;

    if (ch.spec.mode == MotionMode::Tween) {
        ch.startValue = ch.current;
        ch.elapsedSec = 0.0f;
    }

    ch.active = true;
    return true;
}

bool AnimationEngine::setSpec(ChannelId channelId, const MotionSpec& spec, bool keepVelocity) {
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return false;

    ChannelState& ch = it->second;
    ch.spec = spec;
    ch.elapsedSec = 0.0f;
    if (spec.mode == MotionMode::Tween) {
        ch.startValue = ch.current;
    }
    if (!keepVelocity) {
        ch.velocity = 0.0f;
    }
    return true;
}

bool AnimationEngine::stop(ChannelId channelId, bool snapToTarget) {
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return false;

    ChannelState& ch = it->second;
    if (snapToTarget) {
        ch.current = ch.target;
    }
    ch.velocity = 0.0f;
    ch.active = false;
    return true;
}

AnimatedSample AnimationEngine::sample(ChannelId channelId) const {
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return AnimatedSample{};

    const ChannelState& ch = it->second;
    return AnimatedSample{ch.current, ch.velocity, ch.active};
}

bool AnimationEngine::isActive(ChannelId channelId) const {
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return false;
    return it->second.active;
}

std::optional<ChannelId> AnimationEngine::findChannel(const ChannelKey& key) const {
    auto it = m_index.find(key);
    if (it == m_index.end()) return std::nullopt;
    return it->second;
}

const ChannelKey* AnimationEngine::getChannelKey(ChannelId channelId) const {
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return nullptr;
    return &it->second.key;
}

void AnimationEngine::setOnComplete(ChannelId channelId, OnComplete callback) {
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return;
    it->second.onComplete = std::move(callback);
}

bool AnimationEngine::applySpringStep(ChannelState& ch, float dtSec) {
    const SpringParams& s = ch.spec.spring;
    const float m = std::max(0.0001f, s.mass);
    const float omega = std::max(0.01f, s.omega);
    const float zeta = std::max(0.0f, s.zeta);

    const float k = m * omega * omega;
    const float c = 2.0f * m * zeta * omega;

    const float dx = ch.current - ch.target;
    const float acc = (-k * dx - c * ch.velocity) / m;

    ch.velocity += acc * dtSec;
    if (std::isfinite(s.maxVelocity)) {
        ch.velocity = std::clamp(ch.velocity, -s.maxVelocity, s.maxVelocity);
    }
    ch.current += ch.velocity * dtSec;

    const float posErr = std::fabs(ch.current - ch.target);
    const float velErr = std::fabs(ch.velocity);
    if (posErr <= s.settlePosEpsilon && velErr <= s.settleVelEpsilon) {
        ch.current = ch.target;
        ch.velocity = 0.0f;
        ch.active = false;
        return true;
    }

    return false;
}

bool AnimationEngine::applyTweenStep(ChannelState& ch, float dtSec) {
    const TweenParams& t = ch.spec.tween;
    const float duration = std::max(0.0001f, t.durationSec);

    ch.elapsedSec += dtSec;

    if (ch.elapsedSec < t.delaySec) {
        return false;
    }

    const float x = std::clamp((ch.elapsedSec - t.delaySec) / duration, 0.0f, 1.0f);
    const float y = evaluateEasing(t.easing, x);

    const float old = ch.current;
    ch.current = ch.startValue + (ch.target - ch.startValue) * y;

    const float probeX = std::min(1.0f, x + 0.001f);
    const float probeY = evaluateEasing(t.easing, probeX);
    const float dy = std::max(0.0f, probeY - y);
    ch.velocity = (ch.target - ch.startValue) * (dy / std::max(1e-6f, (probeX - x) * duration));

    if (x >= 1.0f) {
        ch.current = ch.target;
        ch.velocity = 0.0f;
        ch.active = false;
        return true;
    }

    (void)old;
    return false;
}

std::vector<ChannelId> AnimationEngine::tick(float dtSec) {
    std::vector<ChannelId> changed;
    if (dtSec <= 0.0f) return changed;

    dtSec = std::clamp(dtSec, 1.0f / 240.0f, 1.0f / 20.0f);
    constexpr float kStepMax = 1.0f / 120.0f;
    const int substeps = std::max(1, static_cast<int>(std::ceil(dtSec / kStepMax)));
    const float subDt = dtSec / static_cast<float>(substeps);

    std::vector<ChannelId> completed;

    for (auto& [id, ch] : m_channels) {
        if (!ch.active) continue;

        const float start = ch.current;
        bool done = false;

        for (int i = 0; i < substeps && ch.active; ++i) {
            if (ch.spec.mode == MotionMode::Spring) {
                done = applySpringStep(ch, subDt);
            } else {
                done = applyTweenStep(ch, subDt);
            }
            if (done) break;
        }

        if (std::fabs(ch.current - start) > 1e-6f || done) {
            changed.push_back(id);
        }

        if (done && ch.onComplete) {
            completed.push_back(id);
        }
    }

    for (ChannelId id : completed) {
        auto it = m_channels.find(id);
        if (it != m_channels.end() && it->second.onComplete) {
            it->second.onComplete(id);
        }
    }

    return changed;
}

size_t AnimationEngine::clearObjectChannels(uint64_t objectId) {
    std::vector<ChannelId> toRemove;
    toRemove.reserve(m_channels.size());

    for (const auto& [id, ch] : m_channels) {
        if (ch.key.objectId == objectId) {
            toRemove.push_back(id);
        }
    }

    for (ChannelId id : toRemove) {
        auto it = m_channels.find(id);
        if (it != m_channels.end()) {
            m_index.erase(it->second.key);
            m_channels.erase(it);
        }
    }

    return toRemove.size();
}

void AnimationEngine::clearAll() {
    m_channels.clear();
    m_index.clear();
    m_nextChannelId = 1;
}

} // namespace lcl::ui
