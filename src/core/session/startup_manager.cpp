#include "core/session/startup_manager.hpp"
#include "core/display/display_scale.hpp"
#include <iostream>

namespace lcl::core {

StartupManager::StartupManager() = default;
StartupManager::~StartupManager() = default;

bool StartupManager::launchDefaultSession(render::WindowManager& windowManager, apps::TerminalApp& terminalApp) {
    std::cout << "[LCL Session] Starting LCL OS Desktop Session...\n";

    // 1. Discover installed .app bundles in /home/user/Applications
    auto apps = discoverApps("/home/user/Applications");
    std::cout << "[LCL Session] Discovered " << apps.size() << " application bundle(s).\n";
    for (const auto& app : apps) {
        std::cout << "  - " << app.name << " v" << app.version << " [" << app.bundlePath << "]\n";
    }

    // 2. Spawn primary user window (LCL Terminal App) — sizes in logical units × UI scale
    uint32_t winId = windowManager.createWindow(
        "LCL Terminal",
        DisplayScale::px(80),
        DisplayScale::px(60),
        DisplayScale::px(DisplayScale::kDefaultWinW),
        DisplayScale::px(DisplayScale::kDefaultWinH),
        0xFF89B4FA);
    terminalApp.initialize(winId);

    std::cout << "[LCL Session] Primary window (ID: " << winId << ") created for LCL Terminal.\n";
    return true;
}

std::vector<AppBundleMetadata> StartupManager::discoverApps(const std::string& appDir) {
    return AppBundleParser::scanDirectory(appDir);
}

} // namespace lcl::core
