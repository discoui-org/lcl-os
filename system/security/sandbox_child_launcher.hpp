#pragma once

#include "system/security/sandbox_contract.hpp"

#include <string>
#include <vector>

namespace lcl::security {

/**
 * Protected sandboxd-side evidence that mandatory kernel stages completed in
 * this child immediately before credential drop. It is never IPC input.
 */
struct SandboxKernelEnforcement {
    bool mountNamespaceReady{false};
    bool pidNamespaceReady{false};
    bool ipcNamespaceReady{false};
    bool networkNamespaceReady{false};
    bool seccompInstalled{false};
    bool landlockInstalled{false};
    bool directDeviceAccessDenied{false};
};

/**
 * Protected sandboxd-side spawn material. No field is accepted from the app
 * protocol: executable/runtime descriptors and arguments come from sandboxd's
 * verified bundle registry.
 */
struct SandboxChildLaunchSpec {
    SandboxLaunchPlan plan;
    AppIdentity identity;
    SandboxKernelEnforcement kernelEnforcement;
    int executableDescriptor{-1};
    /** Required only for JavaScript apps; this is a trusted lcl-js descriptor. */
    int runtimeDescriptor{-1};
    std::vector<std::string> arguments;
};

bool validateSandboxChildLaunchSpec(const SandboxChildLaunchSpec& spec, std::string& error);

/**
 * Forks one app process group and applies the credential/environment boundary
 * before exec. Namespace, mounts, seccomp, Landlock and cgroup placement are
 * intentionally installed by the surrounding sandboxd stages, before this
 * function's child reaches exec.
 */
SandboxLaunchResult spawnSandboxChild(const SandboxChildLaunchSpec& spec);

} // namespace lcl::security
