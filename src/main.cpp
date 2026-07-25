#include <csignal>
#include <iostream>
#include "core/compositor/compositor.hpp"

namespace {
    // Raw pointer — safe for signal-handler context (no heap allocation)
    lcl::core::Compositor* g_compositor{nullptr};

    void signalHandler(int sig) {
        std::cout << "\n[LCL Core] Signal received (" << sig
                  << "). Initiating graceful shutdown...\n";
        if (g_compositor) g_compositor->requestShutdown();
    }
}

int main() {
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    lcl::core::Compositor compositor;
    g_compositor = &compositor;

    if (!compositor.initialize()) {
        std::cerr << "[LCL] Fatal: Compositor initialization failed.\n";
        return 1;
    }

    compositor.run(); // blocks until requestShutdown()
    return 0;
}
