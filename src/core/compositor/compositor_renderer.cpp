#include "core/compositor/compositor_renderer.hpp"
#include "core/compositor/double_inset_border.hpp"
#include "core/compositor/effect_region_geometry.hpp"
#include "core/compositor/window_chrome_material.hpp"
#include "core/compositor/popup_surface_geometry.hpp"
#include "render/window_group_transform.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace lcl::core {
void CompositorRenderer::render(render::Renderer& renderer,
                                 lcl::platform::IDisplayBackend& displayBackend,
                                 const render::WindowManager& windowManager,
                                 const SurfaceRegistry::Snapshot& surfaces,
                                 const std::function<void()>& beforePresent) const {
    using SurfaceEntry = SurfaceRegistry::SurfaceEntry;
    // The snapshot contains only const entry pointers, so protocol/input work
    // cannot mutate the surface state while this frame is being composed.

    // --- Begin LCL raster frame ---
    auto* raster = renderer.getRasterRenderer();
    raster->beginFrame();

    constexpr float kWindowCornerRadiusLogical = 20.0f;

    const float outputScale = raster->getDeviceScale();
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

    // 1. Clear Desktop Canvas (Black background)
    renderer.clear(0xFF000000);

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
                renderer.getRasterRenderer()->drawBufferTransformed(
                    drawX, drawY,
                    static_cast<int>(matchingSurface->previousWidth),
                    static_cast<int>(matchingSurface->previousHeight),
                    reinterpret_cast<const uint32_t*>(matchingSurface->previousPixels),
                    static_cast<int>(matchingSurface->previousStride / 4),
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
                renderer.getRasterRenderer()->drawBufferTransformed(
                    drawX, drawY, srcW, srcH,
                    reinterpret_cast<const uint32_t*>(matchingSurface->pixels),
                    stridePixels, currentOpacity,
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
            std::vector<const SurfaceEntry*> popups;
            for (const auto& surface : surfaces) {
                const auto* popup = surface.entry;
                if (popup && popup->parentSurfaceKey == matchingSurfaceKey &&
                    popup->hasCommittedBuffer && popup->hasRenderableBuffer() &&
                    !popup->pendingDestroy) {
                    popups.push_back(popup);
                }
            }
            std::sort(popups.begin(), popups.end(), [](const auto* lhs, const auto* rhs) {
                return lhs->popupOrder < rhs->popupOrder;
            });

            for (const auto* popup : popups) {
                const auto popupBounds = resolvePopupSurfaceBounds(
                    win, *matchingSurface, *popup);
                const float opacity = windowOpacity *
                    std::clamp(popup->transitionOpacity, 0.0f, 1.0f);
                if (popup->pixels) {
                    renderer.getRasterRenderer()->drawBufferTransformed(
                        popupBounds.x, popupBounds.y,
                        static_cast<int>(popup->width), static_cast<int>(popup->height),
                        reinterpret_cast<const uint32_t*>(popup->pixels),
                        static_cast<int>(popup->stride / 4), opacity,
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

    if (beforePresent) beforePresent();
    renderer.swapBuffers();
}

} // namespace lcl::core
