#pragma once

#include <string>

#include "system/security/sandbox_landlock.hpp"

namespace lcl::security {

/**
 * Builds and enters the private filesystem visible to one third-party app.
 * It runs before credentials are dropped.  Linux calls it from the private
 * PID-namespace init child; Android's capability baseline has no private PID
 * namespace, but retains the same private mount/chroot construction. All
 * mounts originate from protected descriptors supplied by sandboxd; neither a
 * host path nor a mount option comes from app IPC.
 */
bool enterSandboxFilesystemNamespace(const SandboxFilesystemSources& sources,
                                    const AppIdentity& identity,
                                    int executableDescriptor,
                                    bool executableMayRun,
                                    bool requirePidNamespaceInit,
                                    SandboxLandlockRules& landlockRules,
                                    std::string& error);

} // namespace lcl::security
