#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/compositor/frame_scheduler.hpp"
#include "core/compositor/input_router.hpp"
#include "core/compositor/surface_registry.hpp"
#include "core/compositor/system_surface_policy.hpp"
#include "core/compositor/window_group_transform.hpp"
#include "core/scene/focus_controller.hpp"
#include "core/scene/scene_registry.hpp"
#include "core/scene/shell_state_broker.hpp"
#include "render/window_manager.hpp"
#include "render/dma_buf_crop.hpp"

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

TEST(SurfaceRegistryTest, RejectsStaleSerialButAcceptsClientConstrainedDimensions) {
    SurfaceRegistry::SurfaceEntry entry;
    entry.initialWidth = 320;
    entry.initialHeight = 200;
    entry.configuredWidth = 640;
    entry.configuredHeight = 480;
    entry.pendingConfigureSerial = 12;

    EXPECT_FALSE(SurfaceRegistry::acceptsBufferCommit(entry, 0));
    EXPECT_FALSE(SurfaceRegistry::acceptsBufferCommit(entry, 11));
    EXPECT_TRUE(SurfaceRegistry::acceptsBufferCommit(entry, 12));
    EXPECT_TRUE(SurfaceRegistry::hasOutstandingConfigure(entry));
    entry.acceptedConfigureSerial = 12;
    EXPECT_FALSE(SurfaceRegistry::hasOutstandingConfigure(entry));
}

TEST(CompositorRendererTest, DmaBufCropShowsContentWithoutScalingTheBacking) {
    const auto crop = lcl::render::makeDmaBufCrop(640, 480, 1920, 1080);
    EXPECT_FLOAT_EQ(crop.uMax, 640.0f / 1920.0f);
    EXPECT_FLOAT_EQ(crop.vMax, 480.0f / 1080.0f);
    const auto exact = lcl::render::makeDmaBufCrop(800, 600, 800, 600);
    EXPECT_FLOAT_EQ(exact.uMax, 1.0f);
    EXPECT_FLOAT_EQ(exact.vMax, 1.0f);
}

TEST(SurfaceRegistryTest, DmaBufSlotIsReleasedOnlyAfterLeavingThePresentedSet) {
    SurfaceRegistry::SurfaceEntry entry;
    entry.dmaBufId = 2;
    entry.dmaBufTexture = 17;
    SurfaceRegistry::releaseBuffer(entry);
    ASSERT_EQ(entry.pendingDmaBufReleases.size(), 1u);
    EXPECT_EQ(entry.pendingDmaBufReleases.front().bufferId, 2u);
    EXPECT_EQ(entry.pendingDmaBufReleases.front().texture, 17u);
    EXPECT_EQ(entry.dmaBufId, 0u);
    EXPECT_EQ(entry.dmaBufTexture, 0u);
}

TEST(SurfaceRegistryTest, GeometryInterruptionInvalidatesRollbackSerialAndPreviousBuffer) {
    SurfaceRegistry::SurfaceEntry entry;
    entry.resizeTransitionPhase =
        SurfaceRegistry::SurfaceEntry::ResizeTransitionPhase::Crossfading;
    entry.rollbackRequested = true;
    entry.pendingConfigureSerial = 9;
    entry.acceptedConfigureSerial = 7;
    entry.previousDmaBufId = 3;
    entry.previousDmaBufTexture = 19;

    SurfaceRegistry::interruptGeometryTransaction(entry, 42);

    EXPECT_EQ(entry.resizeTransitionPhase,
              SurfaceRegistry::SurfaceEntry::ResizeTransitionPhase::None);
    EXPECT_FALSE(entry.rollbackRequested);
    EXPECT_TRUE(entry.resizeBufferReady);
    EXPECT_FLOAT_EQ(entry.resizeCrossfadeProgress, 1.0f);
    EXPECT_EQ(entry.pendingConfigureSerial, 0u);
    EXPECT_FALSE(SurfaceRegistry::acceptsBufferCommit(entry, 7));
    EXPECT_EQ(entry.configuredGeometryGeneration, 42u);
    EXPECT_TRUE(entry.forceConfigure);
    ASSERT_EQ(entry.pendingDmaBufReleases.size(), 1u);
    EXPECT_EQ(entry.pendingDmaBufReleases.front().bufferId, 3u);
    EXPECT_EQ(entry.pendingDmaBufReleases.front().texture, 19u);
}

TEST(SurfaceRegistryTest, SurfaceKeyUsesPidWhenAvailableAndClientFdOtherwise) {
    EXPECT_EQ(SurfaceRegistry::makeKey(7, 19, 3), (static_cast<uint64_t>(19) << 32) | 3u);
    EXPECT_EQ(SurfaceRegistry::makeKey(7, 0, 3), (static_cast<uint64_t>(7) << 32) | 3u);
}

TEST(SurfaceRegistryTest, DisconnectOwnershipIsPerSocketEvenWithinOneProcess) {
    SurfaceRegistry::SurfaceEntry wallpaper;
    wallpaper.clientFd = 10;
    SurfaceRegistry::SurfaceEntry menu;
    menu.clientFd = 11;
    SurfaceRegistry::SurfaceEntry dock;
    dock.clientFd = 12;

    EXPECT_TRUE(SurfaceRegistry::isOwnedByClientConnection(menu, 11));
    EXPECT_FALSE(SurfaceRegistry::isOwnedByClientConnection(wallpaper, 11));
    EXPECT_FALSE(SurfaceRegistry::isOwnedByClientConnection(dock, 11));
}

TEST(InputRouterTest, CoalescesPointerGeometryWhileConfigureIsOutstanding) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);

    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    const uint32_t windowId = manager.createWindow("Resize", 80, 90, 320, 200);
    manager.setDecorationMode(windowId, lcl::render::DecorationMode::None);

    SurfaceRegistry registry;
    const auto surfaceKey = SurfaceRegistry::makeKey(sockets[0], 101, 1);
    auto& surface = registry[surfaceKey];
    surface.windowId = windowId;
    surface.clientFd = sockets[0];
    surface.width = 320;
    surface.height = 200;
    surface.stride = 320 * 4;
    surface.pendingConfigureSerial = 5;
    surface.acceptedConfigureSerial = 5;
    surface.nextConfigureSerial = 6;

    SceneRegistry scenes;
    InputRouter router(manager, registry, scenes);
    auto& window = manager.getWindowsMutable().back();
    window.pendingWidth = 360;
    window.pendingHeight = 240;
    window.geometryPhase = lcl::render::GeometryPhase::Resize;
    router.syncWindowState();

    protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(protocol::recvMsgWithFd(sockets[1], header, payload, receivedFd));
    ASSERT_EQ(header.opcode, protocol::LCLOpcode::ConfigureBounds);
    const auto* first = reinterpret_cast<const protocol::LCLMsgConfigureBounds*>(payload.data());
    ASSERT_EQ(first->configureSerial, 6u);
    EXPECT_EQ(first->width, 360u);
    EXPECT_EQ(first->height, 240u);
    EXPECT_EQ(first->resizeReason, protocol::LCLConfigureResizeReason::Interactive);

    window.pendingWidth = 400;
    window.pendingHeight = 280;
    router.syncWindowState();
    const int flags = fcntl(sockets[1], F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(fcntl(sockets[1], F_SETFL, flags | O_NONBLOCK), 0);
    EXPECT_EQ(protocol::recvPacketWithFd(sockets[1], header, payload, receivedFd),
              protocol::ReceiveStatus::WouldBlock);

    surface.acceptedConfigureSerial = surface.pendingConfigureSerial;
    router.syncWindowState();
    ASSERT_EQ(protocol::recvPacketWithFd(sockets[1], header, payload, receivedFd),
              protocol::ReceiveStatus::Received);
    const auto* coalesced = reinterpret_cast<const protocol::LCLMsgConfigureBounds*>(payload.data());
    EXPECT_EQ(coalesced->configureSerial, 7u);
    EXPECT_EQ(coalesced->width, 400u);
    EXPECT_EQ(coalesced->height, 280u);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(InputRouterTest, LiveResizePublishesLatestSerialAtRefreshCadenceWithWorkspaceBacking) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    const uint32_t windowId = manager.createWindow("Live", 40, 50, 320, 200);
    manager.setDecorationMode(windowId, lcl::render::DecorationMode::None);
    SurfaceRegistry registry;
    auto& surface = registry[SurfaceRegistry::makeKey(sockets[0], 102, 1)];
    surface.windowId = windowId;
    surface.clientFd = sockets[0];
    surface.width = 320;
    surface.height = 200;
    surface.pendingConfigureSerial = 4;
    surface.acceptedConfigureSerial = 4;
    surface.nextConfigureSerial = 5;
    surface.resizePresentation = protocol::LCLResizePresentationMode::Live;
    SceneRegistry scenes;
    InputRouter router(manager, registry, scenes);
    router.setRefreshInterval(std::chrono::nanoseconds(1));
    auto& window = manager.getWindowsMutable().back();
    window.geometryPhase = lcl::render::GeometryPhase::Resize;
    window.pendingWidth = 360;
    window.pendingHeight = 240;
    router.syncWindowState();

    protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int fd = -1;
    ASSERT_TRUE(protocol::recvMsgWithFd(sockets[1], header, payload, fd));
    const auto* first = reinterpret_cast<const protocol::LCLMsgConfigureBounds*>(payload.data());
    EXPECT_EQ(first->configureSerial, 5u);
    EXPECT_EQ(first->backingWidth, 1000u);
    EXPECT_EQ(first->backingHeight, 700u);

    window.pendingWidth = 400;
    window.pendingHeight = 280;
    router.syncWindowState();
    ASSERT_TRUE(protocol::recvMsgWithFd(sockets[1], header, payload, fd));
    const auto* latest = reinterpret_cast<const protocol::LCLMsgConfigureBounds*>(payload.data());
    EXPECT_EQ(latest->configureSerial, 6u);
    EXPECT_EQ(latest->width, 400u);
    EXPECT_EQ(surface.acceptedConfigureSerial, 4u);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(InputRouterTest, ForwardsPointerWhileWindowGeometryMorphs) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);

    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    const uint32_t windowId = manager.createWindow("Morph", 80, 90, 320, 200);
    manager.setDecorationMode(windowId, lcl::render::DecorationMode::None);
    auto& window = manager.getWindowsMutable().back();
    window.geometryPhase = lcl::render::GeometryPhase::Morph;
    window.x = 400;
    window.y = 300;
    window.width = 700;
    window.height = 500;
    window.presentationX = 80.0f;
    window.presentationY = 90.0f;
    window.presentationWidth = 320.0f;
    window.presentationHeight = 200.0f;

    SurfaceRegistry registry;
    const auto surfaceKey = SurfaceRegistry::makeKey(sockets[0], 101, 1);
    auto& surface = registry[surfaceKey];
    surface.windowId = windowId;
    surface.clientFd = sockets[0];
    surface.hasCommittedBuffer = true;
    surface.bufferScale = 1.0f;

    SceneRegistry scenes;
    InputRouter router(manager, registry, scenes);
    InputEvent motion{};
    motion.type = InputEventType::PointerMotion;
    motion.absoluteX = 100.0;
    motion.absoluteY = 110.0;
    EXPECT_TRUE(router.route(motion));

    protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(protocol::recvMsgWithFd(sockets[1], header, payload, receivedFd));
    // The morph may request its outstanding configure first; it must not
    // suppress the following pointer event.
    EXPECT_EQ(header.opcode, protocol::LCLOpcode::ConfigureBounds);
    ASSERT_TRUE(protocol::recvMsgWithFd(sockets[1], header, payload, receivedFd));
    EXPECT_EQ(header.opcode, protocol::LCLOpcode::InputEvent);
    ASSERT_EQ(payload.size(), sizeof(protocol::LCLMsgInputEvent));
    const auto* input = reinterpret_cast<const protocol::LCLMsgInputEvent*>(payload.data());
    EXPECT_EQ(input->type, 3u);
    EXPECT_FLOAT_EQ(input->x, 20.0f);
    EXPECT_FLOAT_EQ(input->y, 20.0f);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(InputRouterTest, ManualGestureCancelsOutstandingGeometryRollback) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);

    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    const uint32_t windowId = manager.createWindow("Gesture", 80, 90, 320, 200);
    SurfaceRegistry registry;
    auto& surface = registry[SurfaceRegistry::makeKey(sockets[0], 103, 1)];
    surface.windowId = windowId;
    surface.clientFd = sockets[0];
    surface.hasCommittedBuffer = true;
    surface.width = 320;
    surface.height = 168;
    surface.configuredWidth = 320;
    surface.configuredHeight = 168;
    surface.pendingConfigureSerial = 9;
    surface.acceptedConfigureSerial = 7;
    surface.nextConfigureSerial = 10;
    surface.resizeTransitionPhase =
        SurfaceRegistry::SurfaceEntry::ResizeTransitionPhase::AwaitingBuffer;
    surface.rollbackRequested = true;

    SceneRegistry scenes;
    InputRouter router(manager, registry, scenes);
    InputEvent move{};
    move.type = InputEventType::PointerMotion;
    move.absoluteX = 200.0;
    move.absoluteY = 100.0;
    router.route(move);

    InputEvent down{};
    down.type = InputEventType::PointerButton;
    down.button = BTN_LEFT;
    down.pressed = true;
    EXPECT_TRUE(router.route(down));

    const auto& window = manager.getWindows().back();
    EXPECT_TRUE(window.isDragging());
    EXPECT_EQ(surface.resizeTransitionPhase,
              SurfaceRegistry::SurfaceEntry::ResizeTransitionPhase::None);
    EXPECT_FALSE(surface.rollbackRequested);
    EXPECT_EQ(surface.pendingConfigureSerial, 10u);
    EXPECT_EQ(surface.configuredGeometryGeneration, window.geometryGeneration);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(InputRouterTest, RestoreVisibilityTransitionBlocksPointerDispatch) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets), 0);

    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(800, 600));
    const uint32_t windowId = manager.createWindow("Restoring", 80, 60, 400, 300);
    SurfaceRegistry registry;
    auto& surface = registry[SurfaceRegistry::makeKey(sockets[0], 104, 1)];
    surface.windowId = windowId;
    surface.clientFd = sockets[0];
    surface.hasCommittedBuffer = true;
    surface.transitionPhase = SurfaceRegistry::SurfaceEntry::TransitionPhase::Restoring;
    SceneRegistry scenes;
    InputRouter router(manager, registry, scenes);

    InputEvent down{};
    down.type = InputEventType::PointerButton;
    down.button = BTN_LEFT;
    down.pressed = true;
    EXPECT_FALSE(router.route(down));
    char byte = 0;
    EXPECT_EQ(recv(sockets[1], &byte, sizeof(byte), MSG_DONTWAIT), -1);
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);

    close(sockets[0]);
    close(sockets[1]);
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

TEST(SystemSurfacePolicyTest, WallpaperPlacementAlwaysUsesCompositorOutputBounds) {
    const auto wallpaper = SystemSurfacePolicyRegistry::policyFor(protocol::LCLSystemSurfaceKind::Wallpaper);
    ASSERT_EQ(wallpaper.placement, SystemSurfacePlacement::OutputBounds);

    int x = 40;
    int y = 50;
    int width = 800;
    int height = 600;
    SystemSurfacePolicyRegistry::applyInitialPlacement(wallpaper, 1920, 1080, x, y, width, height);
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);
    EXPECT_EQ(width, 1920);
    EXPECT_EQ(height, 1080);

    const auto dock = SystemSurfacePolicyRegistry::policyFor(protocol::LCLSystemSurfaceKind::Dock);
    x = 22;
    y = 33;
    width = 444;
    height = 55;
    SystemSurfacePolicyRegistry::applyInitialPlacement(dock, 1920, 1080, x, y, width, height);
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 1025);
    EXPECT_EQ(width, 1920);
    EXPECT_EQ(height, 55);

    const auto menu = SystemSurfacePolicyRegistry::policyFor(protocol::LCLSystemSurfaceKind::MenuBar);
    x = 22;
    y = 33;
    width = 444;
    height = 32;
    SystemSurfacePolicyRegistry::applyInitialPlacement(menu, 1920, 1080, x, y, width, height);
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);
    EXPECT_EQ(width, 1920);
    EXPECT_EQ(height, 32);
}

TEST(WindowGroupTransformTest, ChromeAndClientShareOneSubpixelAnimatedFrame) {
    render::Window window{};
    window.x = 81;
    window.y = 63;
    window.width = 541;
    window.height = 367;

    const auto group = makeWindowGroupTransform(window, 32, 0.963f);
    EXPECT_NEAR(group.titleHeight, 32.0f * group.scale, 0.0001f);
    EXPECT_NEAR(group.y + group.titleHeight + (group.height - group.titleHeight),
                group.y + group.height, 0.0001f);
    EXPECT_NEAR(group.x + group.width,
                static_cast<float>(window.x) +
                    (static_cast<float>(window.width) + group.width) * 0.5f,
                0.0001f);
    EXPECT_NE(group.width, std::round(group.width));

    const auto resting = makeWindowGroupTransform(window, 32, 1.0f);
    EXPECT_FLOAT_EQ(resting.x, std::round(resting.x));
    EXPECT_FLOAT_EQ(resting.y, std::round(resting.y));
    EXPECT_FLOAT_EQ(resting.width, std::round(resting.width));
    EXPECT_FLOAT_EQ(resting.height, std::round(resting.height));
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

TEST(FrameSchedulerTest, MinimizeAndRestoreTransitionsOwnVisibilityEndpoints) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::time_point{};
    FrameScheduler scheduler;
    scheduler.reset(start);

    SurfaceRegistry registry;
    auto& surface = registry[1];
    surface.transitionPhase = SurfaceRegistry::SurfaceEntry::TransitionPhase::Minimizing;
    surface.transitionDurationSec = 0.18f;
    surface.transitionOpacity = 1.0f;
    surface.transitionScale = 1.0f;

    EXPECT_TRUE(scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(45)));
    EXPECT_LT(surface.transitionOpacity, 1.0f);
    EXPECT_GT(surface.transitionOpacity, 0.0f);
    EXPECT_LT(surface.transitionScale, 1.0f);
    EXPECT_FALSE(surface.pendingMinimize);

    scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(90));
    scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(135));
    scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(180));
    EXPECT_EQ(surface.transitionPhase, SurfaceRegistry::SurfaceEntry::TransitionPhase::None);
    EXPECT_TRUE(surface.pendingMinimize);
    EXPECT_FLOAT_EQ(surface.transitionOpacity, 0.0f);
    EXPECT_FLOAT_EQ(surface.transitionScale, 0.92f);

    surface.pendingMinimize = false;
    surface.transitionPhase = SurfaceRegistry::SurfaceEntry::TransitionPhase::Restoring;
    surface.transitionElapsedSec = 0.0f;
    surface.transitionDurationSec = 0.22f;
    surface.transitionOpacity = 0.0f;
    surface.transitionScale = 0.92f;
    scheduler.reset(start);

    EXPECT_TRUE(scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(50)));
    EXPECT_GT(surface.transitionOpacity, 0.0f);
    EXPECT_LT(surface.transitionOpacity, 1.0f);
    EXPECT_GT(surface.transitionScale, 0.92f);
    EXPECT_LT(surface.transitionScale, 1.0f);

    scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(100));
    scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(150));
    scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(200));
    scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(250));
    EXPECT_EQ(surface.transitionPhase, SurfaceRegistry::SurfaceEntry::TransitionPhase::None);
    EXPECT_FLOAT_EQ(surface.transitionOpacity, 1.0f);
    EXPECT_FLOAT_EQ(surface.transitionScale, 1.0f);
}

TEST(FrameSchedulerTest, ResizeCrossfadeAndTimeoutOwnBufferLifecycle) {
    SurfaceRegistry registry;
    auto& crossfade = registry[1];
    crossfade.resizeTransitionPhase = SurfaceRegistry::SurfaceEntry::ResizeTransitionPhase::Crossfading;
    crossfade.resizeCrossfadeProgress = 0.0f;
    crossfade.resizeBufferReady = false;

    FrameScheduler scheduler;
    const auto start = std::chrono::steady_clock::now();
    scheduler.reset(start);
    EXPECT_TRUE(scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(50)));
    EXPECT_GT(crossfade.resizeCrossfadeProgress, 0.0f);
    EXPECT_LT(crossfade.resizeCrossfadeProgress, 1.0f);
    scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(100));
    EXPECT_EQ(crossfade.resizeTransitionPhase, SurfaceRegistry::SurfaceEntry::ResizeTransitionPhase::None);
    EXPECT_TRUE(crossfade.resizeBufferReady);

    auto& timeout = registry[2];
    timeout.resizeTransitionPhase = SurfaceRegistry::SurfaceEntry::ResizeTransitionPhase::AwaitingBuffer;
    timeout.resizeDeadline = start + std::chrono::milliseconds(750);
    timeout.pendingConfigureSerial = 9;
    timeout.acceptedConfigureSerial = 7;
    scheduler.advanceTransitions(registry, start + std::chrono::milliseconds(751));
    EXPECT_TRUE(timeout.rollbackRequested);
    EXPECT_EQ(timeout.pendingConfigureSerial, 7u);
    EXPECT_TRUE(timeout.resizeBufferReady);
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

TEST(SystemSurfacePolicyTest, MenuAndDockAreCompositorOwnedUnfocusablePanels) {
    const auto menu = SystemSurfacePolicyRegistry::policyFor(protocol::LCLSystemSurfaceKind::MenuBar);
    const auto dock = SystemSurfacePolicyRegistry::policyFor(protocol::LCLSystemSurfaceKind::Dock);
    const auto wallpaper = SystemSurfacePolicyRegistry::policyFor(protocol::LCLSystemSurfaceKind::Wallpaper);

    EXPECT_TRUE(menu.isSystemSurface);
    EXPECT_EQ(menu.layer, protocol::LCLWindowLayer::TopMost);
    EXPECT_TRUE(menu.unfocusable);
    EXPECT_TRUE(menu.reservesWorkArea);
    EXPECT_FALSE(menu.insetBorderEnabled);

    EXPECT_TRUE(dock.isSystemSurface);
    EXPECT_TRUE(dock.reservesWorkArea);
    EXPECT_TRUE(wallpaper.isSystemSurface);
    EXPECT_EQ(wallpaper.layer, protocol::LCLWindowLayer::Bottom);
    EXPECT_FALSE(wallpaper.reservesWorkArea);
    EXPECT_FALSE(SystemSurfacePolicyRegistry::isValidKind(protocol::LCLSystemSurfaceKind::None));
}

} // namespace lcl::core
