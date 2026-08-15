#pragma once

#include "platform/common/runtime_paths.hpp"
#include <string>
#include <vector>

namespace lcl::platform::android {

/**
 * @brief Android-specific runtime filesystem and socket paths.
 *
 * NOTE: /data/local/tmp paths are development-only temporary paths used during
 * AVD bring-up and headless testing. Production paths in system/init will be defined
 * in a subsequent stage.
 */
class AndroidRuntimePaths final : public lcl::platform::IRuntimePaths {
public:
    AndroidRuntimePaths() = default;
    ~AndroidRuntimePaths() override = default;

    // Development-only temporary socket paths
    std::string compositorSocketPath() const override {
        return "/data/local/tmp/lcl-compositor.sock";
    }

    std::string sessionSocketPath() const override {
        return "/data/local/tmp/lcl-session.sock";
    }

    std::string appCatalogDirectory() const override {
        return "/data/local/tmp/apps";
    }

    std::vector<std::string> fontSearchDirectories() const override {
        return {"/system/fonts"};
    }

    std::string temporaryDirectory() const override {
        return "/data/local/tmp";
    }
};

} // namespace lcl::platform::android
