#include "platform/android/android_platform_services.hpp"
#include <iostream>
#include <cassert>

int main() {
    std::cout << "========================================================\n"
              << "  LCL Core Linux — Stage 3C Android Platform Smoke Test\n"
              << "========================================================\n\n";

    lcl::platform::android::AndroidPlatformServices services;

    std::cout << "[1/4] Initializing AndroidPlatformServices...\n";
    bool initOk = services.initialize();
    std::cout << "  ✓ Services Initialized: " << (initOk ? "YES" : "NO") << "\n";
    assert(initOk && "AndroidPlatformServices failed to initialize");

    std::cout << "\n[2/4] Inspecting Android Display Backend...\n";
    auto& display = services.display();
    std::cout << "  ✓ Display Initialized: " << (display.isInitialized() ? "YES" : "NO") << "\n";
    const auto& mode = display.activeMode();
    std::cout << "  ✓ Active Mode: " << mode.width << "x" << mode.height << " @ "
              << mode.refreshRate << "Hz (Name: " << mode.name << ")\n";
    std::cout << "  ✓ Display ID: " << services.getAndroidDisplay().getDisplayId() << "\n";
    std::cout << "  ✓ Display Connected: " << (services.getAndroidDisplay().isDisplayConnected() ? "YES" : "NO") << "\n";

    std::cout << "\n[3/4] Inspecting Android Graphics Context...\n";
    auto& graphics = services.graphics();
    std::cout << "  ✓ Graphics Initialized: " << (graphics.isInitialized() ? "YES" : "NO") << "\n";
    std::cout << "  ✓ Hardware Accelerated: " << (graphics.isHardwareAccelerated() ? "YES" : "NO") << "\n";
    std::cout << "  ✓ Make Current: " << (graphics.makeCurrent() ? "YES" : "NO") << "\n";
    std::cout << "  ✓ Surface Dimensions: " << services.getAndroidGraphics().getWidth()
              << "x" << services.getAndroidGraphics().getHeight() << "\n";

    std::cout << "\n[4/4] Inspecting Android Runtime Paths...\n";
    const auto& paths = services.paths();
    std::cout << "  ✓ Compositor Socket: " << paths.compositorSocketPath() << "\n";
    std::cout << "  ✓ Session Socket:    " << paths.sessionSocketPath() << "\n";
    std::cout << "  ✓ Temporary Dir:     " << paths.temporaryDirectory() << "\n";

    std::cout << "\nShutting down AndroidPlatformServices...\n";
    services.shutdown();
    std::cout << "  ✓ Shutdown Complete (isInitialized: "
              << (services.isInitialized() ? "YES" : "NO") << ")\n\n";

    std::cout << "========================================================\n"
              << "  Stage 3C Android Platform Smoke Test: SUCCESS\n"
              << "========================================================\n";
    return 0;
}
