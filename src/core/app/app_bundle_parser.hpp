#pragma once

#include <string>
#include <vector>
#include <optional>

namespace lcl::core {

struct AppBundleMetadata {
    std::string bundlePath;      // e.g. "/home/user/Applications/SystemMonitor.app"
    std::string name;            // e.g. "System Monitor"
    std::string executablePath;  // e.g. "/home/user/Applications/SystemMonitor.app/bin/sysmon"
    std::string version;         // e.g. "1.0.0"
    std::string icon;            // e.g. "assets/icon.png"
    std::string type;            // "gui" or "cli"
    bool valid{false};
};

class AppBundleParser {
public:
    /**
     * @brief Parse a .app bundle directory (reading its metadata.json).
     * @param bundlePath Absolute or relative path to the .app directory
     * @return AppBundleMetadata if valid, std::nullopt otherwise
     */
    static std::optional<AppBundleMetadata> parseBundle(const std::string& bundlePath);

    /**
     * @brief Scan a directory for all .app bundles.
     * @param searchDir Directory to scan (e.g. "/home/user/Applications")
     * @return List of parsed AppBundleMetadata objects
     */
    static std::vector<AppBundleMetadata> scanDirectory(const std::string& searchDir);
};

} // namespace lcl::core
