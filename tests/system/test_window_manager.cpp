#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "system/render/window_manager.hpp"

namespace {

const lcl::render::Window* findWindow(const lcl::render::WindowManager& manager,
                                      uint32_t windowId) {
    const auto& windows = manager.getWindows();
    const auto it = std::find_if(windows.begin(), windows.end(), [windowId](const lcl::render::Window& window) {
        return window.id == windowId;
    });
    return it == windows.end() ? nullptr : &*it;
}

} // namespace

TEST(WindowManagerTest, WindowActionsPreserveRestoreGeometryAndFocus) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    manager.setReservedZone(32, 60, 0, 0);

    const uint32_t first = manager.createWindow("First", 80, 80, 400, 300);
    const uint32_t second = manager.createWindow("Second", 160, 120, 320, 240);
    ASSERT_EQ(manager.getFocusedWindowId(), second);

    ASSERT_TRUE(manager.maximizeWindow(first));
    ASSERT_TRUE(manager.updateAnimations(1.0f));
    const auto* maximized = findWindow(manager, first);
    ASSERT_NE(maximized, nullptr);
    EXPECT_TRUE(maximized->isMaximized);
    EXPECT_FALSE(maximized->isMinimized);
    EXPECT_EQ(maximized->x, 80);
    EXPECT_EQ(maximized->y, 80);
    EXPECT_EQ(maximized->width, 400);
    EXPECT_EQ(maximized->height, 300);
    EXPECT_EQ(maximized->pendingX, 0);
    EXPECT_EQ(maximized->pendingY, 32);
    EXPECT_EQ(maximized->pendingWidth, 1000);
    EXPECT_EQ(maximized->pendingHeight, 608);
    EXPECT_EQ(manager.getFocusedWindowId(), first);
    manager.commitSurfaceGeometry(first, 1000, 608, false, 0, 32);

    ASSERT_TRUE(manager.minimizeWindow(first));
    const auto* minimized = findWindow(manager, first);
    ASSERT_NE(minimized, nullptr);
    EXPECT_TRUE(minimized->isMinimized);
    EXPECT_TRUE(minimized->isMaximized);
    EXPECT_EQ(manager.getFocusedWindowId(), second);

    ASSERT_TRUE(manager.restoreWindow(first));
    const auto* restoredMaximized = findWindow(manager, first);
    ASSERT_NE(restoredMaximized, nullptr);
    EXPECT_FALSE(restoredMaximized->isMinimized);
    EXPECT_TRUE(restoredMaximized->isMaximized);
    EXPECT_EQ(manager.getFocusedWindowId(), first);

    ASSERT_TRUE(manager.toggleMaximizeWindow(first));
    ASSERT_TRUE(manager.updateAnimations(1.0f));
    manager.commitSurfaceGeometry(first, 400, 300, false, 80, 80);
    const auto* restored = findWindow(manager, first);
    ASSERT_NE(restored, nullptr);
    EXPECT_FALSE(restored->isMaximized);
    EXPECT_EQ(restored->x, 80);
    EXPECT_EQ(restored->y, 80);
    EXPECT_EQ(restored->width, 400);
    EXPECT_EQ(restored->height, 300);

    ASSERT_TRUE(manager.beginWindowDrag(first, 24, 18));
    const auto* dragging = findWindow(manager, first);
    ASSERT_NE(dragging, nullptr);
    EXPECT_TRUE(dragging->isDragging());
    EXPECT_EQ(dragging->dragOffsetX, 24);
    EXPECT_EQ(dragging->dragOffsetY, 18);
}

TEST(WindowManagerTest, UnfocusableSystemWindowDoesNotStealApplicationFocus) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));

    const uint32_t appWindow = manager.createWindow("App", 80, 80, 400, 300);
    ASSERT_EQ(manager.getFocusedWindowId(), appWindow);

    const uint32_t panel = manager.createWindow(
        "Panel", 0, 0, 1000, 32, false);
    manager.setWindowLayer(panel, lcl::protocol::LCLWindowLayer::TopMost, true);

    EXPECT_EQ(manager.getFocusedWindowId(), appWindow);
    const auto* panelWindow = findWindow(manager, panel);
    ASSERT_NE(panelWindow, nullptr);
    EXPECT_TRUE(panelWindow->isUnfocusable);
    EXPECT_FALSE(panelWindow->isFocused);
}

TEST(WindowManagerTest, ExitTransitionTransfersFocusWithoutChangingZOrder) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));

    const uint32_t home = manager.createWindow("Home", 0, 0, 1000, 700);
    const uint32_t app = manager.createWindow("App", 0, 0, 1000, 700);
    ASSERT_EQ(manager.getFocusedWindowId(), app);
    ASSERT_EQ(manager.getWindows().back().id, app);

    EXPECT_TRUE(manager.transferFocusFromWindow(app));

    EXPECT_EQ(manager.getFocusedWindowId(), home);
    EXPECT_EQ(manager.getWindows().back().id, app);
    EXPECT_FALSE(findWindow(manager, app)->isFocused);
    EXPECT_TRUE(findWindow(manager, home)->isFocused);
    EXPECT_FALSE(manager.transferFocusFromWindow(app));
}

TEST(WindowManagerTest, WindowCornerStyleOwnsRadiusAndRoundnessTogether) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    const uint32_t id = manager.createWindow("Styled", 80, 80, 400, 300);

    manager.setWindowCornerStyle(id, 20.0f, 3.2f);
    const auto* window = findWindow(manager, id);
    ASSERT_NE(window, nullptr);
    EXPECT_FLOAT_EQ(window->cornerRadius, 20.0f);
    EXPECT_FLOAT_EQ(window->cornerRoundness, 3.2f);
}

TEST(WindowManagerTest, EdgeToEdgeIsExplicitWindowState) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    const uint32_t id = manager.createWindow("Edge-to-edge", 80, 80, 400, 300);

    manager.setEdgeToEdge(id, true);
    const auto* window = findWindow(manager, id);
    ASSERT_NE(window, nullptr);
    EXPECT_TRUE(window->edgeToEdge);
}

TEST(WindowManagerTest, MaximizeRestoreRetargetKeepsPresentedGroupUntilCommit) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    manager.setReservedZone(32, 60, 0, 0);
    const uint32_t id = manager.createWindow("Morph", 80, 90, 400, 300);

    ASSERT_TRUE(manager.maximizeWindow(id));
    const auto* logical = findWindow(manager, id);
    ASSERT_NE(logical, nullptr);
    EXPECT_EQ(logical->x, 80);
    EXPECT_FLOAT_EQ(logical->presentationX, 80.0f);
    manager.updateAnimations(0.10f);
    const float midWidth = findWindow(manager, id)->presentationWidth;
    EXPECT_FLOAT_EQ(midWidth, 400.0f);

    ASSERT_TRUE(manager.restoreWindow(id));
    EXPECT_FLOAT_EQ(findWindow(manager, id)->presentationWidth, midWidth);
    for (int index = 0; index < 240; ++index) manager.updateAnimations(1.0f / 240.0f);
    manager.commitSurfaceGeometry(id, 400, 300, false, 80, 90);
    const auto* restored = findWindow(manager, id);
    EXPECT_EQ(restored->geometryPhase, lcl::render::GeometryPhase::Idle);
    EXPECT_NEAR(restored->presentationX, 80.0f, 0.01f);
    EXPECT_NEAR(restored->presentationY, 90.0f, 0.01f);
    EXPECT_NEAR(restored->presentationWidth, 400.0f, 0.01f);
    EXPECT_NEAR(restored->presentationHeight, 300.0f, 0.01f);
}

TEST(WindowManagerTest, AtomicRetainedResizeUsesCommittedBuffersWithoutMorph) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    manager.setReservedZone(32, 60, 0, 0);
    const uint32_t id = manager.createWindow("Live", 80, 90, 400, 300);

    ASSERT_TRUE(manager.maximizeWindow(id));
    const auto* started = findWindow(manager, id);
    ASSERT_NE(started, nullptr);
    EXPECT_TRUE(started->isMaximized);
    EXPECT_TRUE(started->isAtomicTargetTransitioning());
    EXPECT_EQ(started->width, 400);
    EXPECT_EQ(started->height, 300);

    manager.updateAnimations(1.0f);
    const auto* pending = findWindow(manager, id);
    ASSERT_NE(pending, nullptr);
    EXPECT_EQ(pending->pendingX, 0);
    EXPECT_EQ(pending->pendingY, 32);
    EXPECT_EQ(pending->pendingWidth, 1000);
    EXPECT_EQ(pending->pendingHeight, 608);

    manager.commitSurfaceGeometry(id, 1000, 608, false, 0, 32);
    const auto* committed = findWindow(manager, id);
    ASSERT_NE(committed, nullptr);
    EXPECT_FALSE(committed->isAtomicTargetTransitioning());
    EXPECT_EQ(committed->x, 0);
    EXPECT_EQ(committed->y, 32);
    EXPECT_EQ(committed->width, 1000);
    EXPECT_EQ(committed->height, 608);
    EXPECT_FLOAT_EQ(committed->presentationWidth, 1000.0f);
    EXPECT_FLOAT_EQ(committed->presentationHeight, 608.0f);
}

TEST(WindowManagerTest, IntermediateResizeCommitPreservesNewerPointerTarget) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    const uint32_t id = manager.createWindow("Resize", 80, 90, 400, 300);
    auto& window = manager.getWindowsMutable().back();
    window.pendingX = 60;
    window.pendingWidth = 560;
    window.pendingHeight = 520;
    window.activeResizeEdge = lcl::render::ResizeEdge::BottomLeft;
    window.anchorBottom = window.y + window.pendingHeight;
    window.geometryGeneration = 9;
    window.committedGeometryGeneration = 7;

    EXPECT_TRUE(manager.commitSurfaceGeometry(
        id, 550, 420, true, 70, 90, 8));
    const auto* intermediate = findWindow(manager, id);
    ASSERT_NE(intermediate, nullptr);
    EXPECT_EQ(intermediate->x, 70);
    EXPECT_EQ(intermediate->pendingX, 60);
    EXPECT_EQ(intermediate->width, 550);
    EXPECT_EQ(intermediate->pendingWidth, 560);
    EXPECT_EQ(intermediate->height, 420);
    EXPECT_EQ(intermediate->pendingHeight, 520);
    EXPECT_FLOAT_EQ(intermediate->presentationX, 70.0f);
    EXPECT_FLOAT_EQ(intermediate->presentationWidth, 550.0f);
    EXPECT_FLOAT_EQ(intermediate->presentationHeight, 420.0f);
    EXPECT_EQ(intermediate->committedGeometryGeneration, 8u);
    EXPECT_EQ(intermediate->activeResizeEdge,
              lcl::render::ResizeEdge::BottomLeft);

    EXPECT_FALSE(manager.commitSurfaceGeometry(
        id, 560, 400, true, 80, 90, 7));
    EXPECT_TRUE(manager.commitSurfaceGeometry(
        id, 560, 520, false, 60, 90, 9));
    const auto* final = findWindow(manager, id);
    ASSERT_NE(final, nullptr);
    EXPECT_EQ(final->x, 60);
    EXPECT_EQ(final->pendingX, 60);
    EXPECT_EQ(final->pendingHeight, 520);
    EXPECT_EQ(final->committedGeometryGeneration, 9u);
    EXPECT_EQ(final->activeResizeEdge, lcl::render::ResizeEdge::None);
}

TEST(WindowManagerTest, RestoreMorphIsPreemptedByDragAtThePresentedRect) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    manager.setReservedZone(32, 60, 0, 0);
    const uint32_t id = manager.createWindow("Interruptible", 80, 90, 400, 300);

    ASSERT_TRUE(manager.maximizeWindow(id, false));
    ASSERT_TRUE(manager.restoreWindow(id, true));
    manager.updateAnimations(0.05f);
    const auto before = lcl::render::presentedBounds(*findWindow(manager, id));
    const uint64_t morphGeneration = findWindow(manager, id)->geometryGeneration;

    const auto interaction = manager.beginWindowDrag(id, 24, 18);
    ASSERT_TRUE(interaction);
    EXPECT_GT(interaction.generation, morphGeneration);
    const auto* pinned = findWindow(manager, id);
    ASSERT_NE(pinned, nullptr);
    EXPECT_TRUE(pinned->isDragging());
    EXPECT_FLOAT_EQ(pinned->x, before.x);
    EXPECT_FLOAT_EQ(pinned->width, before.width);

    manager.updateAnimations(1.0f);
    const auto* afterTick = findWindow(manager, id);
    ASSERT_NE(afterTick, nullptr);
    EXPECT_TRUE(afterTick->isDragging());
    EXPECT_FLOAT_EQ(afterTick->presentationX, static_cast<float>(afterTick->x));
    EXPECT_FLOAT_EQ(afterTick->presentationWidth, static_cast<float>(afterTick->width));
    const float dragStartX = afterTick->x;
    const float dragStartY = afterTick->y;

    lcl::core::InputEvent motion{};
    motion.type = lcl::core::InputEventType::PointerMotion;
    motion.absoluteX = static_cast<double>(dragStartX + 24 + 60);
    motion.absoluteY = static_cast<double>(dragStartY + 18 + 30);
    EXPECT_TRUE(manager.processInputEvent(motion));
    EXPECT_FLOAT_EQ(findWindow(manager, id)->x, dragStartX + 60.0f);
    EXPECT_FLOAT_EQ(findWindow(manager, id)->presentationX,
                    static_cast<float>(findWindow(manager, id)->x));
}

TEST(WindowManagerTest, PresentedBoundsOwnResizeHitTestingDuringRetainedTransition) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    const uint32_t id = manager.createWindow("Presented edge", 0, 32, 1000, 668);
    auto& window = manager.getWindowsMutable().back();
    window.geometryPhase = lcl::render::GeometryPhase::AtomicTargetTransition;
    window.presentationX = 100.0f;
    window.presentationY = 100.0f;
    window.presentationWidth = 400.0f;
    window.presentationHeight = 300.0f;

    lcl::core::InputEvent motion{};
    motion.type = lcl::core::InputEventType::PointerMotion;
    motion.absoluteX = 500.0;
    motion.absoluteY = 220.0;
    manager.processInputEvent(motion);

    lcl::core::InputEvent down{};
    down.type = lcl::core::InputEventType::PointerButton;
    down.button = lcl::platform::PointerButton::Left;
    down.pressed = true;
    const auto result = manager.processInputEvent(down);
    ASSERT_TRUE(result.interaction);
    const auto* resizing = findWindow(manager, id);
    ASSERT_NE(resizing, nullptr);
    EXPECT_TRUE(resizing->isResizing());
    EXPECT_EQ(resizing->resizeEdge, lcl::render::ResizeEdge::Right);
    EXPECT_EQ(resizing->x, 100);
    EXPECT_EQ(resizing->width, 400);

    motion.absoluteX = 540.0;
    EXPECT_TRUE(manager.processInputEvent(motion));
    EXPECT_EQ(findWindow(manager, id)->pendingWidth, 440);
}

TEST(WindowManagerTest, NewDragInvalidatesSnapRollbackAndLateResizeCommit) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    const uint32_t id = manager.createWindow("Generation", 80, 90, 400, 300);

    const auto firstDrag = manager.beginWindowDrag(id, 399, 10);
    ASSERT_TRUE(firstDrag);
    lcl::core::InputEvent motion{};
    motion.type = lcl::core::InputEventType::PointerMotion;
    motion.absoluteX = 0.0;
    motion.absoluteY = 100.0;
    manager.processInputEvent(motion);
    lcl::core::InputEvent release{};
    release.type = lcl::core::InputEventType::PointerButton;
    release.button = lcl::platform::PointerButton::Left;
    release.pressed = false;
    manager.processInputEvent(release);
    ASSERT_TRUE(findWindow(manager, id)->isSnappingBack());
    manager.updateAnimations(1.0f / 120.0f);

    const auto secondDrag = manager.beginWindowDrag(id, 10, 10);
    ASSERT_TRUE(secondDrag);
    ASSERT_GT(secondDrag.generation, firstDrag.generation);
    const float pinnedX = findWindow(manager, id)->x;
    const float pinnedY = findWindow(manager, id)->y;
    const float pinnedWidth = findWindow(manager, id)->width;
    manager.updateAnimations(2.0f);
    EXPECT_TRUE(findWindow(manager, id)->isDragging());
    EXPECT_FLOAT_EQ(findWindow(manager, id)->x, pinnedX);

    EXPECT_FALSE(manager.rollbackWindowGeometry(
        id, {0, 32, 1000, 668}, true, false, firstDrag.generation));
    EXPECT_FALSE(manager.commitSurfaceGeometry(
        id, 700, 500, false, 0, 32, firstDrag.generation));
    EXPECT_FLOAT_EQ(findWindow(manager, id)->x, pinnedX);
    EXPECT_FLOAT_EQ(findWindow(manager, id)->y, pinnedY);
    EXPECT_FLOAT_EQ(findWindow(manager, id)->width, pinnedWidth);
}

TEST(WindowManagerRegressionTest, SuperLeftDragMovesWindow) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1920, 1080));
    const uint32_t id = manager.createWindow("TestWindow", 100, 100, 400, 300);

    const auto* win = findWindow(manager, id);
    ASSERT_NE(win, nullptr);
    EXPECT_EQ(win->x, 100);
    EXPECT_EQ(win->y, 100);

    // 1. Move cursor inside window bounds
    lcl::core::InputEvent motion{};
    motion.type = lcl::core::InputEventType::PointerMotion;
    motion.absoluteX = 200.0;
    motion.absoluteY = 200.0;
    manager.processInputEvent(motion);

    // 2. Press Super + Left Button
    lcl::core::InputEvent down{};
    down.type = lcl::core::InputEventType::PointerButton;
    down.button = lcl::platform::PointerButton::Left;
    down.pressed = true;
    down.superPressed = true;
    const auto downResult = manager.processInputEvent(down);
    EXPECT_TRUE(downResult.stateChanged);

    const auto* draggingWin = findWindow(manager, id);
    ASSERT_NE(draggingWin, nullptr);
    EXPECT_TRUE(draggingWin->isDragging());

    manager.clearAllDirty();
    const auto oldBounds = draggingWin->getBounds();

    // 3. Move pointer with drag (+50px X, +30px Y)
    motion.absoluteX = 250.0;
    motion.absoluteY = 230.0;
    EXPECT_TRUE(manager.processInputEvent(motion));

    const auto* movedWin = findWindow(manager, id);
    ASSERT_NE(movedWin, nullptr);
    EXPECT_EQ(movedWin->x, 150);
    EXPECT_EQ(movedWin->y, 130);
    const auto expectedDamage = oldBounds.unionWith(movedWin->getBounds());
    EXPECT_FLOAT_EQ(movedWin->damageRect.x, expectedDamage.x);
    EXPECT_FLOAT_EQ(movedWin->damageRect.y, expectedDamage.y);
    EXPECT_FLOAT_EQ(movedWin->damageRect.width, expectedDamage.width);
    EXPECT_FLOAT_EQ(movedWin->damageRect.height, expectedDamage.height);

    // 4. Release Left Button
    lcl::core::InputEvent up = down;
    up.pressed = false;
    manager.processInputEvent(up);
    EXPECT_FALSE(findWindow(manager, id)->isDragging());
}

TEST(WindowManagerRegressionTest, SuperRightDragResizesWindow) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1920, 1080));
    const uint32_t id = manager.createWindow("TestWindow", 100, 100, 400, 300);

    // 1. Move cursor near bottom-right quadrant of window
    lcl::core::InputEvent motion{};
    motion.type = lcl::core::InputEventType::PointerMotion;
    motion.absoluteX = 450.0;
    motion.absoluteY = 350.0;
    manager.processInputEvent(motion);

    // 2. Press Super + Right Button
    lcl::core::InputEvent down{};
    down.type = lcl::core::InputEventType::PointerButton;
    down.button = lcl::platform::PointerButton::Right;
    down.pressed = true;
    down.superPressed = true;
    const auto downResult = manager.processInputEvent(down);
    EXPECT_TRUE(downResult.stateChanged);

    const auto* resizingWin = findWindow(manager, id);
    ASSERT_NE(resizingWin, nullptr);
    EXPECT_TRUE(resizingWin->isResizing());
    EXPECT_EQ(resizingWin->resizeEdge, lcl::render::ResizeEdge::BottomRight);

    // 3. Move pointer outwards (+60px X, +40px Y)
    motion.absoluteX = 510.0;
    motion.absoluteY = 390.0;
    EXPECT_TRUE(manager.processInputEvent(motion));

    const auto* resizedWin = findWindow(manager, id);
    ASSERT_NE(resizedWin, nullptr);
    EXPECT_EQ(resizedWin->pendingWidth, 460);
    EXPECT_EQ(resizedWin->pendingHeight, 340);

    // 4. Release Right Button
    lcl::core::InputEvent up = down;
    up.pressed = false;
    manager.processInputEvent(up);
    EXPECT_FALSE(findWindow(manager, id)->isResizing());
}

TEST(WindowManagerRegressionTest,
     InteractiveResizeUsesCompositorOwnedContentGrid) {
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1920, 1080));
    const uint32_t id = manager.createWindow(
        "Grid constrained", 100, 100, 400, 300);
    ASSERT_TRUE(manager.setResizeConstraints(
        id, {16.0f, 16.0f, 8.0f, 16.0f}));

    lcl::core::InputEvent motion{};
    motion.type = lcl::core::InputEventType::PointerMotion;
    motion.absoluteX = 450.0;
    motion.absoluteY = 350.0;
    manager.processInputEvent(motion);

    lcl::core::InputEvent down{};
    down.type = lcl::core::InputEventType::PointerButton;
    down.button = lcl::platform::PointerButton::Right;
    down.pressed = true;
    down.superPressed = true;
    ASSERT_TRUE(manager.processInputEvent(down));

    motion.absoluteX = 515.0;
    motion.absoluteY = 397.0;
    ASSERT_TRUE(manager.processInputEvent(motion));

    const auto* resized = findWindow(manager, id);
    ASSERT_NE(resized, nullptr);
    EXPECT_FLOAT_EQ(resized->pendingWidth, 464.0f);
    // SSD frame height = 304 logical content pixels + 32 titlebar pixels.
    EXPECT_FLOAT_EQ(resized->pendingHeight, 336.0f);
    EXPECT_FLOAT_EQ(
        std::fmod(resized->pendingWidth - 16.0f, 8.0f), 0.0f);
    EXPECT_FLOAT_EQ(
        std::fmod(resized->pendingHeight - 32.0f - 16.0f, 16.0f),
        0.0f);
}
