#include "core/compositor/compositor_renderer.hpp"
#include "core/compositor/window_group_transform.hpp"
#include "core/display/display_scale.hpp"
#include "theme/palette.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace lcl::core {
namespace {
uint8_t applyOpacityToAlpha(uint8_t alpha, float opacity) {
    const float scaled = std::clamp(static_cast<float>(alpha) * std::clamp(opacity, 0.0f, 1.0f), 0.0f, 255.0f);
    return static_cast<uint8_t>(std::lround(scaled));
}

std::string truncateTitle(const std::string& title, float width, float fontSize) {
    const int count = static_cast<int>(width / std::max(1.0f, fontSize * 0.6f));
    if (count <= 0) return {};
    if (static_cast<int>(title.size()) <= count) return title;
    return count <= 3 ? title.substr(0, static_cast<size_t>(count))
                      : title.substr(0, static_cast<size_t>(count - 3)) + "...";
}
} // namespace

void CompositorRenderer::render(render::Renderer& renderer,
                                 DisplayManager& displayManager,
                                 const render::WindowManager& windowManager,
                                 const SurfaceRegistry::Snapshot& surfaces,
                                 const std::function<void()>& beforePresent) const {
    using SurfaceEntry = SurfaceRegistry::SurfaceEntry;
    // The snapshot contains only const entry pointers, so protocol/input work
    // cannot mutate the surface state while this frame is being composed.

    // --- Begin Skia frame ---
    auto* skia = renderer.getSkiaRenderer();
    skia->beginFrame();

    constexpr float kWindowCornerRadiusLogical = 20.0f;

    auto resolveWindowCornerRadiusPx = [&](const render::Window& win) {
        if (win.cornerRadiusPx >= 0.0f) {
            return win.cornerRadiusPx;
        }

        if (win.decorationMode == render::DecorationMode::SSD) {
            return DisplayScale::pxF(kWindowCornerRadiusLogical);
        }

        if (win.decorationMode == render::DecorationMode::None &&
            win.title.find("Terminal") != std::string::npos) {
            return DisplayScale::pxF(kWindowCornerRadiusLogical);
        }

        return 0.0f;
    };
    auto resolveWindowCornerRoundness = [](const render::Window& win) {
        return std::clamp(win.cornerRoundness, 2.0f, 8.0f);
    };

    auto drawChrome = [&](const render::Window& win, const WindowGroupTransform& group,
                          float chromeOpacity, bool drawTitlebar,
                          bool showBackdropThroughTitlebar) {
        const float scale = DisplayScale::factor() * group.scale;
        const float titleHeight = group.titleHeight;
        const float radius = resolveWindowCornerRadiusPx(win) * group.scale;
        const float roundness = resolveWindowCornerRoundness(win);
        const auto& chromeStyle = win.chrome.style();
        const float controlSize = chromeStyle.controlSize * scale;
        const float controlGap = chromeStyle.controlGap * scale;
        const float fontSize = static_cast<float>(DisplayScale::kBaseFontPx) * scale;
        const render::WindowChromeLayout layout = win.chrome.layout(
            group.width, titleHeight, radius, fontSize, scale);

        if (drawTitlebar) {
            // A WindowGroup backdrop already occupies this titlebar area. Keep
            // chrome as a legible tint over it instead of replacing it with an
            // opaque server rectangle.
            const uint8_t titlebarBaseAlpha = showBackdropThroughTitlebar ? 190 : 255;
            skia->drawTopRoundedRect(
                {group.x, group.y, group.width, titleHeight},
                std::min(radius, titleHeight),
                {17, 19, 23, applyOpacityToAlpha(titlebarBaseAlpha, chromeOpacity)},
                roundness);
        }

        for (int index = 0; index < 3; ++index) {
            const auto& control = win.chrome.control(static_cast<size_t>(index));
            const float interactionScale = std::clamp(control.scale, 0.90f, 1.08f);
            const float emphasis = std::clamp(control.emphasis, 0.0f, 2.0f);
            const float stateMix = std::min(1.0f, emphasis);
            const float pressedMix = std::max(0.0f, emphasis - 1.0f);
            const auto mix = [](float from, float to, float amount) {
                return from + (to - from) * amount;
            };
            const auto mixedByte = [&](uint8_t normal, uint8_t hover, uint8_t pressed) {
                const float hoverValue = mix(static_cast<float>(normal), static_cast<float>(hover), stateMix);
                return static_cast<uint8_t>(std::clamp(std::lround(
                    mix(hoverValue, static_cast<float>(pressed), pressedMix)), 0l, 255l));
            };
            const float baseLeft = group.x + layout.controlLeft +
                                   static_cast<float>(index) * (controlSize + controlGap);
            const float baseTop = group.y + layout.controlTop;
            const float drawSize = controlSize * interactionScale;
            const float left = baseLeft + (controlSize - drawSize) * 0.5f;
            const float top = baseTop + (controlSize - drawSize) * 0.5f;
            const auto mixedColor = [&](const render::WindowChromeColor& normal,
                                        const render::WindowChromeColor& hover,
                                        const render::WindowChromeColor& pressed) {
                return render::SkiaColor{
                    mixedByte(normal.r, hover.r, pressed.r),
                    mixedByte(normal.g, hover.g, pressed.g),
                    mixedByte(normal.b, hover.b, pressed.b),
                    applyOpacityToAlpha(mixedByte(normal.a, hover.a, pressed.a), chromeOpacity),
                };
            };
            skia->drawRoundedRect(
                {left, top, drawSize, drawSize}, drawSize * 0.5f,
                mixedColor(chromeStyle.normalBackground, chromeStyle.hoverBackground,
                           chromeStyle.pressedBackground),
                mixedColor(chromeStyle.normalBorder, chromeStyle.hoverBorder,
                           chromeStyle.pressedBorder),
                std::max(chromeStyle.borderWidth, group.scale), chromeStyle.roundness);
        }

        if (drawTitlebar) {
            const std::string title = truncateTitle(win.chrome.title(), layout.titleWidth, fontSize);
            skia->drawString(
                static_cast<int>(std::lround(group.x + layout.titleLeft)),
                static_cast<int>(std::lround(group.y + layout.titleTop)),
                title,
                (static_cast<uint32_t>(applyOpacityToAlpha(245, chromeOpacity)) << 24) | 0x00F0F8FFu,
                fontSize);
        }
    };

    auto drawForcedInsetBorder = [&](const render::Window& win, const WindowGroupTransform& group,
                                     float opacity, float scale) {
        const uint8_t outerA = applyOpacityToAlpha(120, opacity);
        const uint8_t innerA = applyOpacityToAlpha(86, opacity);
        float baseRadius = resolveWindowCornerRadiusPx(win);
        if (baseRadius <= 0.001f) {
            baseRadius = DisplayScale::pxF(kWindowCornerRadiusLogical);
        }
        const float radius = std::max(0.0f, baseRadius * scale);

        auto* sr = renderer.getSkiaRenderer();
        sr->drawRoundedRect(
            {group.x, group.y, group.width, group.height},
            radius,
            {0, 0, 0, 0},
            {10, 12, 16, outerA},
            1.0f,
            resolveWindowCornerRoundness(win));

        const float inset = 1.0f;
        const float innerW = std::max(0.0f, group.width - inset * 2.0f);
        const float innerH = std::max(0.0f, group.height - inset * 2.0f);
        if (innerW > 0.0f && innerH > 0.0f) {
            sr->drawRoundedRect(
                {group.x + inset, group.y + inset, innerW, innerH},
                std::max(0.0f, radius - 1.0f),
                {0, 0, 0, 0},
                {245, 248, 252, innerA},
                1.0f,
                resolveWindowCornerRoundness(win));
        }
    };

    // 1. Clear Desktop Canvas (Black background)
    renderer.clear(0xFF000000);

    // 2. Atomic Z-Stacking Window Group Rendering (Frame + Client Surface per Window in Z-order)
    using core::DisplayScale;
    auto applySurfaceRegionEffects = [&](const render::Window& win,
                                         const SurfaceEntry& surface,
                                         protocol::EffectSourceType sourceType,
                                         float windowOpacity,
                                         const WindowGroupTransform& group) {
        int titleOffset = (win.decorationMode == render::DecorationMode::SSD)
            ? DisplayScale::titleBarHeight()
            : 0;

        for (const auto& fx : surface.effectRegions) {
            if (fx.region.source != sourceType || fx.filters.empty()) continue;

            const bool windowGroup = fx.region.boundsPolicy == protocol::EffectBoundsPolicy::WindowGroup;
            int fxX = 0;
            int fxY = 0;
            int fxW = 0;
            int fxH = 0;
            float cornerRadius = 0.0f;
            float cornerRoundness = 2.0f;
            if (windowGroup) {
                // The compositor owns WindowGroup geometry, including the SSD
                // titlebar. A root backdrop therefore cannot drift below it or
                // select a different corner superellipse than the window mask.
                fxX = static_cast<int>(std::lround(group.x));
                fxY = static_cast<int>(std::lround(group.y));
                fxW = std::max(1, static_cast<int>(std::lround(group.width)));
                fxH = std::max(1, static_cast<int>(std::lround(group.height)));
                cornerRadius = resolveWindowCornerRadiusPx(win) * group.scale;
                cornerRoundness = resolveWindowCornerRoundness(win);
            } else {
                fxX = win.x + (fx.followSurfaceBounds ? 0 : fx.region.x);
                fxY = win.y + titleOffset + (fx.followSurfaceBounds ? 0 : fx.region.y);
                fxW = fx.followSurfaceBounds ? static_cast<int>(surface.width) : static_cast<int>(fx.region.width);
                fxH = fx.followSurfaceBounds ? static_cast<int>(surface.height) : static_cast<int>(fx.region.height);
                cornerRadius = std::max(0.0f, fx.region.cornerRadius);
                cornerRoundness = std::clamp(fx.region.cornerRoundness, 2.0f, 8.0f);
            }
            if (fxW <= 0 || fxH <= 0) continue;

            // Initial executor supports chain filters with source-type routing.
            // Advanced blend modes are currently treated as normal blend.
            renderer.getSkiaRenderer()->applyBackdropFilter(
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
        for (const auto& surface : surfaces) {
            const auto* entry = surface.entry;
            if (entry && entry->windowId == win.id && entry->hasRenderableBuffer()) {
                matchingSurface = entry;
                break;
            }
        }

        int titleOffset = (win.decorationMode == render::DecorationMode::SSD)
            ? DisplayScale::titleBarHeight()
            : 0;

        float windowOpacity = 1.0f;
        float windowScale = 1.0f;
        if (matchingSurface) {
            windowOpacity = std::clamp(matchingSurface->transitionOpacity, 0.0f, 1.0f);
            windowScale = std::clamp(matchingSurface->transitionScale, 0.80f, 1.20f);
        }

        const WindowGroupTransform group = makeWindowGroupTransform(win, titleOffset, windowScale);

        // B. Apply effect-graph backdrop regions (new pipeline only)
        if (matchingSurface && !matchingSurface->effectRegions.empty()) {
            applySurfaceRegionEffects(win, *matchingSurface, protocol::EffectSourceType::Backdrop,
                                      windowOpacity, group);
        }

        const bool hasWindowGroupBackdrop = matchingSurface && std::any_of(
            matchingSurface->effectRegions.begin(), matchingSurface->effectRegions.end(),
            [](const SurfaceRegistry::SurfaceEffectRegion& effect) {
                return effect.region.source == protocol::EffectSourceType::Backdrop &&
                       effect.region.boundsPolicy == protocol::EffectBoundsPolicy::WindowGroup &&
                       !effect.filters.empty();
            });

        if (matchingSurface) {
            int srcW = static_cast<int>(matchingSurface->width);
            int srcH = static_cast<int>(matchingSurface->height);
            int stridePixels = static_cast<int>(matchingSurface->stride / 4);
            float drawX = group.x;
            float drawY = group.y;
            float drawW = group.width;
            float drawH = group.height;

            if (win.decorationMode == render::DecorationMode::SSD) {
                // The client buffer occupies the remaining exact pixels of
                // the same transformed group rect used by the titlebar.
                drawY += group.titleHeight;
                drawH = std::max(1.0f, group.height - group.titleHeight);
            }

            const float windowCornerRadiusPx = resolveWindowCornerRadiusPx(win);
            const bool maskToWindowShape = windowCornerRadiusPx > 0.001f;

            const bool hasPrevious = matchingSurface->previousPixels ||
                                     matchingSurface->previousDmaBufTexture != 0;
            if (matchingSurface->previousPixels) {
                renderer.getSkiaRenderer()->drawBufferTransformed(
                    drawX, drawY,
                    static_cast<int>(matchingSurface->previousWidth),
                    static_cast<int>(matchingSurface->previousHeight),
                    reinterpret_cast<const uint32_t*>(matchingSurface->previousPixels),
                    static_cast<int>(matchingSurface->previousStride / 4),
                    windowOpacity * (1.0f - matchingSurface->resizeCrossfadeProgress),
                    maskToWindowShape ? windowCornerRadiusPx : 0.0f,
                    resolveWindowCornerRoundness(win),
                    win.decorationMode == render::DecorationMode::SSD,
                    drawW,
                    drawH);
            } else if (matchingSurface->previousDmaBufTexture != 0) {
                renderer.getSkiaRenderer()->drawDmaBufTextureTransformed(
                    drawX, drawY,
                    static_cast<int>(matchingSurface->previousWidth),
                    static_cast<int>(matchingSurface->previousHeight),
                    matchingSurface->previousDmaBufTexture,
                    windowOpacity * (1.0f - matchingSurface->resizeCrossfadeProgress),
                    maskToWindowShape ? windowCornerRadiusPx : 0.0f,
                    resolveWindowCornerRoundness(win),
                    win.decorationMode == render::DecorationMode::SSD,
                    drawW, drawH);
            }

            const float currentOpacity = windowOpacity *
                (hasPrevious ? matchingSurface->resizeCrossfadeProgress : 1.0f);
            if (matchingSurface->pixels) {
                renderer.getSkiaRenderer()->drawBufferTransformed(
                    drawX, drawY, srcW, srcH,
                    reinterpret_cast<const uint32_t*>(matchingSurface->pixels),
                    stridePixels, currentOpacity,
                    maskToWindowShape ? windowCornerRadiusPx : 0.0f,
                    resolveWindowCornerRoundness(win),
                    win.decorationMode == render::DecorationMode::SSD,
                    drawW, drawH);
            } else {
                renderer.getSkiaRenderer()->drawDmaBufTextureTransformed(
                    drawX, drawY, srcW, srcH, matchingSurface->dmaBufTexture,
                    currentOpacity,
                    maskToWindowShape ? windowCornerRadiusPx : 0.0f,
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
            drawChrome(win, group, windowOpacity, true, hasWindowGroupBackdrop);
        }

        // Forced compositor-owned inset border for every window, independent from app UI.
        if (win.drawInsetBorder) {
            drawForcedInsetBorder(win, group, windowOpacity, windowScale);
        }
    }

    // 3. Hardware cursor only (no software cursor fallback)
    if (displayManager.isHardwareCursorActive()) {
        displayManager.moveHardwareCursor(windowManager.getMouseX(), windowManager.getMouseY());
    }

    if (beforePresent) beforePresent();
    renderer.swapBuffers();
}

} // namespace lcl::core
