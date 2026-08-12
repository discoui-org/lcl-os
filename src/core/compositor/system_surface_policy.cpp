#include "core/compositor/system_surface_policy.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
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
            policy.layer = protocol::LCLWindowLayer::Bottom;
            policy.unfocusable = true;
            policy.insetBorderEnabled = false;
            policy.suppressInitialTransition = true;
            policy.isSystemSurface = true;
            policy.placement = SystemSurfacePlacement::OutputBounds;
            return policy;
        case protocol::LCLSystemSurfaceKind::MenuBar:
            policy.layer = protocol::LCLWindowLayer::TopMost;
            policy.unfocusable = true;
            policy.insetBorderEnabled = false;
            policy.suppressInitialTransition = true;
            policy.reservesWorkArea = true;
            policy.isSystemSurface = true;
            policy.placement = SystemSurfacePlacement::OutputTopEdge;
            return policy;
        case protocol::LCLSystemSurfaceKind::Dock:
            policy.layer = protocol::LCLWindowLayer::TopMost;
            policy.unfocusable = true;
            policy.insetBorderEnabled = false;
            policy.suppressInitialTransition = true;
            policy.reservesWorkArea = true;
            policy.isSystemSurface = true;
            policy.placement = SystemSurfacePlacement::OutputBottomEdge;
            return policy;
        case protocol::LCLSystemSurfaceKind::None:
        default:
            return policy;
    }
}

void SystemSurfacePolicyRegistry::applyInitialPlacement(const SystemSurfacePolicy& policy,
                                                        uint32_t outputWidth, uint32_t outputHeight,
                                                        int& x, int& y, int& width, int& height) noexcept {
    switch (policy.placement) {
        case SystemSurfacePlacement::OutputBounds:
            x = 0;
            y = 0;
            width = static_cast<int>(outputWidth);
            height = static_cast<int>(outputHeight);
            break;
        case SystemSurfacePlacement::OutputTopEdge:
            x = 0;
            y = 0;
            width = static_cast<int>(outputWidth);
            height = std::min(height, static_cast<int>(outputHeight));
            break;
        case SystemSurfacePlacement::OutputBottomEdge:
            x = 0;
            width = static_cast<int>(outputWidth);
            height = std::min(height, static_cast<int>(outputHeight));
            y = std::max(0, static_cast<int>(outputHeight) - height);
            break;
        case SystemSurfacePlacement::ClientBounds:
            break;
    }
}

bool SystemSurfacePolicyRegistry::isTrustedShellPeer(pid_t pid) noexcept {
    if (pid <= 0) return false;
    std::array<char, 64> procPath{};
    std::snprintf(procPath.data(), procPath.size(), "/proc/%d/exe", static_cast<int>(pid));
    std::array<char, 4096> resolved{};
    const ssize_t count = readlink(procPath.data(), resolved.data(), resolved.size() - 1);
    if (count <= 0) return false;
    resolved[static_cast<size_t>(count)] = '\0';
    const auto executable = std::filesystem::path(resolved.data()).filename();
    return executable == "lcl-desktop-shell" || executable == "lcl-mobile-shell";
}

} // namespace lcl::core
