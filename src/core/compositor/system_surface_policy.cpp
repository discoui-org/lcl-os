#include "core/compositor/system_surface_policy.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unistd.h>

namespace lcl::core {

bool SystemSurfacePolicyRegistry::isValidKind(protocol::LCLSystemSurfaceKind kind) noexcept {
    return kind >= protocol::LCLSystemSurfaceKind::Wallpaper &&
           kind <= protocol::LCLSystemSurfaceKind::Dock;
}

SystemSurfacePolicy SystemSurfacePolicyRegistry::policyFor(protocol::LCLSystemSurfaceKind kind) noexcept {
    SystemSurfacePolicy policy{};
    switch (kind) {
        case protocol::LCLSystemSurfaceKind::Wallpaper:
            policy.role = protocol::LCLRole::DesktopWallpaper;
            policy.layer = protocol::LCLWindowLayer::Bottom;
            policy.unfocusable = true;
            policy.insetBorderEnabled = false;
            policy.suppressInitialTransition = true;
            policy.isSystemSurface = true;
            return policy;
        case protocol::LCLSystemSurfaceKind::MenuBar:
        case protocol::LCLSystemSurfaceKind::Dock:
            policy.role = protocol::LCLRole::ShellPanel;
            policy.layer = protocol::LCLWindowLayer::TopMost;
            policy.unfocusable = true;
            policy.insetBorderEnabled = false;
            policy.suppressInitialTransition = true;
            policy.reservesWorkArea = true;
            policy.isSystemSurface = true;
            return policy;
        case protocol::LCLSystemSurfaceKind::None:
        default:
            return policy;
    }
}

protocol::LCLSystemSurfaceKind SystemSurfacePolicyRegistry::inferLegacyKind(
    protocol::LCLRole role, const char* title) noexcept {
    if (role == protocol::LCLRole::DesktopWallpaper) return protocol::LCLSystemSurfaceKind::Wallpaper;
    if (role != protocol::LCLRole::ShellPanel || title == nullptr) return protocol::LCLSystemSurfaceKind::None;
    if (std::strcmp(title, "LCL MenuBar") == 0) return protocol::LCLSystemSurfaceKind::MenuBar;
    if (std::strcmp(title, "LCL Dock") == 0) return protocol::LCLSystemSurfaceKind::Dock;
    return protocol::LCLSystemSurfaceKind::None;
}

bool SystemSurfacePolicyRegistry::isTrustedShellPeer(pid_t pid) noexcept {
    if (pid <= 0) return false;
    std::array<char, 64> procPath{};
    std::snprintf(procPath.data(), procPath.size(), "/proc/%d/exe", static_cast<int>(pid));
    std::array<char, 4096> resolved{};
    const ssize_t count = readlink(procPath.data(), resolved.data(), resolved.size() - 1);
    if (count <= 0) return false;
    resolved[static_cast<size_t>(count)] = '\0';
    return std::filesystem::path(resolved.data()).filename() == "lcl-desktop-shell";
}

} // namespace lcl::core
