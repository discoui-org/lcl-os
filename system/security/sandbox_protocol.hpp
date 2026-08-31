#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "system/security/sandbox_contract.hpp"

namespace lcl::security {

/** Root-owned sessiond ↔ sandboxd control endpoint. */
inline constexpr const char* kSandboxSocket = "/Runtime/lcl-sandboxd.sock";
inline constexpr std::uint32_t kSandboxProtocolMagic = 0x4C435342; // "LCSB"
inline constexpr std::uint32_t kSandboxProtocolVersion = 2;
inline constexpr std::uint32_t kSandboxWireHeaderSize = 20;
inline constexpr std::uint32_t kSandboxMaxPayload = 8u * 1024u;

enum class SandboxOpcode : std::uint32_t {
    LaunchRequest = 1,
    LaunchResult = 2,
    ErrorResponse = 3,
    ProcessExited = 4,
    RegisterApplication = 5,
    RegistrationResult = 6,
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

/** Daemon-owned process lifecycle notification for the session authority. */
struct SandboxProcessExited {
    std::uint64_t instanceId{0};
    std::int32_t exitCode{0};
};

struct SandboxRegistrationResult {
    bool registered{false};
    std::string message;
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
bool encodeSandboxApplicationRegistration(const SandboxApplicationRegistration& registration,
                                          std::vector<std::uint8_t>& payload);
bool decodeSandboxApplicationRegistration(const std::vector<std::uint8_t>& payload,
                                          SandboxApplicationRegistration& registration);
bool encodeSandboxRegistrationResult(const SandboxRegistrationResult& result,
                                     std::vector<std::uint8_t>& payload);
bool decodeSandboxRegistrationResult(const std::vector<std::uint8_t>& payload,
                                     SandboxRegistrationResult& result);
bool encodeSandboxProcessExited(const SandboxProcessExited& event,
                                std::vector<std::uint8_t>& payload);
bool decodeSandboxProcessExited(const std::vector<std::uint8_t>& payload,
                                SandboxProcessExited& event);
/** A bounded diagnostic for an invalid sandbox control-plane packet. */
bool encodeSandboxError(const std::string& message, std::vector<std::uint8_t>& payload);
bool decodeSandboxError(const std::vector<std::uint8_t>& payload, std::string& message);

} // namespace lcl::security
