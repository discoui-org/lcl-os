#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

#include "core/session/session_service.hpp"

namespace {
std::atomic<bool> g_running{true};
void onSignal(int) { g_running = false; }
}

int main() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    lcl::session::SessionService service;
    if (!service.initialize()) {
        std::cerr << "[LCL Session ERROR] Could not start lcl-sessiond\n";
        return 1;
    }
    service.launchDefaultProfile();
    while (g_running.load()) {
        service.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return 0;
}
