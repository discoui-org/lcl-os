#include "core/compositor/compositor_renderer.hpp"
#include "core/compositor/double_inset_border.hpp"
#include "core/compositor/effect_region_geometry.hpp"
#include "core/compositor/window_chrome_material.hpp"
#include "core/compositor/popup_surface_geometry.hpp"
#include "render/window_group_transform.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace lcl::core {
void CompositorRenderer::render(render::Renderer& renderer,
                                 lcl::platform::IDisplayBackend& displayBackend,
                                 const render::WindowManager& windowManager,
                                 const SurfaceRegistry::Snapshot& surfaces,
                                 const std::function<void()>& beforePresent,
                                 bool allowIncrementalMove) const {
    using SurfaceEntry = SurfaceRegistry::SurfaceEntry;
    // The snapshot contains only const entry pointers, so protocol/input work
    // cannot mutate the surface state while this frame is being composed.

    auto* raster = renderer.getRasterRenderer();

    std::optional<graphics::RectF> incrementalDamage;
#if defined(__ANDROID__)
    // Blur and Glass sample pixels outside their own effect bounds. Replaying
    // only old+new window damage would therefore sample a partly stale retained
    // scene and leave trails. Until damage dependencies are closed transitively,
    // render the complete scene whenever a visible surface uses either filter.
    const bool hasNonLocalSceneEffect = std::any_of(
        surfaces.begin(), surfaces.end(), [](const auto& surface) {
            if (!surface.entry || !surface.entry->hasRenderableBuffer()) {
                return false;
            }
            return std::any_of(
                surface.entry->effectRegions.begin(), surface.entry->effectRegions.end(),
                [](const auto& effectRegion) {
                    return std::any_of(
                        effectRegion.filters.begin(), effectRegion.filters.end(),
                        [](const auto& filter) {
                            return filterReadsNeighboringPixels(filter.type);
                        });
                });
        });

    if (allowIncrementalMove && m_hasCompleteRetainedFrame &&
        !hasNonLocalSceneEffect) {
        bool hasDirtyWindow = false;
        bool moveOnly = true;
        graphics::RectF mergedDamage{};
        for (const auto& window : windowManager.getWindows()) {
            if (!window.isDirty) continue;
            hasDirtyWindow = true;
            if (!window.isDragging()) {
                moveOnly = false;
                break;
            }
            mergedDamage = mergedDamage.unionWith(window.damageRect);
        }

        // Popup surfaces may extend beyond their parent WindowGroup. Until
        // popup visual bounds participate in Window damage, keep that less
        // common move path on the correctness-first full redraw.
        if (moveOnly) {
            for (const auto& window : windowManager.getWindows()) {
                if (!window.isDirty) continue;
                SurfaceRegistry::Key parentSurfaceKey = 0;
                for (const auto& surface : surfaces) {
                    if (surface.entry && !surface.entry->isPopup() &&
                        surface.entry->windowId == window.id) {
                        parentSurfaceKey = surface.key;
                        break;
                    }
                }
                if (parentSurfaceKey == 0) continue;
                const bool hasPopup = std::any_of(
                    surfaces.begin(), surfaces.end(),
                    [parentSurfaceKey](const auto& surface) {
                        return surface.entry &&
                            surface.entry->parentSurfaceKey == parentSurfaceKey &&
                            surface.entry->hasRenderableBuffer() &&
                            !surface.entry->pendingDestroy;
                    });
                if (hasPopup) {
                    moveOnly = false;
                    break;
                }
            }
        }

        if (hasDirtyWindow && moveOnly && !mergedDamage.isEmpty()) {
            // Cover antialiased window edges and fractional motion before
            // clipping to the logical output. There are currently no exterior
            // compositor shadows; if one is introduced its radius must be
            // included here as part of the WindowGroup visual bounds.
            constexpr float kEdgeSafety = 2.0f;
            const graphics::RectF expanded{
                mergedDamage.x - kEdgeSafety,
                mergedDamage.y - kEdgeSafety,
                mergedDamage.width + kEdgeSafety * 2.0f,
                mergedDamage.height + kEdgeSafety * 2.0f,
            };
            const graphics::RectF outputBounds{
                0.0f, 0.0f,
                windowManager.getScreenWidth(),
                windowManager.getScreenHeight(),
            };
            const auto clipped = expanded.intersection(outputBounds);
            if (!clipped.isEmpty()) incrementalDamage = clipped;
        }
    }

    // The Android scene FBO is the retained source of truth. Other platform
    // binaries keep their existing full-frame beginFrame behavior.
    raster->setRetainsFrameBacking(true);
#endif

    // --- Begin LCL raster frame ---
    raster->beginFrame();

    constexpr float kWindowCornerRadiusLogical = 20.0f;

    const float outputScale = raster->getDeviceScale();
    auto drawShmSurface = [&](SurfaceRegistry::Key cacheKey,
                              uint64_t contentSerial,
                              float dstX, float dstY,
                              int srcW, int srcH, int backingW, int backingH,
                              const uint32_t* pixels, int stridePixels,
                              int damageX, int damageY, int damageW, int damageH,
                              float opacity, float cornerRadius,
                              float cornerRoundness, bool squareTopCorners,
                              float drawWidth, float drawHeight) {
#if defined(__ANDROID__)
        raster->drawCachedShmBufferTransformed(
            cacheKey, contentSerial, dstX, dstY, srcW, srcH,
            backingW, backingH, pixels, stridePixels,
            damageX, damageY, damageW, damageH,
            opacity, cornerRadius, cornerRoundness,
            squareTopCorners, drawWidth, drawHeight);
#else
        (void)cacheKey;
        (void)contentSerial;
        (void)backingW;
        (void)backingH;
        (void)damageX;
        (void)damageY;
        (void)damageW;
        (void)damageH;
        raster->drawBufferTransformed(
            dstX, dstY, srcW, srcH, pixels, stridePixels, opacity,
            cornerRadius, cornerRoundness, squareTopCorners,
            drawWidth, drawHeight);
#endif
    };
    auto resolveWindowCornerRadiusLogical = [&](const render::Window& win) {
        if (win.cornerRadius >= 0.0f) {
            return win.cornerRadius;
        }

        if (win.decorationMode == render::DecorationMode::SSD) {
            return kWindowCornerRadiusLogical;
        }

        if (win.decorationMode == render::DecorationMode::None &&
            win.title.find("Terminal") != std::string::npos) {
            return kWindowCornerRadiusLogical;
        }

        return 0.0f;
    };
    auto resolveWindowCornerRoundness = [](const render::Window& win) {
        return std::clamp(win.cornerRoundness, 2.0f, 8.0f);
    };

    auto drawChrome = [&](const render::Window& win, const render::WindowGroupTransform& group,
                          float chromeOpacity, bool drawTitlebar) {
        const lcl::graphics::RectF logicalBounds = group.localBounds;
        const float unscaledTitleHeight = group.scale > 0.0f
            ? group.titleHeight / group.scale : 0.0f;
        const lcl::graphics::Color titlebarColor =
            drawTitlebar && paintsOpaqueSsdTitlebar(win.edgeToEdge)
                ? lcl::graphics::Color{kOpaqueSsdTitlebarMaterial.r,
                                       kOpaqueSsdTitlebarMaterial.g,
                                       kOpaqueSsdTitlebarMaterial.b,
                                       kOpaqueSsdTitlebarMaterial.a}
                : lcl::graphics::Color{};
        const auto displayList = win.chrome.buildDisplayList({
            logicalBounds,
            unscaledTitleHeight,
            resolveWindowCornerRadiusLogical(win),
            16.0f,
            chromeOpacity,
            drawTitlebar,
            titlebarColor,
        });
        raster->replayDisplayList(displayList,
            {{static_cast<float>(renderer.getWidth()) / outputScale,
              static_cast<float>(renderer.getHeight()) / outputScale},
             {renderer.getWidth(), renderer.getHeight()}, outputScale},
            group.localToGlobal);
    };

    auto replayLogicalList = [&](const graphics::DisplayList& displayList,
                                 const graphics::Matrix3& rootTransform = {}) {
        raster->replayDisplayList(displayList,
            {{static_cast<float>(renderer.getWidth()) / outputScale,
              static_cast<float>(renderer.getHeight()) / outputScale},
             {renderer.getWidth(), renderer.getHeight()}, outputScale},
            rootTransform);
    };

    // 1. Clear either the complete desktop or only the old+new move damage.
#if defined(__ANDROID__)
    if (incrementalDamage) {
        const render::RasterRect damage{
            incrementalDamage->x, incrementalDamage->y,
            incrementalDamage->width, incrementalDamage->height};
        raster->setFrameDamageRect(damage);
        raster->clearRect(damage, {0, 0, 0, 255});
    } else {
        raster->setFrameDamageRect(std::nullopt);
        raster->setClipRect(std::nullopt);
        raster->clear({0, 0, 0, 255});
        m_hasCompleteRetainedFrame = true;
    }
#else
    renderer.clear(0xFF000000);
#endif

    // 2. Atomic Z-Stacking Window Group Rendering (Frame + Client Surface per Window in Z-order)
    auto applySurfaceRegionEffects = [&](const render::Window& win,
                                         const SurfaceEntry& surface,
                                         protocol::EffectSourceType sourceType,
                                         float windowOpacity,
        const render::WindowGroupTransform& group) {
        const float titleOffset = (win.decorationMode == render::DecorationMode::SSD)
            ? 32.0f
            : 0.0f;

        for (const auto& fx : surface.effectRegions) {
            if (fx.region.source != sourceType || fx.filters.empty()) continue;

            const bool outerSurface =
                fx.region.boundsPolicy == protocol::EffectBoundsPolicy::OuterSurface;
            float fxX = 0.0f;
            float fxY = 0.0f;
            float fxW = 0.0f;
            float fxH = 0.0f;
            float cornerRadius = 0.0f;
            float cornerRoundness = 2.0f;
            if (outerSurface) {
                // The compositor owns the outer surface geometry, including
                // desktop or mobile system insets. A root backdrop therefore
                // cannot drift below it or select a different corner shape.
                fxX = group.globalBounds.x;
                fxY = group.globalBounds.y;
                fxW = std::max(1.0f, group.globalBounds.width);
                fxH = std::max(1.0f, group.globalBounds.height);
                cornerRadius = group.mapLength(resolveWindowCornerRadiusLogical(win));
                cornerRoundness = resolveWindowCornerRoundness(win);
            } else {
                const auto local = resolveLocalEffectGeometry(
                    0.0f, 0.0f, titleOffset,
                    std::max(1.0f, group.localBounds.width),
                    std::max(1.0f, group.localBounds.height - titleOffset),
                    fx.region, fx.followSurfaceBounds);
                const auto mapped = group.mapRect(
                    {local.x, local.y, local.width, local.height});
                fxX = mapped.x;
                fxY = mapped.y;
                fxW = mapped.width;
                fxH = mapped.height;
                cornerRadius = group.mapLength(
                    std::max(0.0f, fx.region.cornerRadius));
                cornerRoundness = std::clamp(fx.region.cornerRoundness, 2.0f, 8.0f);
            }
            if (fxW <= 0.0f || fxH <= 0.0f) continue;
            if (incrementalDamage && !incrementalDamage->intersects(
                    {fxX, fxY, fxW, fxH})) {
                continue;
            }

            // Initial executor supports chain filters with source-type routing.
            // Advanced blend modes are currently treated as normal blend.
            renderer.getRasterRenderer()->applyBackdropFilter(
                fxX, fxY, fxW, fxH,
                cornerRadius,
                cornerRoundness,
                std::clamp(fx.region.opacity * windowOpacity, 0.0f, 1.0f),
                fx.filters);
        }
    };

    for (const auto& win : windowManager.getWindows()) {
        if (win.isMinimized) {
            continue;
        }
        // A. Find matching client SHM surface buffer for this window
        const SurfaceEntry* matchingSurface = nullptr;
        SurfaceRegistry::Key matchingSurfaceKey = 0;
        for (const auto& surface : surfaces) {
            const auto* entry = surface.entry;
            if (entry && !entry->isPopup() && entry->windowId == win.id &&
                entry->hasRenderableBuffer()) {
                matchingSurface = entry;
                matchingSurfaceKey = surface.key;
                break;
            }
        }

        const float titleOffset = (win.decorationMode == render::DecorationMode::SSD)
            ? 32.0f
            : 0.0f;

        float windowOpacity = 1.0f;
        float windowScale = 1.0f;
        if (matchingSurface) {
            windowOpacity = std::clamp(matchingSurface->transitionOpacity, 0.0f, 1.0f);
            windowScale = std::clamp(matchingSurface->transitionScale, 0.80f, 1.20f);
        }

        const render::WindowGroupTransform group =
            render::makeWindowGroupTransform(win, titleOffset, windowScale);

        // B. Apply effect-graph backdrop regions (new pipeline only)
        if (matchingSurface && !matchingSurface->effectRegions.empty()) {
            applySurfaceRegionEffects(win, *matchingSurface, protocol::EffectSourceType::Backdrop,
                                      windowOpacity, group);
        }

        if (matchingSurface) {
            int srcW = static_cast<int>(matchingSurface->width);
            int srcH = static_cast<int>(matchingSurface->height);
            int stridePixels = static_cast<int>(matchingSurface->stride / 4);
            float drawX = group.globalBounds.x;
            float drawY = group.globalBounds.y;
            float drawW = group.globalBounds.width;
            float drawH = group.globalBounds.height;

            if (win.decorationMode == render::DecorationMode::SSD) {
                // The client buffer occupies the remaining exact pixels of
                // the same transformed group rect used by the titlebar.
                drawY += group.titleHeight;
                drawH = std::max(1.0f, group.globalBounds.height - group.titleHeight);
            }

            const float windowCornerRadius = resolveWindowCornerRadiusLogical(win);
            const bool maskToWindowShape = windowCornerRadius > 0.001f;
            const float presentedCornerRadius = group.mapLength(windowCornerRadius);

            const bool hasPrevious = matchingSurface->previousPixels ||
                                     matchingSurface->previousDmaBufTexture != 0;
            if (matchingSurface->previousPixels) {
                // Android retains current and crossfade source independently.
                // Surface keys are PID-backed and therefore never use bit 63.
                constexpr uint64_t kPreviousShmCacheBit = uint64_t{1} << 63;
                drawShmSurface(
                    matchingSurfaceKey ^ kPreviousShmCacheBit,
                    matchingSurface->previousShmContentSerial,
                    drawX, drawY,
                    static_cast<int>(matchingSurface->previousWidth),
                    static_cast<int>(matchingSurface->previousHeight),
                    static_cast<int>(matchingSurface->previousBackingWidth),
                    static_cast<int>(matchingSurface->previousBackingHeight),
                    reinterpret_cast<const uint32_t*>(matchingSurface->previousPixels),
                    static_cast<int>(matchingSurface->previousStride / 4),
                    0, 0,
                    static_cast<int>(matchingSurface->previousWidth),
                    static_cast<int>(matchingSurface->previousHeight),
                    windowOpacity * (1.0f - matchingSurface->resizeCrossfadeProgress),
                    maskToWindowShape ? presentedCornerRadius : 0.0f,
                    resolveWindowCornerRoundness(win),
                    win.decorationMode == render::DecorationMode::SSD,
                    drawW,
                    drawH);
            } else if (matchingSurface->previousDmaBufTexture != 0) {
                renderer.getRasterRenderer()->drawDmaBufTextureTransformed(
                    drawX, drawY,
                    static_cast<int>(matchingSurface->previousWidth),
                    static_cast<int>(matchingSurface->previousHeight),
                    static_cast<int>(matchingSurface->previousBackingWidth),
                    static_cast<int>(matchingSurface->previousBackingHeight),
                    matchingSurface->previousDmaBufTexture,
                    windowOpacity * (1.0f - matchingSurface->resizeCrossfadeProgress),
                    maskToWindowShape ? presentedCornerRadius : 0.0f,
                    resolveWindowCornerRoundness(win),
                    win.decorationMode == render::DecorationMode::SSD,
                    drawW, drawH);
            }

            const float currentOpacity = windowOpacity *
                (hasPrevious ? matchingSurface->resizeCrossfadeProgress : 1.0f);
            if (matchingSurface->pixels) {
                drawShmSurface(
                    matchingSurfaceKey, matchingSurface->shmContentSerial,
                    drawX, drawY, srcW, srcH,
                    static_cast<int>(matchingSurface->backingWidth),
                    static_cast<int>(matchingSurface->backingHeight),
                    reinterpret_cast<const uint32_t*>(matchingSurface->pixels),
                    stridePixels,
                    static_cast<int>(matchingSurface->shmDamageX),
                    static_cast<int>(matchingSurface->shmDamageY),
                    static_cast<int>(matchingSurface->shmDamageWidth),
                    static_cast<int>(matchingSurface->shmDamageHeight),
                    currentOpacity,
                    maskToWindowShape ? presentedCornerRadius : 0.0f,
                    resolveWindowCornerRoundness(win),
                    win.decorationMode == render::DecorationMode::SSD,
                    drawW, drawH);
            } else {
                renderer.getRasterRenderer()->drawDmaBufTextureTransformed(
                    drawX, drawY, srcW, srcH,
                    static_cast<int>(matchingSurface->backingWidth),
                    static_cast<int>(matchingSurface->backingHeight),
                    matchingSurface->dmaBufTexture,
                    currentOpacity,
                    maskToWindowShape ? presentedCornerRadius : 0.0f,
                    resolveWindowCornerRoundness(win),
                    win.decorationMode == render::DecorationMode::SSD,
                    drawW, drawH);
            }

            if (!matchingSurface->effectRegions.empty()) {
                applySurfaceRegionEffects(win, *matchingSurface, protocol::EffectSourceType::Foreground,
                                          windowOpacity, group);
            }

        }

        // C. Render Server-Side Window Frame (Titlebar & Inset Border) on top of content.
        if (win.decorationMode == render::DecorationMode::SSD) {
            drawChrome(win, group, windowOpacity, true);
        }

        // Forced compositor-owned inset border for every window, independent from app UI.
        if (win.drawInsetBorder) {
            float borderRadius = resolveWindowCornerRadiusLogical(win);
            if (borderRadius <= 0.001f) {
                borderRadius = kWindowCornerRadiusLogical;
            }
            const graphics::RectF borderBounds = group.localBounds;
            replayLogicalList(
                buildDoubleInsetBorderDisplayList(
                    borderBounds, borderRadius,
                    resolveWindowCornerRoundness(win), windowOpacity),
                group.localToGlobal);
        }

        // PopupSurface entries are not windows. Compose them immediately above
        // their parent WindowGroup and before the next unrelated window.
        if (matchingSurfaceKey != 0) {
            std::vector<SurfaceRegistry::SnapshotEntry> popups;
            for (const auto& surface : surfaces) {
                const auto* popup = surface.entry;
                if (popup && popup->parentSurfaceKey == matchingSurfaceKey &&
                    popup->hasCommittedBuffer && popup->hasRenderableBuffer() &&
                    !popup->pendingDestroy) {
                    popups.push_back(surface);
                }
            }
            std::sort(popups.begin(), popups.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.entry->popupOrder < rhs.entry->popupOrder;
            });

            for (const auto& popupSurface : popups) {
                const auto* popup = popupSurface.entry;
                const auto popupBounds = resolvePopupSurfaceBounds(
                    win, *matchingSurface, *popup);
                const float opacity = windowOpacity *
                    std::clamp(popup->transitionOpacity, 0.0f, 1.0f);
                if (popup->pixels) {
                    drawShmSurface(
                        popupSurface.key, popup->shmContentSerial,
                        popupBounds.x, popupBounds.y,
                        static_cast<int>(popup->width), static_cast<int>(popup->height),
                        static_cast<int>(popup->backingWidth),
                        static_cast<int>(popup->backingHeight),
                        reinterpret_cast<const uint32_t*>(popup->pixels),
                        static_cast<int>(popup->stride / 4),
                        static_cast<int>(popup->shmDamageX),
                        static_cast<int>(popup->shmDamageY),
                        static_cast<int>(popup->shmDamageWidth),
                        static_cast<int>(popup->shmDamageHeight), opacity,
                        0.0f, 2.0f, false,
                        popupBounds.width, popupBounds.height);
                } else {
                    renderer.getRasterRenderer()->drawDmaBufTextureTransformed(
                        popupBounds.x, popupBounds.y,
                        static_cast<int>(popup->width), static_cast<int>(popup->height),
                        static_cast<int>(popup->backingWidth),
                        static_cast<int>(popup->backingHeight),
                        popup->dmaBufTexture, opacity,
                        0.0f, 2.0f, false,
                        popupBounds.width, popupBounds.height);
                }

                if (popup->insetBorderEnabled) {
                    constexpr float kPopupCornerRadiusLogical = 10.0f;
                    const float borderRadius = popup->cornerRadius >= 0.0f
                        ? popup->cornerRadius
                        : kPopupCornerRadiusLogical;
                    const graphics::RectF popupLogical{
                        popup->popupX,
                        popup->popupY,
                        popup->initialWidth, popup->initialHeight};
                    replayLogicalList(
                        buildDoubleInsetBorderDisplayList(
                            popupLogical, borderRadius,
                            std::clamp(popup->cornerRoundness, 2.0f, 8.0f),
                            opacity),
                        group.localToGlobal);
                }
            }
        }
    }

    // 3. Hardware cursor only (no software cursor fallback)
    if (displayBackend.isHardwareCursorActive()) {
        displayBackend.moveHardwareCursor(
            static_cast<int>(std::lround(windowManager.getMouseX() * outputScale)),
            static_cast<int>(std::lround(windowManager.getMouseY() * outputScale)));
    }

    // Presentation and diagnostic overlays must not inherit scene damage.
    raster->setFrameDamageRect(std::nullopt);
    raster->setClipRect(std::nullopt);
    if (beforePresent) beforePresent();
    renderer.swapBuffers();
}

} // namespace lcl::core
