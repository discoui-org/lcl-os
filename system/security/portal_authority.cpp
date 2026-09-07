#include "system/security/portal_authority.hpp"

#include "system/security/app_identity_registry.hpp"
#include "system/security/permission_policy.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace lcl::security {
namespace {

constexpr std::size_t kMaximumScopeBytes = 1024;
constexpr std::size_t kMaximumDisplayNameBytes = 256;

bool isVisibleText(std::string_view value, std::size_t maximum, bool permitEmpty) {
    if ((!permitEmpty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return character >= 0x20 && character != 0x7f;
    });
}

bool subjectMatches(const PortalRequest& request, const PermissionSubject& subject) {
    return subject.userUid != 0 && request.appId == subject.appId &&
           !isZeroDigest(subject.bundleRecordDigest);
}

bool sameSubject(const PermissionSubject& left, const PermissionSubject& right) {
    return left.userUid == right.userUid && left.appId == right.appId &&
           left.bundleRecordDigest == right.bundleRecordDigest &&
           left.publisherIdentity == right.publisherIdentity &&
           left.permissionVersion == right.permissionVersion;
}

} // namespace

std::optional<std::string_view> permissionForPortalOperation(PortalOperation operation) noexcept {
    switch (operation) {
        case PortalOperation::OpenFile: return "files.user-selected";
        case PortalOperation::ReadDocument: return "files.documents.read";
        case PortalOperation::WriteDocument: return "files.documents.write";
        case PortalOperation::ReadClipboard: return "clipboard.read";
        case PortalOperation::CaptureCamera: return "camera";
        case PortalOperation::CaptureMicrophone: return "microphone";
        case PortalOperation::PostNotification: return "notifications";
        case PortalOperation::OpenApplication: return "apps.open";
    }
    return std::nullopt;
}

bool validatePortalRequest(const PortalRequest& request, std::string& error) {
    error.clear();
    const auto permission = permissionForPortalOperation(request.operation);
    if (request.contractVersion != kPortalContractVersion || request.requestId == 0 ||
        request.instanceId == 0 || !AppIdentityRegistry::isValidAppId(request.appId) ||
        !permission || !isKnownPermission(*permission) ||
        !isVisibleText(request.scope, kMaximumScopeBytes, true)) {
        error = "portal request is invalid";
        return false;
    }
    return true;
}

PortalAuthority::PortalAuthority(ApplicationPeerAuthenticatorConfig peers,
                                 PermissionStoreConfig permissions)
    : peers_(std::move(peers)), permissions_(std::move(permissions)) {}

PortalAuthorization PortalAuthority::authorize(
        const PortalPeerCredentials& peer, const PortalRequest& request,
        const PermissionSubject& subject,
        const std::vector<std::string>& requestedPermissions, std::string& error) {
    error.clear();
    if (!validatePortalRequest(request, error) || !validatePermissionSubject(subject, error) ||
        !subjectMatches(request, subject)) {
        if (error.empty()) error = "portal subject does not match the request";
        return PortalAuthorization::Denied;
    }
    if (!peers_.authenticate(request.appId, request.instanceId,
                             peer.pid, peer.uid, peer.gid, error)) {
        return PortalAuthorization::Denied;
    }
    const auto permission = permissionForPortalOperation(request.operation);
    if (!permission || std::find(requestedPermissions.begin(), requestedPermissions.end(),
                                 *permission) == requestedPermissions.end()) {
        error = "application manifest did not request this portal permission";
        return PortalAuthorization::Denied;
    }
    if (permissions_.isGranted(subject, std::string(*permission), error)) {
        return PortalAuthorization::Granted;
    }
    if (!error.empty()) return PortalAuthorization::Denied;
    if (pending_.contains(request.requestId)) {
        error = "portal request ID is already pending";
        return PortalAuthorization::Denied;
    }
    pending_.emplace(request.requestId,
                     PendingConsent{request, subject, std::string(*permission)});
    return PortalAuthorization::NeedsVisibleConsent;
}

std::optional<PortalPrompt> PortalAuthority::makeTrustedPrompt(
        const PortalRequest& request, const PermissionSubject& subject,
        std::string displayName, std::string& error) const {
    error.clear();
    const auto permission = permissionForPortalOperation(request.operation);
    if (!validatePortalRequest(request, error) || !validatePermissionSubject(subject, error) ||
        !subjectMatches(request, subject) || !permission ||
        !isVisibleText(displayName, kMaximumDisplayNameBytes, false)) {
        if (error.empty()) error = "portal prompt identity is invalid";
        return std::nullopt;
    }
    const auto pending = pending_.find(request.requestId);
    if (pending == pending_.end() || pending->second.request.appId != request.appId ||
        pending->second.request.instanceId != request.instanceId ||
        pending->second.request.operation != request.operation ||
        pending->second.request.scope != request.scope ||
        pending->second.permission != *permission ||
        !sameSubject(pending->second.subject, subject)) {
        error = "portal prompt does not match a pending authenticated request";
        return std::nullopt;
    }
    return PortalPrompt{request.requestId, request.appId, std::move(displayName),
                        subject.publisherIdentity, std::string(*permission), request.scope,
                        {PortalConsentDuration::Once, PortalConsentDuration::Session,
                         PortalConsentDuration::Persistent}};
}

bool PortalAuthority::recordVisibleDecision(const PortalRequest& request,
                                            const PermissionSubject& subject,
                                            bool granted,
                                            PortalConsentDuration duration,
                                            std::string& error) {
    error.clear();
    const auto permission = permissionForPortalOperation(request.operation);
    if (!validatePortalRequest(request, error) || !validatePermissionSubject(subject, error) ||
        !subjectMatches(request, subject) || !permission) {
        if (error.empty()) error = "portal decision identity is invalid";
        return false;
    }
    const auto pending = pending_.find(request.requestId);
    if (pending == pending_.end() || pending->second.request.appId != request.appId ||
        pending->second.request.instanceId != request.instanceId ||
        pending->second.request.operation != request.operation ||
        pending->second.request.scope != request.scope ||
        pending->second.permission != *permission ||
        !sameSubject(pending->second.subject, subject)) {
        error = "portal decision does not match a pending authenticated request";
        return false;
    }
    if (duration != PortalConsentDuration::Persistent) {
        error = "one-shot and session grants remain broker-owned and are not persisted";
        return false;
    }
    if (!permissions_.setDecision(subject, std::string(*permission), granted, error)) {
        return false;
    }
    pending_.erase(pending);
    return true;
}

} // namespace lcl::security
