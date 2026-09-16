#include "system/security/sandbox_launch_authorizer.hpp"

#include <utility>

namespace lcl::security {

SandboxLaunchAuthorizer::SandboxLaunchAuthorizer(PermissionStoreConfig permissionStoreConfig)
    : permissionStore_(std::move(permissionStoreConfig)) {}

bool SandboxLaunchAuthorizer::registerVerifiedApplication(
    const VerifiedApplication& application,
    const std::vector<std::string>& grantedPermissions,
    std::string& error) {
    error.clear();
    // Rootfs bundles receive immutable grants from the trusted system-image
    // profile. User decisions remain authoritative for every other publisher.
    if (application.permissionSubject &&
        application.permissionSubject->publisherIdentity != "system-image") {
        if (!validatePermissionSubject(*application.permissionSubject, error) ||
            application.permissionSubject->appId != application.appId ||
            application.permissionSubject->bundleRecordDigest != application.bundleRecordDigest) {
            if (error.empty()) {
                error = "verified application permission subject does not match its bundle record";
            }
            return false;
        }
        std::lock_guard permissionStoreLock(permissionStoreMutex_);
        if (!permissionStore_.load(error)) {
            return false;
        }
    }
    // Plan construction performs all identity, digest and grant-subset checks.
    const auto probe = makeThirdPartySandboxLaunchPlan(application, grantedPermissions, 1, error);
    if (!probe) {
        return false;
    }

    std::lock_guard lock(mutex_);
    applications_.insert_or_assign(application.appId,
                                   RegisteredApplication{application, grantedPermissions});
    return true;
}

std::optional<SandboxLaunchPlan> SandboxLaunchAuthorizer::authorize(
    const SandboxLaunchRequest& request,
    std::string& error) const {
    error.clear();
    if (!validateSandboxLaunchRequest(request, error)) {
        return std::nullopt;
    }

    RegisteredApplication application;
    {
        std::lock_guard lock(mutex_);
        const auto found = applications_.find(request.appId);
        if (found == applications_.end()) {
            error = "sandbox launch request has no registered verified application";
            return std::nullopt;
        }
        application = found->second;
    }

    if (application.application.bundleRecordDigest != request.bundleRecordDigest) {
        error = "sandbox launch request bundle digest does not match the verified record";
        return std::nullopt;
    }
    std::vector<std::string> grantedPermissions = application.grantedPermissions;
    if (application.application.permissionSubject &&
        application.application.permissionSubject->publisherIdentity != "system-image") {
        std::lock_guard permissionStoreLock(permissionStoreMutex_);
        grantedPermissions = permissionStore_.grantedPermissions(
            *application.application.permissionSubject,
            application.application.requestedPermissions, error);
        if (!error.empty()) {
            return std::nullopt;
        }
    }
    const auto plan = makeThirdPartySandboxLaunchPlan(application.application,
                                                      grantedPermissions,
                                                      request.instanceId, error);
    if (!plan) {
        return std::nullopt;
    }
    if (plan->request.profileDigest != request.profileDigest) {
        error = "sandbox launch request profile digest does not match the verified policy";
        return std::nullopt;
    }
    return plan;
}

void SandboxLaunchAuthorizer::remove(const std::string& appId) {
    std::lock_guard lock(mutex_);
    applications_.erase(appId);
}

bool SandboxLaunchAuthorizer::contains(const std::string& appId) const {
    std::lock_guard lock(mutex_);
    return applications_.contains(appId);
}

std::size_t SandboxLaunchAuthorizer::size() const {
    std::lock_guard lock(mutex_);
    return applications_.size();
}

} // namespace lcl::security
