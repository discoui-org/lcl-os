#pragma once

#include "system/security/bundle_approval_authority.hpp"
#include "system/security/security_admin_protocol.hpp"
#include "system/security/session_user.hpp"

#include <string>
#include <sys/types.h>
#include <vector>

namespace lcl::security {

/** Root-owned, sessiond-only configuration for lcl-securityd. */
struct SecurityAdminDaemonConfig {
    std::string socketPath{kSecurityAdminSocket};
    uid_t sessionUid{kSessionUserUid};
    gid_t sessionGid{kSessionUserGid};
    uid_t ownerUid{0};
    gid_t ownerGid{0};
    BundleApprovalAuthorityConfig bundleApprovals{};
};

/**
 * Narrow root service for a future trusted Settings surface.
 *
 * It accepts only root-sessiond connections, then exposes one session user's
 * pending unverified bundle identities and allows exactly one hash approval
 * or revoke. Sessiond passes that connected socket only to canonical Settings.
 * It never receives an executable path, launch request, permission grant,
 * elevation request, or application-supplied file descriptor.
 */
class SecurityAdminDaemon final {
public:
    explicit SecurityAdminDaemon(SecurityAdminDaemonConfig config = {});
    ~SecurityAdminDaemon();

    SecurityAdminDaemon(const SecurityAdminDaemon&) = delete;
    SecurityAdminDaemon& operator=(const SecurityAdminDaemon&) = delete;

    bool initialize(std::string& error);
    void shutdown();
    void poll();

private:
    bool validateConfig(std::string& error) const;
    bool validateSocketParent(std::string& error) const;
    bool peerIsTrustedSessionAuthority(int descriptor) const;
    bool sendPacket(int descriptor, SecurityAdminOpcode opcode, std::uint32_t requestId,
                    const std::vector<std::uint8_t>& payload);
    void acceptConnections();
    void serviceClient(int descriptor);
    void removeClient(int descriptor);
    void sendErrorAndClose(int descriptor, std::uint32_t requestId, const std::string& message);

    BundleApprovalAuthority authority_;
    SecurityAdminDaemonConfig config_;
    int serverDescriptor_{-1};
    bool ownsSocketPath_{false};
    std::vector<int> clientDescriptors_;
};

} // namespace lcl::security
