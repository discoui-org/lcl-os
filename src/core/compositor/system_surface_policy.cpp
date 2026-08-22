#include "core/compositor/system_surface_policy.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
                                                        float outputWidth, float outputHeight,
                                                        float& x, float& y, float& width, float& height) noexcept {
    switch (policy.placement) {
        case SystemSurfacePlacement::OutputBounds:
            x = 0;
            y = 0;
            width = outputWidth;
            height = outputHeight;
            break;
        case SystemSurfacePlacement::OutputTopEdge:
            x = 0;
            y = 0;
            width = outputWidth;
            height = std::min(height, outputHeight);
            break;
        case SystemSurfacePlacement::OutputBottomEdge:
            x = 0;
            width = outputWidth;
            height = std::min(height, outputHeight);
            y = std::max(0.0f, outputHeight - height);
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
    const auto executable = std::filesystem::path(resolved.data()).filename().string();
    if (executable == "lcl-desktop-shell" || executable == "lcl-mobile-shell") {
        return true;
    }
    // If invoked via an explicit dynamic linker (e.g. ld-linux-x86-64.so.2),
    // inspect /proc/<pid>/cmdline to check the target executable argument.
    if (executable.rfind("ld-linux", 0) == 0 || executable.rfind("ld.so", 0) == 0 || executable.rfind("ld-", 0) == 0) {
        std::array<char, 64> cmdlinePath{};
        std::snprintf(cmdlinePath.data(), cmdlinePath.size(), "/proc/%d/cmdline", static_cast<int>(pid));
        std::ifstream cmdlineFile(cmdlinePath.data(), std::ios::binary);
        if (cmdlineFile) {
            std::string arg;
            while (std::getline(cmdlineFile, arg, '\0')) {
                const auto targetName = std::filesystem::path(arg).filename().string();
                if (targetName == "lcl-desktop-shell" || targetName == "lcl-mobile-shell") {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace lcl::core
