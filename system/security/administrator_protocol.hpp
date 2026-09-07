#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lcl::security {

inline constexpr const char* kAdministratorSocket = "/Runtime/lcl-admind.sock";
inline constexpr std::uint32_t kAdministratorProtocolMagic = 0x4c434144; // LCAD
inline constexpr std::uint32_t kAdministratorProtocolVersion = 1;
inline constexpr std::uint32_t kAdministratorWireHeaderSize = 20;
inline constexpr std::uint32_t kAdministratorMaxPayload = 64u * 1024u;

enum class AdministratorOpcode : std::uint32_t {
    ShellHello = 1,
    ExecuteRequest = 2,
    PermissionPrompt = 3,
    PermissionDecision = 4,
    CommandOutput = 5,
    CommandResult = 6,
    ErrorResponse = 7,
    RevokeSession = 8,
};

enum class AdministratorOutputStream : std::uint8_t {
    StandardOutput = 1,
    StandardError = 2,
};

struct AdministratorHeader {
    std::uint32_t magic{kAdministratorProtocolMagic};
    std::uint32_t version{kAdministratorProtocolVersion};
    AdministratorOpcode opcode{AdministratorOpcode::ErrorResponse};
    std::uint32_t requestId{0};
    std::uint32_t payloadSize{0};
};

struct DecodedAdministratorPacket {
    AdministratorHeader header;
    std::vector<std::uint8_t> payload;
};

struct AdministratorExecuteRequest {
    std::string workingDirectory;
    std::vector<std::string> arguments;
};

struct AdministratorPrompt {
    std::string appName;
    std::string title;
    std::string description;
};

struct AdministratorCommandOutput {
    AdministratorOutputStream stream{AdministratorOutputStream::StandardOutput};
    std::vector<std::uint8_t> bytes;
};

bool encodeAdministratorPacket(const AdministratorHeader& header,
                               const std::vector<std::uint8_t>& payload,
                               std::vector<std::uint8_t>& packet);
bool decodeAdministratorPacket(const std::uint8_t* packet, std::size_t packetSize,
                               DecodedAdministratorPacket& decoded);
bool encodeAdministratorExecuteRequest(const AdministratorExecuteRequest& request,
                                       std::vector<std::uint8_t>& payload);
bool decodeAdministratorExecuteRequest(const std::vector<std::uint8_t>& payload,
                                       AdministratorExecuteRequest& request);
bool encodeAdministratorPrompt(const AdministratorPrompt& prompt,
                               std::vector<std::uint8_t>& payload);
bool decodeAdministratorPrompt(const std::vector<std::uint8_t>& payload,
                               AdministratorPrompt& prompt);
bool encodeAdministratorDecision(bool allowed, std::vector<std::uint8_t>& payload);
bool decodeAdministratorDecision(const std::vector<std::uint8_t>& payload, bool& allowed);
bool encodeAdministratorCommandOutput(const AdministratorCommandOutput& output,
                                      std::vector<std::uint8_t>& payload);
bool decodeAdministratorCommandOutput(const std::vector<std::uint8_t>& payload,
                                      AdministratorCommandOutput& output);
bool encodeAdministratorCommandResult(int exitCode, std::vector<std::uint8_t>& payload);
bool decodeAdministratorCommandResult(const std::vector<std::uint8_t>& payload,
                                      int& exitCode);
bool encodeAdministratorError(const std::string& message,
                              std::vector<std::uint8_t>& payload);
bool decodeAdministratorError(const std::vector<std::uint8_t>& payload,
                              std::string& message);

} // namespace lcl::security
