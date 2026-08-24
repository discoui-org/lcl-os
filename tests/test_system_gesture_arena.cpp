#include <gtest/gtest.h>

#include "core/input/system_gesture_arena.hpp"

namespace lcl::core {
namespace {

InputEvent touchEvent(InputEventType type, float x, float y,
                      uint32_t pointerId, bool pressed = false) {
    InputEvent event{};
    event.type = type;
    event.source = lcl::platform::PointerSource::Touch;
    event.pointerId = pointerId;
    event.absoluteX = x;
    event.absoluteY = y;
    event.button = lcl::platform::PointerButton::Left;
    event.pressed = pressed;
    return event;
}

TEST(SystemGestureArenaTest, ClaimsVerticalBottomEdgeSwipeAndEmitsHomeOnRelease) {
    SystemGestureArena arena;

    EXPECT_EQ(arena.process(
                  touchEvent(InputEventType::PointerButton, 100.0f, 795.0f, 9, true),
                  800.0f),
              SystemGestureDecision::Tracking);
    EXPECT_EQ(arena.process(
                  touchEvent(InputEventType::PointerMotion, 102.0f, 770.0f, 9),
                  800.0f),
              SystemGestureDecision::Claim);
    EXPECT_TRUE(arena.hasClaimed(9));
    EXPECT_EQ(arena.process(
                  touchEvent(InputEventType::PointerMotion, 104.0f, 730.0f, 9),
                  800.0f),
              SystemGestureDecision::Update);
    EXPECT_EQ(arena.process(
                  touchEvent(InputEventType::PointerButton, 104.0f, 730.0f, 9, false),
                  800.0f),
              SystemGestureDecision::Home);
    EXPECT_FALSE(arena.isTracking(9));
}

TEST(SystemGestureArenaTest, ReportsClaimedGestureProgressBeforeRelease) {
    SystemGestureArena arena;
    SystemGestureProgress progress{};

    EXPECT_EQ(arena.process(
                  touchEvent(InputEventType::PointerButton, 120.0f, 795.0f, 10, true),
                  800.0f, &progress),
              SystemGestureDecision::Tracking);
    EXPECT_EQ(arena.process(
                  touchEvent(InputEventType::PointerMotion, 126.0f, 750.0f, 10),
                  800.0f, &progress),
              SystemGestureDecision::Claim);
    EXPECT_FLOAT_EQ(progress.startX, 120.0f);
    EXPECT_FLOAT_EQ(progress.startY, 795.0f);
    EXPECT_FLOAT_EQ(progress.x, 126.0f);
    EXPECT_FLOAT_EQ(progress.y, 750.0f);
}

TEST(SystemGestureArenaTest, LeavesHorizontalAndNonEdgeStreamsToApplications) {
    SystemGestureArena arena;

    EXPECT_EQ(arena.process(
                  touchEvent(InputEventType::PointerButton, 100.0f, 700.0f, 3, true),
                  800.0f),
              SystemGestureDecision::PassThrough);
    EXPECT_EQ(arena.process(
                  touchEvent(InputEventType::PointerButton, 100.0f, 795.0f, 4, true),
                  800.0f),
              SystemGestureDecision::Tracking);
    EXPECT_EQ(arena.process(
                  touchEvent(InputEventType::PointerMotion, 125.0f, 792.0f, 4),
                  800.0f),
              SystemGestureDecision::PassThrough);
    EXPECT_FALSE(arena.isTracking(4));
}

TEST(SystemGestureArenaTest, IgnoresOtherPointerIdsAndResetsOnCancel) {
    SystemGestureArena arena;

    EXPECT_EQ(arena.process(
                  touchEvent(InputEventType::PointerButton, 100.0f, 795.0f, 5, true),
                  800.0f),
              SystemGestureDecision::Tracking);
    EXPECT_EQ(arena.process(
                  touchEvent(InputEventType::PointerMotion, 100.0f, 760.0f, 6),
                  800.0f),
              SystemGestureDecision::PassThrough);
    EXPECT_TRUE(arena.isTracking(5));
    EXPECT_EQ(arena.process(
                  touchEvent(InputEventType::PointerCancel, 100.0f, 795.0f, 5),
                  800.0f),
              SystemGestureDecision::PassThrough);
    EXPECT_FALSE(arena.isTracking(5));
}

} // namespace
} // namespace lcl::core
