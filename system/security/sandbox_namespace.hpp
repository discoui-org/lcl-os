#pragma once

#include <string>

#include "system/security/sandbox_contract.hpp"
#include "system/security/sandbox_kernel_enforcement.hpp"

namespace lcl::security {

/**
 * Creates IPC, network and pending PID namespaces in the root sandboxd child.
 * CLONE_NEWPID only affects subsequently created children, so the caller must
 * fork once and invoke finalizeSandboxPidNamespace() in that inner child.
 */
bool beginSandboxNamespaces(const SandboxPlatformHardening& hardening,
                            SandboxKernelEnforcement& enforcement,
                            std::string& error);

/** Marks the post-fork child as the verified PID-namespace init process. */
bool finalizeSandboxPidNamespace(const SandboxPlatformHardening& hardening,
                                 SandboxKernelEnforcement& enforcement,
                                 std::string& error);

} // namespace lcl::security
