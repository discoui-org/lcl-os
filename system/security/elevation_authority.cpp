#include "system/security/elevation_authority.hpp"

#include "system/security/app_identity_registry.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sstream>
#include <utility>

namespace lcl::security {
namespace {

constexpr std::size_t kMaximumReasonBytes = 512;
constexpr std::size_t kMaximumTargetBytes = 256;
constexpr std::size_t kMaximumDisplayNameBytes = 256;
constexpr std::chrono::seconds kMaximumGrantLifetime{300};

bool visibleText(std::string_view value, std::size_t maximum, bool allowEmpty = false) {
    if ((!allowEmpty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return character >= 0x20 && character != 0x7f;
    });
}

bool validScope(ElevationScope scope) {
    return scope >= ElevationScope::ManageBundleTrust &&
           scope <= ElevationScope::RunSignedHelper;
}

bool validRequest(const ElevationRequest& request) {
    return request.contractVersion == kElevationContractVersion &&
           request.requestId != 0 && request.instanceId != 0 &&
           AppIdentityRegistry::isValidAppId(request.appId) &&
           !isZeroDigest(request.bundleRecordDigest) &&
           visibleText(request.publisherIdentity, 80) &&
           visibleText(request.reason, kMaximumReasonBytes) && validScope(request.scope) &&
           visibleText(request.target, kMaximumTargetBytes);
}

bool sameDigest(const Sha256Digest& left, const Sha256Digest& right) {
    return left == right;
}

bool writeAll(int descriptor, std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t count = write(descriptor, bytes.data(), bytes.size());
        if (count > 0) {
            bytes.remove_prefix(static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

} // namespace

ElevationAuditStore::ElevationAuditStore(ElevationAuditStoreConfig config)
    : config_(std::move(config)) {}

bool ElevationAuditStore::append(std::string_view event,
                                 const ElevationRequest& request,
                                 bool accepted, std::string_view result,
                                 std::string& error) const {
    error.clear();
    const std::filesystem::path path(config_.path);
    const std::filesystem::path parent = path.parent_path();
    struct stat parentStatus {};
    if (!path.is_absolute() || parent.empty() || !visibleText(event, 32) ||
        !visibleText(result, 256, true) || lstat(parent.c_str(), &parentStatus) != 0 ||
        !S_ISDIR(parentStatus.st_mode) || S_ISLNK(parentStatus.st_mode) ||
        parentStatus.st_uid != config_.ownerUid || parentStatus.st_gid != config_.ownerGid ||
        (parentStatus.st_mode & 0022) != 0) {
        error = "elevation audit path or record is unsafe";
        return false;
    }
    const mode_t previousUmask = umask(0077);
    const int descriptor = open(config_.path.c_str(),
                                O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                                0600);
    umask(previousUmask);
    if (descriptor < 0) {
        error = std::string("could not open elevation audit: ") + std::strerror(errno);
        return false;
    }
    struct stat status {};
    if (flock(descriptor, LOCK_EX) != 0 || fstat(descriptor, &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_uid != config_.ownerUid ||
        status.st_gid != config_.ownerGid || (status.st_mode & 0777) != 0600 ||
        status.st_nlink != 1) {
        error = "elevation audit is not an owner-only regular file";
        close(descriptor);
        return false;
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream line;
    line << now << '\t' << event << '\t' << request.appId << '\t'
         << request.instanceId << '\t' << request.requestId << '\t'
         << static_cast<unsigned>(request.scope) << '\t' << request.target << '\t'
         << (accepted ? "allow" : "deny") << '\t' << result << '\n';
    const std::string encoded = line.str();
    const bool ok = writeAll(descriptor, encoded) && fsync(descriptor) == 0;
    const int savedErrno = errno;
    flock(descriptor, LOCK_UN);
    close(descriptor);
    if (!ok) {
        error = std::string("could not append elevation audit: ") + std::strerror(savedErrno);
    }
    return ok;
}

ElevationAuthority::ElevationAuthority(ApplicationPeerAuthenticatorConfig peers,
                                       PermissionStoreConfig permissions,
                                       ElevationAuditStoreConfig audit)
    : peers_(std::move(peers)), permissions_(std::move(permissions)),
      audit_(std::move(audit)) {}

std::optional<ElevationPrompt> ElevationAuthority::request(
        const ElevationPeerCredentials& peer, const ElevationRequest& requestValue,
        const PermissionSubject& subject,
        const std::vector<std::string>& requestedPermissions,
        const TrustedInteractionContext& interaction, std::string displayName,
        std::string& error) {
    error.clear();
    if (!validRequest(requestValue) || !validatePermissionSubject(subject, error) ||
        subject.appId != requestValue.appId ||
        !sameDigest(subject.bundleRecordDigest, requestValue.bundleRecordDigest) ||
        subject.publisherIdentity != requestValue.publisherIdentity ||
        !visibleText(displayName, kMaximumDisplayNameBytes)) {
        if (error.empty()) error = "elevation request identity is invalid";
        return std::nullopt;
    }
    if (!peers_.authenticate(requestValue.appId, requestValue.instanceId,
                             peer.pid, peer.uid, peer.gid, error)) {
        return std::nullopt;
    }
    if (interaction.appId != requestValue.appId ||
        interaction.instanceId != requestValue.instanceId ||
        interaction.interactionSerial == 0 || !interaction.visible || !interaction.focused) {
        error = "elevation requires a focused visible app and trusted interaction serial";
        const std::string diagnostic = error;
        std::string auditError;
        audit_.append("request", requestValue, false, diagnostic, auditError);
        return std::nullopt;
    }
    if (std::find(requestedPermissions.begin(), requestedPermissions.end(),
        "admin.elevation") == requestedPermissions.end()) {
        error = "application manifest did not request admin.elevation";
        const std::string diagnostic = error;
        std::string auditError;
        audit_.append("request", requestValue, false, diagnostic, auditError);
        return std::nullopt;
    }
    if (!permissions_.isGranted(subject, "admin.elevation", error)) {
        if (error.empty()) error = "admin.elevation is not enabled for this bundle";
        const std::string diagnostic = error;
        std::string auditError;
        audit_.append("request", requestValue, false, diagnostic, auditError);
        return std::nullopt;
    }
    if (pending_.contains(requestValue.requestId)) {
        error = "elevation request ID is already pending";
        return std::nullopt;
    }
    ElevationPrompt prompt{requestValue, std::move(displayName)};
    pending_.emplace(requestValue.requestId, Pending{prompt});
    std::string auditError;
    if (!audit_.append("request", requestValue, true, "pending-visible-consent", auditError)) {
        pending_.erase(requestValue.requestId);
        error = auditError;
        return std::nullopt;
    }
    return prompt;
}

std::uint64_t ElevationAuthority::allocateGrantId() {
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        std::uint64_t value = 0;
        if (getrandom(&value, sizeof(value), 0) != static_cast<ssize_t>(sizeof(value))) {
            return 0;
        }
        if (value != 0 && !grants_.contains(value)) return value;
    }
    return 0;
}

void ElevationAuthority::removeExpired() {
    const auto now = std::chrono::steady_clock::now();
    for (auto iterator = grants_.begin(); iterator != grants_.end();) {
        if (iterator->second.expiresAt <= now) {
            std::string auditError;
            audit_.append("expire", iterator->second.request, false,
                          "grant-expired", auditError);
            iterator = grants_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

std::optional<ElevationGrant> ElevationAuthority::decide(
        std::uint64_t requestId, bool approved, std::chrono::seconds lifetime,
        std::string& error) {
    error.clear();
    removeExpired();
    const auto found = pending_.find(requestId);
    if (found == pending_.end()) {
        error = "elevation request is not pending";
        return std::nullopt;
    }
    const ElevationRequest requestValue = found->second.prompt.request;
    pending_.erase(found);
    if (!approved) {
        if (!audit_.append("decision", requestValue, false, "user-denied", error)) return std::nullopt;
        error = "elevation denied by user";
        return std::nullopt;
    }
    if (lifetime <= std::chrono::seconds::zero() || lifetime > kMaximumGrantLifetime) {
        error = "elevation grant lifetime is invalid";
        std::string auditError;
        audit_.append("decision", requestValue, false, error, auditError);
        return std::nullopt;
    }
    const std::uint64_t grantId = allocateGrantId();
    if (grantId == 0) {
        error = "could not allocate elevation grant";
        return std::nullopt;
    }
    ElevationGrant grant{grantId, requestValue.instanceId, requestValue.appId,
                         requestValue.bundleRecordDigest, requestValue.scope,
                         requestValue.target};
    if (!audit_.append("decision", requestValue, true, "one-shot-grant", error)) {
        return std::nullopt;
    }
    grants_.emplace(grantId, LiveGrant{grant, requestValue,
                                      std::chrono::steady_clock::now() + lifetime});
    return grant;
}

bool ElevationAuthority::consume(const ElevationGrant& grant, std::string& error) {
    error.clear();
    removeExpired();
    const auto found = grants_.find(grant.grantId);
    if (found == grants_.end() || found->second.grant.instanceId != grant.instanceId ||
        found->second.grant.appId != grant.appId ||
        !sameDigest(found->second.grant.bundleRecordDigest, grant.bundleRecordDigest) ||
        found->second.grant.scope != grant.scope || found->second.grant.target != grant.target) {
        error = "elevation grant is absent, expired, or bound to another operation";
        return false;
    }
    const ElevationRequest requestValue = found->second.request;
    if (!audit_.append("consume", requestValue, true, "operation-consumed", error)) {
        return false;
    }
    grants_.erase(found);
    return true;
}

void ElevationAuthority::revokeInstance(std::string_view appId, std::uint64_t instanceId) {
    std::erase_if(pending_, [&](const auto& item) {
        return item.second.prompt.request.appId == appId &&
               item.second.prompt.request.instanceId == instanceId;
    });
    for (auto iterator = grants_.begin(); iterator != grants_.end();) {
        if (iterator->second.grant.appId == appId &&
            iterator->second.grant.instanceId == instanceId) {
            std::string auditError;
            audit_.append("revoke", iterator->second.request, false,
                          "application-exit", auditError);
            iterator = grants_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void ElevationAuthority::revokeAll() {
    pending_.clear();
    for (const auto& [grantId, live] : grants_) {
        (void)grantId;
        std::string auditError;
        audit_.append("revoke", live.request, false, "session-lock", auditError);
    }
    grants_.clear();
}

} // namespace lcl::security
