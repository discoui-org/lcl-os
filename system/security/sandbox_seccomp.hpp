#pragma once

#include <string>

#include "system/security/sandbox_contract.hpp"

namespace lcl::security {

/**
 * Installs the architecture-checked baseline seccomp filter for one sandbox
 * runtime.  This is deliberately a privilege-boundary filter, not the final
 * traced runtime allowlist: it blocks namespace creation, mounts, kernel
 * loading, tracing and other escalation primitives while later work narrows
 * normal syscall access per native/JavaScript runtime.
 */
bool installSandboxBaselineSeccomp(SandboxRuntime runtime, std::string& error);

} // namespace lcl::security
