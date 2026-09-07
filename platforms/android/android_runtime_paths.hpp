#pragma once

#include "platforms/common/runtime_paths.hpp"
#include <cstdlib>
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

    // Canonical LCL runtime paths
    std::string compositorSocketPath() const override {
        return runtimeDirectory() + "/lcl-compositor.sock";
    }

    std::string rasterSocketPath() const override {
        return runtimeDirectory() + "/lcl-raster.sock";
    }

    std::string rasterServiceExecutable() const override {
        const char* overridePath = std::getenv("LCL_RASTERD_PATH");
        return overridePath && overridePath[0] != '\0'
            ? overridePath : "/system/bin/lcl-rasterd-android";
    }

    std::string sessionSocketPath() const override {
        return runtimeDirectory() + "/lcl-sessiond.sock";
    }

    std::string appCatalogDirectory() const override {
        return "/System/Applications";
    }

    std::string applicationIdentityRegistryPath() const override {
        const char* rootfs = std::getenv("LCL_ROOTFS_MOUNT");
        const std::string root = rootfs && rootfs[0] == '/'
            ? rootfs : "/data/local/tmp/lcl-rootfs";
        return root + "/var/lib/lcl-security/app-identities.v1";
    }

    std::vector<std::string> fontSearchDirectories() const override {
        return {"/System/Library/Fonts", "/usr/share/fonts", "/system/fonts"};
    }

    std::string temporaryDirectory() const override {
        return runtimeDirectory() + "/Temporary";
    }

    std::string gestaltFilePath() const override {
        return "/vendor/etc/lcl/gestalt.json";
    }

private:
    static std::string runtimeDirectory() {
        const char* overridePath = std::getenv("LCL_RUNTIME_DIR");
        if (overridePath && overridePath[0] == '/') return overridePath;
        return "/Runtime";
    }
};

} // namespace lcl::platform::android
