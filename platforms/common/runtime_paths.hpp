#pragma once

#include <string>
#include <vector>

namespace lcl::platform {

/**
 * @brief Platform-agnostic filesystem & socket path provider.
 *
 * Prevents hardcoded path branches in Core, IPC, and Session layers.
 */
class IRuntimePaths {
public:
    virtual ~IRuntimePaths() = default;

    virtual std::string compositorSocketPath() const = 0;
    virtual std::string rasterSocketPath() const = 0;
    virtual std::string rasterServiceExecutable() const = 0;
    virtual std::string sessionSocketPath() const = 0;
    virtual std::string appCatalogDirectory() const = 0;
    virtual std::string applicationIdentityRegistryPath() const = 0;
    virtual std::string applicationLaunchRegistryPath() const = 0;
    virtual std::vector<std::string> fontSearchDirectories() const = 0;
    virtual std::string temporaryDirectory() const = 0;
    virtual std::string gestaltFilePath() const = 0;
};

} // namespace lcl::platform
