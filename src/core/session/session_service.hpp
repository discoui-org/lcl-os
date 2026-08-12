#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/session/app_registry.hpp"
#include "core/session/session_protocol.hpp"

namespace lcl::session {

struct AppInstance {
    uint64_t instanceId{0};
    std::string appId;
    int32_t pid{0};
    bool running{false};
    int32_t exitCode{0};
};

/**
 * The session authority for application catalog, launch and process lifecycle.
 * It deliberately has no compositor, surface, scene, or focus dependency.
 */
class SessionService {
public:
    explicit SessionService(std::vector<std::string> appSearchPaths = {
        "/home/user/Applications", "/Applications"});
    ~SessionService();

    SessionService(const SessionService&) = delete;
    SessionService& operator=(const SessionService&) = delete;

    bool initialize(const std::string& socketPath = kSessionSocket);
    void shutdown();
    void poll();

    void refreshCatalog();
    const AppRegistry& registry() const noexcept { return m_registry; }
    const std::unordered_map<uint64_t, AppInstance>& instances() const noexcept {
        return m_instances;
    }

    LaunchResponse launch(const LaunchRequest& request);
    bool launchDefaultProfile();

private:
    bool sendPacket(int fd, SessionOpcode opcode, uint32_t requestId,
                    const std::vector<uint8_t>& payload);
    void acceptConnections();
    void serviceClient(int fd);
    void removeClient(int fd);
    void reapChildren();
    void notifyExit(const ProcessExited& event);
    static int exitCodeFromStatus(int status);

    AppRegistry m_registry;
    std::string m_socketPath;
    int m_serverFd{-1};
    std::vector<int> m_clientFds;
    std::unordered_map<uint64_t, AppInstance> m_instances;
    std::unordered_map<int32_t, uint64_t> m_instanceByPid;
    std::unordered_map<uint64_t, std::vector<int>> m_waiters;
    uint64_t m_nextInstanceId{1};
};

} // namespace lcl::session
