#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "core/session/session_service.hpp"
#include "platform/common/gestalt.hpp"

namespace {
std::atomic<bool> g_running{true};
void onSignal(int) { g_running = false; }

std::string defaultGestaltPath() {
    constexpr const char* kQemuGestalt =
        "/sys/firmware/qemu_fw_cfg/by_name/opt/lcl/gestalt/raw";
    constexpr const char* kRuntimeGestalt = "/Runtime/gestalt.json";
    if (std::filesystem::is_regular_file(kQemuGestalt)) return kQemuGestalt;
    if (std::filesystem::is_regular_file(kRuntimeGestalt)) return kRuntimeGestalt;
    return "/System/Library/Gestalt/default.json";
}
}

int main() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    const auto gestalt = lcl::platform::loadGestalt(defaultGestaltPath());
    if (!gestalt.ok()) {
        std::cerr << "[LCL Session ERROR] " << gestalt.error << "\n";
        return 1;
    }

    lcl::session::SessionService service;
    if (!service.initialize()) {
        std::cerr << "[LCL Session ERROR] Could not start lcl-sessiond\n";
        return 1;
    }
    if (gestalt.gestalt.shell == lcl::platform::ShellKind::Desktop) {
        service.launchDefaultProfile();
    }
    while (g_running.load()) {
        service.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return 0;
}
