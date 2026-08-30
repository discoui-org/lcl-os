#pragma once

#include <string>
#include <vector>

#include "system/session/session_protocol.hpp"

namespace lcl::session {

class SessionClient {
public:
    SessionClient() = default;
    ~SessionClient();
    SessionClient(const SessionClient&) = delete;
    SessionClient& operator=(const SessionClient&) = delete;

    bool connect(const std::string& socketPath = kSessionSocket);
    bool requestCatalog(std::vector<CatalogEntry>& entries, std::string& error);
    bool launch(const LaunchRequest& request, LaunchResponse& response, std::string& error);
    bool waitForExit(uint64_t instanceId, ProcessExited& event, std::string& error);

private:
    bool sendRequest(SessionOpcode opcode, const std::vector<uint8_t>& payload,
                     uint32_t& requestId, std::string& error);
    bool receive(DecodedPacket& packet, std::string& error);

    int m_fd{-1};
    uint32_t m_nextRequestId{1};
};

} // namespace lcl::session
