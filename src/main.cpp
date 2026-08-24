#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include "core/compositor/compositor.hpp"
#include "platform/desktop/desktop_platform_services.hpp"

namespace {
    // Raw pointer — safe for signal-handler context (no heap allocation)
    lcl::core::Compositor* g_compositor{nullptr};

    void signalHandler(int sig) {
        std::cout << "\n[LCL Core] Signal received (" << sig
                  << "). Initiating graceful shutdown...\n";
        if (g_compositor) g_compositor->requestShutdown();
    }
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--gestalt") {
            if (i + 1 >= argc) {
                std::cerr << "[LCL] --gestalt requires an absolute JSON path.\n";
                return 2;
            }
            setenv("LCL_GESTALT_PATH", argv[++i], 1);
        } else {
            std::cerr << "[LCL] Unknown argument: " << argv[i] << "\n";
            return 2;
        }
    }

    // 1. Instantiate concrete Desktop Platform Services (Composition Root)
    lcl::platform::desktop::DesktopPlatformServices platformServices;
    if (!platformServices.initialize()) {
        std::cerr << "[LCL] Fatal: Desktop Platform Services initialization failed.\n";
        return 1;
    }

    // 2. Inject IPlatformServices into LCL Core Compositor
    lcl::core::Compositor compositor(platformServices);
    g_compositor = &compositor;

    if (!compositor.initialize()) {
        std::cerr << "[LCL] Fatal: Compositor initialization failed.\n";
        platformServices.shutdown();
        return 1;
    }

    // 3. Run compositor event loop (blocks until requestShutdown())
    compositor.run();

    // 4. Clean platform shutdown
    platformServices.shutdown();
    return 0;
}
