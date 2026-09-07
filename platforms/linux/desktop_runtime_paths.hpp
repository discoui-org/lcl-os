#pragma once

#include "platforms/common/runtime_paths.hpp"
#include <string>
#include <vector>

namespace lcl::platform::desktop {

class DesktopRuntimePaths final : public lcl::platform::IRuntimePaths {
public:
    DesktopRuntimePaths() = default;
    ~DesktopRuntimePaths() override = default;

    std::string compositorSocketPath() const override;
    std::string rasterSocketPath() const override;
    std::string rasterServiceExecutable() const override;
    std::string sessionSocketPath() const override;
    std::string appCatalogDirectory() const override;
    std::string applicationIdentityRegistryPath() const override;
    std::string applicationLaunchRegistryPath() const override;
    std::vector<std::string> fontSearchDirectories() const override;
    std::string temporaryDirectory() const override;
    std::string gestaltFilePath() const override;
};

} // namespace lcl::platform::desktop
