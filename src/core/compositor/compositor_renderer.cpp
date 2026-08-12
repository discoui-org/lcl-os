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

struct ChromeLayout {
    float controlLeft{0.0f};
    float controlTop{0.0f};
    float titleLeft{0.0f};
    float titleTop{0.0f};
    float titleWidth{0.0f};
};

ChromeLayout calculateChromeLayout(float width, float titleHeight, float cornerRadius,
                                   float fontSize, float scale) {
    const float controlSize = 16.0f * scale;
    const float controlGap = 6.0f * scale;
    const float inset = std::max({8.0f * scale, cornerRadius - 8.0f * scale, 4.0f * scale});
    const float titleLeft = std::max(14.0f * scale,
        inset + controlSize * 3.0f + controlGap * 2.0f + 12.0f * scale);
    return {
        inset,
        inset,
        titleLeft,
        std::clamp(inset + (controlSize - fontSize) * 0.5f, 0.0f,
                   std::max(0.0f, titleHeight - fontSize)),
        std::max(0.0f, width - titleLeft - 10.0f * scale),
    };
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
                                 const SurfaceRegistry::Snapshot& surfaces) const {
    using SurfaceEntry = SurfaceRegistry::SurfaceEntry;
    // The snapshot contains only const entry pointers, so protocol/input work
    // cannot mutate the surface state while this frame is being composed.

    // --- Begin Skia frame ---
    auto* skia = renderer.getSkiaRenderer();
    skia->beginFrame();

    constexpr float kWindowCornerRadiusLogical = 20.0f;
    constexpr float kWindowCornerRoundness = 2.0f;

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

    auto drawChrome = [&](const render::Window& win, const WindowGroupTransform& group,
                          float chromeOpacity, bool drawTitlebar) {
        const float scale = DisplayScale::factor() * group.scale;
        const float titleHeight = static_cast<float>(group.titleHeight);
        const float radius = DisplayScale::pxF(kWindowCornerRadiusLogical) * group.scale;
        const float controlSize = 16.0f * scale;
        const float controlGap = 6.0f * scale;
        const float fontSize = static_cast<float>(DisplayScale::kBaseFontPx) * scale;
        const ChromeLayout layout = calculateChromeLayout(
            static_cast<float>(group.width), titleHeight, radius, fontSize, scale);

        if (drawTitlebar) {
            skia->drawTopRoundedRect(
                {static_cast<float>(group.x), static_cast<float>(group.y),
                 static_cast<float>(group.width), titleHeight},
                std::min(radius, titleHeight),
                {17, 19, 23, applyOpacityToAlpha(255, chromeOpacity)},
                kWindowCornerRoundness);
        }

        const char* glyphs[] = {"x", "-", "+"};
        for (int index = 0; index < 3; ++index) {
            const float left = static_cast<float>(group.x) + layout.controlLeft +
                               static_cast<float>(index) * (controlSize + controlGap);
            const float top = static_cast<float>(group.y) + layout.controlTop;
            skia->drawRoundedRect(
                {left, top, controlSize, controlSize}, controlSize * 0.5f,
                {235, 241, 248, applyOpacityToAlpha(56, chromeOpacity)},
                {230, 238, 248, applyOpacityToAlpha(120, chromeOpacity)},
                std::max(1.0f, group.scale), 2.0f);
            skia->drawString(
                static_cast<int>(std::lround(left + controlSize * 0.32f)),
                static_cast<int>(std::lround(top + controlSize * 0.16f)),
                glyphs[index],
                (static_cast<uint32_t>(applyOpacityToAlpha(224, chromeOpacity)) << 24) | 0x00ECF4FCu,
                11.0f * scale);
        }

        if (drawTitlebar) {
            const std::string title = truncateTitle(win.title, layout.titleWidth, fontSize);
            skia->drawString(
                static_cast<int>(std::lround(static_cast<float>(group.x) + layout.titleLeft)),
                static_cast<int>(std::lround(static_cast<float>(group.y) + layout.titleTop)),
                title,
                (static_cast<uint32_t>(applyOpacityToAlpha(245, chromeOpacity)) << 24) | 0x00F0F8FFu,
                fontSize);
        }
    };

    auto drawForcedInsetBorder = [&](const render::Window& win, float opacity, float scale) {
        const uint8_t outerA = applyOpacityToAlpha(120, opacity);
        const uint8_t innerA = applyOpacityToAlpha(86, opacity);
        float baseRadius = resolveWindowCornerRadiusPx(win);
        if (baseRadius <= 0.001f) {
            baseRadius = DisplayScale::pxF(kWindowCornerRadiusLogical);
        }
        const float radius = std::max(0.0f, baseRadius * scale);

        auto* sr = renderer.getSkiaRenderer();
        sr->drawRoundedRect(
            {static_cast<float>(win.x), static_cast<float>(win.y), static_cast<float>(win.width), static_cast<float>(win.height)},
            radius,
            {0, 0, 0, 0},
            {10, 12, 16, outerA},
            1.0f,
            kWindowCornerRoundness);

        const float inset = 1.0f;
        const float innerW = std::max(0.0f, static_cast<float>(win.width) - inset * 2.0f);
        const float innerH = std::max(0.0f, static_cast<float>(win.height) - inset * 2.0f);
        if (innerW > 0.0f && innerH > 0.0f) {
            sr->drawRoundedRect(
                {static_cast<float>(win.x) + inset, static_cast<float>(win.y) + inset, innerW, innerH},
                std::max(0.0f, radius - 1.0f),
                {0, 0, 0, 0},
                {245, 248, 252, innerA},
                1.0f,
                kWindowCornerRoundness);
        }
    };

    // 1. Clear Desktop Canvas (Black background)
    renderer.clear(0xFF000000);

    // 2. Atomic Z-Stacking Window Group Rendering (Frame + Client Surface per Window in Z-order)
    using core::DisplayScale;
    auto applySurfaceRegionEffects = [&](const render::Window& win,
                                         const SurfaceEntry& surface,
                                         protocol::EffectSourceType sourceType,
                                         float windowOpacity) {
        int titleOffset = (win.decorationMode == render::DecorationMode::SSD)
            ? DisplayScale::titleBarHeight()
            : 0;

        for (const auto& fx : surface.effectRegions) {
            if (fx.region.source != sourceType || fx.filters.empty()) continue;

            int fxX = win.x + (fx.followSurfaceBounds ? 0 : fx.region.x);
            int fxY = win.y + titleOffset + (fx.followSurfaceBounds ? 0 : fx.region.y);
            int fxW = fx.followSurfaceBounds ? static_cast<int>(surface.width) : static_cast<int>(fx.region.width);
            int fxH = fx.followSurfaceBounds ? static_cast<int>(surface.height) : static_cast<int>(fx.region.height);
            if (fxW <= 0 || fxH <= 0) continue;

            // Initial executor supports chain filters with source-type routing.
            // Advanced blend modes are currently treated as normal blend.
            renderer.getSkiaRenderer()->applyBackdropFilter(
                fxX, fxY, fxW, fxH,
                std::max(0.0f, fx.region.cornerRadius),
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
            if (entry && entry->windowId == win.id && entry->pixels) {
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
            applySurfaceRegionEffects(win, *matchingSurface, protocol::EffectSourceType::Backdrop, windowOpacity);
        }

        if (matchingSurface) {
            int srcW = static_cast<int>(matchingSurface->width);
            int srcH = static_cast<int>(matchingSurface->height);
            int stridePixels = static_cast<int>(matchingSurface->stride / 4);
            int drawX = group.x;
            int drawY = group.y;
            int drawW = group.width;
            int drawH = group.height;

            if (win.decorationMode == render::DecorationMode::SSD) {
                // The client buffer occupies the remaining exact pixels of
                // the same transformed group rect used by the titlebar.
                drawY += group.titleHeight;
                drawH = std::max(1, group.height - group.titleHeight);
            }

            const float windowCornerRadiusPx = resolveWindowCornerRadiusPx(win);
            const bool maskToWindowShape = windowCornerRadiusPx > 0.001f;

            if (matchingSurface->previousPixels) {
                renderer.getSkiaRenderer()->drawBuffer(
                    drawX, drawY,
                    static_cast<int>(matchingSurface->previousWidth),
                    static_cast<int>(matchingSurface->previousHeight),
                    reinterpret_cast<const uint32_t*>(matchingSurface->previousPixels),
                    static_cast<int>(matchingSurface->previousStride / 4),
                    windowOpacity * (1.0f - matchingSurface->resizeCrossfadeProgress),
                    maskToWindowShape ? windowCornerRadiusPx : 0.0f,
                    kWindowCornerRoundness,
                    win.decorationMode == render::DecorationMode::SSD,
                    drawW,
                    drawH);
            }

            renderer.getSkiaRenderer()->drawBuffer(
                drawX, drawY, srcW, srcH,
                reinterpret_cast<const uint32_t*>(matchingSurface->pixels),
                stridePixels,
                windowOpacity * (matchingSurface->previousPixels
                    ? matchingSurface->resizeCrossfadeProgress : 1.0f),
                maskToWindowShape ? windowCornerRadiusPx : 0.0f,
                kWindowCornerRoundness,
                win.decorationMode == render::DecorationMode::SSD,
                drawW,
                drawH);

            if (!matchingSurface->effectRegions.empty()) {
                applySurfaceRegionEffects(win, *matchingSurface, protocol::EffectSourceType::Foreground, windowOpacity);
            }

            // Keep CSD for terminal content while rendering controls through compositor
            // so buttons match SSD quality (AA/blend pipeline) exactly.
            if (win.decorationMode == render::DecorationMode::None &&
                win.title.find("Terminal") != std::string::npos) {
                drawChrome(win, group, windowOpacity, false);
            }
        }

        // C. Render Server-Side Window Frame (Titlebar & Inset Border) on top of content.
        if (win.decorationMode == render::DecorationMode::SSD) {
            drawChrome(win, group, windowOpacity, true);
        }

        // Forced compositor-owned inset border for every window, independent from app UI.
        if (win.drawInsetBorder) {
            render::Window borderWin = win;
            borderWin.x = group.x;
            borderWin.y = group.y;
            borderWin.width = group.width;
            borderWin.height = group.height;
            drawForcedInsetBorder(borderWin, windowOpacity, windowScale);
        }
    }

    // 3. Hardware cursor only (no software cursor fallback)
    if (displayManager.isHardwareCursorActive()) {
        displayManager.moveHardwareCursor(windowManager.getMouseX(), windowManager.getMouseY());
    }

    renderer.swapBuffers();
}

} // namespace lcl::core
