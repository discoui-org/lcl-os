#pragma once

#include <cstdint>
#include <string>

namespace lcl::security {

/**
 * Runtime availability of the kernel mechanisms required by the third-party
 * sandbox profile. Every namespace probe runs in a short-lived child, so the
 * caller's namespaces and seccomp state are never changed by diagnostics.
 */
struct SandboxPlatformCapabilities {
    bool mountNamespace{false};
    bool pidNamespace{false};
    bool ipcNamespace{false};
    bool networkNamespace{false};
    bool namespaceCombination{false};
    bool noNewPrivileges{false};
    bool seccompFilter{false};
    std::uint32_t landlockAbi{0};
    bool cgroupV2{false};
    bool cgroupMemoryController{false};
    bool cgroupPidsController{false};
    bool cgroupCpuController{false};
    std::string mountNamespaceError;
    std::string pidNamespaceError;
    std::string ipcNamespaceError;
    std::string networkNamespaceError;
    std::string namespaceCombinationError;
    std::string noNewPrivilegesError;
    std::string seccompError;
    std::string landlockError;
    std::string cgroupError;

    bool supportsMandatoryThirdPartyProfile() const noexcept {
        return mountNamespace && pidNamespace && ipcNamespace && networkNamespace &&
               namespaceCombination && noNewPrivileges && seccompFilter && landlockAbi != 0 &&
               cgroupV2 && cgroupMemoryController && cgroupPidsController &&
               cgroupCpuController;
    }
};

SandboxPlatformCapabilities probeSandboxPlatformCapabilities();
std::string formatSandboxPlatformCapabilities(const SandboxPlatformCapabilities& capabilities);

} // namespace lcl::security
