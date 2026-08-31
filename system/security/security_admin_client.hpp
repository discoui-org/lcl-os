#pragma once

#include "system/security/security_admin_protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lcl::security {

/**
 * Client for the already-connected, Settings-only security decision
 * capability. It deliberately has no public direct-connect operation: only
 * root sessiond may open the daemon socket, then pass one connected descriptor
 * across the canonical Settings exec boundary.
 */
class SecurityAdminClient final {
public:
    SecurityAdminClient() = default;
    ~SecurityAdminClient();

    SecurityAdminClient(const SecurityAdminClient&) = delete;
    SecurityAdminClient& operator=(const SecurityAdminClient&) = delete;

    /** Root-sessiond-only creation of a capability to pass to Settings. */
    bool connectAsSessionAuthority(const std::string& socketPath, std::string& error);
    /** Adopt a connected descriptor supplied by sessiond; ownership transfers. */
    bool adoptCapabilityDescriptor(int descriptor, std::string& error);
    /** Releases the owned descriptor for the narrowly scoped Settings exec path. */
    int releaseCapabilityDescriptor() noexcept;
    int capabilityDescriptor() const noexcept { return descriptor_; }
    void close();
    bool connected() const noexcept { return descriptor_ >= 0; }

    bool pendingBundleApprovals(std::vector<PendingBundleApproval>& pending, std::string& error);
    bool approveBundle(const SecurityAdminBundleIdentity& identity, std::string& error);
    bool revokeBundleApproval(const SecurityAdminBundleIdentity& identity, std::string& error);

private:
    bool sendPacket(SecurityAdminOpcode opcode, const std::vector<std::uint8_t>& payload,
                    std::uint32_t& requestId, std::string& error);
    bool receivePacket(DecodedSecurityAdminPacket& packet, std::string& error);
    bool changeApproval(SecurityAdminOpcode requestOpcode, SecurityAdminOpcode responseOpcode,
                        const SecurityAdminBundleIdentity& identity, std::string& error);

    int descriptor_{-1};
    std::uint32_t nextRequestId_{1};
};

} // namespace lcl::security
