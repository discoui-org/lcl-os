#pragma once

#include "system/security/application_peer_authenticator.hpp"
#include "system/security/permission_store.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

namespace lcl::security {

inline constexpr std::uint32_t kPortalContractVersion = 1;

enum class PortalOperation : std::uint8_t {
    OpenFile = 1,
    ReadDocument = 2,
    WriteDocument = 3,
    ReadClipboard = 4,
    CaptureCamera = 5,
    CaptureMicrophone = 6,
    PostNotification = 7,
    OpenApplication = 8,
};

enum class PortalConsentDuration : std::uint8_t {
    Once = 1,
    Session = 2,
    Persistent = 3,
};

/** Untrusted request fields carried by an app-facing portal endpoint. */
struct PortalRequest {
    std::uint32_t contractVersion{kPortalContractVersion};
    std::uint64_t requestId{0};
    std::uint64_t instanceId{0};
    std::string appId;
    PortalOperation operation{PortalOperation::OpenFile};
    /** Human-readable object/scope; never interpreted as an authority path. */
    std::string scope;
};

/** Trusted-shell text. The requesting app never supplies these identity fields. */
struct PortalPrompt {
    std::uint64_t requestId{0};
    std::string appId;
    std::string displayName;
    std::string publisherIdentity;
    std::string permission;
    std::string scope;
    std::vector<PortalConsentDuration> allowedDurations;
};

struct PortalPeerCredentials {
    pid_t pid{0};
    uid_t uid{0};
    gid_t gid{0};
};

enum class PortalAuthorization : std::uint8_t {
    Denied = 0,
    NeedsVisibleConsent = 1,
    Granted = 2,
};

std::optional<std::string_view> permissionForPortalOperation(PortalOperation operation) noexcept;
bool validatePortalRequest(const PortalRequest& request, std::string& error);

/**
 * Policy core shared by file, clipboard, media, notification and launch portals.
 *
 * It authenticates the kernel peer against the live app launch registry before
 * consulting PermissionStore. It has no filesystem-opening or device access;
 * concrete trusted brokers retain those resources and transfer only the result
 * descriptor for one authorized request.
 */
class PortalAuthority final {
public:
    PortalAuthority(ApplicationPeerAuthenticatorConfig peers,
                    PermissionStoreConfig permissions = {});

    PortalAuthorization authorize(const PortalPeerCredentials& peer,
                                  const PortalRequest& request,
                                  const PermissionSubject& subject,
                                  const std::vector<std::string>& requestedPermissions,
                                  std::string& error);

    std::optional<PortalPrompt> makeTrustedPrompt(const PortalRequest& request,
                                                  const PermissionSubject& subject,
                                                  std::string displayName,
                                                  std::string& error) const;

    /** Called only by the trusted shell/admin capability, never an app socket. */
    bool recordVisibleDecision(const PortalRequest& request,
                               const PermissionSubject& subject,
                               bool granted,
                               PortalConsentDuration duration,
                               std::string& error);

    /** Consumes an exact one-shot grant or checks a live session grant. */
    bool consumeTransientGrant(const PortalRequest& request,
                               const PermissionSubject& subject,
                               std::string& error);

    /** App-exit and session-lock hooks for broker-owned transient authority. */
    void revokeInstance(std::string_view appId, std::uint64_t instanceId);
    void revokeAll();

private:
    struct PendingConsent {
        PortalRequest request;
        PermissionSubject subject;
        std::string permission;
    };
    struct TransientGrant {
        PortalRequest request;
        PermissionSubject subject;
        std::string permission;
        PortalConsentDuration duration{PortalConsentDuration::Once};
    };

    ApplicationPeerAuthenticator peers_;
    PermissionStore permissions_;
    std::unordered_map<std::uint64_t, PendingConsent> pending_;
    std::unordered_map<std::uint64_t, TransientGrant> transient_;
};

/**
 * Transfers exactly one broker-owned descriptor for one authorized request.
 * The wire message contains only a version and request ID; paths and ambient
 * directory authority never cross the app-facing socket.
 */
bool sendPortalDescriptor(int socketDescriptor, std::uint64_t requestId,
                          int transferredDescriptor, std::string& error);
bool receivePortalDescriptor(int socketDescriptor, std::uint64_t expectedRequestId,
                             int& transferredDescriptor, std::string& error);

} // namespace lcl::security
