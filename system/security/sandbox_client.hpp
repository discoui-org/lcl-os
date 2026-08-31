#pragma once

#include <cstdint>
#include <deque>
#include <string>

#include "system/security/sandbox_protocol.hpp"

namespace lcl::security {

/**
 * Session-authority client for the narrow sandboxd control protocol.  It has
 * no operation for registering paths, identities, filesystem sources, or
 * capabilities; those remain daemon-local verifier material.
 */
class SandboxClient final {
public:
    SandboxClient() = default;
    ~SandboxClient();

    SandboxClient(const SandboxClient&) = delete;
    SandboxClient& operator=(const SandboxClient&) = delete;

    bool connect(const std::string& socketPath, std::string& error);
    void close();
    bool connected() const noexcept { return descriptor_ >= 0; }

    bool launch(const SandboxLaunchRequest& request, SandboxLaunchResult& result,
                std::string& error);
    /** Returns a queued or newly received process-exit event, if any. */
    bool pollExit(SandboxProcessExited& event, std::string& error);

private:
    bool sendPacket(SandboxOpcode opcode, const std::vector<std::uint8_t>& payload,
                    std::uint32_t& requestId, std::string& error);
    bool receivePacket(DecodedSandboxPacket& packet, int flags, std::string& error);

    int descriptor_{-1};
    std::uint32_t nextRequestId_{1};
    std::deque<SandboxProcessExited> pendingExits_;
};

} // namespace lcl::security
