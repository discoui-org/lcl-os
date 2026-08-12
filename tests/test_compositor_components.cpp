#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "core/compositor/frame_scheduler.hpp"
#include "core/compositor/surface_registry.hpp"
#include "core/scene/focus_controller.hpp"
#include "core/scene/scene_registry.hpp"
#include "core/scene/shell_state_broker.hpp"
#include "render/window_manager.hpp"

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

TEST(SceneStateTest, RegistryReflectsWindowLifecycleAndFocusWithoutOwningWindowManager) {
    render::WindowManager windowManager;
    ASSERT_TRUE(windowManager.initialize(1280, 720));

    const uint32_t terminalWindow = windowManager.createWindow("Terminal", 80, 60, 540, 360);
    const uint32_t demoWindow = windowManager.createWindow("Demo", 140, 100, 480, 320);

    SceneRegistry scenes;
    const auto terminalScene = scenes.mapClientSurface(
        SurfaceRegistry::makeKey(7, 101, 1), 101, terminalWindow, "terminal", "Terminal");
    const auto demoScene = scenes.mapClientSurface(
        SurfaceRegistry::makeKey(8, 102, 1), 102, demoWindow, "ui-demo", "Demo");
    scenes.reconcileWindowState(windowManager);

    ASSERT_EQ(scenes.snapshot().size(), 2u);
    EXPECT_EQ(scenes.sceneIdForWindow(terminalWindow), terminalScene);
    EXPECT_EQ(scenes.sceneIdForWindow(demoWindow), demoScene);
    const auto* demo = scenes.find(demoScene);
    ASSERT_NE(demo, nullptr);
    EXPECT_EQ(demo->x, 140);
    EXPECT_EQ(demo->height, 320);
    EXPECT_EQ(demo->visibility, SceneVisibility::Visible);

    FocusController focus;
    const auto initialFocus = focus.reconcile(scenes, windowManager);
    ASSERT_TRUE(initialFocus.has_value());
    EXPECT_EQ(initialFocus->activeSceneId, demoScene);

    ASSERT_TRUE(windowManager.minimizeWindow(demoWindow));
    scenes.reconcileWindowState(windowManager);
    const auto focusedAfterMinimize = focus.reconcile(scenes, windowManager);
    ASSERT_TRUE(focusedAfterMinimize.has_value());
    EXPECT_EQ(focusedAfterMinimize->activeSceneId, terminalScene);
    demo = scenes.find(demoScene);
    ASSERT_NE(demo, nullptr);
    EXPECT_EQ(demo->visibility, SceneVisibility::Minimized);
}

TEST(SceneStateTest, BrokerProducesMonotonicDeltasAndSnapshotFallback) {
    render::WindowManager windowManager;
    ASSERT_TRUE(windowManager.initialize(1280, 720));
    const uint32_t windowId = windowManager.createWindow("Terminal", 80, 60, 540, 360);

    SceneRegistry scenes;
    const auto surfaceKey = SurfaceRegistry::makeKey(7, 101, 1);
    const auto sceneId = scenes.mapClientSurface(surfaceKey, 101, windowId, "terminal", "Terminal");
    scenes.reconcileWindowState(windowManager);

    FocusController focus;
    ShellStateBroker broker;
    for (const auto& change : scenes.takePendingChanges()) {
        broker.publish(change);
    }
    const auto focusChange = focus.reconcile(scenes, windowManager);
    ASSERT_TRUE(focusChange.has_value());
    broker.publish(*focusChange);

    const auto initial = broker.snapshot(scenes, focus);
    ASSERT_GE(initial.revision, 2u);
    ASSERT_EQ(initial.scenes.size(), 1u);
    EXPECT_EQ(initial.scenes.front().id, sceneId);
    EXPECT_EQ(initial.focus.activeSceneId, sceneId);

    const auto replay = broker.deltasSince(0);
    EXPECT_FALSE(replay.requiresSnapshot);
    EXPECT_EQ(replay.revision, initial.revision);
    ASSERT_FALSE(replay.deltas.empty());
    EXPECT_EQ(replay.deltas.back().kind, ShellStateDelta::Kind::FocusChanged);
    EXPECT_EQ(replay.deltas.back().focus.activeSceneId, sceneId);

    scenes.markClosing(surfaceKey);
    for (const auto& change : scenes.takePendingChanges()) {
        broker.publish(change);
    }
    const uint64_t closingRevision = broker.revision();
    scenes.removeSurface(surfaceKey);
    for (const auto& change : scenes.takePendingChanges()) {
        broker.publish(change);
    }

    const auto afterClosing = broker.deltasSince(closingRevision);
    ASSERT_EQ(afterClosing.deltas.size(), 1u);
    EXPECT_EQ(afterClosing.deltas.front().kind, ShellStateDelta::Kind::SceneRemoved);
    EXPECT_EQ(afterClosing.deltas.front().scene.id, sceneId);
    EXPECT_TRUE(broker.deltasSince(broker.revision() + 1).requiresSnapshot);
}

} // namespace lcl::core
