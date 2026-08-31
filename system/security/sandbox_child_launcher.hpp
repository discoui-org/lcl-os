#pragma once

#include "system/security/sandbox_contract.hpp"
#include "system/security/sandbox_cgroup.hpp"
#include "system/security/sandbox_filesystem_sources.hpp"
#include "system/security/sandbox_kernel_enforcement.hpp"

#include <optional>
#include <string>
#include <vector>

namespace lcl::security {

/**
 * A daemon-owned cgroup allocation.  The non-owning manager reference stays
 * valid for the lifetime of sandboxd and is never supplied by launch IPC.
 */
struct SandboxCgroupBinding {
    const SandboxCgroupManager* manager{nullptr};
    SandboxCgroup cgroup;
};

/**
 * Protected sandboxd-side spawn material. No field is accepted from the app
 * protocol: executable/runtime descriptors and arguments come from sandboxd's
 * verified bundle registry.
 */
struct SandboxChildLaunchSpec {
    SandboxLaunchPlan plan;
    /** sandboxd-selected, non-IPC platform enforcement details. */
    SandboxPlatformHardening hardening;
    AppIdentity identity;
    std::optional<SandboxCgroupBinding> cgroup;
    /** Reset and populated by the launcher itself before application exec. */
    SandboxKernelEnforcement kernelEnforcement;
    SandboxFilesystemSources filesystemSources;
    /** Verified, bundle-relative location that must resolve to executableDescriptor. */
    std::string executableBundlePath;
    int executableDescriptor{-1};
    /** Required only for JavaScript apps; this is a trusted lcl-js descriptor. */
    int runtimeDescriptor{-1};
    std::vector<std::string> arguments;
};

bool validateSandboxChildLaunchSpec(const SandboxChildLaunchSpec& spec, std::string& error);

/**
 * Forks one daemon-owned app process group and applies cgroup placement,
 * namespaces, private mounts, Landlock, credential drop, seccomp and the
 * environment boundary before exec.  It never falls back to direct exec.
 */
SandboxLaunchResult spawnSandboxChild(const SandboxChildLaunchSpec& spec);

} // namespace lcl::security
