#pragma once

#include <string>
#include <vector>
#include "core/app/app_bundle_parser.hpp"
#include "apps/terminal/terminal_app.hpp"
#include "render/window_manager.hpp"

namespace lcl::core {

class StartupManager {
public:
    StartupManager();
    ~StartupManager();

    // Non-copyable
    StartupManager(const StartupManager&) = delete;
    StartupManager& operator=(const StartupManager&) = delete;

    /**
     * @brief Initialize default OS session startup applications.
     */
    bool launchDefaultSession(render::WindowManager& windowManager);

    /**
     * @brief Scan Applications directory and register available .app bundles.
     */
    std::vector<AppBundleMetadata> discoverApps(const std::string& appDir = "/home/user/Applications");
};

} // namespace lcl::core
