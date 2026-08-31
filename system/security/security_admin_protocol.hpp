#pragma once

#include "system/security/pending_bundle_approval_store.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lcl::security {

/** Root-owned sessiond endpoint; only a connected FD reaches Settings. */
inline constexpr const char* kSecurityAdminSocket = "/Runtime/lcl-securityd.sock";
inline constexpr std::uint32_t kSecurityAdminProtocolMagic = 0x4C435343; // "LCSC"
inline constexpr std::uint32_t kSecurityAdminProtocolVersion = 1;
inline constexpr std::uint32_t kSecurityAdminWireHeaderSize = 20;
inline constexpr std::uint32_t kSecurityAdminMaxPayload = 64u * 1024u;

enum class SecurityAdminOpcode : std::uint32_t {
    PendingBundleApprovalsRequest = 1,
    PendingBundleApprovalsResponse = 2,
    ApproveBundleRequest = 3,
    ApproveBundleResponse = 4,
    RevokeBundleApprovalRequest = 5,
    RevokeBundleApprovalResponse = 6,
    ErrorResponse = 7,
};

struct SecurityAdminHeader {
    std::uint32_t magic{kSecurityAdminProtocolMagic};
    std::uint32_t version{kSecurityAdminProtocolVersion};
    SecurityAdminOpcode opcode{SecurityAdminOpcode::ErrorResponse};
    std::uint32_t requestId{0};
    std::uint32_t payloadSize{0};
};

struct DecodedSecurityAdminPacket {
    SecurityAdminHeader header;
    std::vector<std::uint8_t> payload;
};

/** The only approval selector accepted from the trusted user-facing client. */
struct SecurityAdminBundleIdentity {
    std::string appId;
    Sha256Digest bundleRecordDigest{};
};

struct SecurityAdminApprovalResult {
    bool accepted{false};
    std::string message;
};

bool encodeSecurityAdminPacket(const SecurityAdminHeader& header,
                               const std::vector<std::uint8_t>& payload,
                               std::vector<std::uint8_t>& packet);
bool decodeSecurityAdminPacket(const std::uint8_t* packet, std::size_t packetSize,
                               DecodedSecurityAdminPacket& decoded);

bool encodePendingBundleApprovals(const std::vector<PendingBundleApproval>& pending,
                                  std::vector<std::uint8_t>& payload);
bool decodePendingBundleApprovals(const std::vector<std::uint8_t>& payload,
                                  std::vector<PendingBundleApproval>& pending);
bool encodeSecurityAdminBundleIdentity(const SecurityAdminBundleIdentity& identity,
                                       std::vector<std::uint8_t>& payload);
bool decodeSecurityAdminBundleIdentity(const std::vector<std::uint8_t>& payload,
                                       SecurityAdminBundleIdentity& identity);
bool encodeSecurityAdminApprovalResult(const SecurityAdminApprovalResult& result,
                                       std::vector<std::uint8_t>& payload);
bool decodeSecurityAdminApprovalResult(const std::vector<std::uint8_t>& payload,
                                       SecurityAdminApprovalResult& result);
bool encodeSecurityAdminError(const std::string& message, std::vector<std::uint8_t>& payload);
bool decodeSecurityAdminError(const std::vector<std::uint8_t>& payload, std::string& message);

} // namespace lcl::security
