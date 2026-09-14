#include "platforms/linux/desktop_runtime_paths.hpp"

#include <cstdlib>
#include <filesystem>

namespace lcl::platform::desktop {

std::string DesktopRuntimePaths::compositorSocketPath() const {
    return "/Runtime/lcl-compositor.sock";
}

std::string DesktopRuntimePaths::rasterSocketPath() const {
    return "/Runtime/lcl-raster.sock";
}

std::string DesktopRuntimePaths::rasterServiceExecutable() const {
    if (const char* overridePath = std::getenv("LCL_RASTERD_PATH");
        overridePath && overridePath[0] != '\0') {
        return overridePath;
    }
    return "/System/Core/lcl-rasterd";
}

std::string DesktopRuntimePaths::sessionSocketPath() const {
    return "/Runtime/lcl-sessiond.sock";
}

std::string DesktopRuntimePaths::appCatalogDirectory() const {
    return "/System/Applications";
}

std::string DesktopRuntimePaths::applicationIdentityRegistryPath() const {
    return "/var/lib/lcl-security/app-identities.v1";
}

std::string DesktopRuntimePaths::applicationLaunchRegistryPath() const {
    return "/var/lib/lcl-security/app-launches";
}

std::vector<std::string> DesktopRuntimePaths::fontSearchDirectories() const {
    return {
        "/System/Library/Fonts",
        "/Library/Fonts",
        "/usr/share/fonts",
        "/usr/local/share/fonts"
    };
}

std::string DesktopRuntimePaths::temporaryDirectory() const {
    return "/Runtime/Temporary";
}

std::string DesktopRuntimePaths::gestaltFilePath() const {
    constexpr const char* qemuGestalt =
        "/sys/firmware/qemu_fw_cfg/by_name/opt/lcl/gestalt/raw";
    if (std::filesystem::is_regular_file(qemuGestalt)) return qemuGestalt;
    return "/System/Library/Gestalt/default.json";
}

} // namespace lcl::platform::desktop
