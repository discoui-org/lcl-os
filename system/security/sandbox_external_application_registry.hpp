#pragma once

#include <mutex>
#include <string>
#include <unordered_set>

#include "system/security/app_data_store.hpp"
#include "system/security/app_identity_registry.hpp"
#include "system/security/sandbox_contract.hpp"

namespace lcl::security {

class SandboxDaemon;

/** Root-controlled inputs used only while accepting an immutable bundle snapshot. */
struct SandboxExternalApplicationRegistryConfig {
    std::string systemPath{"/System"};
    std::string systemApplicationsPath{"/System/Applications"};
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
 * Turns a root-sessiond supplied immutable bundle snapshot into daemon-owned
 * launch material.  It never accepts an app-selected UID, storage path,
 * system path, runtime path, capability grant or process argument.
 */
class SandboxExternalApplicationRegistry final {
public:
    explicit SandboxExternalApplicationRegistry(
        SandboxExternalApplicationRegistryConfig config = {});

    bool registerSnapshot(SandboxDaemon& daemon,
                          const SandboxApplicationRegistration& registration,
                          int appBundleDescriptor, int executableDescriptor,
                          std::string& error);

private:
    SandboxExternalApplicationRegistryConfig config_;
    std::mutex mutex_;
    std::unordered_set<std::string> externalAppIds_;
};

} // namespace lcl::security
