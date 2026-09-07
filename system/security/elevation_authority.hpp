#pragma once

#include "system/security/application_peer_authenticator.hpp"
#include "system/security/permission_store.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

namespace lcl::security {

inline constexpr std::uint32_t kElevationContractVersion = 1;

enum class ElevationScope : std::uint8_t {
    ManageBundleTrust = 1,
    WriteSystemSetting = 2,
    RunSignedHelper = 3,
};

struct ElevationRequest {
    std::uint32_t contractVersion{kElevationContractVersion};
    std::uint64_t requestId{0};
    std::uint64_t instanceId{0};
    std::string appId;
    Sha256Digest bundleRecordDigest{};
    std::string publisherIdentity;
    std::string reason;
    ElevationScope scope{ElevationScope::WriteSystemSetting};
    /** Policy key or registered helper ID; never an executable path or command. */
    std::string target;
};

/** Produced by the compositor/trusted shell, never accepted from the app socket. */
struct TrustedInteractionContext {
    std::string appId;
    std::uint64_t instanceId{0};
    std::uint64_t interactionSerial{0};
    bool visible{false};
    bool focused{false};
};

struct ElevationPeerCredentials {
    pid_t pid{0};
    uid_t uid{0};
    gid_t gid{0};
};

struct ElevationPrompt {
    ElevationRequest request;
    std::string displayName;
};

struct ElevationGrant {
    std::uint64_t grantId{0};
    std::uint64_t instanceId{0};
    std::string appId;
    Sha256Digest bundleRecordDigest{};
    ElevationScope scope{ElevationScope::WriteSystemSetting};
    std::string target;
};

struct ElevationAuditStoreConfig {
    std::string path{"/var/lib/lcl-security/admin-audit.v1"};
    uid_t ownerUid{0};
    gid_t ownerGid{0};
};

/** Root-owned append-only decision log. */
class ElevationAuditStore final {
public:
    explicit ElevationAuditStore(ElevationAuditStoreConfig config = {});
    bool append(std::string_view event, const ElevationRequest& request,
                bool accepted, std::string_view result, std::string& error) const;

private:
    ElevationAuditStoreConfig config_;
};

/**
 * In-memory, non-transferable elevation grant authority.
 *
 * The app receives no token. A trusted broker consumes a grant by the exact
 * app/instance/bundle/scope/target tuple. Grants are one-shot, expire quickly,
 * and disappear on app exit, lock, or daemon restart.
 */
class ElevationAuthority final {
public:
    ElevationAuthority(ApplicationPeerAuthenticatorConfig peers,
                       PermissionStoreConfig permissions = {},
                       ElevationAuditStoreConfig audit = {});

    std::optional<ElevationPrompt> request(
        const ElevationPeerCredentials& peer,
        const ElevationRequest& request,
        const PermissionSubject& subject,
        const std::vector<std::string>& requestedPermissions,
        const TrustedInteractionContext& interaction,
        std::string displayName,
        std::string& error);

    std::optional<ElevationGrant> decide(std::uint64_t requestId, bool approved,
                                         std::chrono::seconds lifetime,
                                         std::string& error);
    bool consume(const ElevationGrant& grant, std::string& error);
    void revokeInstance(std::string_view appId, std::uint64_t instanceId);
    void revokeAll();

private:
    struct Pending {
        ElevationPrompt prompt;
    };
    struct LiveGrant {
        ElevationGrant grant;
        ElevationRequest request;
        std::chrono::steady_clock::time_point expiresAt;
    };

    std::uint64_t allocateGrantId();
    void removeExpired();

    ApplicationPeerAuthenticator peers_;
    PermissionStore permissions_;
    ElevationAuditStore audit_;
    std::unordered_map<std::uint64_t, Pending> pending_;
    std::unordered_map<std::uint64_t, LiveGrant> grants_;
};

} // namespace lcl::security
