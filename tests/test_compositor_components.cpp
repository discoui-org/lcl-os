#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "core/compositor/frame_scheduler.hpp"
#include "core/compositor/surface_registry.hpp"

namespace lcl::core {

TEST(SurfaceRegistryTest, SnapshotProvidesReadOnlyViewsWithoutCopyingEntries) {
    SurfaceRegistry registry;
    const auto firstKey = SurfaceRegistry::makeKey(9, 42, 1);
    const auto secondKey = SurfaceRegistry::makeKey(9, 42, 2);

    registry[firstKey].windowId = 11;
    registry[secondKey].windowId = 12;

    const auto snapshot = registry.snapshot();
    ASSERT_EQ(snapshot.size(), 2u);

    const auto found = std::find_if(snapshot.begin(), snapshot.end(), [firstKey](const auto& item) {
        return item.key == firstKey;
    });
    ASSERT_NE(found, snapshot.end());
    ASSERT_NE(found->entry, nullptr);
    EXPECT_EQ(found->entry, &registry.find(firstKey)->second);
    EXPECT_EQ(found->entry->windowId, 11u);
}

TEST(SurfaceRegistryTest, SurfaceKeyUsesPidWhenAvailableAndClientFdOtherwise) {
    EXPECT_EQ(SurfaceRegistry::makeKey(7, 19, 3), (static_cast<uint64_t>(19) << 32) | 3u);
    EXPECT_EQ(SurfaceRegistry::makeKey(7, 0, 3), (static_cast<uint64_t>(7) << 32) | 3u);
}

TEST(SurfaceRegistryTest, ErasingAnEntryClosesItsOwnedDescriptor) {
    const int descriptor = eventfd(0, EFD_CLOEXEC);
    ASSERT_GE(descriptor, 0);

    SurfaceRegistry registry;
    registry[1].shmFd = descriptor;
    EXPECT_EQ(registry.erase(1), 1u);

    errno = 0;
    EXPECT_EQ(fcntl(descriptor, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(FrameSchedulerTest, AdvancesEnteringAndClosingTransitionsAtBoundedDelta) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::time_point{};
    FrameScheduler scheduler;
    scheduler.reset(start);

    SurfaceRegistry registry;
    auto& entering = registry[1];
    entering.transitionPhase = SurfaceRegistry::SurfaceEntry::TransitionPhase::Entering;
    entering.transitionDurationSec = 0.20f;
    entering.transitionOpacity = 0.0f;
    entering.transitionScale = 0.96f;

    EXPECT_TRUE(scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(50)));
    EXPECT_NEAR(entering.transitionOpacity, 0.578125f, 0.0001f);
    EXPECT_NEAR(entering.transitionScale, 0.983125f, 0.0001f);

    scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(100));
    scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(150));
    scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(200));
    EXPECT_EQ(entering.transitionPhase, SurfaceRegistry::SurfaceEntry::TransitionPhase::None);
    EXPECT_FLOAT_EQ(entering.transitionOpacity, 1.0f);
    EXPECT_FLOAT_EQ(entering.transitionScale, 1.0f);

    auto& closing = registry[2];
    closing.transitionPhase = SurfaceRegistry::SurfaceEntry::TransitionPhase::Closing;
    closing.transitionDurationSec = 0.20f;
    scheduler.reset(start);
    scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(50));
    EXPECT_NEAR(closing.transitionOpacity, 0.984375f, 0.0001f);
    scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(100));
    scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(150));
    scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(200));
    EXPECT_TRUE(closing.pendingDestroy);
}

TEST(FrameSchedulerTest, CursorBlinkAndFrameBudgetUseTheExistingCadence) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::time_point{};
    FrameScheduler scheduler;
    scheduler.reset(start);

    EXPECT_FALSE(scheduler.cursorBlinkDue(start + std::chrono::milliseconds(499)));
    EXPECT_TRUE(scheduler.cursorBlinkDue(start + std::chrono::milliseconds(500)));
    EXPECT_FALSE(scheduler.cursorBlinkDue(start + std::chrono::milliseconds(501)));
    EXPECT_EQ(scheduler.frameBudgetForHz(60), std::chrono::microseconds(16166));
}

} // namespace lcl::core
