#include "core/compositor/compositor_renderer.hpp"
#include "core/display/display_scale.hpp"
#include "theme/palette.hpp"
#include "lcl-ui/core/rect.hpp"
#include "lcl-ui/core/render_pass.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/text.hpp"
#include "lcl-ui/widgets/window_chrome.hpp"
#include "render/skia_canvas.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace lcl::core {
namespace {
uint8_t applyOpacityToAlpha(uint8_t alpha, float opacity) {
    const float scaled = std::clamp(static_cast<float>(alpha) * std::clamp(opacity, 0.0f, 1.0f), 0.0f, 255.0f);
    return static_cast<uint8_t>(std::lround(scaled));
}
} // namespace

void CompositorRenderer::render(render::Renderer& renderer,
                                 DisplayManager& displayManager,
                                 const render::WindowManager& windowManager,
                                 const SurfaceRegistry::Snapshot& surfaces) const {
    using SurfaceEntry = SurfaceRegistry::SurfaceEntry;
    // The snapshot contains only const entry pointers, so protocol/input work
    // cannot mutate the surface state while this frame is being composed.

    std::vector<render::WindowRenderContent> contents;
    for (const auto& win : windowManager.getWindows()) {
        if (win.isMinimized) {
            continue;
        }
        bool hasShmBuffer = false;
        for (const auto& surface : surfaces) {
            const auto* entry = surface.entry;
            if (entry && entry->windowId == win.id && entry->pixels) {
                hasShmBuffer = true;
                break;
            }
        }
        if (!hasShmBuffer) {
            render::WindowRenderContent content;
            content.windowId = win.id;
            content.lines = {"LCL OS Desktop", "Waiting for client surface buffer..."};
            content.cursorCol = 0;
            contents.push_back(std::move(content));
        }
    }

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

    auto drawSsdChromeWithLclUi = [&](const render::Window& win,
                                      float chromeOpacity,
                                      float chromeScale) {
        auto fadeUiColor = [&](const lcl::ui::Color& c) {
            return lcl::ui::Color{c.r, c.g, c.b, applyOpacityToAlpha(c.a, chromeOpacity)};
        };

        // Chrome is an lcl-ui subtree: all of its style and Yoga dimensions stay
        // logical.  Only the renderer knows how to map it into the physical
        // compositor framebuffer (including the per-window entrance scale).
        const float dpr = DisplayScale::factor();
        const float logicalWidth = static_cast<float>(win.width) / dpr;
        const float logicalHeight = static_cast<float>(win.height) / dpr;
        const float physicalWidth = static_cast<float>(win.width) * chromeScale;
        const float physicalHeight = static_cast<float>(win.height) * chromeScale;
        const float physicalX = static_cast<float>(win.x) +
            (static_cast<float>(win.width) - physicalWidth) * 0.5f;
        const float physicalY = static_cast<float>(win.y) +
            (static_cast<float>(win.height) - physicalHeight) * 0.5f;

        const float previousScale = skia->getContentScale();
        const float previousOriginX = skia->getContentOriginX();
        const float previousOriginY = skia->getContentOriginY();
        skia->setContentScale(dpr * chromeScale);
        skia->setContentOrigin(physicalX, physicalY);

        lcl::ui::RenderPass pass;
        auto root = std::make_unique<lcl::ui::Container>();
        root->setRenderPass(&pass);
        root->setBackgroundColor(lcl::ui::Color{0, 0, 0, 0});
        root->getYogaNode().setWidth(logicalWidth);
        root->getYogaNode().setHeight(logicalHeight);

        lcl::ui::chrome::WindowChromeStyle chromeStyle;
        chromeStyle.titleBarBackground = fadeUiColor(lcl::ui::Color{17, 19, 23, 255});
        chromeStyle.titleBarCornerRadiusAdjust = 0.0f;
        chromeStyle.titleBarRoundness = kWindowCornerRoundness;
        chromeStyle.buttonRoundness = 2.0f;
        chromeStyle.buttonBackground = fadeUiColor(chromeStyle.buttonBackground);
        chromeStyle.buttonBorder = fadeUiColor(chromeStyle.buttonBorder);
        chromeStyle.buttonGlyph = fadeUiColor(chromeStyle.buttonGlyph);
        chromeStyle.titleColor = fadeUiColor(chromeStyle.titleColor);

        const float titleH = static_cast<float>(DisplayScale::kTitleBarHeight);

        auto titleBar = lcl::ui::chrome::buildWindowTitlebar(
            logicalWidth,
            titleH,
            kWindowCornerRadiusLogical,
            win.title,
            static_cast<float>(DisplayScale::kBaseFontPx),
            chromeStyle);

        root->addChild(std::move(titleBar));

        root->getYogaNode().calculateLayout(logicalWidth, logicalHeight);
        root->syncLayout(0.0f, 0.0f);

        lcl::ui::Rect damage{0.0f, 0.0f, logicalWidth, logicalHeight};
        lcl::render::SkiaCanvas canvas(*skia);
        root->draw(canvas, damage);

        skia->setContentOrigin(previousOriginX, previousOriginY);
        skia->setContentScale(previousScale);
    };

    auto drawCsdHeaderControlsOverlay = [&](const render::Window& win) {
        lcl::ui::RenderPass pass;
        auto root = std::make_unique<lcl::ui::Container>();
        root->setRenderPass(&pass);
        root->setBackgroundColor(lcl::ui::Color{0, 0, 0, 0});

        const float titleH = static_cast<float>(DisplayScale::titleBarHeight());
        root->getYogaNode().setWidth(static_cast<float>(win.width));
        root->getYogaNode().setHeight(static_cast<float>(win.height));

        // This compatibility overlay has no title strip of its own, but its
        // controls are the exact same lcl-ui widgets as SSD and CSD clients.
        lcl::ui::chrome::WindowChromeStyle chromeStyle;
        chromeStyle.titleBarBackground = {0, 0, 0, 0};
        root->addChild(lcl::ui::chrome::buildWindowTitlebar(
            static_cast<float>(win.width),
            titleH,
            DisplayScale::pxF(kWindowCornerRadiusLogical),
            "",
            static_cast<float>(DisplayScale::kBaseFontPx),
            chromeStyle));

        root->getYogaNode().calculateLayout(static_cast<float>(win.width), static_cast<float>(win.height));
        root->syncLayout(static_cast<float>(win.x), static_cast<float>(win.y));

        lcl::ui::Rect damage{static_cast<float>(win.x), static_cast<float>(win.y), static_cast<float>(win.width), static_cast<float>(win.height)};
        lcl::render::SkiaCanvas canvas(*skia);
        root->draw(canvas, damage);
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

        const float winCenterX = static_cast<float>(win.x) + static_cast<float>(win.width) * 0.5f;
        const float winCenterY = static_cast<float>(win.y) + static_cast<float>(win.height) * 0.5f;
        const int scaledWinW = std::max(1, static_cast<int>(std::lround(static_cast<float>(win.width) * windowScale)));
        const int scaledWinH = std::max(1, static_cast<int>(std::lround(static_cast<float>(win.height) * windowScale)));
        const int scaledWinX = static_cast<int>(std::lround(winCenterX - static_cast<float>(scaledWinW) * 0.5f));
        const int scaledWinY = static_cast<int>(std::lround(winCenterY - static_cast<float>(scaledWinH) * 0.5f));
        const int scaledTitleOffset = static_cast<int>(std::lround(static_cast<float>(titleOffset) * windowScale));

        // B. Apply effect-graph backdrop regions (new pipeline only)
        if (matchingSurface && !matchingSurface->effectRegions.empty()) {
            applySurfaceRegionEffects(win, *matchingSurface, protocol::EffectSourceType::Backdrop, windowOpacity);
        }

        if (matchingSurface) {
            int dstX = win.x;
            int dstY = win.y + titleOffset;
            int srcW = static_cast<int>(matchingSurface->width);
            int srcH = static_cast<int>(matchingSurface->height);
            int stridePixels = static_cast<int>(matchingSurface->stride / 4);
            int drawW = std::max(1, static_cast<int>(std::lround(static_cast<float>(srcW) * windowScale)));
            int drawH = std::max(1, static_cast<int>(std::lround(static_cast<float>(srcH) * windowScale)));
            int drawX = static_cast<int>(std::lround(winCenterX + (static_cast<float>(dstX) - winCenterX) * windowScale));
            int drawY = static_cast<int>(std::lround(winCenterY + (static_cast<float>(dstY) - winCenterY) * windowScale));

            if (win.decorationMode == render::DecorationMode::SSD) {
                // Keep a strict non-overlapping partition between SSD titlebar and client content
                // under scaling to avoid double-alpha where layers touch.
                drawX = scaledWinX;
                drawY = scaledWinY + scaledTitleOffset;
                drawW = std::max(1, scaledWinW);
                drawH = std::max(1, scaledWinH - scaledTitleOffset);
            }

            const float windowCornerRadiusPx = resolveWindowCornerRadiusPx(win);
            const bool maskToWindowShape = windowCornerRadiusPx > 0.001f;

            renderer.getSkiaRenderer()->drawBuffer(
                drawX, drawY, srcW, srcH,
                reinterpret_cast<const uint32_t*>(matchingSurface->pixels),
                stridePixels,
                windowOpacity,
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
                render::Window scaledOverlayWin = win;
                scaledOverlayWin.x = scaledWinX;
                scaledOverlayWin.y = scaledWinY;
                scaledOverlayWin.width = scaledWinW;
                scaledOverlayWin.height = scaledWinH;
                drawCsdHeaderControlsOverlay(scaledOverlayWin);
            }
        } else {
            // Render text fallback content ONLY for standard SSD decorated application windows
            if (win.decorationMode == render::DecorationMode::SSD) {
                const render::WindowRenderContent* content = nullptr;
                for (const auto& c : contents) {
                    if (c.windowId == win.id) { content = &c; break; }
                }
                if (content) {
                    int titleOffset = DisplayScale::titleBarHeight();
                    int textX = win.x + DisplayScale::windowPad();
                    int textY = win.y + titleOffset + DisplayScale::px(12);
                    for (const auto& line : content->lines) {
                        skia->drawString(textX, textY, line, lcl::theme::UI::TerminalText);
                        textY += DisplayScale::px(18);
                    }
                }
            }
        }

        // C. Render Server-Side Window Frame (Titlebar & Inset Border) on top of content.
        if (win.decorationMode == render::DecorationMode::SSD) {
            drawSsdChromeWithLclUi(win, windowOpacity, windowScale);
        }

        // Forced compositor-owned inset border for every window, independent from app UI.
        if (win.drawInsetBorder) {
            render::Window borderWin = win;
            borderWin.x = scaledWinX;
            borderWin.y = scaledWinY;
            borderWin.width = scaledWinW;
            borderWin.height = scaledWinH;
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
