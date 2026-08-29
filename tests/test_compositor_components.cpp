#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "apps/mobile_shell/gesture_indicator.hpp"
#include "core/compositor/frame_scheduler.hpp"
#include "core/compositor/compositor_renderer.hpp"
#include "core/compositor/double_inset_border.hpp"
#include "core/compositor/effect_region_geometry.hpp"
#include "core/compositor/input_router.hpp"
#include "core/compositor/layer_feedback_handler.hpp"
#include "core/compositor/mobile_launch_backdrop.hpp"
#include "core/compositor/popup_surface_geometry.hpp"
#include "core/compositor/surface_registry.hpp"
#include "core/compositor/surface_damage_geometry.hpp"
#include "core/compositor/surface_transaction_coordinator.hpp"
#include "core/compositor/system_surface_policy.hpp"
#include "render/window_group_transform.hpp"
#include "core/scene/focus_controller.hpp"
#include "core/scene/scene_registry.hpp"
#include "core/scene/shell_state_broker.hpp"
#include "lcl-theme/theme.hpp"
#include "render/window_manager.hpp"
#include "render/dma_buf_crop.hpp"
#include "render/raster_destination.hpp"

namespace lcl::core {

TEST(LayerFeedbackHandlerTest, RejectedStaleLayerReturnsExactFrameCredit) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets), 0);

    raster_protocol::LayerReady layer{};
    layer.grant.surfaceId = 42;
    layer.configureSerial = 17;
    layer.frameSerial = 23;
    layer.geometryGeneration = 31;
    ASSERT_TRUE(LayerFeedbackHandler::discard(
        sockets[0], layer, protocol::LCLFrameDiscardReason::Superseded));

    protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(protocol::recvMsgWithFd(
        sockets[1], header, payload, receivedFd));
    EXPECT_EQ(receivedFd, -1);
    ASSERT_EQ(header.opcode, protocol::LCLOpcode::FrameDiscarded);
    ASSERT_EQ(payload.size(), sizeof(protocol::LCLMsgFrameDiscarded));
    const auto* discarded = reinterpret_cast<const
        protocol::LCLMsgFrameDiscarded*>(payload.data());
    EXPECT_EQ(discarded->surfaceId, 42u);
    EXPECT_EQ(discarded->configureSerial, 17u);
    EXPECT_EQ(discarded->frameSerial, 23u);
    EXPECT_EQ(discarded->geometryGeneration, 31u);
    EXPECT_EQ(discarded->reason,
              protocol::LCLFrameDiscardReason::Superseded);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(MobileLaunchBackdropTest, UsesScaleAndBrightnessWithoutEffectOpacity) {
    const auto start = resolveMobileLaunchBackdrop(0.0f);
    EXPECT_FLOAT_EQ(start.wallpaperScale, 1.0f);
    EXPECT_FLOAT_EQ(start.homeScale, 1.0f);
    EXPECT_FLOAT_EQ(start.brightness, 1.0f);

    const auto halfway = resolveMobileLaunchBackdrop(0.5f);
    EXPECT_FLOAT_EQ(halfway.progress, 0.5f);
    EXPECT_FLOAT_EQ(halfway.wallpaperScale, 1.03f);
    EXPECT_FLOAT_EQ(halfway.homeScale, 0.97f);
    EXPECT_FLOAT_EQ(halfway.brightness, 0.91f);

    const auto covered = resolveMobileLaunchBackdrop(1.0f);
    EXPECT_FLOAT_EQ(covered.wallpaperScale, 1.06f);
    EXPECT_FLOAT_EQ(covered.homeScale, 0.94f);
    EXPECT_FLOAT_EQ(covered.brightness, 0.82f);
}

TEST(MobileWindowDecorationTest, GesturePillScalesFromWindowWidthAndStaysBottomAnchored) {
    const auto reference = lcl::mobile::layoutGestureIndicator(
        {10.0f, 20.0f, 393.0f, 852.0f});
    EXPECT_FLOAT_EQ(reference.scale, 1.0f);
    EXPECT_FLOAT_EQ(reference.bounds.x, 139.5f);
    EXPECT_FLOAT_EQ(reference.bounds.y, 859.0f);
    EXPECT_FLOAT_EQ(reference.bounds.width, 134.0f);
    EXPECT_FLOAT_EQ(reference.bounds.height, 5.0f);

    const auto doubled = lcl::mobile::layoutGestureIndicator(
        {10.0f, 20.0f, 786.0f, 852.0f});
    EXPECT_FLOAT_EQ(doubled.scale, 2.0f);
    EXPECT_FLOAT_EQ(doubled.bounds.x, 269.0f);
    EXPECT_FLOAT_EQ(doubled.bounds.y, 846.0f);
    EXPECT_FLOAT_EQ(doubled.bounds.width, 268.0f);
    EXPECT_FLOAT_EQ(doubled.bounds.height, 10.0f);
}

TEST(MobileWindowDecorationTest, GesturePillIsOneRoundedDecorationPath) {
    const auto list = lcl::mobile::buildGestureIndicatorDisplayList(
        {0.0f, 0.0f, 393.0f, 852.0f}, 1.0f);
    ASSERT_EQ(list.commands().size(), 1u);
    const auto* command = std::get_if<graphics::DrawPathCommand>(
        &list.commands().front());
    ASSERT_NE(command, nullptr);
    const auto* primitive = command->path.primitive();
    ASSERT_NE(primitive, nullptr);
    EXPECT_EQ(primitive->kind, graphics::PathPrimitiveKind::RRect);
    EXPECT_FLOAT_EQ(primitive->bounds.width, 134.0f);
    EXPECT_FLOAT_EQ(primitive->bounds.height, 5.0f);
    EXPECT_FLOAT_EQ(primitive->radiusX, 2.5f);
    EXPECT_EQ(command->paint.color.a, 235u);
}

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

TEST(SurfaceRegistryTest, AtomicConfigureWaitsWithoutDuplicatingBuffers) {
    SurfaceRegistry registry;
    const auto parentKey = SurfaceRegistry::makeKey(10, 70, 1);
    const auto frameKey = SurfaceRegistry::makeKey(11, 71, 2);
    auto& parent = registry[parentKey];
    parent.windowId = 9;
    parent.pendingConfigureSerial = 5;
    parent.acceptedConfigureSerial = 4;
    parent.configuredGeometryGeneration = 22;
    auto& frame = registry[frameKey];
    frame.windowId = 9;
    frame.pendingConfigureSerial = 8;
    frame.acceptedConfigureSerial = 7;
    frame.configuredGeometryGeneration = 22;

    ASSERT_TRUE(SurfaceTransactionCoordinator::begin(
        registry, 9, 22, {parentKey, frameKey}));
    EXPECT_TRUE(SurfaceTransactionCoordinator::promoteReady(registry));

    parent.acceptedConfigureSerial = 5;
    SurfaceRegistry::queuePresentation(parent, 5);
    EXPECT_TRUE(SurfaceTransactionCoordinator::promoteReady(registry));
    frame.atomicConfigureIssued = true;
    frame.acceptedConfigureSerial = 8;
    SurfaceRegistry::queuePresentation(frame, 8);
    EXPECT_FALSE(SurfaceTransactionCoordinator::promoteReady(registry));
    EXPECT_EQ(parent.atomicConfigureGeneration, 0u);
    EXPECT_EQ(frame.atomicConfigureGeneration, 0u);
    EXPECT_EQ(parent.presentationSerial, 5u);
    EXPECT_EQ(frame.presentationSerial, 8u);
}

TEST(SurfaceRegistryTest, AtomicConfigureWithoutDeadlineNeverOpensTornGroup) {
    SurfaceRegistry registry;
    const auto parentKey = SurfaceRegistry::makeKey(12, 72, 1);
    const auto frameKey = SurfaceRegistry::makeKey(13, 73, 2);
    auto& parent = registry[parentKey];
    parent.windowId = 10;
    parent.pendingConfigureSerial = 2;
    parent.acceptedConfigureSerial = 1;
    parent.configuredGeometryGeneration = 23;
    auto& frame = registry[frameKey];
    frame.windowId = 10;
    frame.pendingConfigureSerial = 3;
    frame.acceptedConfigureSerial = 2;
    frame.configuredGeometryGeneration = 23;

    ASSERT_TRUE(SurfaceTransactionCoordinator::begin(
        registry, 10, 23, {parentKey, frameKey}));
    EXPECT_TRUE(SurfaceTransactionCoordinator::promoteReady(registry));
    EXPECT_EQ(parent.atomicConfigureGeneration, 23u);
    EXPECT_EQ(frame.atomicConfigureGeneration, 23u);
}

TEST(SurfaceRegistryTest, AtomicReplacementChangesMembershipOnlyAfterValidation) {
    SurfaceRegistry registry;
    const auto parentKey = SurfaceRegistry::makeKey(14, 74, 1);
    const auto frameKey = SurfaceRegistry::makeKey(15, 75, 2);
    registry[parentKey].windowId = 11;
    registry[frameKey].windowId = 11;

    ASSERT_TRUE(SurfaceTransactionCoordinator::begin(
        registry, 11, 30, {parentKey, frameKey}));
    const auto missingKey = SurfaceRegistry::makeKey(16, 76, 3);
    EXPECT_FALSE(SurfaceTransactionCoordinator::begin(
        registry, 11, 31, {parentKey, missingKey}));
    EXPECT_EQ(registry[parentKey].atomicConfigureGeneration, 30u);
    EXPECT_EQ(registry[frameKey].atomicConfigureGeneration, 30u);

    ASSERT_TRUE(SurfaceTransactionCoordinator::begin(
        registry, 11, 31, {parentKey}));
    EXPECT_EQ(registry[parentKey].atomicConfigureGeneration, 31u);
    EXPECT_EQ(registry[parentKey].atomicConfigureParticipantCount, 1u);
    EXPECT_EQ(registry[frameKey].atomicConfigureGeneration, 0u);
}

TEST(SurfaceRegistryTest, PopupChildrenKeepStableParentLocalStackOrder) {
    SurfaceRegistry registry;
    const auto parentKey = SurfaceRegistry::makeKey(7, 42, 1);
    const auto firstPopupKey = SurfaceRegistry::makeKey(8, 42, 2);
    const auto secondPopupKey = SurfaceRegistry::makeKey(9, 42, 3);
    registry[parentKey].windowId = 11;
    registry[firstPopupKey].parentSurfaceKey = parentKey;
    registry[firstPopupKey].popupOrder = registry.allocatePopupOrder();
    registry[secondPopupKey].parentSurfaceKey = parentKey;
    registry[secondPopupKey].popupOrder = registry.allocatePopupOrder();

    const auto children = registry.popupChildren(parentKey);
    ASSERT_EQ(children.size(), 2u);
    EXPECT_EQ(children[0], firstPopupKey);
    EXPECT_EQ(children[1], secondPopupKey);

    registry.erase(secondPopupKey);
    EXPECT_TRUE(registry.contains(parentKey));
    ASSERT_EQ(registry.popupChildren(parentKey).size(), 1u);
}

TEST(SurfaceRegistryTest, KeyboardSurfaceFocusReturnsToParentAndNeverStaysStale) {
    SurfaceRegistry registry;
    const auto parentKey = SurfaceRegistry::makeKey(7, 42, 1);
    const auto popupKey = SurfaceRegistry::makeKey(8, 42, 2);
    registry[parentKey].windowId = 11;
    registry[popupKey].parentSurfaceKey = parentKey;

    ASSERT_TRUE(registry.focusKeyboardSurface(popupKey));
    EXPECT_EQ(registry.keyboardFocusSurface(), popupKey);
    registry.releaseKeyboardFocus(popupKey);
    EXPECT_EQ(registry.keyboardFocusSurface(), parentKey);

    ASSERT_TRUE(registry.focusKeyboardSurface(popupKey));
    registry.erase(popupKey);
    EXPECT_EQ(registry.keyboardFocusSurface(), parentKey);
    registry.erase(parentKey);
    EXPECT_EQ(registry.keyboardFocusSurface(), 0u);
    EXPECT_FALSE(registry.focusKeyboardSurface(popupKey));
}

TEST(PopupSurfaceTest, GeometryFollowsParentAndCanExtendPastWindowBounds) {
    render::Window parentWindow{};
    parentWindow.x = 100;
    parentWindow.y = 80;
    parentWindow.width = 200;
    parentWindow.height = 140;
    parentWindow.decorationMode = render::DecorationMode::None;
    parentWindow.presentationInitialized = false;

    SurfaceRegistry::SurfaceEntry parentSurface;
    SurfaceRegistry::SurfaceEntry popup;
    popup.parentSurfaceKey = 1;
    popup.popupX = 180;
    popup.popupY = 20;
    popup.initialWidth = 100.0f;
    popup.initialHeight = 60.0f;

    const auto initial = resolvePopupSurfaceBounds(parentWindow, parentSurface, popup);
    EXPECT_FLOAT_EQ(initial.x, 280.0f);
    EXPECT_FLOAT_EQ(initial.y, 100.0f);
    EXPECT_FLOAT_EQ(initial.width, 100.0f);
    EXPECT_GT(initial.x + initial.width,
              static_cast<float>(parentWindow.x + parentWindow.width));

    parentWindow.x += 45;
    parentWindow.y += 30;
    const auto moved = resolvePopupSurfaceBounds(parentWindow, parentSurface, popup);
    EXPECT_FLOAT_EQ(moved.x - initial.x, 45.0f);
    EXPECT_FLOAT_EQ(moved.y - initial.y, 30.0f);
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

TEST(SurfaceRegistryTest, ClientConstrainedCommitDefinesSnappedLogicalExtent) {
    // An exact physical realization keeps the configured float because ceil
    // at the buffer boundary is not reversible for fractional logical sizes.
    EXPECT_FLOAT_EQ(SurfaceRegistry::committedLogicalExtent(1073, 536.25f, 2.0f),
                    536.25f);
    EXPECT_FLOAT_EQ(SurfaceRegistry::committedLogicalExtent(677, 541.0f, 1.25f),
                    541.0f);

    // A different physical extent is an intentional client constraint (for
    // example Terminal's cell grid), so it becomes the committed frame size.
    EXPECT_FLOAT_EQ(SurfaceRegistry::committedLogicalExtent(1072, 541.0f, 2.0f),
                    536.0f);
    EXPECT_FLOAT_EQ(SurfaceRegistry::committedLogicalExtent(670, 541.0f, 1.25f),
                    536.0f);
    EXPECT_FLOAT_EQ(SurfaceRegistry::committedLogicalExtent(536, 541.0f, 0.0f),
                    536.0f);
}

TEST(SurfaceRegistryTest, CloseTransitionAcceptsRetainedRasterLayers) {
    SurfaceRegistry::SurfaceEntry shm;
    shm.windowId = 1;
    shm.width = 640;
    shm.height = 480;
    shm.pixels = reinterpret_cast<void*>(1);

    ASSERT_TRUE(SurfaceRegistry::beginClosingTransition(shm));
    EXPECT_TRUE(shm.ignoreBufferCommits);
    EXPECT_EQ(shm.transitionPhase,
              SurfaceRegistry::SurfaceEntry::TransitionPhase::Closing);
    EXPECT_GT(shm.transitionDurationSec, 0.0f);
    EXPECT_FLOAT_EQ(shm.transitionOpacity, 1.0f);
    EXPECT_FLOAT_EQ(shm.transitionScale, 1.0f);
    EXPECT_FALSE(shm.pendingDestroy);

    SurfaceRegistry::SurfaceEntry secondLayer;
    secondLayer.windowId = 2;
    secondLayer.width = 800;
    secondLayer.height = 600;
    secondLayer.pixels = reinterpret_cast<void*>(2);

    ASSERT_TRUE(SurfaceRegistry::beginClosingTransition(secondLayer));
    EXPECT_TRUE(secondLayer.ignoreBufferCommits);
    EXPECT_EQ(secondLayer.transitionPhase,
              SurfaceRegistry::SurfaceEntry::TransitionPhase::Closing);
    EXPECT_GT(secondLayer.transitionDurationSec, 0.0f);
    EXPECT_FLOAT_EQ(secondLayer.transitionOpacity, 1.0f);
    EXPECT_FLOAT_EQ(secondLayer.transitionScale, 1.0f);
    EXPECT_FALSE(secondLayer.pendingDestroy);
}

TEST(SurfaceRegistryTest, CloseTransitionSkipsUnrenderableAndSuppressedSurfaces) {
    SurfaceRegistry::SurfaceEntry empty;
    empty.windowId = 1;
    empty.width = 640;
    empty.height = 480;
    EXPECT_FALSE(SurfaceRegistry::beginClosingTransition(empty));
    EXPECT_TRUE(empty.ignoreBufferCommits);
    EXPECT_EQ(empty.transitionPhase,
              SurfaceRegistry::SurfaceEntry::TransitionPhase::None);

    SurfaceRegistry::SurfaceEntry systemSurface;
    systemSurface.windowId = 2;
    systemSurface.width = 640;
    systemSurface.height = 32;
    systemSurface.pixels = reinterpret_cast<void*>(1);
    systemSurface.suppressInitialTransition = true;
    EXPECT_FALSE(SurfaceRegistry::beginClosingTransition(systemSurface));
    EXPECT_TRUE(systemSurface.ignoreBufferCommits);
    EXPECT_EQ(systemSurface.transitionPhase,
              SurfaceRegistry::SurfaceEntry::TransitionPhase::None);
}

TEST(CompositorRendererTest, DmaBufCropShowsContentWithoutScalingTheBacking) {
    const auto crop = lcl::render::makeDmaBufCrop(640, 480, 1920, 1080);
    EXPECT_FLOAT_EQ(crop.uMax, 640.0f / 1920.0f);
    EXPECT_FLOAT_EQ(crop.vMax, 480.0f / 1080.0f);
    const auto exact = lcl::render::makeDmaBufCrop(800, 600, 800, 600);
    EXPECT_FLOAT_EQ(exact.uMax, 1.0f);
    EXPECT_FLOAT_EQ(exact.vMax, 1.0f);
}

TEST(CompositorRendererTest, CroppedTextureSamplingStopsAtActiveTexelCenters) {
    EXPECT_FLOAT_EQ(lcl::render::normalizedHalfTexel(1920), 0.5f / 1920.0f);
    EXPECT_FLOAT_EQ(lcl::render::normalizedHalfTexel(1080), 0.5f / 1080.0f);
    EXPECT_FLOAT_EQ(lcl::render::normalizedHalfTexel(1), 0.5f);
    EXPECT_FLOAT_EQ(lcl::render::normalizedHalfTexel(0), 0.0f);

    const auto crop = lcl::render::makeDmaBufCrop(640, 480, 1920, 1080);
    const float rightSample = crop.uMax - lcl::render::normalizedHalfTexel(1920);
    const float bottomSample = crop.vMax - lcl::render::normalizedHalfTexel(1080);
    EXPECT_LT(rightSample, crop.uMax);
    EXPECT_LT(bottomSample, crop.vMax);
}

TEST(CompositorRendererTest, ShmAndDmaBufShareLogicalDestinationMapping) {
    constexpr std::array<float, 4> scales{1.0f, 1.25f, 1.5f, 2.0f};
    for (const float scale : scales) {
        const auto destination = lcl::render::mapLogicalRasterDestination(
            12.5f, 18.25f, 320.0f, 180.0f, 14.0f,
            scale, 3.0f, 5.0f);
        EXPECT_FLOAT_EQ(destination.x, 12.5f * scale + 3.0f);
        EXPECT_FLOAT_EQ(destination.y, 18.25f * scale + 5.0f);
        EXPECT_FLOAT_EQ(destination.width, 320.0f * scale);
        EXPECT_FLOAT_EQ(destination.height, 180.0f * scale);
        EXPECT_FLOAT_EQ(destination.cornerRadius, 14.0f * scale);

        // Content/backing extents remain physical and therefore produce the
        // same crop regardless of destination DPR.
        const auto crop = lcl::render::makeDmaBufCrop(640, 480, 1920, 1080);
        EXPECT_FLOAT_EQ(crop.uMax, 640.0f / 1920.0f);
        EXPECT_FLOAT_EQ(crop.vMax, 480.0f / 1080.0f);
    }
}

TEST(CompositorRendererTest, CurrentPreviousAndPopupDestinationsUseOneDprRule) {
    constexpr std::array<std::array<float, 5>, 3> logicalDestinations{{
        {80.0f, 92.0f, 800.0f, 568.0f, 20.0f},
        {80.0f, 92.0f, 640.0f, 448.0f, 20.0f},
        {260.0f, 120.0f, 180.0f, 90.0f, 10.0f},
    }};
    constexpr float scale = 1.5f;
    for (const auto& logical : logicalDestinations) {
        const auto physical = lcl::render::mapLogicalRasterDestination(
            logical[0], logical[1], logical[2], logical[3], logical[4], scale);
        EXPECT_FLOAT_EQ(physical.x, logical[0] * scale);
        EXPECT_FLOAT_EQ(physical.y, logical[1] * scale);
        EXPECT_FLOAT_EQ(physical.width, logical[2] * scale);
        EXPECT_FLOAT_EQ(physical.height, logical[3] * scale);
        EXPECT_FLOAT_EQ(physical.cornerRadius, logical[4] * scale);
    }
}

TEST(CompositorRendererTest, MapsProducerPixelDamageIntoLogicalDestination) {
    const graphics::RectF destination{10.0f, 20.0f, 672.0f, 1496.0f};
    const graphics::RectF output{0.0f, 0.0f, 672.0f, 1496.0f};
    const auto damage = mapSurfaceDamageToDestination(
        1344, 2992, 600, 1400, 144, 144,
        destination, output, 2.0f);

    ASSERT_TRUE(damage.has_value());
    EXPECT_FLOAT_EQ(damage->x, 308.0f);
    EXPECT_FLOAT_EQ(damage->y, 718.0f);
    EXPECT_FLOAT_EQ(damage->width, 76.0f);
    EXPECT_FLOAT_EQ(damage->height, 76.0f);
}

TEST(CompositorRendererTest, ClipsMappedDamageAndRejectsEmptyDamage) {
    const graphics::RectF destination{-10.0f, -20.0f, 200.0f, 100.0f};
    const graphics::RectF output{0.0f, 0.0f, 180.0f, 80.0f};
    const auto damage = mapSurfaceDamageToDestination(
        200, 100, 0, 0, 20, 20,
        destination, output, 2.0f);

    ASSERT_TRUE(damage.has_value());
    EXPECT_FLOAT_EQ(damage->x, 0.0f);
    EXPECT_FLOAT_EQ(damage->y, 0.0f);
    EXPECT_FLOAT_EQ(damage->width, 12.0f);
    EXPECT_FLOAT_EQ(damage->height, 2.0f);
    EXPECT_FALSE(mapSurfaceDamageToDestination(
        200, 100, 0, 0, 0, 20, destination, output).has_value());
}

TEST(SurfaceRegistryTest, GeometryInterruptionPreservesInFlightSerial) {
    SurfaceRegistry::SurfaceEntry entry;
    entry.pendingConfigureSerial = 9;
    entry.acceptedConfigureSerial = 7;
    entry.configuredGeometryGeneration = 12;

    SurfaceRegistry::interruptGeometryTransaction(entry, 13);

    EXPECT_EQ(entry.pendingConfigureSerial, 9u);
    EXPECT_EQ(entry.acceptedConfigureSerial, 7u);
    EXPECT_EQ(entry.configuredGeometryGeneration, 12u);
    EXPECT_TRUE(entry.forceConfigure);
}

TEST(SurfaceRegistryTest, RasterLayerPresentationCreditIsExplicit) {
    SurfaceRegistry::SurfaceEntry entry;

    EXPECT_FALSE(SurfaceRegistry::hasUnpresentedFrame(entry));
    SurfaceRegistry::queuePresentation(entry, 21);
    EXPECT_TRUE(SurfaceRegistry::hasUnpresentedFrame(entry));
    EXPECT_EQ(entry.presentationSerial, 21u);

    SurfaceRegistry::completePresentation(entry);
    EXPECT_FALSE(SurfaceRegistry::hasUnpresentedFrame(entry));
    EXPECT_EQ(entry.presentationSerial, 0u);

    SurfaceRegistry::queuePresentation(entry, 22);
    EXPECT_TRUE(SurfaceRegistry::hasUnpresentedFrame(entry));
    EXPECT_EQ(entry.presentationSerial, 22u);
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

TEST(InputRouterTest, PopupOutsideParentBoundsReceivesParentLocalInput) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);

    render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(800, 600));
    const uint32_t windowId = manager.createWindow("Parent", 100, 80, 200, 140);
    manager.setDecorationMode(windowId, render::DecorationMode::None);

    SurfaceRegistry registry;
    const auto parentKey = SurfaceRegistry::makeKey(sockets[0], 501, 1);
    const auto popupKey = SurfaceRegistry::makeKey(sockets[0], 501, 2);
    auto& parent = registry[parentKey];
    parent.windowId = windowId;
    parent.clientFd = sockets[0];
    parent.hasCommittedBuffer = true;
    parent.configuredWidth = 200;
    parent.configuredHeight = 140;
    parent.transitionScale = 0.8f;
    auto& popup = registry[popupKey];
    popup.parentSurfaceKey = parentKey;
    popup.popupOrder = registry.allocatePopupOrder();
    popup.popupX = 180;
    popup.popupY = 20;
    popup.initialWidth = 100.0f;
    popup.initialHeight = 60.0f;
    popup.stride = 400;
    popup.clientFd = sockets[0];
    popup.hasCommittedBuffer = true;
    popup.pixels = reinterpret_cast<void*>(1);

    SceneRegistry scenes;
    InputRouter router(manager, registry, scenes);
    InputEvent down{};
    down.type = InputEventType::PointerButton;
    down.source = lcl::platform::PointerSource::Mouse;
    down.button = lcl::platform::PointerButton::Left;
    down.pressed = true;
    // Parent group origin is (120, 94) at scale 0.8. Popup-local (70, 10)
    // therefore lands at global (320, 118), outside the parent frame.
    down.absoluteX = 320.0;
    down.absoluteY = 118.0;
    EXPECT_TRUE(router.route(down));

    protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(protocol::recvMsgWithFd(sockets[1], header, payload, receivedFd));
    ASSERT_EQ(header.opcode, protocol::LCLOpcode::InputEvent);
    ASSERT_EQ(payload.size(), sizeof(protocol::LCLMsgInputEvent));
    const auto* input = reinterpret_cast<const protocol::LCLMsgInputEvent*>(payload.data());
    EXPECT_EQ(input->surfaceId, 2u);
    EXPECT_EQ(input->type, 4u);
    EXPECT_FLOAT_EQ(input->x, 70.0f);
    EXPECT_FLOAT_EQ(input->y, 10.0f);

    InputEvent key{};
    key.type = InputEventType::KeyboardKey;
    key.key = lcl::platform::PhysicalKey::A;
    key.pressed = true;
    EXPECT_FALSE(router.route(key));

    ASSERT_TRUE(protocol::recvMsgWithFd(sockets[1], header, payload, receivedFd));
    ASSERT_EQ(header.opcode, protocol::LCLOpcode::InputEvent);
    ASSERT_EQ(payload.size(), sizeof(protocol::LCLMsgInputEvent));
    input = reinterpret_cast<const protocol::LCLMsgInputEvent*>(payload.data());
    EXPECT_EQ(input->surfaceId, 2u);
    EXPECT_EQ(input->type, 1u);

    popup.pixels = nullptr;
    close(sockets[0]);
    close(sockets[1]);
}

TEST(InputRouterTest, KeyboardUsesSelectedPopupSurfaceThenItsParentOnRelease) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);

    render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(800, 600));
    const uint32_t windowId = manager.createWindow("Parent", 100, 80, 200, 140);
    manager.setDecorationMode(windowId, render::DecorationMode::None);

    SurfaceRegistry registry;
    const auto parentKey = SurfaceRegistry::makeKey(sockets[0], 504, 1);
    const auto popupKey = SurfaceRegistry::makeKey(sockets[0], 504, 2);
    auto& parent = registry[parentKey];
    parent.windowId = windowId;
    parent.clientFd = sockets[0];
    parent.hasCommittedBuffer = true;
    auto& popup = registry[popupKey];
    popup.parentSurfaceKey = parentKey;
    popup.clientFd = sockets[0];
    popup.hasCommittedBuffer = true;

    SceneRegistry scenes;
    InputRouter router(manager, registry, scenes);
    ASSERT_TRUE(registry.focusKeyboardSurface(popupKey));

    InputEvent key{};
    key.type = InputEventType::KeyboardKey;
    key.key = lcl::platform::PhysicalKey::Tab;
    key.pressed = true;
    EXPECT_FALSE(router.route(key));

    protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(protocol::recvMsgWithFd(
        sockets[1], header, payload, receivedFd));
    ASSERT_EQ(header.opcode, protocol::LCLOpcode::InputEvent);
    ASSERT_EQ(payload.size(), sizeof(protocol::LCLMsgInputEvent));
    EXPECT_EQ(reinterpret_cast<const protocol::LCLMsgInputEvent*>(
                  payload.data())->surfaceId,
              2u);

    registry.releaseKeyboardFocus(popupKey);
    EXPECT_FALSE(router.route(key));
    ASSERT_TRUE(protocol::recvMsgWithFd(
        sockets[1], header, payload, receivedFd));
    ASSERT_EQ(payload.size(), sizeof(protocol::LCLMsgInputEvent));
    EXPECT_EQ(reinterpret_cast<const protocol::LCLMsgInputEvent*>(
                  payload.data())->surfaceId,
              1u);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(InputRouterTest, ParentCloseMarksPopupForAutomaticDestroy) {
    int parentSockets[2];
    int popupSockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, parentSockets), 0);
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, popupSockets), 0);

    render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(800, 600));
    const uint32_t windowId = manager.createWindow("Parent", 100, 80, 200, 140);
    manager.setDecorationMode(windowId, render::DecorationMode::None);

    SurfaceRegistry registry;
    const auto parentKey = SurfaceRegistry::makeKey(parentSockets[0], 502, 1);
    const auto popupKey = SurfaceRegistry::makeKey(popupSockets[0], 502, 2);
    auto& parent = registry[parentKey];
    parent.windowId = windowId;
    parent.clientFd = parentSockets[0];
    parent.width = 200;
    parent.height = 140;
    parent.stride = 800;
    parent.configuredWidth = 200;
    parent.configuredHeight = 140;
    parent.hasCommittedBuffer = true;
    parent.pixels = reinterpret_cast<void*>(1);
    auto& popup = registry[popupKey];
    popup.parentSurfaceKey = parentKey;
    popup.popupOrder = registry.allocatePopupOrder();
    popup.clientFd = popupSockets[0];
    popup.width = 80;
    popup.height = 40;
    popup.stride = 320;
    popup.hasCommittedBuffer = true;
    popup.pixels = reinterpret_cast<void*>(1);

    manager.getWindowsMutable().back().closeRequested = true;
    SceneRegistry scenes;
    InputRouter router(manager, registry, scenes);
    InputEvent motion{};
    motion.type = InputEventType::PointerMotion;
    motion.absoluteX = 401.0;
    motion.absoluteY = 301.0;
    EXPECT_TRUE(router.route(motion));

    ASSERT_TRUE(registry.contains(parentKey));
    ASSERT_TRUE(registry.contains(popupKey));
    EXPECT_EQ(registry.find(parentKey)->second.transitionPhase,
              SurfaceRegistry::SurfaceEntry::TransitionPhase::Closing);
    EXPECT_TRUE(registry.find(popupKey)->second.pendingDestroy);
    EXPECT_TRUE(registry.find(popupKey)->second.ignoreBufferCommits);

    registry.find(parentKey)->second.pixels = nullptr;
    registry.find(popupKey)->second.pixels = nullptr;
    close(parentSockets[0]);
    close(parentSockets[1]);
    close(popupSockets[0]);
    close(popupSockets[1]);
}

TEST(InputRouterTest, PopupDoesNotEscapeParentWindowGroupStacking) {
    int parentSockets[2];
    int unrelatedSockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, parentSockets), 0);
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, unrelatedSockets), 0);

    render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(800, 600));
    const uint32_t parentWindow = manager.createWindow("Parent", 100, 80, 200, 140);
    manager.setDecorationMode(parentWindow, render::DecorationMode::None);
    const uint32_t unrelatedWindow = manager.createWindow("Unrelated", 320, 90, 120, 100);
    manager.setDecorationMode(unrelatedWindow, render::DecorationMode::None);

    SurfaceRegistry registry;
    const auto parentKey = SurfaceRegistry::makeKey(parentSockets[0], 503, 1);
    const auto popupKey = SurfaceRegistry::makeKey(parentSockets[0], 503, 2);
    const auto unrelatedKey = SurfaceRegistry::makeKey(unrelatedSockets[0], 503, 3);
    auto& parent = registry[parentKey];
    parent.windowId = parentWindow;
    parent.clientFd = parentSockets[0];
    parent.hasCommittedBuffer = true;
    parent.configuredWidth = 200;
    parent.configuredHeight = 140;
    auto& popup = registry[popupKey];
    popup.parentSurfaceKey = parentKey;
    popup.popupOrder = registry.allocatePopupOrder();
    popup.popupX = 180;
    popup.popupY = 20;
    popup.initialWidth = 100.0f;
    popup.initialHeight = 60.0f;
    popup.stride = 400;
    popup.clientFd = parentSockets[0];
    popup.hasCommittedBuffer = true;
    popup.pixels = reinterpret_cast<void*>(1);
    auto& unrelated = registry[unrelatedKey];
    unrelated.windowId = unrelatedWindow;
    unrelated.clientFd = unrelatedSockets[0];
    unrelated.hasCommittedBuffer = true;
    unrelated.configuredWidth = 120;
    unrelated.configuredHeight = 100;

    SceneRegistry scenes;
    InputRouter router(manager, registry, scenes);
    InputEvent motion{};
    motion.type = InputEventType::PointerMotion;
    motion.absoluteX = 350.0;
    motion.absoluteY = 110.0;
    EXPECT_TRUE(router.route(motion));

    protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(protocol::recvMsgWithFd(
        unrelatedSockets[1], header, payload, receivedFd));
    ASSERT_EQ(header.opcode, protocol::LCLOpcode::InputEvent);

    InputEvent down{};
    down.type = InputEventType::PointerButton;
    down.button = lcl::platform::PointerButton::Left;
    down.pressed = true;
    down.absoluteX = 350.0;
    down.absoluteY = 110.0;
    // The unrelated window was already focused by the preceding motion, so
    // this click forwards input without changing WindowManager state.
    EXPECT_FALSE(router.route(down));

    ASSERT_TRUE(protocol::recvMsgWithFd(
        unrelatedSockets[1], header, payload, receivedFd));
    ASSERT_EQ(header.opcode, protocol::LCLOpcode::InputEvent);
    const auto* input = reinterpret_cast<const protocol::LCLMsgInputEvent*>(payload.data());
    EXPECT_EQ(input->surfaceId, 3u);
    EXPECT_EQ(input->type, 4u);
    EXPECT_FLOAT_EQ(input->x, 30.0f);
    EXPECT_FLOAT_EQ(input->y, 20.0f);

    popup.pixels = nullptr;
    close(parentSockets[0]);
    close(parentSockets[1]);
    close(unrelatedSockets[0]);
    close(unrelatedSockets[1]);
}

TEST(InputRouterTest, FractionalOutputScaleKeepsHorizontalResizeLogical) {
    render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(800, 600));
    const uint32_t windowId = manager.createWindow("Fractional resize", 100, 80, 300, 200);
    manager.setDecorationMode(windowId, render::DecorationMode::None);

    SurfaceRegistry registry;
    SceneRegistry scenes;
    InputRouter router(manager, registry, scenes, 1.25f);

    InputEvent motion{};
    motion.type = InputEventType::PointerMotion;
    motion.absoluteX = 399.5 * 1.25;
    motion.absoluteY = 150.0 * 1.25;
    router.route(motion);

    InputEvent down{};
    down.type = InputEventType::PointerButton;
    down.button = lcl::platform::PointerButton::Left;
    down.pressed = true;
    down.absoluteX = motion.absoluteX;
    down.absoluteY = motion.absoluteY;
    ASSERT_TRUE(router.route(down));

    motion.absoluteX = 409.5 * 1.25;
    ASSERT_TRUE(router.route(motion));
    const auto& resized = manager.getWindows().back();
    EXPECT_EQ(resized.resizeEdge, render::ResizeEdge::Right);
    EXPECT_FLOAT_EQ(resized.pendingWidth, 310.0f);
    EXPECT_FLOAT_EQ(resized.pendingX, 100.0f);
}

TEST(InputRouterTest, SsdResizeBorderWinsOverInteractiveFrameAttachment) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);

    render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(800, 600));
    const uint32_t windowId = manager.createWindow(
        "Attached edge", 100, 80, 300, 200);

    SurfaceRegistry registry;
    auto& parent = registry[SurfaceRegistry::makeKey(20, 60, 1)];
    parent.windowId = windowId;
    parent.hasCommittedBuffer = true;
    auto& frame = registry[SurfaceRegistry::makeKey(sockets[0], 60, 1000)];
    frame.attachedWindowId = windowId;
    frame.attachedRole = protocol::LCLAttachedSurfaceRole::Frame;
    frame.attachedWidth = 300;
    frame.attachedHeight = 32;
    frame.attachedFollowParentWidth = true;
    frame.attachedAcceptsInput = true;
    frame.hasCommittedBuffer = true;
    frame.pixels = reinterpret_cast<void*>(1);
    frame.clientFd = sockets[0];
    frame.configuredWidth = 300;
    frame.configuredHeight = 32;
    frame.pendingConfigureSerial = 1;
    frame.acceptedConfigureSerial = 1;

    SceneRegistry scenes;
    InputRouter router(manager, registry, scenes);
    InputEvent motion{};
    motion.type = InputEventType::PointerMotion;
    motion.absoluteX = 399.5;
    motion.absoluteY = 96.0;
    router.route(motion);

    protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(protocol::recvMsgWithFd(
        sockets[1], header, payload, receivedFd));
    ASSERT_EQ(header.opcode, protocol::LCLOpcode::InputEvent);

    InputEvent down{};
    down.type = InputEventType::PointerButton;
    down.button = lcl::platform::PointerButton::Left;
    down.pressed = true;
    down.absoluteX = motion.absoluteX;
    down.absoluteY = motion.absoluteY;
    ASSERT_TRUE(router.route(down));
    EXPECT_TRUE(manager.getWindows().back().isResizing());
    EXPECT_EQ(manager.getWindows().back().resizeEdge,
              render::ResizeEdge::Right);

    const int flags = fcntl(sockets[1], F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(fcntl(sockets[1], F_SETFL, flags | O_NONBLOCK), 0);
    EXPECT_EQ(protocol::recvPacketWithFd(
                  sockets[1], header, payload, receivedFd),
              protocol::ReceiveStatus::WouldBlock);

    frame.pixels = nullptr;
    close(sockets[0]);
    close(sockets[1]);
}

TEST(InputRouterTest, TwoXOutputQuantizesInteractiveResizeToLogicalPixels) {
    render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(800, 600));
    const uint32_t windowId = manager.createWindow("2x resize", 100, 80, 300, 200);
    manager.setDecorationMode(windowId, render::DecorationMode::None);

    SurfaceRegistry registry;
    SceneRegistry scenes;
    InputRouter router(manager, registry, scenes, 2.0f);

    InputEvent motion{};
    motion.type = InputEventType::PointerMotion;
    motion.absoluteX = 399.5 * 2.0;
    motion.absoluteY = 150.0 * 2.0;
    router.route(motion);

    InputEvent down{};
    down.type = InputEventType::PointerButton;
    down.button = lcl::platform::PointerButton::Left;
    down.pressed = true;
    down.absoluteX = motion.absoluteX;
    down.absoluteY = motion.absoluteY;
    ASSERT_TRUE(router.route(down));

    // One physical pointer pixel is half a logical unit at DPR 2. The window
    // edge advances one whole logical unit, i.e. two physical pixels.
    motion.absoluteX += 1.0;
    ASSERT_TRUE(router.route(motion));
    EXPECT_FLOAT_EQ(manager.getWindows().back().pendingWidth, 301.0f);

    motion.absoluteX += 1.0;
    router.route(motion);
    EXPECT_FLOAT_EQ(manager.getWindows().back().pendingWidth, 301.0f);

    motion.absoluteX += 1.0;
    ASSERT_TRUE(router.route(motion));
    EXPECT_FLOAT_EQ(manager.getWindows().back().pendingWidth, 302.0f);
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
    ++window.geometryGeneration;
    router.syncWindowState();
    const int flags = fcntl(sockets[1], F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(fcntl(sockets[1], F_SETFL, flags | O_NONBLOCK), 0);
    EXPECT_EQ(protocol::recvPacketWithFd(sockets[1], header, payload, receivedFd),
              protocol::ReceiveStatus::WouldBlock);

    surface.acceptedConfigureSerial = surface.pendingConfigureSerial;
    surface.lastConfigureSent = std::chrono::steady_clock::now() -
        std::chrono::milliseconds(20);
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

TEST(InputRouterTest, AttachedFrameFollowsPendingParentResizeGeometry) {
    int parentSockets[2];
    int frameSockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, parentSockets), 0);
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, frameSockets), 0);

    render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    const uint32_t windowId = manager.createWindow(
        "Attached resize", 80, 90, 320, 232);

    SurfaceRegistry registry;
    uint32_t parentPixel = 0xFF000000u;
    uint32_t framePixel = 0xFF000000u;
    auto& parent = registry[
        SurfaceRegistry::makeKey(parentSockets[0], 102, 1)];
    parent.windowId = windowId;
    parent.clientFd = parentSockets[0];
    parent.configuredX = 80.0f;
    parent.configuredY = 90.0f;
    parent.configuredWidth = 320.0f;
    parent.configuredHeight = 200.0f;
    parent.pendingConfigureSerial = 1;
    parent.acceptedConfigureSerial = 1;
    parent.nextConfigureSerial = 2;
    parent.hasCommittedBuffer = true;
    parent.pixels = &parentPixel;
    parent.width = 1;
    parent.height = 1;
    auto& frame = registry[
        SurfaceRegistry::makeKey(frameSockets[0], 103, 1000)];
    frame.windowId = windowId;
    frame.attachedWindowId = windowId;
    frame.attachedRole = protocol::LCLAttachedSurfaceRole::Adornment;
    frame.attachedWidth = 320.0f;
    frame.attachedHeight = 32.0f;
    frame.attachedFollowParentWidth = true;
    frame.clientFd = frameSockets[0];
    frame.configuredWidth = 320.0f;
    frame.configuredHeight = 32.0f;
    frame.pendingConfigureSerial = 1;
    frame.acceptedConfigureSerial = 1;
    frame.nextConfigureSerial = 2;
    frame.hasCommittedBuffer = true;
    frame.pixels = &framePixel;
    frame.width = 1;
    frame.height = 1;

    SceneRegistry scenes;
    InputRouter router(manager, registry, scenes);
    auto& window = manager.getWindowsMutable().back();
    window.pendingWidth = 420.0f;
    window.pendingHeight = 280.0f;
    window.geometryPhase = render::GeometryPhase::Resize;

    router.syncWindowState();

    protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(protocol::recvMsgWithFd(
        parentSockets[1], header, payload, receivedFd));
    ASSERT_EQ(header.opcode, protocol::LCLOpcode::ConfigureBounds);
    const auto* parentConfigure = reinterpret_cast<const
        protocol::LCLMsgConfigureBounds*>(payload.data());
    EXPECT_EQ(parentConfigure->surfaceId, 1u);
    EXPECT_FLOAT_EQ(parentConfigure->width, 420.0f);
    EXPECT_FLOAT_EQ(parentConfigure->height, 248.0f);
    EXPECT_EQ(parentConfigure->resizeReason,
              protocol::LCLConfigureResizeReason::Interactive);

    payload.clear();
    ASSERT_TRUE(protocol::recvMsgWithFd(
        frameSockets[1], header, payload, receivedFd));
    ASSERT_EQ(header.opcode, protocol::LCLOpcode::ConfigureBounds);
    const auto* frameConfigure = reinterpret_cast<const
        protocol::LCLMsgConfigureBounds*>(payload.data());
    EXPECT_EQ(frameConfigure->surfaceId, 1000u);
    EXPECT_FLOAT_EQ(frameConfigure->width, 420.0f);
    EXPECT_FLOAT_EQ(frameConfigure->height, 32.0f);
    EXPECT_EQ(frameConfigure->geometryGeneration,
              parentConfigure->geometryGeneration);
    EXPECT_EQ(parent.atomicConfigureGeneration, window.geometryGeneration);
    EXPECT_EQ(frame.atomicConfigureGeneration, window.geometryGeneration);

    // Parent and frame raster independently. The retained group remains closed
    // after only the parent layer is ready.
    parent.acceptedConfigureSerial = parent.pendingConfigureSerial;
    SurfaceRegistry::queuePresentation(
        parent, parent.acceptedConfigureSerial);
    EXPECT_TRUE(SurfaceTransactionCoordinator::promoteReady(registry));
    frame.acceptedConfigureSerial = frame.pendingConfigureSerial;
    SurfaceRegistry::queuePresentation(
        frame, frame.acceptedConfigureSerial);
    EXPECT_FALSE(SurfaceTransactionCoordinator::promoteReady(registry));

    close(parentSockets[0]);
    close(parentSockets[1]);
    close(frameSockets[0]);
    close(frameSockets[1]);
}

TEST(InputRouterTest, SingleSurfaceResizeUsesStrictAtomicGeneration) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);
    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    const uint32_t windowId = manager.createWindow("Live", 40, 50, 320, 200);
    manager.setDecorationMode(windowId, lcl::render::DecorationMode::None);
    SurfaceRegistry registry;
    uint32_t pixel = 0xFF000000u;
    auto& surface = registry[SurfaceRegistry::makeKey(sockets[0], 102, 1)];
    surface.windowId = windowId;
    surface.clientFd = sockets[0];
    surface.width = 320;
    surface.height = 200;
    surface.pendingConfigureSerial = 4;
    surface.acceptedConfigureSerial = 4;
    surface.nextConfigureSerial = 5;
    surface.hasCommittedBuffer = true;
    surface.pixels = &pixel;
    SceneRegistry scenes;
    InputRouter router(manager, registry, scenes);
    auto& window = manager.getWindowsMutable().back();
    window.geometryPhase = lcl::render::GeometryPhase::AtomicTargetTransition;
    window.isMaximized = true;
    window.pendingWidth = 360;
    window.pendingHeight = 240;
    router.syncWindowState();

    protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int fd = -1;
    ASSERT_TRUE(protocol::recvMsgWithFd(sockets[1], header, payload, fd));
    const auto* first = reinterpret_cast<const protocol::LCLMsgConfigureBounds*>(payload.data());
    EXPECT_EQ(first->configureSerial, 5u);
    EXPECT_EQ(first->resizeReason,
              protocol::LCLConfigureResizeReason::WindowStateTransition);
    EXPECT_EQ(first->backingWidth, 360u);
    EXPECT_EQ(first->backingHeight, 240u);
    EXPECT_EQ(surface.atomicConfigureGeneration, window.geometryGeneration);
    EXPECT_EQ(surface.atomicConfigureParticipantCount, 1u);
    EXPECT_TRUE(SurfaceTransactionCoordinator::promoteReady(registry));

    close(sockets[0]);
    close(sockets[1]);
}

TEST(InputRouterTest, SsdMaximizeBeginsStrictAtomicGeneration) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets), 0);

    render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    const uint32_t windowId = manager.createWindow("SSD", 80, 60, 400, 300);
    SurfaceRegistry registry;
    uint32_t pixel = 0xFF000000u;
    auto& surface = registry[SurfaceRegistry::makeKey(sockets[0], 105, 1)];
    surface.windowId = windowId;
    surface.clientFd = sockets[0];
    surface.width = 400;
    surface.height = 268;
    surface.backingWidth = 400;
    surface.backingHeight = 268;
    surface.stride = 400 * 4;
    surface.pixels = &pixel;
    surface.hasCommittedBuffer = true;
    surface.configuredX = 80;
    surface.configuredY = 60;
    surface.configuredWidth = 400;
    surface.configuredHeight = 268;
    surface.configuredFocused = 1;
    surface.pendingConfigureSerial = 4;
    surface.acceptedConfigureSerial = 4;
    surface.nextConfigureSerial = 5;

    SceneRegistry scenes;
    InputRouter router(manager, registry, scenes);
    ASSERT_TRUE(manager.maximizeWindow(windowId));
    ASSERT_TRUE(manager.updateAnimations(1.0f));
    router.syncWindowState();

    const auto& window = manager.getWindows().back();
    EXPECT_TRUE(window.isMaximized);
    EXPECT_EQ(surface.atomicConfigureGeneration, window.geometryGeneration);
    EXPECT_EQ(surface.atomicConfigureParticipantCount, 1u);

    protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int fd = -1;
    for (int packet = 0; packet < 4; ++packet) {
        ASSERT_TRUE(protocol::recvMsgWithFd(sockets[1], header, payload, fd));
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
        if (header.opcode == protocol::LCLOpcode::ConfigureBounds) break;
    }
    ASSERT_EQ(header.opcode, protocol::LCLOpcode::ConfigureBounds);
    const auto* configure =
        reinterpret_cast<const protocol::LCLMsgConfigureBounds*>(payload.data());
    EXPECT_EQ(configure->resizeReason,
              protocol::LCLConfigureResizeReason::WindowStateTransition);
    close(sockets[0]);
    close(sockets[1]);
}

TEST(InputRouterTest, ForwardsPointerWhileWindowGeometryIsRetained) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);

    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    const uint32_t windowId = manager.createWindow("Morph", 80, 90, 320, 200);
    manager.setDecorationMode(windowId, lcl::render::DecorationMode::None);
    auto& window = manager.getWindowsMutable().back();
    window.geometryPhase = lcl::render::GeometryPhase::AtomicTargetTransition;
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
    surface.transitionScale = 0.8f;

    SceneRegistry scenes;
    InputRouter router(manager, registry, scenes);
    InputEvent motion{};
    motion.type = InputEventType::PointerMotion;
    // The 0.8 group is centered inside the presented rect. Local (20, 20)
    // maps to global (128, 126), and inverse input mapping must recover it.
    motion.absoluteX = 128.0;
    motion.absoluteY = 126.0;
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
    EXPECT_EQ(input->source, static_cast<uint8_t>(protocol::LCLPointerSource::Mouse));
    EXPECT_FLOAT_EQ(input->x, 20.0f);
    EXPECT_FLOAT_EQ(input->y, 20.0f);

    // Route a Touch event
    InputEvent touchMotion{};
    touchMotion.type = InputEventType::PointerMotion;
    touchMotion.source = lcl::platform::PointerSource::Touch;
    touchMotion.pointerId = 41;
    touchMotion.absoluteX = 128.0;
    touchMotion.absoluteY = 126.0;
    EXPECT_TRUE(router.route(touchMotion));

    ASSERT_TRUE(protocol::recvMsgWithFd(sockets[1], header, payload, receivedFd));
    EXPECT_EQ(header.opcode, protocol::LCLOpcode::InputEvent);
    ASSERT_EQ(payload.size(), sizeof(protocol::LCLMsgInputEvent));
    const auto* touchInput = reinterpret_cast<const protocol::LCLMsgInputEvent*>(payload.data());
    EXPECT_EQ(touchInput->type, 3u);
    EXPECT_EQ(touchInput->source, static_cast<uint8_t>(protocol::LCLPointerSource::Touch));
    EXPECT_EQ(touchInput->pointerId, 41u);

    InputEvent touchCancel = touchMotion;
    touchCancel.type = InputEventType::PointerCancel;
    EXPECT_FALSE(router.route(touchCancel));

    ASSERT_TRUE(protocol::recvMsgWithFd(sockets[1], header, payload, receivedFd));
    EXPECT_EQ(header.opcode, protocol::LCLOpcode::InputEvent);
    ASSERT_EQ(payload.size(), sizeof(protocol::LCLMsgInputEvent));
    const auto* cancelInput =
        reinterpret_cast<const protocol::LCLMsgInputEvent*>(payload.data());
    EXPECT_EQ(cancelInput->type, static_cast<uint32_t>(
        protocol::LCLInputEventType::PointerCancel));
    EXPECT_EQ(cancelInput->pointerId, 41u);

    // Route a PointerScroll event
    InputEvent scrollEvent{};
    scrollEvent.type = InputEventType::PointerScroll;
    scrollEvent.source = lcl::platform::PointerSource::Mouse;
    scrollEvent.dx = 0.0;
    scrollEvent.dy = 1.0;
    router.route(scrollEvent);

    ASSERT_TRUE(protocol::recvMsgWithFd(sockets[1], header, payload, receivedFd));
    EXPECT_EQ(header.opcode, protocol::LCLOpcode::InputEvent);
    ASSERT_EQ(payload.size(), sizeof(protocol::LCLMsgInputEvent));
    const auto* scrollInput = reinterpret_cast<const protocol::LCLMsgInputEvent*>(payload.data());
    EXPECT_EQ(scrollInput->type, 6u);
    EXPECT_FLOAT_EQ(scrollInput->deltaY, 1.0f);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(InputRouterTest, TouchReleaseKeepsEventCoordinatesAfterCursorReset) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);

    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    const uint32_t windowId = manager.createWindow("CSD", 80, 90, 320, 200);
    manager.setDecorationMode(windowId, lcl::render::DecorationMode::CSD);

    SurfaceRegistry registry;
    const auto surfaceKey = SurfaceRegistry::makeKey(sockets[0], 102, 1);
    auto& surface = registry[surfaceKey];
    surface.windowId = windowId;
    surface.clientFd = sockets[0];
    surface.hasCommittedBuffer = true;
    surface.bufferScale = 1.0f;

    SceneRegistry scenes;
    InputRouter router(manager, registry, scenes);

    InputEvent motion{};
    motion.type = InputEventType::PointerMotion;
    motion.source = lcl::platform::PointerSource::Touch;
    motion.absoluteX = 100.0;
    motion.absoluteY = 100.0;
    router.route(motion);

    InputEvent down{};
    down.type = InputEventType::PointerButton;
    down.source = lcl::platform::PointerSource::Touch;
    down.button = lcl::platform::PointerButton::Left;
    down.pressed = true;
    down.absoluteX = 100.0;
    down.absoluteY = 100.0;
    router.route(down);

    InputEvent up = down;
    up.pressed = false;
    router.route(up);

    EXPECT_EQ(manager.getMouseX(), -10000);
    EXPECT_EQ(manager.getMouseY(), -10000);

    protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    for (uint8_t expectedType : {3u, 4u, 4u}) {
        bool receivedInput = false;
        for (int packet = 0; packet < 4; ++packet) {
            ASSERT_TRUE(protocol::recvMsgWithFd(
                sockets[1], header, payload, receivedFd));
            if (receivedFd >= 0) {
                close(receivedFd);
                receivedFd = -1;
            }
            if (header.opcode == protocol::LCLOpcode::InputEvent) {
                receivedInput = true;
                break;
            }
        }
        ASSERT_TRUE(receivedInput);
        ASSERT_EQ(payload.size(), sizeof(protocol::LCLMsgInputEvent));
        const auto* input =
            reinterpret_cast<const protocol::LCLMsgInputEvent*>(payload.data());
        EXPECT_EQ(input->type, expectedType);
        EXPECT_EQ(input->source,
                  static_cast<uint8_t>(protocol::LCLPointerSource::Touch));
        EXPECT_FLOAT_EQ(input->x, 20.0f);
        EXPECT_FLOAT_EQ(input->y, 10.0f);
    }

    close(sockets[0]);
    close(sockets[1]);
}

TEST(InputRouterTest, MobileBottomEdgeClaimCancelsClientThenTriggersHome) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets), 0);

    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(1000, 700));
    const uint32_t windowId = manager.createWindow("Mobile App", 0, 0, 1000, 700);
    manager.setDecorationMode(windowId, lcl::render::DecorationMode::None);

    SurfaceRegistry registry;
    const auto surfaceKey = SurfaceRegistry::makeKey(sockets[0], 103, 1);
    auto& surface = registry[surfaceKey];
    surface.windowId = windowId;
    surface.clientFd = sockets[0];
    surface.hasCommittedBuffer = true;
    surface.bufferScale = 1.0f;

    SceneRegistry scenes;
    InputRouter router(manager, registry, scenes, 1.0f, true, false);
    int homeEvents = 0;
    router.setSystemGestureHandler([&](SystemGestureDecision decision,
                                       const SystemGestureProgress&) {
        if (decision == SystemGestureDecision::Home) ++homeEvents;
        return true;
    });

    InputEvent down{};
    down.type = InputEventType::PointerButton;
    down.source = lcl::platform::PointerSource::Touch;
    down.pointerId = 77;
    down.button = lcl::platform::PointerButton::Left;
    down.pressed = true;
    down.absoluteX = 100.0;
    down.absoluteY = 695.0;
    router.route(down);

    protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(protocol::recvMsgWithFd(sockets[1], header, payload, receivedFd));
    ASSERT_EQ(header.opcode, protocol::LCLOpcode::InputEvent);
    const auto* clientDown =
        reinterpret_cast<const protocol::LCLMsgInputEvent*>(payload.data());
    EXPECT_EQ(clientDown->type, static_cast<uint32_t>(
        protocol::LCLInputEventType::PointerButton));
    EXPECT_EQ(clientDown->pointerId, 77u);

    InputEvent move = down;
    move.type = InputEventType::PointerMotion;
    move.pressed = false;
    move.absoluteY = 660.0;
    EXPECT_TRUE(router.route(move));

    ASSERT_TRUE(protocol::recvMsgWithFd(sockets[1], header, payload, receivedFd));
    ASSERT_EQ(header.opcode, protocol::LCLOpcode::InputEvent);
    const auto* clientCancel =
        reinterpret_cast<const protocol::LCLMsgInputEvent*>(payload.data());
    EXPECT_EQ(clientCancel->type, static_cast<uint32_t>(
        protocol::LCLInputEventType::PointerCancel));
    EXPECT_EQ(clientCancel->pointerId, 77u);

    InputEvent up = move;
    up.type = InputEventType::PointerButton;
    up.pressed = false;
    EXPECT_TRUE(router.route(up));
    EXPECT_EQ(homeEvents, 1);

    close(sockets[0]);
    close(sockets[1]);
}

TEST(InputRouterTest, LaunchMorphForwardsPointerInPresentedCoordinates) {
    int sockets[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sockets), 0);

    lcl::render::WindowManager manager;
    ASSERT_TRUE(manager.initialize(800, 600));
    const uint32_t windowId = manager.createWindow("Restoring", 80, 60, 400, 300);
    manager.setDecorationMode(windowId, lcl::render::DecorationMode::None);
    SurfaceRegistry registry;
    auto& surface = registry[SurfaceRegistry::makeKey(sockets[0], 104, 1)];
    surface.windowId = windowId;
    surface.clientFd = sockets[0];
    surface.hasCommittedBuffer = true;
    surface.transitionPhase = SurfaceRegistry::SurfaceEntry::TransitionPhase::Restoring;
    surface.launchMorphActive = true;
    surface.launchMorphX = 200.0f;
    surface.launchMorphY = 150.0f;
    surface.launchMorphWidth = 200.0f;
    surface.launchMorphHeight = 150.0f;
    SceneRegistry scenes;
    InputRouter router(manager, registry, scenes, 1.0f, false, false);

    InputEvent down{};
    down.type = InputEventType::PointerButton;
    down.button = lcl::platform::PointerButton::Left;
    down.pressed = true;
    // The presented morph is half the fullscreen surface size. Global
    // (250, 187.5) therefore maps to client-local (100, 75).
    down.absoluteX = 250.0;
    down.absoluteY = 187.5;
    router.route(down);

    protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    ASSERT_TRUE(protocol::recvMsgWithFd(
        sockets[1], header, payload, receivedFd));
    EXPECT_EQ(header.opcode, protocol::LCLOpcode::InputEvent);
    ASSERT_EQ(payload.size(), sizeof(protocol::LCLMsgInputEvent));
    const auto* input = reinterpret_cast<const protocol::LCLMsgInputEvent*>(
        payload.data());
    EXPECT_EQ(input->type, static_cast<uint32_t>(
        protocol::LCLInputEventType::PointerButton));
    EXPECT_FLOAT_EQ(input->x, 100.0f);
    EXPECT_FLOAT_EQ(input->y, 75.0f);

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

    float x = 40.0f;
    float y = 50.0f;
    float width = 800.0f;
    float height = 600.0f;
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

TEST(SystemSurfacePolicyTest, ReservedZoneUsesLogicalGeometryAtEveryDeviceScale) {
    for (const float scale : std::array{1.0f, 1.25f, 1.5f, 2.0f}) {
        SurfaceRegistry registry;
        auto& menu = registry[1];
        menu.windowId = 10;
        menu.systemSurfaceKind = protocol::LCLSystemSurfaceKind::MenuBar;
        menu.configuredHeight = 32.0f;
        menu.bufferScale = scale;
        menu.height = static_cast<uint32_t>(std::ceil(menu.configuredHeight * scale));
        menu.backingHeight = menu.height;

        auto& dock = registry[2];
        dock.windowId = 11;
        dock.systemSurfaceKind = protocol::LCLSystemSurfaceKind::Dock;
        dock.configuredHeight = 88.0f;
        dock.bufferScale = scale;
        dock.height = static_cast<uint32_t>(std::ceil(dock.configuredHeight * scale));
        dock.backingHeight = dock.height;

        const auto zone = SystemSurfacePolicyRegistry::computeReservedZone(registry);
        EXPECT_FLOAT_EQ(zone.top, 32.0f) << "device scale " << scale;
        EXPECT_FLOAT_EQ(zone.bottom, 88.0f) << "device scale " << scale;
    }
}

TEST(SystemSurfacePolicyTest, ReservedZonePreservesFractionsAndIgnoresIneligibleSurfaces) {
    SurfaceRegistry registry;

    auto& menu = registry[1];
    menu.windowId = 10;
    menu.systemSurfaceKind = protocol::LCLSystemSurfaceKind::MenuBar;
    menu.configuredHeight = 31.5f;
    menu.height = 63;

    auto& largerMenu = registry[2];
    largerMenu.windowId = 11;
    largerMenu.systemSurfaceKind = protocol::LCLSystemSurfaceKind::MenuBar;
    largerMenu.configuredHeight = 32.25f;
    largerMenu.height = 129;

    auto& dock = registry[3];
    dock.windowId = 12;
    dock.systemSurfaceKind = protocol::LCLSystemSurfaceKind::Dock;
    dock.configuredHeight = 87.25f;
    dock.height = 349;

    auto& unmappedMenu = registry[4];
    unmappedMenu.systemSurfaceKind = protocol::LCLSystemSurfaceKind::MenuBar;
    unmappedMenu.configuredHeight = 1000.0f;

    auto& wallpaper = registry[5];
    wallpaper.windowId = 13;
    wallpaper.systemSurfaceKind = protocol::LCLSystemSurfaceKind::Wallpaper;
    wallpaper.configuredHeight = 1000.0f;

    auto& application = registry[6];
    application.windowId = 14;
    application.systemSurfaceKind = protocol::LCLSystemSurfaceKind::None;
    application.configuredHeight = 1000.0f;

    const auto zone = SystemSurfacePolicyRegistry::computeReservedZone(registry);
    EXPECT_FLOAT_EQ(zone.top, 32.25f);
    EXPECT_FLOAT_EQ(zone.bottom, 87.25f);
}

TEST(WindowGroupTransformTest, ChromeAndClientShareOneSubpixelAnimatedFrame) {
    render::Window window{};
    window.x = 81;
    window.y = 63;
    window.width = 541;
    window.height = 367;

    const auto group = render::makeWindowGroupTransform(window, 32, 0.963f);
    EXPECT_NEAR(group.titleHeight, 32.0f * group.scale, 0.0001f);
    EXPECT_NEAR(group.globalBounds.y + group.titleHeight +
                    (group.globalBounds.height - group.titleHeight),
                group.globalBounds.y + group.globalBounds.height, 0.0001f);
    EXPECT_NEAR(group.globalBounds.x + group.globalBounds.width,
                static_cast<float>(window.x) +
                    (static_cast<float>(window.width) + group.globalBounds.width) * 0.5f,
                0.0001f);
    EXPECT_NE(group.globalBounds.width, std::round(group.globalBounds.width));
    EXPECT_NEAR(group.mapLength(20.0f), 20.0f * group.scale, 0.0001f);

    const graphics::PointF local{123.25f, 87.5f};
    const auto global = group.mapPoint(local);
    const auto roundTrip = group.unmapPoint(global);
    EXPECT_NEAR(roundTrip.x, local.x, 0.0001f);
    EXPECT_NEAR(roundTrip.y, local.y, 0.0001f);

    const auto resting = render::makeWindowGroupTransform(window, 32, 1.0f);
    EXPECT_FLOAT_EQ(resting.globalBounds.x, std::round(resting.globalBounds.x));
    EXPECT_FLOAT_EQ(resting.globalBounds.y, std::round(resting.globalBounds.y));
    EXPECT_FLOAT_EQ(resting.globalBounds.width, std::round(resting.globalBounds.width));
    EXPECT_FLOAT_EQ(resting.globalBounds.height, std::round(resting.globalBounds.height));
}

TEST(WindowGroupTransformTest, ExplicitLaunchBoundsSupportNonUniformIconMorph) {
    render::Window window{};
    window.width = 390;
    window.height = 844;

    const graphics::RectF iconBounds{24.5f, 40.25f, 60.0f, 60.0f};
    const auto group = render::makeWindowGroupTransformToBounds(
        window, 0.0f, iconBounds);
    EXPECT_NEAR(group.globalBounds.x, iconBounds.x, 0.0001f);
    EXPECT_NEAR(group.globalBounds.y, iconBounds.y, 0.0001f);
    EXPECT_NEAR(group.globalBounds.width, iconBounds.width, 0.0001f);
    EXPECT_NEAR(group.globalBounds.height, iconBounds.height, 0.0001f);
    const auto mapped = group.mapRect(group.localBounds);
    EXPECT_NEAR(mapped.x, iconBounds.x, 0.0001f);
    EXPECT_NEAR(mapped.y, iconBounds.y, 0.0001f);
    EXPECT_NEAR(mapped.width, iconBounds.width, 0.0001f);
    EXPECT_NEAR(mapped.height, iconBounds.height, 0.0001f);
}

TEST(CompositorRendererTest, LocalBackdropStartsBelowManagedFrameInset) {
    protocol::EffectRegion region{};
    region.x = 0;
    region.y = 0;
    region.width = 800;
    region.height = 600;

    const auto geometry = resolveLocalEffectGeometry(
        80, 60, 32, 800, 600, region, true);
    EXPECT_EQ(geometry.x, 80);
    EXPECT_EQ(geometry.y, 92);
    EXPECT_EQ(geometry.width, 800);
    EXPECT_EQ(geometry.height, 600);
}

TEST(CompositorRendererTest, FullSurfaceEffectMatchingUsesLogicalGeometry) {
    protocol::EffectRegion region{};
    region.width = 800.0f;
    region.height = 600.0f;

    EXPECT_TRUE(effectMatchesLogicalSurfaceBounds(region, 800.0f, 600.0f));
    EXPECT_FALSE(effectMatchesLogicalSurfaceBounds(region, 1600.0f, 1200.0f));

    region.width = 800.0005f;
    region.height = 599.9995f;
    EXPECT_TRUE(effectMatchesLogicalSurfaceBounds(region, 800.0f, 600.0f));
}

TEST(CompositorRendererTest, NonLocalFiltersRequireDamageDependencyClosure) {
    EXPECT_TRUE(filterReadsNeighboringPixels(protocol::FilterType::Blur));
    EXPECT_TRUE(filterReadsNeighboringPixels(protocol::FilterType::Glass));
    EXPECT_FALSE(filterReadsNeighboringPixels(protocol::FilterType::Tint));
    EXPECT_FALSE(filterReadsNeighboringPixels(protocol::FilterType::Brightness));
}

TEST(CompositorRendererTest,
     ClientDamageReusesOnlyItsSingleMatchingBackdropCache) {
    EXPECT_TRUE(canReuseRetainedBackdropForClientDamage(
        1, 1, true, true, true));
    EXPECT_FALSE(canReuseRetainedBackdropForClientDamage(
        2, 1, true, true, true));
    EXPECT_FALSE(canReuseRetainedBackdropForClientDamage(
        1, 2, true, true, true));
    EXPECT_FALSE(canReuseRetainedBackdropForClientDamage(
        1, 1, false, true, true));
    EXPECT_FALSE(canReuseRetainedBackdropForClientDamage(
        1, 1, true, false, true));
    EXPECT_FALSE(canReuseRetainedBackdropForClientDamage(
        1, 1, true, true, false));
}

TEST(CompositorRendererTest, DoubleInsetBorderKeepsLogicalStrokeInDisplayList) {
    const auto list = buildDoubleInsetBorderDisplayList(
        {10.0f, 20.0f, 100.0f, 60.0f}, 10.0f, 2.0f, 1.0f);
    ASSERT_EQ(list.commands().size(), 2u);
    for (const auto& command : list.commands()) {
        const auto* path = std::get_if<graphics::DrawPathCommand>(&command);
        ASSERT_NE(path, nullptr);
        EXPECT_EQ(path->paint.style, graphics::PaintStyle::Stroke);
        EXPECT_FLOAT_EQ(path->paint.stroke.width, 1.0f);
        EXPECT_EQ(path->paint.stroke.scaling,
                  graphics::StrokeScaling::ScaleWithTransform);
    }
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

TEST(FrameSchedulerTest, LaunchOriginSpringsBetweenIconAndFullscreenSurface) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::time_point{};
    FrameScheduler scheduler;
    scheduler.setDisplayCornerStyle(24.0f, 2.5f);
    scheduler.reset(start);

    SurfaceRegistry registry;
    auto& surface = registry[41];
    surface.launchToken = 41;
    surface.launchOwnerFd = 9;
    surface.transitionPhase = SurfaceRegistry::SurfaceEntry::TransitionPhase::Entering;
    surface.hasLaunchOrigin = true;
    surface.launchOriginX = 24.0f;
    surface.launchOriginY = 40.0f;
    surface.launchOriginWidth = 60.0f;
    surface.launchOriginHeight = 60.0f;
    surface.launchOriginCornerRadius =
        lcl::theme::mobile::kAppIconCornerRadius;
    surface.configuredWidth = 390.0f;
    surface.configuredHeight = 844.0f;

    EXPECT_TRUE(scheduler.advanceTransitions(
        registry, start + std::chrono::milliseconds(16)));
    EXPECT_TRUE(surface.launchMorphActive);
    EXPECT_LT(surface.launchMorphWidth, surface.configuredWidth);
    EXPECT_LT(surface.launchMorphHeight, surface.configuredHeight);
    EXPECT_GE(surface.transitionOpacity, 0.0f);
    EXPECT_GT(surface.launchHomeTransitionProgress, 0.0f);

    for (int frame = 2; frame <= 180; ++frame) {
        scheduler.advanceTransitions(
            registry, start + std::chrono::milliseconds(frame * 16));
    }
    EXPECT_EQ(surface.transitionPhase,
              SurfaceRegistry::SurfaceEntry::TransitionPhase::None);
    EXPECT_FALSE(surface.launchMorphActive);
    EXPECT_FLOAT_EQ(surface.launchMorphCornerRadius, 24.0f);
    EXPECT_FLOAT_EQ(surface.launchMorphCornerRoundness, 2.5f);
    EXPECT_FLOAT_EQ(surface.transitionOpacity, 1.0f);
    EXPECT_FLOAT_EQ(surface.launchHomeTransitionProgress, 1.0f);

    surface.transitionPhase =
        SurfaceRegistry::SurfaceEntry::TransitionPhase::Minimizing;
    for (int frame = 181; frame <= 360; ++frame) {
        scheduler.advanceTransitions(
            registry, start + std::chrono::milliseconds(frame * 16));
    }
    EXPECT_TRUE(surface.pendingMinimize);
    EXPECT_TRUE(surface.launchIconRevealPending);
    EXPECT_TRUE(surface.launchIconHandoffActive);
    EXPECT_TRUE(surface.launchMorphActive);
    EXPECT_EQ(surface.transitionPhase,
              SurfaceRegistry::SurfaceEntry::TransitionPhase::None);
    EXPECT_FLOAT_EQ(surface.launchHomeTransitionProgress, 0.0f);
}

TEST(FrameSchedulerTest, LaunchMorphCanReverseBeforeOpeningSettles) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::time_point{};
    FrameScheduler scheduler;
    scheduler.reset(start);

    SurfaceRegistry registry;
    auto& surface = registry[42];
    surface.launchToken = 42;
    surface.launchOwnerFd = 9;
    surface.transitionPhase = SurfaceRegistry::SurfaceEntry::TransitionPhase::Entering;
    surface.hasLaunchOrigin = true;
    surface.launchOriginX = 24.0f;
    surface.launchOriginY = 40.0f;
    surface.launchOriginWidth = 60.0f;
    surface.launchOriginHeight = 60.0f;
    surface.launchOriginCornerRadius =
        lcl::theme::mobile::kAppIconCornerRadius;
    surface.configuredWidth = 390.0f;
    surface.configuredHeight = 844.0f;

    ASSERT_TRUE(scheduler.advanceTransitions(
        registry, start + std::chrono::milliseconds(16)));
    ASSERT_TRUE(surface.launchMorphActive);
    surface.launchGestureActive = true;
    surface.launchGestureStartX = 195.0f;
    surface.launchGestureStartY = 840.0f;
    surface.launchGestureX = 195.0f;
    surface.launchGestureY = 700.0f;
    surface.transitionPhase =
        SurfaceRegistry::SurfaceEntry::TransitionPhase::Interactive;
    for (int frame = 2; frame <= 12; ++frame) {
        scheduler.advanceTransitions(
            registry, start + std::chrono::milliseconds(frame * 16));
    }
    EXPECT_TRUE(surface.launchMorphActive);
    EXPECT_LT(surface.launchMorphWidth, surface.configuredWidth);

    surface.launchGestureActive = false;
    surface.launchGestureVelocityY = -2400.0f;
    surface.launchGestureFlingPending = true;
    surface.transitionPhase =
        SurfaceRegistry::SurfaceEntry::TransitionPhase::Minimizing;

    for (int frame = 13; frame <= 190; ++frame) {
        scheduler.advanceTransitions(
            registry, start + std::chrono::milliseconds(frame * 16));
    }
    EXPECT_TRUE(surface.pendingMinimize);
    EXPECT_TRUE(surface.launchIconRevealPending);
    EXPECT_TRUE(surface.launchIconHandoffActive);
    EXPECT_TRUE(surface.launchMorphActive);
    EXPECT_FALSE(surface.launchGestureFlingPending);
    EXPECT_EQ(surface.transitionPhase,
              SurfaceRegistry::SurfaceEntry::TransitionPhase::None);
}

TEST(FrameSchedulerTest, InteractiveGestureQuartersOnlyHomeEffectProgress) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::time_point{};
    FrameScheduler scheduler;
    scheduler.reset(start);

    SurfaceRegistry registry;
    auto configureInteractiveSurface = [](auto& surface, uint64_t token) {
        surface.launchToken = token;
        surface.transitionPhase =
            SurfaceRegistry::SurfaceEntry::TransitionPhase::Interactive;
        surface.hasLaunchOrigin = true;
        surface.launchOriginX = 24.0f;
        surface.launchOriginY = 40.0f;
        surface.launchOriginWidth = 60.0f;
        surface.launchOriginHeight = 60.0f;
        surface.launchOriginCornerRadius =
            lcl::theme::mobile::kAppIconCornerRadius;
        surface.configuredWidth = 390.0f;
        surface.configuredHeight = 844.0f;
        surface.launchGestureActive = true;
        surface.launchGestureStartX = 195.0f;
        surface.launchGestureStartY = 840.0f;
        surface.launchGestureX = 195.0f;
        surface.launchGestureY = 700.0f;
    };

    auto& slowFling = registry[44];
    auto& fastFling = registry[45];
    configureInteractiveSurface(slowFling, 44);
    configureInteractiveSurface(fastFling, 45);
    for (int frame = 1; frame <= 180; ++frame) {
        scheduler.advanceTransitions(
            registry, start + std::chrono::milliseconds(frame * 16));
    }

    constexpr float deltaY = 140.0f;
    const float thumbnailProgress = 1.0f -
        deltaY / (slowFling.configuredHeight * 2.0f / 3.0f);
    const float expectedThumbnailWidth = slowFling.launchOriginWidth *
        (1.0f +
         (slowFling.configuredWidth - slowFling.launchOriginWidth) /
             slowFling.launchOriginWidth * thumbnailProgress);
    const float expectedHomeProgress =
        1.0f - (1.0f - thumbnailProgress) * 0.25f;
    EXPECT_NEAR(slowFling.launchMorphWidth, expectedThumbnailWidth, 0.05f);
    EXPECT_NEAR(slowFling.launchHomeTransitionProgress,
                expectedHomeProgress, 0.001f);

    slowFling.transitionPhase =
        SurfaceRegistry::SurfaceEntry::TransitionPhase::Minimizing;
    slowFling.launchGestureActive = false;
    slowFling.launchGestureVelocityY = -600.0f;
    slowFling.launchGestureFlingPending = true;
    fastFling.transitionPhase =
        SurfaceRegistry::SurfaceEntry::TransitionPhase::Minimizing;
    fastFling.launchGestureActive = false;
    fastFling.launchGestureVelocityY = -6000.0f;
    fastFling.launchGestureFlingPending = true;

    scheduler.advanceTransitions(
        registry, start + std::chrono::milliseconds(181 * 16));

    EXPECT_NEAR(slowFling.launchHomeTransitionProgress,
                fastFling.launchHomeTransitionProgress, 0.001f);
    EXPECT_NE(slowFling.launchMorphWidth, fastFling.launchMorphWidth);
}

TEST(FrameSchedulerTest, HomeEffectOpensFasterThanItCloses) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::time_point{};
    FrameScheduler scheduler;
    scheduler.reset(start);

    SurfaceRegistry registry;
    auto configureSurface = [](auto& surface, uint64_t token) {
        surface.launchToken = token;
        surface.hasLaunchOrigin = true;
        surface.launchOriginX = 24.0f;
        surface.launchOriginY = 40.0f;
        surface.launchOriginWidth = 60.0f;
        surface.launchOriginHeight = 60.0f;
        surface.launchOriginCornerRadius =
            lcl::theme::mobile::kAppIconCornerRadius;
        surface.configuredWidth = 390.0f;
        surface.configuredHeight = 844.0f;
    };

    auto& opening = registry[46];
    auto& closing = registry[47];
    configureSurface(opening, 46);
    configureSurface(closing, 47);
    opening.transitionPhase =
        SurfaceRegistry::SurfaceEntry::TransitionPhase::Entering;
    closing.transitionPhase =
        SurfaceRegistry::SurfaceEntry::TransitionPhase::Minimizing;

    scheduler.advanceTransitions(
        registry, start + std::chrono::milliseconds(16));

    const float openingDistance = opening.launchHomeTransitionProgress;
    const float closingDistance =
        1.0f - closing.launchHomeTransitionProgress;
    EXPECT_GT(openingDistance, closingDistance);
}

TEST(FrameSchedulerTest, LaunchMorphHasDeterministicExitDeadline) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::time_point{};
    FrameScheduler scheduler;
    scheduler.reset(start);

    SurfaceRegistry registry;
    auto& surface = registry[43];
    surface.launchToken = 43;
    surface.launchOwnerFd = 9;
    surface.transitionPhase =
        SurfaceRegistry::SurfaceEntry::TransitionPhase::Closing;
    surface.hasLaunchOrigin = true;
    surface.launchOriginX = 24.0f;
    surface.launchOriginY = 40.0f;
    surface.launchOriginWidth = 60.0f;
    surface.launchOriginHeight = 60.0f;
    surface.launchOriginCornerRadius =
        lcl::theme::mobile::kAppIconCornerRadius;
    surface.configuredWidth = 1000000000.0f;
    surface.configuredHeight = 1000000000.0f;

    for (int frame = 1; frame <= 100; ++frame) {
        scheduler.advanceTransitions(
            registry, start + std::chrono::milliseconds(frame * 16));
    }
    EXPECT_TRUE(surface.pendingDestroy);
    EXPECT_TRUE(surface.launchIconRevealPending);
    EXPECT_TRUE(surface.launchIconHandoffActive);
    EXPECT_TRUE(surface.launchMorphActive);
    EXPECT_EQ(surface.transitionPhase,
              SurfaceRegistry::SurfaceEntry::TransitionPhase::None);
}

TEST(FrameSchedulerTest, LaunchPlaceholderFadesOutAfterFirstContentFrame) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::time_point{};
    FrameScheduler scheduler;
    scheduler.reset(start);

    SurfaceRegistry registry;
    auto& surface = registry[51];
    surface.launchPlaceholderActive = true;
    surface.launchContentFadeActive = true;
    surface.launchContentOpacity = 0.0f;

    EXPECT_TRUE(scheduler.advanceTransitions(
        registry, start + std::chrono::milliseconds(50)));
    EXPECT_GT(surface.launchContentOpacity, 0.0f);
    EXPECT_LT(surface.launchContentOpacity, 1.0f);
    EXPECT_TRUE(surface.launchPlaceholderActive);

    scheduler.advanceTransitions(
        registry, start + std::chrono::milliseconds(100));
    scheduler.advanceTransitions(
        registry, start + std::chrono::milliseconds(150));
    scheduler.advanceTransitions(
        registry, start + std::chrono::milliseconds(200));
    EXPECT_FALSE(surface.launchContentFadeActive);
    EXPECT_FALSE(surface.launchPlaceholderActive);
    EXPECT_FLOAT_EQ(surface.launchContentOpacity, 1.0f);
}

TEST(FrameSchedulerTest, LaunchTokenPreservesSpringAcrossSurfaceHandoff) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::time_point{};
    constexpr uint64_t token = 73;
    constexpr SurfaceRegistry::Key placeholderKey =
        (uint64_t{1} << 63) | token;
    constexpr SurfaceRegistry::Key clientSurfaceKey = 0x123400000001ull;
    FrameScheduler scheduler;
    scheduler.reset(start);

    SurfaceRegistry registry;
    auto& placeholder = registry[placeholderKey];
    placeholder.launchToken = token;
    placeholder.transitionPhase =
        SurfaceRegistry::SurfaceEntry::TransitionPhase::Entering;
    placeholder.hasLaunchOrigin = true;
    placeholder.launchOriginX = 24.0f;
    placeholder.launchOriginY = 40.0f;
    placeholder.launchOriginWidth = 60.0f;
    placeholder.launchOriginHeight = 60.0f;
    placeholder.launchOriginCornerRadius =
        lcl::theme::mobile::kAppIconCornerRadius;
    placeholder.configuredWidth = 390.0f;
    placeholder.configuredHeight = 844.0f;

    for (int frame = 1; frame <= 8; ++frame) {
        scheduler.advanceTransitions(
            registry, start + std::chrono::milliseconds(frame * 16));
    }
    const float widthBeforeHandoff = placeholder.launchMorphWidth;
    ASSERT_GT(widthBeforeHandoff, placeholder.launchOriginWidth);

    SurfaceRegistry::SurfaceEntry client = std::move(placeholder);
    registry.erase(placeholderKey);
    registry[clientSurfaceKey] = std::move(client);
    auto& adopted = registry[clientSurfaceKey];
    scheduler.advanceTransitions(
        registry, start + std::chrono::milliseconds(9 * 16));

    EXPECT_GE(adopted.launchMorphWidth, widthBeforeHandoff);
}

TEST(FrameSchedulerTest, FrameBudgetUsesTheExistingCadence) {
    FrameScheduler scheduler;
    EXPECT_EQ(scheduler.frameBudgetForHz(60), std::chrono::microseconds(16166));
}

TEST(SceneStateTest, RegistryReflectsWindowLifecycleAndFocusWithoutOwningWindowManager) {
    render::WindowManager windowManager;
    ASSERT_TRUE(windowManager.initialize(1280, 720));

    const uint32_t terminalWindow = windowManager.createWindow("Terminal", 80, 60, 540, 360);
    const uint32_t demoWindow = windowManager.createWindow("Demo", 140, 100, 480, 320);
    windowManager.setEdgeToEdge(terminalWindow, true);

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
    const auto* terminal = scenes.find(terminalScene);
    ASSERT_NE(terminal, nullptr);
    EXPECT_TRUE(terminal->edgeToEdge);

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
    const auto home = SystemSurfacePolicyRegistry::policyFor(
        protocol::LCLSystemSurfaceKind::HomeScreen);

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
    EXPECT_TRUE(home.isSystemSurface);
    EXPECT_EQ(home.layer, protocol::LCLWindowLayer::Normal);
    EXPECT_FALSE(home.unfocusable);
    EXPECT_EQ(home.placement, SystemSurfacePlacement::OutputBounds);
    EXPECT_FALSE(home.insetBorderEnabled);
    EXPECT_FALSE(SystemSurfacePolicyRegistry::isValidKind(protocol::LCLSystemSurfaceKind::None));
}

} // namespace lcl::core
