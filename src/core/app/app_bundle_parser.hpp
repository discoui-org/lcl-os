#pragma once

#include <string>
#include <vector>
#include <optional>

namespace lcl::core {

struct AppBundleMetadata {
    std::string bundlePath;      // e.g. "/System/Applications/Terminal.app"
    std::string appId;           // Canonical manifest identity, e.g. "org.lcl.terminal"
    std::string name;            // e.g. "Terminal"
    std::string executablePath;  // e.g. "/System/Applications/Terminal.app/Executables/Terminal"
    std::string version;         // e.g. "1.0.0"
    std::string icon;            // e.g. "Resources/Icon.png"
    std::string type;            // "gui" or "cli"
    std::string runtime;         // e.g. "org.lcl.javascript"
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
