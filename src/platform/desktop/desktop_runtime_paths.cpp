#include "platform/desktop/desktop_runtime_paths.hpp"

namespace lcl::platform::desktop {

std::string DesktopRuntimePaths::compositorSocketPath() const {
    return "/run/user/1000/lcl-compositor.sock";
}

std::string DesktopRuntimePaths::sessionSocketPath() const {
    return "/run/user/1000/lcl-sessiond.sock";
}

std::string DesktopRuntimePaths::appCatalogDirectory() const {
    return "/usr/share/lcl/apps";
}

std::vector<std::string> DesktopRuntimePaths::fontSearchDirectories() const {
    return {
        "/usr/share/fonts",
        "/usr/local/share/fonts"
    };
}

std::string DesktopRuntimePaths::temporaryDirectory() const {
    return "/tmp";
}

} // namespace lcl::platform::desktop
