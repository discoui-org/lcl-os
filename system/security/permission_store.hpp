#pragma once

#include "system/security/sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

namespace lcl::security {

/** The version of one named permission's user decision semantics. */
inline constexpr std::uint32_t kPermissionDecisionVersion = 1;

struct PermissionStoreConfig {
    std::string storePath{"/var/lib/lcl-security/permissions.v1"};
    uid_t ownerUid{0};
    gid_t ownerGid{0};
};

/**
 * Immutable identity that a user decision is bound to.  A publisher identity
 * is either `system-image`, `unverified`, or an `ed25519:<sha256>`
 * fingerprint.  The exact bundle record digest prevents an old grant from
 * surviving a payload, manifest, or signing-envelope change.
 */
struct PermissionSubject {
    uid_t userUid{0};
    std::string appId;
    Sha256Digest bundleRecordDigest{};
    std::string publisherIdentity;
    std::uint32_t permissionVersion{kPermissionDecisionVersion};
};

struct PermissionDecision {
    PermissionSubject subject;
    std::string permission;
    bool granted{false};
};

/** Validates the complete, immutable key to which a decision is bound. */
bool validatePermissionSubject(const PermissionSubject& subject, std::string& error);

/** Builds the canonical identity used for a rootfs-provided system .app. */
PermissionSubject makeSystemImagePermissionSubject(uid_t userUid, std::string appId,
                                                   const Sha256Digest& bundleRecordDigest);

/**
 * Root-owned atomic permission decision database.
 *
 * This is deliberately only storage and policy evaluation.  It has no UI or
 * untrusted-app IPC surface; a future trusted Settings/admin broker is the
 * only writer.  An absent record is always deny.
 */
class PermissionStore final {
public:
    explicit PermissionStore(PermissionStoreConfig config = {});

    PermissionStore(const PermissionStore&) = delete;
    PermissionStore& operator=(const PermissionStore&) = delete;

    bool load(std::string& error);
    bool setDecision(const PermissionSubject& subject, std::string permission,
                     bool granted, std::string& error);
    /** Removes an explicit decision; the resulting behavior is default-deny. */
    bool revoke(const PermissionSubject& subject, const std::string& permission,
                std::string& error);
    bool isGranted(const PermissionSubject& subject, const std::string& permission,
                   std::string& error);
    /** Returns only granted permissions that the exact bundle currently requests. */
    std::vector<std::string> grantedPermissions(const PermissionSubject& subject,
                                                const std::vector<std::string>& requested,
                                                std::string& error);
    std::size_t size() const noexcept { return decisions_.size(); }

private:
    bool validateConfig(std::string& error) const;
    bool validateStoreParent(std::string& error) const;
    bool loadUnlocked(std::string& error);
    bool persistUnlocked(std::string& error) const;

    PermissionStoreConfig config_;
    std::vector<PermissionDecision> decisions_;
};

} // namespace lcl::security
