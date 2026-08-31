#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "system/security/sandbox_contract.hpp"

namespace lcl::security {

/**
 * Protected sandboxd-side registry of previously verified applications.
 *
 * Its registration API is for the trusted verifier/registry path only. An IPC
 * client can call only authorize(), which resolves its narrow request against
 * this record and never supplies executable paths, credentials or mounts.
 */
class SandboxLaunchAuthorizer final {
public:
    explicit SandboxLaunchAuthorizer(PermissionStoreConfig permissionStoreConfig = {});

    SandboxLaunchAuthorizer(const SandboxLaunchAuthorizer&) = delete;
    SandboxLaunchAuthorizer& operator=(const SandboxLaunchAuthorizer&) = delete;

    bool registerVerifiedApplication(const VerifiedApplication& application,
                                     const std::vector<std::string>& grantedPermissions,
                                     std::string& error);
    std::optional<SandboxLaunchPlan> authorize(const SandboxLaunchRequest& request,
                                               std::string& error) const;
    void remove(const std::string& appId);
    std::size_t size() const;

private:
    struct RegisteredApplication {
        VerifiedApplication application;
        std::vector<std::string> grantedPermissions;
    };

    mutable std::mutex mutex_;
    mutable std::mutex permissionStoreMutex_;
    mutable PermissionStore permissionStore_;
    std::map<std::string, RegisteredApplication> applications_;
};

} // namespace lcl::security
