#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#include <unistd.h>

#include "platforms/common/gestalt.hpp"
#include "system/security/desktop_user.hpp"

namespace {

std::string defaultGestaltPath() {
    constexpr const char* kQemuGestalt =
        "/sys/firmware/qemu_fw_cfg/by_name/opt/lcl/gestalt/raw";
    constexpr const char* kRuntimeGestalt = "/Runtime/gestalt.json";
    if (std::filesystem::is_regular_file(kQemuGestalt)) return kQemuGestalt;
    if (std::filesystem::is_regular_file(kRuntimeGestalt)) return kRuntimeGestalt;
    return "/System/Library/Gestalt/default.json";
}

const char* executableFor(lcl::platform::ShellKind shell) {
    switch (shell) {
    case lcl::platform::ShellKind::Desktop:
        return "/System/Core/lcl-desktop-shell";
    case lcl::platform::ShellKind::Mobile:
        return "/System/Core/lcl-mobile-shell";
    }
    return nullptr;
}

const char* nameFor(lcl::platform::ShellKind shell) {
    return shell == lcl::platform::ShellKind::Mobile ? "mobile" : "desktop";
}

} // namespace

int main() {
    std::string identityError;
    if (!lcl::security::dropToDesktopUser(identityError)) {
        std::cerr << "[LCL Shell Launcher] " << identityError << "\n";
        return 1;
    }

    const auto gestalt = lcl::platform::loadGestalt(defaultGestaltPath());
    if (!gestalt.ok()) {
        std::cerr << "[LCL Shell Launcher] " << gestalt.error << "\n";
        return 1;
    }

    const char* executable = executableFor(gestalt.gestalt.shell);
    if (!executable || !std::filesystem::is_regular_file(executable)) {
        std::cerr << "[LCL Shell Launcher] Selected " << nameFor(gestalt.gestalt.shell)
                  << " shell is missing: " << (executable ? executable : "<invalid>") << "\n";
        return 1;
    }

    std::cout << "[LCL Shell Launcher] Starting " << nameFor(gestalt.gestalt.shell)
              << " shell from "
              << (gestalt.loadedFromFile ? gestalt.path : "built-in Gestalt") << std::endl;
    char* const arguments[] = {const_cast<char*>(executable), nullptr};
    execv(executable, arguments);
    std::cerr << "[LCL Shell Launcher] execv(" << executable
              << ") failed: " << std::strerror(errno) << "\n";
    return 1;
}
