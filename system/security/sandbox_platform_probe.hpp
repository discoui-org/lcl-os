#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "system/security/sandbox_contract.hpp"

namespace lcl::security {

/**
 * Root-owned launch context for the canonical same-binary userspace. This is
 * selected by the trusted substrate bootstrap, never inferred from the app
 * request or its requested permissions.
 */
enum class SandboxPlatformMode : std::uint8_t {
    LinuxFull = 1,
    AndroidCapability = 2,
};

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
               namespaceCombination && noNewPrivileges && seccompFilter && landlockAbi >= 3 &&
               cgroupV2 && cgroupMemoryController && cgroupPidsController &&
               cgroupCpuController;
    }

    /**
     * Minimum mechanisms needed to preserve the common LCL capability ABI:
     * a private mounted filesystem view, no host network view, no_new_privs,
     * and a seccomp privilege boundary. Linux can then add the fuller profile
     * above; Android can use this baseline when its stock kernel omits PID,
     * IPC, Landlock, or cgroup-v2 support inside the direct-deploy chroot.
     */
    bool supportsCommonCapabilityProfile() const noexcept {
        return mountNamespace && networkNamespace && noNewPrivileges && seccompFilter;
    }
};

SandboxPlatformCapabilities probeSandboxPlatformCapabilities();
/** Selects non-IPC platform hardening after probing the running kernel. */
std::optional<SandboxPlatformHardening> makeSandboxPlatformHardening(
    const SandboxPlatformCapabilities& capabilities, SandboxPlatformMode mode, std::string& error);
std::string formatSandboxPlatformCapabilities(const SandboxPlatformCapabilities& capabilities);

} // namespace lcl::security
