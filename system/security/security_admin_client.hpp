#pragma once

#include "system/security/security_admin_protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lcl::security {

/** Future trusted Settings client for the narrow lcl-securityd decision API. */
class SecurityAdminClient final {
public:
    SecurityAdminClient() = default;
    ~SecurityAdminClient();

    SecurityAdminClient(const SecurityAdminClient&) = delete;
    SecurityAdminClient& operator=(const SecurityAdminClient&) = delete;

    bool connect(const std::string& socketPath, std::string& error);
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
