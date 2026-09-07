#pragma once

#include <string>

#include "system/security/app_data_store.hpp"
#include "system/security/app_identity_registry.hpp"

namespace lcl::security {

class SandboxDaemon;

/**
 * Root-owned inputs for the system-image bundle registry.  Android may supply
 * a substrate-specific UID range without changing the canonical sandboxd
 * binary or its launch contract.
 */
struct SandboxSystemApplicationRegistryConfig {
    std::string systemApplicationsPath{"/System/Applications"};
    std::string systemPath{"/System"};
    std::string javascriptRuntimePath{"/System/Core/lcl-js"};
    std::string compositorSocketPath{"/Runtime/lcl-compositor.sock"};
    std::string rasterSocketPath{"/Runtime/lcl-raster.sock"};
    AppIdentityRegistryConfig identities{
        .registryPath = "/var/lib/lcl-security/app-identities.v1",
        .firstAppUid = 61000,
        .lastAppUid = 61999,
        .ownerUid = 0,
        .ownerGid = 0,
    };
    AppDataStoreConfig dataStore{
        .containersRoot = "/Users/Rei/Library/Containers",
        .ownerUid = 0,
        .ownerGid = 0,
    };
};

/**
 * Registers canonical system-image bundles as protected sandbox launch
 * material.  It is a daemon-local, root-only operation: normal launch IPC
 * still carries only the narrow policy-bound SandboxLaunchRequest.
 */
class SandboxSystemApplicationRegistry final {
public:
    explicit SandboxSystemApplicationRegistry(SandboxSystemApplicationRegistryConfig config = {});

    SandboxSystemApplicationRegistry(const SandboxSystemApplicationRegistry&) = delete;
    SandboxSystemApplicationRegistry& operator=(const SandboxSystemApplicationRegistry&) = delete;

    bool registerSystemApplications(SandboxDaemon& daemon, std::string& error) const;

private:
    SandboxSystemApplicationRegistryConfig config_;
};

} // namespace lcl::security
