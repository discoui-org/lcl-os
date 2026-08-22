#include "platform/desktop/desktop_runtime_paths.hpp"

namespace lcl::platform::desktop {

std::string DesktopRuntimePaths::compositorSocketPath() const {
    return "/Runtime/lcl-compositor.sock";
}

std::string DesktopRuntimePaths::sessionSocketPath() const {
    return "/Runtime/lcl-sessiond.sock";
}

std::string DesktopRuntimePaths::appCatalogDirectory() const {
    return "/System/Applications";
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

} // namespace lcl::platform::desktop
