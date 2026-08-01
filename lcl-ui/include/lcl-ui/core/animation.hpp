#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lcl::ui {

enum class EasingName {
    Linear,
    EaseInSine,
    EaseOutSine,
    EaseInOutSine,
    EaseInQuad,
    EaseOutQuad,
    EaseInOutQuad,
    EaseInCubic,
    EaseOutCubic,
    EaseInOutCubic,
    EaseInQuart,
    EaseOutQuart,
    EaseInOutQuart,
    EaseInQuint,
    EaseOutQuint,
    EaseInOutQuint,
    EaseInExpo,
    EaseOutExpo,
    EaseInOutExpo,
    EaseInCirc,
    EaseOutCirc,
    EaseInOutCirc,
    EaseInBack,
    EaseOutBack,
    EaseInOutBack,
    EaseInElastic,
    EaseOutElastic,
    EaseInOutElastic,
    EaseInBounce,
    EaseOutBounce,
    EaseInOutBounce,
};

float evaluateEasing(EasingName easing, float x);

enum class MotionMode {
    Spring,
    Tween,
};

enum class DampingMode {
    UnderDamped,
    Critical,
    OverDamped,
};

enum class InterruptBehavior {
    PreserveVelocityAndRetarget,
    PreserveVelocityAndBlendToNewSpec,
    HardSnap,
};

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
};

struct ChannelKey {
    uint64_t objectId{0};
    uint32_t propertyId{0};

    bool operator==(const ChannelKey& other) const {
        return objectId == other.objectId && propertyId == other.propertyId;
    }
};

struct ChannelKeyHasher {
    size_t operator()(const ChannelKey& key) const noexcept {
        const size_t h1 = std::hash<uint64_t>{}(key.objectId);
        const size_t h2 = std::hash<uint32_t>{}(key.propertyId);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

using ChannelId = uint64_t;

struct AnimatedSample {
    float value{0.0f};
    float velocity{0.0f};
    bool active{false};
};

class AnimationEngine {
public:
    using OnComplete = std::function<void(ChannelId)>;

    ChannelId createChannel(const ChannelKey& key, float initialValue);
    ChannelId ensureChannel(const ChannelKey& key, float initialValue);

    bool animateTo(ChannelId channelId, float target, const MotionSpec& spec);
    bool retarget(ChannelId channelId, float target);
    bool setSpec(ChannelId channelId, const MotionSpec& spec, bool keepVelocity = true);
    bool stop(ChannelId channelId, bool snapToTarget = true);

    AnimatedSample sample(ChannelId channelId) const;
    bool isActive(ChannelId channelId) const;
    std::optional<ChannelId> findChannel(const ChannelKey& key) const;
    const ChannelKey* getChannelKey(ChannelId channelId) const;

    void setOnComplete(ChannelId channelId, OnComplete callback);

    std::vector<ChannelId> tick(float dtSec);

    size_t clearObjectChannels(uint64_t objectId);
    void clearAll();

private:
    struct ChannelState {
        ChannelKey key{};
        float current{0.0f};
        float target{0.0f};
        float velocity{0.0f};
        float startValue{0.0f};
        float elapsedSec{0.0f};
        bool active{false};
        MotionSpec spec{};
        OnComplete onComplete{};
    };

    bool applySpringStep(ChannelState& ch, float dtSec);
    bool applyTweenStep(ChannelState& ch, float dtSec);

    ChannelId m_nextChannelId{1};
    std::unordered_map<ChannelId, ChannelState> m_channels;
    std::unordered_map<ChannelKey, ChannelId, ChannelKeyHasher> m_index;
};

SpringParams springFromSettling(float settleSec, float zeta = 1.0f, float mass = 1.0f);
float estimateSettlingTime(const SpringParams& spring);

} // namespace lcl::ui
