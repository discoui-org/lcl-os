#pragma once

#include <string>

#include "system/security/sandbox_landlock.hpp"

namespace lcl::security {

/**
 * Builds and enters the private filesystem visible to one third-party app.
 * It is called only by the PID-namespace init child, before credentials are
 * dropped.  All mounts originate from protected descriptors supplied by
 * sandboxd; neither a host path nor a mount option comes from app IPC.
 */
bool enterSandboxFilesystemNamespace(const SandboxFilesystemSources& sources,
                                    const AppIdentity& identity,
                                    int executableDescriptor,
                                    bool executableMayRun,
                                    SandboxLandlockRules& landlockRules,
                                    std::string& error);

} // namespace lcl::security
