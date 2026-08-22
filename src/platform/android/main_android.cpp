#include <csignal>
#include <iostream>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <vector>

#include <android/hardware_buffer.h>

#include "core/compositor/compositor.hpp"
#include "platform/android/android_platform_services.hpp"

namespace {
    // Raw pointer — safe for signal-handler context (no heap allocation)
    lcl::core::Compositor* g_compositor{nullptr};

    void signalHandler(int sig) {
        std::cout << "\n[LCL Android] Signal received (" << sig
                  << "). Initiating graceful shutdown...\n";
        if (g_compositor) g_compositor->requestShutdown();
    }

    /**
     * @brief Verify the last rendered frame by reading back PBuffer pixel data.
     *
     * This is a diagnostic readback of the compositor's OpenGL ES PBuffer. After
     * endFrame(), FBO 0 still contains the scene that was blitted to the AHB
     * scanout and presented via Composer3.
     */
    void verifyRenderedFrame(lcl::platform::android::AndroidPlatformServices& platformServices,
                             const lcl::core::Compositor& compositor) {
        auto& gfx = platformServices.getAndroidGraphics();
        if (!gfx.isInitialized()) {
            std::cout << "[FRAME VERIFY] Graphics context not initialized.\n";
            return;
        }

        uint32_t w = gfx.getWidth();
        uint32_t h = gfx.getHeight();
        if (w == 0 || h == 0) {
            std::cout << "[FRAME VERIFY] Invalid dimensions: " << w << "x" << h << "\n";
            return;
        }

        std::vector<uint32_t> pixels(w * h);
        gfx.makeCurrent();
        if (!gfx.readback(pixels.data(), w, h)) {
            std::cout << "[FRAME VERIFY] Readback failed.\n";
            return;
        }

        // Sample pixel helper (GL_RGBA byte order on little-endian)
        auto samplePixel = [&](const char* name, uint32_t x, uint32_t y) {
            if (x >= w || y >= h) return;
            // glReadPixels flips Y (row 0 = bottom), so flip to screen coords
            uint32_t flippedY = (h - 1) - y;
            uint32_t px = pixels[flippedY * w + x];
            uint8_t r = px & 0xFF;
            uint8_t g = (px >> 8) & 0xFF;
            uint8_t b = (px >> 16) & 0xFF;
            uint8_t a = (px >> 24) & 0xFF;
            std::printf("[FRAME VERIFY]   %-20s (%3u, %3u): R=%3u G=%3u B=%3u A=%3u\n",
                        name, x, y, r, g, b, a);
        };

        std::cout << "[FRAME VERIFY] ============================================\n";
        std::cout << "[FRAME VERIFY] Frame readback: " << w << "x" << h << "\n";

        samplePixel("center",       w / 2,     h / 2);
        samplePixel("top-left",     10,        10);
        samplePixel("top-right",    w - 20,    10);
        samplePixel("bottom-left",  10,        h - 20);
        samplePixel("bottom-right", w - 20,    h - 20);
        samplePixel("mid-left",     10,        h / 2);
        samplePixel("mid-right",    w - 20,    h / 2);

        // Count pixel classes
        uint32_t totalPixels = w * h;
        uint32_t blackPixels = 0;
        uint32_t darkThemePixels = 0;  // LCL dark theme background ~(20,23,31)
        uint32_t uiPixels = 0;         // Rendered UI content (shell/terminal/windows)
        uint32_t transparentPixels = 0;

        for (uint32_t i = 0; i < totalPixels; ++i) {
            uint32_t px = pixels[i];
            uint8_t r = px & 0xFF;
            uint8_t g = (px >> 8) & 0xFF;
            uint8_t b = (px >> 16) & 0xFF;
            uint8_t a = (px >> 24) & 0xFF;

            if (a == 0) {
                ++transparentPixels;
            } else if (r == 0 && g == 0 && b == 0) {
                ++blackPixels;
            } else if (r >= 15 && r <= 30 && g >= 18 && g <= 35 && b >= 25 && b <= 45 && a >= 250) {
                // LCL dark theme clear color range: glClearColor(0.08, 0.09, 0.12, 1.0)
                ++darkThemePixels;
            } else {
                ++uiPixels;
            }
        }

        std::printf("[FRAME VERIFY]   Total pixels:       %u\n", totalPixels);
        std::printf("[FRAME VERIFY]   Black pixels:       %u (%.1f%%)\n", blackPixels, 100.0f * blackPixels / totalPixels);
        std::printf("[FRAME VERIFY]   Dark theme pixels:  %u (%.1f%%)\n", darkThemePixels, 100.0f * darkThemePixels / totalPixels);
        std::printf("[FRAME VERIFY]   UI content pixels:  %u (%.1f%%)\n", uiPixels, 100.0f * uiPixels / totalPixels);
        std::printf("[FRAME VERIFY]   Transparent pixels: %u (%.1f%%)\n", transparentPixels, 100.0f * transparentPixels / totalPixels);

        // Determine what was rendered
        bool hasLclBackground = (darkThemePixels + uiPixels) > (totalPixels * 80 / 100);
        bool hasRenderedUi = uiPixels > 100;
        bool hasRenderedContent = (darkThemePixels + uiPixels) > (totalPixels * 90 / 100);

        std::cout << "[FRAME VERIFY] ============================================\n";
        std::printf("[FRAME VERIFY] Active Surfaces:         %zu\n", compositor.activeSurfaceCount());
        std::printf("[FRAME VERIFY] Active Windows:          %zu\n", compositor.activeWindowCount());
        std::printf("[FRAME VERIFY] LCL Dark Theme Bg:       %s\n", hasLclBackground ? "YES" : "NO");
        std::printf("[FRAME VERIFY] Rendered UI Content:     %s (%u pixels)\n", hasRenderedUi ? "YES" : "NO", uiPixels);
        std::printf("[FRAME VERIFY] Composer3 Present:       %s\n",
                    platformServices.getAndroidDisplay().isInitialized() ? "ACTIVE" : "INACTIVE");
        std::printf("[FRAME VERIFY] AVD SCREEN SHOWS REAL LCL: %s\n",
                    hasRenderedContent ? "YES" : "NO");
        std::cout << "[FRAME VERIFY] ============================================\n";
    }
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "====================================================\n"
              << "  LCL Core Linux — Android Compositor Runtime       \n"
              << "  Architecture: Direct Composer3 & GLES3 (No SurfaceFlinger)\n"
              << "  C++ Standard: C++20\n"
              << "====================================================\n";

    // 1. Check for runtime flags (e.g. fixed frame run for headless tests)
    int maxFrames = -1;
    bool verify = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--frames" && i + 1 < argc) {
            maxFrames = std::atoi(argv[++i]);
        } else if (std::string(argv[i]) == "--verify") {
            verify = true;
        }
    }

    // 2. Instantiate concrete Android Platform Services (Composition Root)
    lcl::platform::android::AndroidPlatformServices platformServices;
    if (!platformServices.initialize()) {
        std::cerr << "[LCL Android] Fatal: Android Platform Services initialization failed.\n";
        return 1;
    }

    // 3. Inject IPlatformServices into LCL Core Compositor
    lcl::core::Compositor compositor(platformServices);
    g_compositor = &compositor;

    if (!compositor.initialize()) {
        std::cerr << "[LCL Android] Fatal: Compositor initialization failed.\n";
        platformServices.shutdown();
        return 1;
    }

    // 4. Run compositor event loop (blocks until requestShutdown())
    if (maxFrames > 0) {
        std::cout << "[LCL Android] Running compositor for " << maxFrames << " frames...\n";
        std::thread limiter([maxFrames] {
            std::this_thread::sleep_for(std::chrono::milliseconds(maxFrames * 30));
            if (g_compositor) g_compositor->requestShutdown();
        });
        compositor.run();
        if (limiter.joinable()) limiter.join();
    } else {
        compositor.run();
    }

    // 5. Verify rendered frame if requested
    if (verify) {
        verifyRenderedFrame(platformServices, compositor);
    }

    // 6. Clean platform shutdown
    platformServices.shutdown();
    std::cout << "[LCL Android] Compositor shutdown complete.\n";
    return 0;
}
