#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "system/security/sandbox_contract.hpp"

namespace lcl::security {

inline constexpr std::uint32_t kSandboxProtocolMagic = 0x4C435342; // "LCSB"
inline constexpr std::uint32_t kSandboxProtocolVersion = 1;
inline constexpr std::uint32_t kSandboxWireHeaderSize = 20;
inline constexpr std::uint32_t kSandboxMaxPayload = 8u * 1024u;

enum class SandboxOpcode : std::uint32_t {
    LaunchRequest = 1,
    LaunchResult = 2,
    ErrorResponse = 3,
};

struct SandboxHeader {
    std::uint32_t magic{kSandboxProtocolMagic};
    std::uint32_t version{kSandboxProtocolVersion};
    SandboxOpcode opcode{SandboxOpcode::ErrorResponse};
    std::uint32_t requestId{0};
    std::uint32_t payloadSize{0};
};

struct DecodedSandboxPacket {
    SandboxHeader header;
    std::vector<std::uint8_t> payload;
};

bool encodeSandboxPacket(const SandboxHeader& header, const std::vector<std::uint8_t>& payload,
                         std::vector<std::uint8_t>& packet);
bool decodeSandboxPacket(const std::uint8_t* packet, std::size_t packetSize,
                         DecodedSandboxPacket& decoded);
bool encodeSandboxLaunchRequest(const SandboxLaunchRequest& request,
                                std::vector<std::uint8_t>& payload);
bool decodeSandboxLaunchRequest(const std::vector<std::uint8_t>& payload,
                                SandboxLaunchRequest& request);
bool encodeSandboxLaunchResult(const SandboxLaunchResult& result,
                               std::vector<std::uint8_t>& payload);
bool decodeSandboxLaunchResult(const std::vector<std::uint8_t>& payload,
                               SandboxLaunchResult& result);
/** A bounded diagnostic for an invalid sandbox control-plane packet. */
bool encodeSandboxError(const std::string& message, std::vector<std::uint8_t>& payload);
bool decodeSandboxError(const std::vector<std::uint8_t>& payload, std::string& message);

} // namespace lcl::security
