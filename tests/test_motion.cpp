#include <gtest/gtest.h>

#include <cmath>

#include "lcl-motion/motion.hpp"
#include "lcl-ui/core/animation.hpp"

using namespace lcl::motion;

TEST(MotionTest, EveryNamedEasingHasExactEndpoints) {
    for (int value = static_cast<int>(EasingName::Linear);
         value <= static_cast<int>(EasingName::EaseInOutBounce); ++value) {
        const auto easing = static_cast<EasingName>(value);
        EXPECT_FLOAT_EQ(evaluateEasing(easing, 0.0f), 0.0f);
        EXPECT_FLOAT_EQ(evaluateEasing(easing, 1.0f), 1.0f);
    }
}

TEST(MotionTest, CubicBezierSolvesTimeAxisAndStepsHonorPosition) {
    const Easing ease = Easing::cubicBezier(0.42f, 0.0f, 0.58f, 1.0f);
    EXPECT_NEAR(ease.evaluate(0.5f), 0.5f, 0.0001f);
    EXPECT_LT(ease.evaluate(0.25f), 0.25f);

    EXPECT_FLOAT_EQ(Easing::steps(4, StepPosition::End).evaluate(0.24f), 0.0f);
    EXPECT_FLOAT_EQ(Easing::steps(4, StepPosition::End).evaluate(0.25f), 0.25f);
    EXPECT_FLOAT_EQ(Easing::steps(4, StepPosition::Start).evaluate(0.01f), 0.25f);
    EXPECT_FLOAT_EQ(Easing::steps(4, StepPosition::Start).evaluate(0.76f), 1.0f);
}

TEST(MotionTest, SpringSettlesConsistentlyAt60And144Hz) {
    const auto simulate = [](float dt) {
        AnimationEngine engine;
        const auto channel = engine.createChannel({1, 1}, 0.0f);
        engine.animateTo(channel, 1.0f, Motion::spring(0.32f, 0.06f));
        for (float elapsed = 0.0f; elapsed < 0.24f; elapsed += dt) engine.tick(dt);
        return engine.sample(channel);
    };
    const auto at60 = simulate(1.0f / 60.0f);
    const auto at144 = simulate(1.0f / 144.0f);
    EXPECT_NEAR(at60.value, at144.value, 0.02f);
    EXPECT_NEAR(at60.velocity, at144.velocity, 0.2f);
}

TEST(MotionTest, LargeDeltaUsesSubstepsAndRetargetPreservesSpringVelocity) {
    AnimationEngine engine;
    const auto channel = engine.createChannel({7, 9}, 0.0f);
    engine.animateTo(channel, 1.0f, Motion::spring(0.4f, 0.08f));
    engine.tick(0.1f);
    const auto before = engine.sample(channel);
    ASSERT_GT(before.velocity, 0.0f);
    ASSERT_TRUE(engine.retarget(channel, 2.0f));
    EXPECT_FLOAT_EQ(engine.sample(channel).velocity, before.velocity);
    engine.tick(0.1f);
    EXPECT_TRUE(std::isfinite(engine.sample(channel).value));
}

TEST(MotionTest, TweenRetargetStartsAtPresentationValue) {
    AnimationEngine engine;
    const auto channel = engine.createChannel({3, 2}, 0.0f);
    engine.animateTo(channel, 10.0f, Motion::tween(1.0f, Easing::linear()));
    engine.tick(0.4f);
    EXPECT_NEAR(engine.sample(channel).value, 4.0f, 0.001f);
    engine.retarget(channel, 8.0f);
    engine.tick(0.5f);
    EXPECT_NEAR(engine.sample(channel).value, 6.0f, 0.001f);
}

TEST(MotionTest, TimelineControlsRepeatAlternateFillAndCommit) {
    Timeline timeline;
    float presentation = -1.0f;
    float model = 5.0f;
    AnimationOptions options;
    options.durationSec = 1.0f;
    options.repeat = 1;
    options.direction = PlaybackDirection::Alternate;
    options.fill = FillMode::Forwards;
    auto handle = timeline.animate({{0.0f, 0.0f}, {1.0f, 10.0f}}, options,
        [&](float value) { presentation = value; },
        [&] { return model; },
        [&](float value) { model = value; });

    timeline.tick(0.5f);
    EXPECT_NEAR(presentation, 5.0f, 0.001f);
    handle.pause();
    timeline.tick(0.25f);
    EXPECT_NEAR(presentation, 5.0f, 0.001f);
    handle.seek(0.25f);
    EXPECT_NEAR(presentation, 2.5f, 0.001f);
    handle.reverse();
    timeline.tick(0.25f);
    EXPECT_NEAR(handle.progress(), 0.0f, 0.001f);
    handle.finish();
    EXPECT_EQ(handle.state(), PlayState::Finished);
    handle.commitFinalStyles();
    EXPECT_FLOAT_EQ(model, presentation);
}

TEST(MotionTest, LegacyUiAnimationEngineDelegatesOmegaZetaSpec) {
    lcl::ui::AnimationEngine engine;
    const auto channel = engine.createChannel({99, 1}, 0.0f);
    lcl::ui::MotionSpec spec;
    spec.mode = lcl::ui::MotionMode::Spring;
    spec.spring.mass = 1.0f;
    spec.spring.omega = 20.0f;
    spec.spring.zeta = 1.0f;
    ASSERT_TRUE(engine.animateTo(channel, 1.0f, spec));
    for (int index = 0; index < 300 && engine.isActive(channel); ++index)
        engine.tick(1.0f / 240.0f);
    EXPECT_FLOAT_EQ(engine.sample(channel).value, 1.0f);
}
