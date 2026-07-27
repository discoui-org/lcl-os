#include <iostream>
#include <memory>
#include <cmath>
#include <chrono>
#include <thread>
#include <string>
#include <csignal>

#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "render/skia_renderer.hpp"

using namespace lcl::ui;
using namespace lcl::render;

class ShaderCanvasWidget : public Widget {
public:
    ShaderCanvasWidget() {
        m_startTime = std::chrono::steady_clock::now();
        m_lastFpsTime = m_startTime;
    }

    void draw(SkCanvas* canvas, const Rect& damageRect) override {
        (void)damageRect;
        auto* skia = reinterpret_cast<SkiaRenderer*>(canvas);
        if (!skia) return;

        auto now = std::chrono::steady_clock::now();
        float t = std::chrono::duration<float>(now - m_startTime).count();

        // 1. Calculate rolling client FPS
        m_frameCount++;
        float elapsedMs = std::chrono::duration<float, std::milli>(now - m_lastFpsTime).count();
        if (elapsedMs >= 250.0f) {
            m_clientFps = (m_frameCount * 1000.0f) / elapsedMs;
            m_frameCount = 0;
            m_lastFpsTime = now;
        }

        // 2. Animated Background Gradient (Cycling HSL-inspired palette)
        uint8_t topR = static_cast<uint8_t>((std::sin(t * 0.8f + 0.0f) * 0.5f + 0.5f) * 120.0f + 15.0f);
        uint8_t topG = static_cast<uint8_t>((std::sin(t * 0.8f + 2.0f) * 0.5f + 0.5f) * 80.0f + 20.0f);
        uint8_t topB = static_cast<uint8_t>((std::sin(t * 0.8f + 4.0f) * 0.5f + 0.5f) * 160.0f + 40.0f);

        uint8_t botR = static_cast<uint8_t>((std::sin(t * 0.5f + 3.0f) * 0.5f + 0.5f) * 40.0f + 10.0f);
        uint8_t botG = static_cast<uint8_t>((std::sin(t * 0.5f + 1.0f) * 0.5f + 0.5f) * 60.0f + 15.0f);
        uint8_t botB = static_cast<uint8_t>((std::sin(t * 0.5f + 5.0f) * 0.5f + 0.5f) * 100.0f + 30.0f);

        skia->drawBackgroundGradient(
            SkiaColor{topR, topG, topB, 255},
            SkiaColor{botR, botG, botB, 255}
        );

        // 3. Render Animated Glowing Orbitals
        float w = m_absoluteBounds.width;
        float h = m_absoluteBounds.height;
        float cx = w * 0.5f;
        float cy = h * 0.5f;

        for (int i = 0; i < 4; ++i) {
            float phase = t * (1.5f + i * 0.5f) + i * 1.57f;
            float orbitR = 80.0f + i * 25.0f;
            float ox = cx + std::cos(phase) * orbitR;
            float oy = cy + std::sin(phase) * (orbitR * 0.6f);

            uint8_t orbR = static_cast<uint8_t>((std::sin(phase) * 0.5f + 0.5f) * 200.0f + 55.0f);
            uint8_t orbG = static_cast<uint8_t>((std::cos(phase) * 0.5f + 0.5f) * 200.0f + 55.0f);
            uint8_t orbB = 240;

            skia->drawCircle(ox, oy, 14.0f, SkiaColor{orbR, orbG, orbB, 220});
        }

        // 4. Central Glassmorphic Card Overlay
        float cardW = 380.0f;
        float cardH = 180.0f;
        float cardX = cx - cardW * 0.5f;
        float cardY = cy - cardH * 0.5f;

        SkiaRect cardRect{cardX, cardY, cardW, cardH};

        // Soft drop shadow + glassmorphic card fill
        skia->drawDropShadow(cardRect, 16.0f, 12.0f, SkiaColor{0, 0, 0, 160});
        skia->drawRoundedRect(
            cardRect, 16.0f,
            SkiaColor{15, 23, 42, 210},     // Translucent Dark Slate
            SkiaColor{56, 189, 248, 180},   // Cyan Accent Border
            2.0f
        );

        // 5. Card Text Info & FPS Metrics
        char titleBuf[128];
        std::snprintf(titleBuf, sizeof(titleBuf), "LCL-UI 144Hz Shader Demo");
        skia->drawString(static_cast<int>(cardX + 24), static_cast<int>(cardY + 32), titleBuf, 0xFFF8FAFC);

        char subBuf[128];
        std::snprintf(subBuf, sizeof(subBuf), "Procedural Skia/EGL Rendering");
        skia->drawString(static_cast<int>(cardX + 24), static_cast<int>(cardY + 60), subBuf, 0xFF94A3B8);

        char fpsBuf[128];
        std::snprintf(fpsBuf, sizeof(fpsBuf), "Client Render: %.0f FPS (%.1f ms)", m_clientFps, (m_clientFps > 0 ? 1000.0f / m_clientFps : 0.0f));
        skia->drawString(static_cast<int>(cardX + 24), static_cast<int>(cardY + 100), fpsBuf, 0xFF4ADE80);

        char timeBuf[128];
        std::snprintf(timeBuf, sizeof(timeBuf), "Animation Time: %.2fs", t);
        skia->drawString(static_cast<int>(cardX + 24), static_cast<int>(cardY + 130), timeBuf, 0xFF38BDF8);

        // Continuously mark dirty to trigger next animation frame
        markDirty();
    }

private:
    std::chrono::steady_clock::time_point m_startTime;
    std::chrono::steady_clock::time_point m_lastFpsTime;
    uint32_t m_frameCount{0};
    float m_clientFps{144.0f};
};

int main() {
    std::cout << "====================================================\n";
    std::cout << "  LCL OS - 144Hz Procedural Shader & GPU Demo App   \n";
    std::cout << "====================================================\n";

    uint32_t width = 600;
    uint32_t height = 400;
    WindowApp app(width, height, "144Hz Procedural Shader Demo");

    // Build widget hierarchy
    auto rootContainer = std::make_unique<Container>();
    rootContainer->getYogaNode().setWidth(static_cast<float>(width));
    rootContainer->getYogaNode().setHeight(static_cast<float>(height));

    auto shaderWidget = std::make_unique<ShaderCanvasWidget>();
    shaderWidget->getYogaNode().setWidth(static_cast<float>(width));
    shaderWidget->getYogaNode().setHeight(static_cast<float>(height));

    rootContainer->addChild(std::move(shaderWidget));
    app.setRootWidget(std::move(rootContainer));

    if (app.connectCompositor()) {
        std::cout << "[ShaderDemo] App connected to compositor! Running live desktop UI loop...\n";
        app.runEventLoop();
    }

    return 0;
}
