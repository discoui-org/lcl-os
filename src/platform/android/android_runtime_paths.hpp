#pragma once

#include "platform/common/runtime_paths.hpp"
#include <string>
#include <vector>

namespace lcl::platform::android {

/**
 * @brief Android-specific runtime filesystem and socket paths.
 */
class AndroidRuntimePaths final : public lcl::platform::IRuntimePaths {
public:
    AndroidRuntimePaths() = default;
    ~AndroidRuntimePaths() override = default;

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
