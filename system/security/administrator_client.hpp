#pragma once

#include "system/security/administrator_protocol.hpp"

#include <cstdint>
#include <string>

namespace lcl::security {

/** Non-blocking trusted-shell side of the administrator prompt channel. */
class AdministratorPromptClient final {
public:
    AdministratorPromptClient() = default;
    ~AdministratorPromptClient();

    AdministratorPromptClient(const AdministratorPromptClient&) = delete;
    AdministratorPromptClient& operator=(const AdministratorPromptClient&) = delete;

    bool connect(const std::string& socketPath = kAdministratorSocket);
    void close();
    bool isConnected() const noexcept { return descriptor_ >= 0; }
    bool poll(std::uint32_t& requestId, AdministratorPrompt& prompt);
    bool decide(std::uint32_t requestId, bool allowed);

private:
    bool sendPacket(AdministratorOpcode opcode, std::uint32_t requestId,
                    const std::vector<std::uint8_t>& payload);

    int descriptor_{-1};
};

/** Blocking command-side client used only by the immutable lcl-sudo binary. */
class AdministratorCommandClient final {
public:
    AdministratorCommandClient() = default;
    ~AdministratorCommandClient();

    AdministratorCommandClient(const AdministratorCommandClient&) = delete;
    AdministratorCommandClient& operator=(const AdministratorCommandClient&) = delete;

    bool connect(const std::string& socketPath, std::string& error);
    int execute(const AdministratorExecuteRequest& request, std::string& error);
    void close();

private:
    bool sendPacket(AdministratorOpcode opcode, std::uint32_t requestId,
                    const std::vector<std::uint8_t>& payload, std::string& error);
    bool receivePacket(DecodedAdministratorPacket& packet, std::string& error);

    int descriptor_{-1};
    std::uint32_t nextRequestId_{1};
};

} // namespace lcl::security
