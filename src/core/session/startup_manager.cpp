#include "core/session/startup_manager.hpp"
#include "core/display/display_scale.hpp"
#include "theme/palette.hpp"
#include <iostream>

namespace lcl::core {

StartupManager::StartupManager() = default;
StartupManager::~StartupManager() = default;

bool StartupManager::launchDefaultSession(render::WindowManager& windowManager) {
    (void)windowManager;
    std::cout << "[LCL Session] Starting LCL OS Desktop Session...\n";

    // 1. Discover installed .app bundles in /home/user/Applications
    auto apps = discoverApps("/home/user/Applications");
    std::cout << "[LCL Session] Discovered " << apps.size() << " application bundle(s).\n";
    for (const auto& app : apps) {
        std::cout << "  - " << app.name << " v" << app.version << " [" << app.bundlePath << "]\n";
    }

    std::cout << "[LCL Session] Compositor canvas active (0 initial windows). Listening for IPC client surface registrations.\n";
    return true;
}

std::vector<AppBundleMetadata> StartupManager::discoverApps(const std::string& appDir) {
    return AppBundleParser::scanDirectory(appDir);
}

} // namespace lcl::core
