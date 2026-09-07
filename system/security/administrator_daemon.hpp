#pragma once

#include "system/security/administrator_protocol.hpp"
#include "system/security/session_user.hpp"

#include <optional>
#include <chrono>
#include <string>
#include <sys/types.h>
#include <vector>

namespace lcl::security {

struct AdministratorDaemonConfig {
    std::string socketPath{kAdministratorSocket};
    uid_t sessionUid{kSessionUserUid};
    gid_t sessionGid{kSessionUserGid};
};

/** Root broker for trusted Terminal sudo requests and shell-owned consent. */
class AdministratorDaemon final {
public:
    explicit AdministratorDaemon(AdministratorDaemonConfig config = {});
    ~AdministratorDaemon();

    AdministratorDaemon(const AdministratorDaemon&) = delete;
    AdministratorDaemon& operator=(const AdministratorDaemon&) = delete;

    bool initialize(std::string& error);
    void poll();
    void shutdown();

private:
    enum class ClientRole { Unknown, Shell, Command };
    struct Client {
        int descriptor{-1};
        pid_t pid{0};
        uid_t uid{0};
        gid_t gid{0};
        ClientRole role{ClientRole::Unknown};
        pid_t terminalPid{0};
        std::uint64_t terminalStartTime{0};
    };
    struct PendingCommand {
        int clientDescriptor{-1};
        std::uint32_t requestId{0};
        AdministratorExecuteRequest request;
        pid_t terminalPid{0};
        std::uint64_t terminalStartTime{0};
    };
    struct CachedPermission {
        pid_t terminalPid{0};
        std::uint64_t terminalStartTime{0};
        std::chrono::steady_clock::time_point expiresAt;
    };
    struct RunningCommand {
        int clientDescriptor{-1};
        std::uint32_t requestId{0};
        pid_t pid{-1};
        int stdoutDescriptor{-1};
        int stderrDescriptor{-1};
        bool childExited{false};
        int exitCode{125};
    };

    void acceptConnections();
    void serviceClient(int descriptor);
    void removeClient(int descriptor);
    Client* findClient(int descriptor);
    bool sendPacket(int descriptor, AdministratorOpcode opcode,
                    std::uint32_t requestId,
                    const std::vector<std::uint8_t>& payload);
    void sendError(int descriptor, std::uint32_t requestId,
                   const std::string& message);
    bool startCommand(PendingCommand pending, std::string& error);
    void serviceOutput(int descriptor, AdministratorOutputStream stream);
    void finishCommandIfReady();
    void pruneCachedPermission();

    AdministratorDaemonConfig config_;
    int serverDescriptor_{-1};
    int shellDescriptor_{-1};
    bool ownsSocketPath_{false};
    std::vector<Client> clients_;
    std::optional<PendingCommand> pending_;
    std::optional<RunningCommand> running_;
    std::optional<CachedPermission> cachedPermission_;
};

} // namespace lcl::security
