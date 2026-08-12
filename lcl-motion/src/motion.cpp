#include "lcl-motion/motion.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace lcl::motion {
namespace {

constexpr float kPi = 3.14159265358979323846f;
// Preserve elapsed monotonic time after stalls. Springs still integrate in
// bounded substeps; this only rejects pathological multi-second jumps.
constexpr float kMaxTickSec = 10.0f;
constexpr float kSpringStepSec = 1.0f / 240.0f;

float clamp01(float value) { return std::clamp(value, 0.0f, 1.0f); }

float bounceOut(float value) {
    constexpr float n = 7.5625f;
    constexpr float d = 2.75f;
    if (value < 1.0f / d) return n * value * value;
    if (value < 2.0f / d) {
        value -= 1.5f / d;
        return n * value * value + 0.75f;
    }
    if (value < 2.5f / d) {
        value -= 2.25f / d;
        return n * value * value + 0.9375f;
    }
    value -= 2.625f / d;
    return n * value * value + 0.984375f;
}

float cubic(float a, float b, float c, float t) {
    const float inv = 1.0f - t;
    return 3.0f * inv * inv * t * a + 3.0f * inv * t * t * b + t * t * t * c;
}

float cubicDerivative(float a, float b, float c, float t) {
    const float inv = 1.0f - t;
    return 3.0f * inv * inv * a + 6.0f * inv * t * (b - a) +
           3.0f * t * t * (c - b);
}

float sampleKeyframes(const std::vector<Keyframe>& frames,
                      const AnimationOptions& options, float progress) {
    if (frames.empty()) return 0.0f;
    if (frames.size() == 1) return frames.front().value;
    progress = clamp01(progress);
    if (progress <= frames.front().offset) return frames.front().value;
    if (progress >= frames.back().offset) return frames.back().value;

    for (size_t index = 1; index < frames.size(); ++index) {
        if (progress > frames[index].offset) continue;
        const Keyframe& from = frames[index - 1];
        const Keyframe& to = frames[index];
        const float span = std::max(0.000001f, to.offset - from.offset);
        const float local = (progress - from.offset) / span;
        const float eased = from.easing.value_or(options.easing).evaluate(local);
        return from.value + (to.value - from.value) * eased;
    }
    return frames.back().value;
}

bool reverseIteration(PlaybackDirection direction, uint32_t iteration) {
    switch (direction) {
        case PlaybackDirection::Normal: return false;
        case PlaybackDirection::Reverse: return true;
        case PlaybackDirection::Alternate: return (iteration % 2u) == 1u;
        case PlaybackDirection::AlternateReverse: return (iteration % 2u) == 0u;
    }
    return false;
}

} // namespace

float evaluateEasing(EasingName easing, float x) {
    x = clamp01(x);
    switch (easing) {
        case EasingName::Linear: return x;
        case EasingName::EaseInSine: return 1.0f - std::cos(x * kPi * 0.5f);
        case EasingName::EaseOutSine: return std::sin(x * kPi * 0.5f);
        case EasingName::EaseInOutSine: return -(std::cos(kPi * x) - 1.0f) * 0.5f;
        case EasingName::EaseInQuad: return x * x;
        case EasingName::EaseOutQuad: return 1.0f - (1.0f - x) * (1.0f - x);
        case EasingName::EaseInOutQuad: return x < 0.5f ? 2.0f * x * x : 1.0f - std::pow(-2.0f * x + 2.0f, 2.0f) * 0.5f;
        case EasingName::EaseInCubic: return x * x * x;
        case EasingName::EaseOutCubic: return 1.0f - std::pow(1.0f - x, 3.0f);
        case EasingName::EaseInOutCubic: return x < 0.5f ? 4.0f * x * x * x : 1.0f - std::pow(-2.0f * x + 2.0f, 3.0f) * 0.5f;
        case EasingName::EaseInQuart: return std::pow(x, 4.0f);
        case EasingName::EaseOutQuart: return 1.0f - std::pow(1.0f - x, 4.0f);
        case EasingName::EaseInOutQuart: return x < 0.5f ? 8.0f * std::pow(x, 4.0f) : 1.0f - std::pow(-2.0f * x + 2.0f, 4.0f) * 0.5f;
        case EasingName::EaseInQuint: return std::pow(x, 5.0f);
        case EasingName::EaseOutQuint: return 1.0f - std::pow(1.0f - x, 5.0f);
        case EasingName::EaseInOutQuint: return x < 0.5f ? 16.0f * std::pow(x, 5.0f) : 1.0f - std::pow(-2.0f * x + 2.0f, 5.0f) * 0.5f;
        case EasingName::EaseInExpo: return x == 0.0f ? 0.0f : std::pow(2.0f, 10.0f * x - 10.0f);
        case EasingName::EaseOutExpo: return x == 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * x);
        case EasingName::EaseInOutExpo:
            if (x == 0.0f || x == 1.0f) return x;
            return x < 0.5f ? std::pow(2.0f, 20.0f * x - 10.0f) * 0.5f
                            : (2.0f - std::pow(2.0f, -20.0f * x + 10.0f)) * 0.5f;
        case EasingName::EaseInCirc: return 1.0f - std::sqrt(std::max(0.0f, 1.0f - x * x));
        case EasingName::EaseOutCirc: return std::sqrt(std::max(0.0f, 1.0f - (x - 1.0f) * (x - 1.0f)));
        case EasingName::EaseInOutCirc:
            return x < 0.5f ? (1.0f - std::sqrt(std::max(0.0f, 1.0f - 4.0f * x * x))) * 0.5f
                            : (std::sqrt(std::max(0.0f, 1.0f - std::pow(-2.0f * x + 2.0f, 2.0f))) + 1.0f) * 0.5f;
        case EasingName::EaseInBack: {
            constexpr float c1 = 1.70158f;
            return (c1 + 1.0f) * x * x * x - c1 * x * x;
        }
        case EasingName::EaseOutBack: {
            constexpr float c1 = 1.70158f;
            return 1.0f + (c1 + 1.0f) * std::pow(x - 1.0f, 3.0f) + c1 * std::pow(x - 1.0f, 2.0f);
        }
        case EasingName::EaseInOutBack: {
            constexpr float c2 = 1.70158f * 1.525f;
            return x < 0.5f
                ? std::pow(2.0f * x, 2.0f) * ((c2 + 1.0f) * 2.0f * x - c2) * 0.5f
                : (std::pow(2.0f * x - 2.0f, 2.0f) * ((c2 + 1.0f) * (2.0f * x - 2.0f) + c2) + 2.0f) * 0.5f;
        }
        case EasingName::EaseInElastic:
            if (x == 0.0f || x == 1.0f) return x;
            return -std::pow(2.0f, 10.0f * x - 10.0f) * std::sin((10.0f * x - 10.75f) * (2.0f * kPi / 3.0f));
        case EasingName::EaseOutElastic:
            if (x == 0.0f || x == 1.0f) return x;
            return std::pow(2.0f, -10.0f * x) * std::sin((10.0f * x - 0.75f) * (2.0f * kPi / 3.0f)) + 1.0f;
        case EasingName::EaseInOutElastic:
            if (x == 0.0f || x == 1.0f) return x;
            return x < 0.5f
                ? -std::pow(2.0f, 20.0f * x - 10.0f) * std::sin((20.0f * x - 11.125f) * (2.0f * kPi / 4.5f)) * 0.5f
                : std::pow(2.0f, -20.0f * x + 10.0f) * std::sin((20.0f * x - 11.125f) * (2.0f * kPi / 4.5f)) * 0.5f + 1.0f;
        case EasingName::EaseInBounce: return 1.0f - bounceOut(1.0f - x);
        case EasingName::EaseOutBounce: return bounceOut(x);
        case EasingName::EaseInOutBounce: return x < 0.5f ? (1.0f - bounceOut(1.0f - 2.0f * x)) * 0.5f : (1.0f + bounceOut(2.0f * x - 1.0f)) * 0.5f;
    }
    return x;
}

Easing Easing::cubicBezier(float x1, float y1, float x2, float y2) {
    if (!std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2) || !std::isfinite(y2) ||
        x1 < 0.0f || x1 > 1.0f || x2 < 0.0f || x2 > 1.0f) {
        throw std::invalid_argument("cubicBezier x coordinates must be finite and within [0, 1]");
    }
    Easing result;
    result.m_kind = Kind::CubicBezier;
    result.m_x1 = x1; result.m_y1 = y1; result.m_x2 = x2; result.m_y2 = y2;
    return result;
}

Easing Easing::steps(uint32_t count, StepPosition position) {
    if (count == 0) throw std::invalid_argument("steps count must be greater than zero");
    Easing result;
    result.m_kind = Kind::Steps;
    result.m_stepCount = count;
    result.m_stepPosition = position;
    return result;
}

float Easing::evaluate(float progress) const {
    progress = clamp01(progress);
    if (progress == 0.0f || progress == 1.0f) return progress;
    if (m_kind == Kind::Named) return evaluateEasing(m_name, progress);
    if (m_kind == Kind::Steps) {
        const float scaled = progress * static_cast<float>(m_stepCount);
        const float step = m_stepPosition == StepPosition::Start ? std::ceil(scaled) : std::floor(scaled);
        return clamp01(step / static_cast<float>(m_stepCount));
    }

    float parameter = progress;
    for (int iteration = 0; iteration < 8; ++iteration) {
        const float error = cubic(m_x1, m_x2, 1.0f, parameter) - progress;
        const float slope = cubicDerivative(m_x1, m_x2, 1.0f, parameter);
        if (std::fabs(error) < 1e-6f || std::fabs(slope) < 1e-6f) break;
        parameter = std::clamp(parameter - error / slope, 0.0f, 1.0f);
    }
    float low = 0.0f;
    float high = 1.0f;
    for (int iteration = 0; iteration < 12; ++iteration) {
        const float value = cubic(m_x1, m_x2, 1.0f, parameter);
        if (std::fabs(value - progress) < 1e-6f) break;
        if (value < progress) low = parameter; else high = parameter;
        parameter = (low + high) * 0.5f;
    }
    return cubic(m_y1, m_y2, 1.0f, parameter);
}

float SpringParams::omega() const { return std::sqrt(std::max(0.0f, stiffness) / std::max(0.0001f, mass)); }
float SpringParams::zeta() const { return damping / std::max(0.0001f, 2.0f * std::sqrt(std::max(0.0f, stiffness * mass))); }

SpringParams springFromSettling(float settleSec, float bounce, float mass) {
    SpringParams result;
    result.mass = std::max(0.0001f, mass);
    const float ratio = std::clamp(1.0f - bounce, 0.05f, 1.0f);
    // Near-critical springs carry the (1 + wt) polynomial term, so six time
    // constants tracks the caller-visible settling duration more closely than
    // the underdamped four-time-constant approximation.
    const float omega = 6.0f / (ratio * std::max(0.01f, settleSec));
    result.stiffness = result.mass * omega * omega;
    result.damping = 2.0f * result.mass * ratio * omega;
    return result;
}

float estimateSettlingTime(const SpringParams& spring) {
    return 6.0f / std::max(0.0001f, spring.zeta() * spring.omega());
}

Motion Motion::spring(float durationSec, float bounce) {
    Motion result;
    result.mode = MotionMode::Spring;
    result.springParams = springFromSettling(durationSec, bounce);
    return result;
}

Motion Motion::spring(float mass, float stiffness, float damping, float initialVelocity) {
    if (mass <= 0.0f || stiffness <= 0.0f || damping < 0.0f)
        throw std::invalid_argument("spring mass/stiffness must be positive and damping non-negative");
    Motion result;
    result.mode = MotionMode::Spring;
    result.springParams.mass = mass;
    result.springParams.stiffness = stiffness;
    result.springParams.damping = damping;
    result.springParams.initialVelocity = initialVelocity;
    return result;
}

Motion Motion::tween(float durationSec, Easing easing, float delaySec) {
    if (durationSec < 0.0f || delaySec < 0.0f)
        throw std::invalid_argument("tween duration and delay must be non-negative");
    Motion result;
    result.mode = MotionMode::Tween;
    result.tweenParams = {durationSec, delaySec, easing};
    return result;
}

size_t ChannelKeyHasher::operator()(const ChannelKey& key) const noexcept {
    const size_t first = std::hash<uint64_t>{}(key.objectId);
    const size_t second = std::hash<uint32_t>{}(key.propertyId);
    return first ^ (second + 0x9e3779b97f4a7c15ULL + (first << 6) + (first >> 2));
}

ChannelId AnimationEngine::createChannel(const ChannelKey& key, float initialValue) {
    if (const auto existing = findChannel(key)) return *existing;
    const ChannelId id = m_nextChannelId++;
    ChannelState state;
    state.key = key;
    state.current = state.target = state.startValue = initialValue;
    m_channels.emplace(id, std::move(state));
    m_index.emplace(key, id);
    return id;
}

ChannelId AnimationEngine::ensureChannel(const ChannelKey& key, float initialValue) {
    const auto found = findChannel(key);
    return found ? *found : createChannel(key, initialValue);
}

bool AnimationEngine::animateTo(ChannelId id, float target, const Motion& motion) {
    auto found = m_channels.find(id);
    if (found == m_channels.end() || !std::isfinite(target)) return false;
    auto& channel = found->second;
    channel.motion = motion;
    channel.target = target;
    channel.elapsedSec = 0.0f;
    if (motion.interruptBehavior == InterruptBehavior::HardSnap) {
        channel.current = channel.startValue = target;
        channel.velocity = 0.0f;
        channel.active = false;
        return true;
    }
    if (motion.mode == MotionMode::Tween) {
        channel.startValue = channel.current;
        channel.velocity = 0.0f;
    } else if (!channel.active) {
        channel.velocity = motion.springParams.initialVelocity;
    }
    channel.active = true;
    return true;
}

bool AnimationEngine::retarget(ChannelId id, float target) {
    auto found = m_channels.find(id);
    if (found == m_channels.end() || !std::isfinite(target)) return false;
    auto& channel = found->second;
    channel.target = target;
    channel.elapsedSec = 0.0f;
    if (channel.motion.mode == MotionMode::Tween) {
        channel.startValue = channel.current;
        channel.velocity = 0.0f;
    }
    channel.active = true;
    return true;
}

bool AnimationEngine::setSpec(ChannelId id, const Motion& motion, bool keepVelocity) {
    auto found = m_channels.find(id);
    if (found == m_channels.end()) return false;
    found->second.motion = motion;
    found->second.elapsedSec = 0.0f;
    found->second.startValue = found->second.current;
    if (!keepVelocity || motion.mode == MotionMode::Tween) found->second.velocity = 0.0f;
    return true;
}

bool AnimationEngine::setValue(ChannelId id, float value) {
    auto found = m_channels.find(id);
    if (found == m_channels.end() || !std::isfinite(value)) return false;
    auto& channel = found->second;
    channel.current = value;
    channel.target = value;
    channel.startValue = value;
    channel.velocity = 0.0f;
    channel.elapsedSec = 0.0f;
    channel.active = false;
    return true;
}

bool AnimationEngine::stop(ChannelId id, bool snapToTarget) {
    auto found = m_channels.find(id);
    if (found == m_channels.end()) return false;
    if (snapToTarget) found->second.current = found->second.target;
    found->second.velocity = 0.0f;
    found->second.active = false;
    return true;
}

AnimatedSample AnimationEngine::sample(ChannelId id) const {
    const auto found = m_channels.find(id);
    return found == m_channels.end() ? AnimatedSample{} : AnimatedSample{found->second.current, found->second.velocity, found->second.active};
}

bool AnimationEngine::isActive(ChannelId id) const {
    const auto found = m_channels.find(id);
    return found != m_channels.end() && found->second.active;
}

bool AnimationEngine::hasActiveAnimations() const noexcept {
    return std::any_of(m_channels.begin(), m_channels.end(), [](const auto& item) { return item.second.active; });
}

std::optional<ChannelId> AnimationEngine::findChannel(const ChannelKey& key) const {
    const auto found = m_index.find(key);
    return found == m_index.end() ? std::nullopt : std::optional<ChannelId>(found->second);
}

const ChannelKey* AnimationEngine::getChannelKey(ChannelId id) const {
    const auto found = m_channels.find(id);
    return found == m_channels.end() ? nullptr : &found->second.key;
}

void AnimationEngine::setOnComplete(ChannelId id, OnComplete callback) {
    if (auto found = m_channels.find(id); found != m_channels.end()) found->second.onComplete = std::move(callback);
}

bool AnimationEngine::applySpringStep(ChannelState& channel, float dtSec) {
    const auto& params = channel.motion.springParams;
    const float acceleration = (-params.stiffness * (channel.current - channel.target) -
                                params.damping * channel.velocity) / std::max(0.0001f, params.mass);
    channel.velocity += acceleration * dtSec;
    if (std::isfinite(params.maxVelocity))
        channel.velocity = std::clamp(channel.velocity, -params.maxVelocity, params.maxVelocity);
    channel.current += channel.velocity * dtSec;
    if (std::fabs(channel.current - channel.target) <= params.settlePosEpsilon &&
        std::fabs(channel.velocity) <= params.settleVelEpsilon) {
        channel.current = channel.target;
        channel.velocity = 0.0f;
        channel.active = false;
        return true;
    }
    return false;
}

bool AnimationEngine::applyTweenStep(ChannelState& channel, float dtSec) {
    const auto& params = channel.motion.tweenParams;
    channel.elapsedSec += dtSec;
    if (channel.elapsedSec < params.delaySec) return false;
    const float duration = std::max(0.000001f, params.durationSec);
    const float x = clamp01((channel.elapsedSec - params.delaySec) / duration);
    const float y = params.easing.evaluate(x);
    const float previous = channel.current;
    channel.current = channel.startValue + (channel.target - channel.startValue) * y;
    channel.velocity = dtSec > 0.0f ? (channel.current - previous) / dtSec : 0.0f;
    if (x >= 1.0f) {
        channel.current = channel.target;
        channel.velocity = 0.0f;
        channel.active = false;
        return true;
    }
    return false;
}

std::vector<ChannelId> AnimationEngine::tick(float dtSec) {
    std::vector<ChannelId> changed;
    if (!std::isfinite(dtSec) || dtSec <= 0.0f) return changed;
    const float elapsed = std::min(dtSec, kMaxTickSec);
    std::vector<ChannelId> completed;
    for (auto& [id, channel] : m_channels) {
        if (!channel.active) continue;
        const float before = channel.current;
        bool done = false;
        if (channel.motion.mode == MotionMode::Tween) {
            done = applyTweenStep(channel, elapsed);
        } else {
            const int count = std::max(1, static_cast<int>(std::ceil(elapsed / kSpringStepSec)));
            const float step = elapsed / static_cast<float>(count);
            for (int index = 0; index < count && channel.active; ++index)
                done = applySpringStep(channel, step);
        }
        if (done || std::fabs(before - channel.current) > 0.000001f) changed.push_back(id);
        if (done && channel.onComplete) completed.push_back(id);
    }
    for (const ChannelId id : completed) {
        const auto found = m_channels.find(id);
        if (found != m_channels.end() && found->second.onComplete) found->second.onComplete(id);
    }
    return changed;
}

size_t AnimationEngine::clearObjectChannels(uint64_t objectId) {
    std::vector<ChannelId> removed;
    for (const auto& [id, channel] : m_channels)
        if (channel.key.objectId == objectId) removed.push_back(id);
    for (const ChannelId id : removed) {
        m_index.erase(m_channels.at(id).key);
        m_channels.erase(id);
    }
    return removed.size();
}

void AnimationEngine::clearAll() {
    m_channels.clear();
    m_index.clear();
    m_nextChannelId = 1;
}

struct AnimationHandle::State {
    using Apply = Timeline::Apply;
    using Read = Timeline::Read;

    std::vector<Keyframe> frames;
    AnimationOptions options;
    Apply apply;
    Read readModel;
    Apply commitModel;
    std::function<void()> completion;
    float elapsedSec{0.0f};
    float playbackRate{1.0f};
    float originalModel{0.0f};
    float lastValue{0.0f};
    PlayState playState{PlayState::Running};
    bool completionCalled{false};

    float totalDuration() const { return std::max(0.0f, options.delaySec) + std::max(0.0f, options.durationSec) * static_cast<float>(options.repeat + 1u); }
    float normalizedProgress() const {
        const float duration = std::max(0.000001f, options.durationSec);
        return clamp01((elapsedSec - options.delaySec) / duration);
    }
    void sampleAndApply() {
        if (elapsedSec < options.delaySec) return;
        const float duration = std::max(0.000001f, options.durationSec);
        const float animationTime = std::max(0.0f, elapsedSec - options.delaySec);
        const uint32_t maxIteration = options.repeat;
        uint32_t iteration = std::min(maxIteration, static_cast<uint32_t>(animationTime / duration));
        float local = animationTime / duration - static_cast<float>(iteration);
        if (animationTime >= duration * static_cast<float>(options.repeat + 1u)) {
            iteration = maxIteration;
            local = 1.0f;
        }
        if (reverseIteration(options.direction, iteration)) local = 1.0f - local;
        lastValue = sampleKeyframes(frames, options, local);
        if (apply) apply(lastValue);
    }
};

void AnimationHandle::play() { if (m_state && m_state->playState != PlayState::Finished) m_state->playState = PlayState::Running; }
void AnimationHandle::pause() { if (m_state && m_state->playState == PlayState::Running) m_state->playState = PlayState::Paused; }
void AnimationHandle::reverse() {
    if (!m_state) return;
    m_state->playbackRate = -m_state->playbackRate;
    m_state->playState = PlayState::Running;
}
void AnimationHandle::cancel() {
    if (!m_state) return;
    m_state->playState = PlayState::Cancelled;
    if (m_state->apply) m_state->apply(m_state->originalModel);
}
void AnimationHandle::finish() {
    if (!m_state) return;
    m_state->elapsedSec = m_state->playbackRate >= 0.0f ? m_state->totalDuration() : 0.0f;
    m_state->sampleAndApply();
    m_state->playState = PlayState::Finished;
    if (m_state->options.fill == FillMode::None && m_state->apply)
        m_state->apply(m_state->originalModel);
    if (!m_state->completionCalled && m_state->completion) {
        m_state->completionCalled = true;
        m_state->completion();
    }
}
void AnimationHandle::seek(float value) {
    if (!m_state) return;
    m_state->elapsedSec = m_state->options.delaySec + clamp01(value) * std::max(0.0f, m_state->options.durationSec);
    m_state->sampleAndApply();
}
float AnimationHandle::progress() const { return m_state ? m_state->normalizedProgress() : 0.0f; }
PlayState AnimationHandle::state() const { return m_state ? m_state->playState : PlayState::Idle; }
bool AnimationHandle::active() const { return m_state && m_state->playState == PlayState::Running; }
void AnimationHandle::setOnComplete(std::function<void()> callback) { if (m_state) m_state->completion = std::move(callback); }
void AnimationHandle::commitFinalStyles() { if (m_state && m_state->commitModel) m_state->commitModel(m_state->lastValue); }

AnimationHandle Timeline::animate(std::vector<Keyframe> keyframes,
                                  const AnimationOptions& options,
                                  Apply applyPresentation,
                                  Read readModel,
                                  Apply commitModel) {
    if (keyframes.empty()) throw std::invalid_argument("animation requires at least one keyframe");
    if (options.durationSec < 0.0f || options.delaySec < 0.0f)
        throw std::invalid_argument("animation duration and delay must be non-negative");
    std::stable_sort(keyframes.begin(), keyframes.end(), [](const Keyframe& a, const Keyframe& b) { return a.offset < b.offset; });
    if (keyframes.front().offset < 0.0f || keyframes.back().offset > 1.0f)
        throw std::invalid_argument("keyframe offsets must be within [0, 1]");
    auto state = std::make_shared<AnimationHandle::State>();
    state->frames = std::move(keyframes);
    state->options = options;
    state->apply = std::move(applyPresentation);
    state->readModel = std::move(readModel);
    state->commitModel = std::move(commitModel);
    state->originalModel = state->readModel ? state->readModel() : state->frames.front().value;
    state->lastValue = state->originalModel;
    m_animations.emplace_back(state);
    return AnimationHandle(std::move(state));
}

bool Timeline::tick(float dtSec) {
    if (!std::isfinite(dtSec) || dtSec <= 0.0f) return hasActiveAnimations();
    for (auto iterator = m_animations.begin(); iterator != m_animations.end();) {
        auto state = iterator->lock();
        if (!state) {
            iterator = m_animations.erase(iterator);
            continue;
        }
        if (state->playState == PlayState::Running) {
            state->elapsedSec += std::clamp(dtSec * state->playbackRate, -kMaxTickSec, kMaxTickSec);
            const float end = state->totalDuration();
            const bool finishedForward = state->playbackRate >= 0.0f && state->elapsedSec >= end;
            const bool finishedReverse = state->playbackRate < 0.0f && state->elapsedSec <= 0.0f;
            state->elapsedSec = std::clamp(state->elapsedSec, 0.0f, end);
            state->sampleAndApply();
            if (finishedForward || finishedReverse) {
                state->playState = PlayState::Finished;
                if (state->options.fill == FillMode::None && state->apply) state->apply(state->originalModel);
                if (!state->completionCalled && state->completion) {
                    state->completionCalled = true;
                    state->completion();
                }
            }
        }
        ++iterator;
    }
    return hasActiveAnimations();
}

bool Timeline::hasActiveAnimations() const noexcept {
    for (const auto& weak : m_animations) {
        if (const auto state = weak.lock(); state && state->playState == PlayState::Running) return true;
    }
    return false;
}

void Timeline::clear() { m_animations.clear(); }

} // namespace lcl::motion
