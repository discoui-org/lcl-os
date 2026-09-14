#include "system/security/portal_authority.hpp"

#include "system/security/app_identity_registry.hpp"
#include "system/security/permission_policy.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace lcl::security {
namespace {

constexpr std::size_t kMaximumScopeBytes = 1024;
constexpr std::size_t kMaximumDisplayNameBytes = 256;
constexpr std::uint32_t kPortalDescriptorMagic = 0x4c435046; // "LCPF"
constexpr std::size_t kPortalDescriptorPacketSize = 16;

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

bool sameRequest(const PortalRequest& left, const PortalRequest& right) {
    return left.contractVersion == right.contractVersion &&
           left.requestId == right.requestId && left.instanceId == right.instanceId &&
           left.appId == right.appId && left.operation == right.operation &&
           left.scope == right.scope;
}

void appendU32(std::array<std::uint8_t, kPortalDescriptorPacketSize>& packet,
               std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        packet[offset + index] = static_cast<std::uint8_t>(value >> ((3 - index) * 8));
    }
}

void appendU64(std::array<std::uint8_t, kPortalDescriptorPacketSize>& packet,
               std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        packet[offset + index] = static_cast<std::uint8_t>(value >> ((7 - index) * 8));
    }
}

std::uint32_t readU32(const std::uint8_t* bytes) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) value = (value << 8) | bytes[index];
    return value;
}

std::uint64_t readU64(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) value = (value << 8) | bytes[index];
    return value;
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
    if (transient_.contains(request.requestId)) {
        error = "portal request ID already has transient authority";
        return PortalAuthorization::Denied;
    }
    const bool hasSessionGrant = std::any_of(
        transient_.begin(), transient_.end(), [&](const auto& item) {
            const TransientGrant& grant = item.second;
            return grant.duration == PortalConsentDuration::Session &&
                   grant.request.instanceId == request.instanceId &&
                   grant.request.appId == request.appId &&
                   grant.request.operation == request.operation &&
                   grant.request.scope == request.scope && grant.permission == *permission &&
                   sameSubject(grant.subject, subject);
        });
    if (hasSessionGrant) return PortalAuthorization::Granted;
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
    if (duration != PortalConsentDuration::Once &&
        duration != PortalConsentDuration::Session &&
        duration != PortalConsentDuration::Persistent) {
        error = "portal consent duration is invalid";
        return false;
    }
    if (duration == PortalConsentDuration::Persistent) {
        if (!permissions_.setDecision(subject, std::string(*permission), granted, error)) {
            return false;
        }
    } else if (granted) {
        transient_.emplace(request.requestId,
            TransientGrant{request, subject, std::string(*permission), duration});
    }
    pending_.erase(pending);
    return true;
}

bool PortalAuthority::consumeTransientGrant(const PortalRequest& request,
                                            const PermissionSubject& subject,
                                            std::string& error) {
    error.clear();
    const auto found = transient_.find(request.requestId);
    if (found == transient_.end() || found->second.duration != PortalConsentDuration::Once ||
        !sameRequest(found->second.request, request) ||
        !sameSubject(found->second.subject, subject)) {
        error = "portal transient grant is absent or bound to another request";
        return false;
    }
    transient_.erase(found);
    return true;
}

void PortalAuthority::revokeInstance(std::string_view appId, std::uint64_t instanceId) {
    std::erase_if(pending_, [&](const auto& item) {
        return item.second.request.appId == appId &&
               item.second.request.instanceId == instanceId;
    });
    std::erase_if(transient_, [&](const auto& item) {
        return item.second.request.appId == appId &&
               item.second.request.instanceId == instanceId;
    });
}

void PortalAuthority::revokeAll() {
    pending_.clear();
    transient_.clear();
}

bool sendPortalDescriptor(int socketDescriptor, std::uint64_t requestId,
                          int transferredDescriptor, std::string& error) {
    error.clear();
    if (socketDescriptor < 0 || transferredDescriptor < 0 || requestId == 0) {
        error = "portal descriptor transfer arguments are invalid";
        return false;
    }
    std::array<std::uint8_t, kPortalDescriptorPacketSize> packet{};
    appendU32(packet, 0, kPortalDescriptorMagic);
    appendU32(packet, 4, kPortalContractVersion);
    appendU64(packet, 8, requestId);
    iovec vector{.iov_base = packet.data(), .iov_len = packet.size()};
    std::array<char, CMSG_SPACE(sizeof(int))> ancillary{};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = ancillary.data();
    message.msg_controllen = ancillary.size();
    cmsghdr* control = CMSG_FIRSTHDR(&message);
    control->cmsg_level = SOL_SOCKET;
    control->cmsg_type = SCM_RIGHTS;
    control->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(control), &transferredDescriptor, sizeof(int));
    if (sendmsg(socketDescriptor, &message, MSG_NOSIGNAL) !=
        static_cast<ssize_t>(packet.size())) {
        error = std::string("could not transfer portal descriptor: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool receivePortalDescriptor(int socketDescriptor, std::uint64_t expectedRequestId,
                             int& transferredDescriptor, std::string& error) {
    error.clear();
    transferredDescriptor = -1;
    if (socketDescriptor < 0 || expectedRequestId == 0) {
        error = "portal descriptor receive arguments are invalid";
        return false;
    }
    std::array<std::uint8_t, kPortalDescriptorPacketSize> packet{};
    std::array<char, CMSG_SPACE(sizeof(int) * 2)> ancillary{};
    iovec vector{.iov_base = packet.data(), .iov_len = packet.size()};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = ancillary.data();
    message.msg_controllen = ancillary.size();
    const ssize_t count = recvmsg(socketDescriptor, &message,
                                  MSG_CMSG_CLOEXEC | MSG_TRUNC);
    int received = -1;
    std::size_t descriptorCount = 0;
    for (cmsghdr* control = CMSG_FIRSTHDR(&message); control;
         control = CMSG_NXTHDR(&message, control)) {
        if (control->cmsg_level != SOL_SOCKET || control->cmsg_type != SCM_RIGHTS ||
            control->cmsg_len < CMSG_LEN(0)) continue;
        const std::size_t bytes = control->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int) != 0) continue;
        const auto* descriptors = reinterpret_cast<const int*>(CMSG_DATA(control));
        for (std::size_t index = 0; index < bytes / sizeof(int); ++index) {
            ++descriptorCount;
            if (descriptorCount == 1) received = descriptors[index];
            else if (descriptors[index] >= 0) close(descriptors[index]);
        }
    }
    const bool valid = count == static_cast<ssize_t>(packet.size()) &&
        (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) == 0 && descriptorCount == 1 &&
        readU32(packet.data()) == kPortalDescriptorMagic &&
        readU32(packet.data() + 4) == kPortalContractVersion &&
        readU64(packet.data() + 8) == expectedRequestId;
    if (!valid) {
        if (received >= 0) close(received);
        error = "portal descriptor packet is malformed or mismatched";
        return false;
    }
    transferredDescriptor = received;
    return true;
}

} // namespace lcl::security
