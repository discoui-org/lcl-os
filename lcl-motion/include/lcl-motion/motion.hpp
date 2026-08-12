#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lcl::motion {

enum class EasingName : uint8_t {
    Linear,
    EaseInSine, EaseOutSine, EaseInOutSine,
    EaseInQuad, EaseOutQuad, EaseInOutQuad,
    EaseInCubic, EaseOutCubic, EaseInOutCubic,
    EaseInQuart, EaseOutQuart, EaseInOutQuart,
    EaseInQuint, EaseOutQuint, EaseInOutQuint,
    EaseInExpo, EaseOutExpo, EaseInOutExpo,
    EaseInCirc, EaseOutCirc, EaseInOutCirc,
    EaseInBack, EaseOutBack, EaseInOutBack,
    EaseInElastic, EaseOutElastic, EaseInOutElastic,
    EaseInBounce, EaseOutBounce, EaseInOutBounce,
};

enum class StepPosition : uint8_t { Start, End };

class Easing {
public:
    enum class Kind : uint8_t { Named, CubicBezier, Steps };

    constexpr Easing() = default;
    constexpr Easing(EasingName name) : m_name(name) {}

    static constexpr Easing linear() { return Easing(EasingName::Linear); }
    static constexpr Easing named(EasingName name) { return Easing(name); }
    static Easing cubicBezier(float x1, float y1, float x2, float y2);
    static Easing steps(uint32_t count, StepPosition position = StepPosition::End);

    float evaluate(float progress) const;
    Kind kind() const noexcept { return m_kind; }
    EasingName name() const noexcept { return m_name; }

private:
    Kind m_kind{Kind::Named};
    EasingName m_name{EasingName::Linear};
    float m_x1{0.0f};
    float m_y1{0.0f};
    float m_x2{1.0f};
    float m_y2{1.0f};
    uint32_t m_stepCount{1};
    StepPosition m_stepPosition{StepPosition::End};
};

float evaluateEasing(EasingName easing, float progress);

enum class MotionMode : uint8_t { Spring, Tween };
enum class DampingMode : uint8_t { UnderDamped, Critical, OverDamped };
enum class InterruptBehavior : uint8_t {
    PreserveVelocityAndRetarget,
    PreserveVelocityAndBlendToNewSpec,
    HardSnap,
};

struct SpringParams {
    float mass{1.0f};
    float stiffness{324.0f};
    float damping{36.0f};
    float initialVelocity{0.0f};
    float maxVelocity{std::numeric_limits<float>::infinity()};
    float settlePosEpsilon{0.001f};
    float settleVelEpsilon{0.001f};

    // Compatibility accessors for the original omega/zeta representation.
    float omega() const;
    float zeta() const;
};

struct TweenParams {
    float durationSec{0.18f};
    float delaySec{0.0f};
    Easing easing{EasingName::EaseOutCubic};
};

struct Motion {
    MotionMode mode{MotionMode::Spring};
    SpringParams springParams{};
    TweenParams tweenParams{};
    InterruptBehavior interruptBehavior{InterruptBehavior::PreserveVelocityAndRetarget};

    static Motion spring(float durationSec, float bounce = 0.0f);
    static Motion spring(float mass, float stiffness, float damping,
                         float initialVelocity = 0.0f);
    static Motion tween(float durationSec,
                        Easing easing = Easing(EasingName::EaseOutCubic),
                        float delaySec = 0.0f);
};

using MotionSpec = Motion;

struct ChannelKey {
    uint64_t objectId{0};
    uint32_t propertyId{0};
    bool operator==(const ChannelKey&) const = default;
};

struct ChannelKeyHasher {
    size_t operator()(const ChannelKey& key) const noexcept;
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
    bool animateTo(ChannelId channelId, float target, const Motion& motion);
    bool retarget(ChannelId channelId, float target);
    bool setSpec(ChannelId channelId, const Motion& motion, bool keepVelocity = true);
    bool setValue(ChannelId channelId, float value);
    bool stop(ChannelId channelId, bool snapToTarget = true);
    AnimatedSample sample(ChannelId channelId) const;
    bool isActive(ChannelId channelId) const;
    bool hasActiveAnimations() const noexcept;
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
        Motion motion{};
        OnComplete onComplete{};
    };

    static bool applySpringStep(ChannelState& channel, float dtSec);
    static bool applyTweenStep(ChannelState& channel, float dtSec);

    ChannelId m_nextChannelId{1};
    std::unordered_map<ChannelId, ChannelState> m_channels;
    std::unordered_map<ChannelKey, ChannelId, ChannelKeyHasher> m_index;
};

SpringParams springFromSettling(float settleSec, float bounce = 0.0f,
                                float mass = 1.0f);
float estimateSettlingTime(const SpringParams& spring);

enum class PlaybackDirection : uint8_t { Normal, Reverse, Alternate, AlternateReverse };
enum class FillMode : uint8_t { None, Forwards };
enum class PlayState : uint8_t { Idle, Running, Paused, Finished, Cancelled };

struct Keyframe {
    float offset{0.0f};
    float value{0.0f};
    std::optional<Easing> easing{};
};

struct AnimationOptions {
    float durationSec{0.18f};
    float delaySec{0.0f};
    uint32_t repeat{0};
    PlaybackDirection direction{PlaybackDirection::Normal};
    FillMode fill{FillMode::None};
    Easing easing{EasingName::Linear};
};

class AnimationHandle {
public:
    AnimationHandle() = default;

    void play();
    void pause();
    void reverse();
    void cancel();
    void finish();
    void seek(float progress);
    float progress() const;
    PlayState state() const;
    bool active() const;
    void setOnComplete(std::function<void()> callback);
    void commitFinalStyles();

private:
    struct State;
    explicit AnimationHandle(std::shared_ptr<State> state) : m_state(std::move(state)) {}
    std::shared_ptr<State> m_state;
    friend class Timeline;
};

class Timeline {
public:
    using Apply = std::function<void(float)>;
    using Read = std::function<float()>;

    AnimationHandle animate(std::vector<Keyframe> keyframes,
                            const AnimationOptions& options,
                            Apply applyPresentation,
                            Read readModel = {},
                            Apply commitModel = {});
    bool tick(float dtSec);
    bool hasActiveAnimations() const noexcept;
    void clear();

private:
    std::vector<std::weak_ptr<AnimationHandle::State>> m_animations;
};

namespace tokens {
inline Motion hover() { return Motion::spring(0.180f, 0.0f); }
inline Motion pressed() { return Motion::spring(0.100f, 0.0f); }
inline Motion release() { return Motion::spring(0.240f, 0.08f); }
inline Motion focus() { return Motion::tween(0.160f, Easing(EasingName::EaseOutCubic)); }
inline Motion windowOpen() { return Motion::tween(0.200f, Easing(EasingName::EaseOutCubic)); }
inline Motion windowClose() { return Motion::tween(0.140f, Easing(EasingName::EaseInCubic)); }
inline Motion windowMorph() { return Motion::spring(0.320f, 0.06f); }
inline Motion minimize() { return Motion::tween(0.180f, Easing(EasingName::EaseInCubic)); }
inline Motion restore() { return Motion::tween(0.220f, Easing(EasingName::EaseOutCubic)); }
inline Motion dragSnapBack() { return Motion::spring(1.0f, 180.0f, 24.0f); }
} // namespace tokens

} // namespace lcl::motion
