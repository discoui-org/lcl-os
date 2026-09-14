#pragma once

#include "system/security/administrator_protocol.hpp"
#include "system/security/session_user.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace lcl::security {

struct AdministratorTerminalIdentity {
    pid_t pid{0};
    std::uint64_t startTime{0};

    bool operator==(const AdministratorTerminalIdentity&) const = default;
};

/** A short in-memory permission bound to one exact Terminal process lifetime. */
class AdministratorPermissionLease final {
public:
    void grant(AdministratorTerminalIdentity identity,
               std::chrono::steady_clock::time_point now,
               std::chrono::seconds lifetime);
    bool permits(AdministratorTerminalIdentity identity,
                 std::chrono::steady_clock::time_point now) const noexcept;
    bool active() const noexcept { return identity_.has_value(); }
    std::optional<AdministratorTerminalIdentity> identity() const noexcept {
        return identity_;
    }
    void revoke() noexcept;

private:
    std::optional<AdministratorTerminalIdentity> identity_;
    std::chrono::steady_clock::time_point expiresAt_{};
};

struct AdministratorAuditStoreConfig {
    std::string path{"/var/lib/lcl-security/admin-audit.v1"};
    uid_t ownerUid{0};
    gid_t ownerGid{0};
};

/** Root-owned append-only audit for brokered Terminal commands. */
class AdministratorAuditStore final {
public:
    explicit AdministratorAuditStore(AdministratorAuditStoreConfig config = {});
    bool append(std::string_view event, uid_t userUid,
                AdministratorTerminalIdentity terminal,
                std::uint32_t requestId,
                const std::vector<std::string>& arguments,
                bool accepted, std::string_view result,
                std::string& error) const;

private:
    AdministratorAuditStoreConfig config_;
};

struct AdministratorDaemonConfig {
    std::string socketPath{kAdministratorSocket};
    uid_t sessionUid{kSessionUserUid};
    gid_t sessionGid{kSessionUserGid};
    AdministratorAuditStoreConfig audit{};
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
        AdministratorTerminalIdentity terminal;
    };
    struct PendingCommand {
        int clientDescriptor{-1};
        std::uint32_t requestId{0};
        AdministratorExecuteRequest request;
        AdministratorTerminalIdentity terminal;
    };
    struct RunningCommand {
        int clientDescriptor{-1};
        std::uint32_t requestId{0};
        pid_t pid{-1};
        int stdoutDescriptor{-1};
        int stderrDescriptor{-1};
        bool childExited{false};
        int exitCode{125};
        AdministratorTerminalIdentity terminal;
        std::vector<std::string> arguments;
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
    bool terminalIsAlive(AdministratorTerminalIdentity terminal) const;
    void pruneCachedPermission();
    void revokeCachedPermission(std::string_view reason);
    void revokeTransientAuthority(std::string_view reason);
    bool audit(std::string_view event, AdministratorTerminalIdentity terminal,
               std::uint32_t requestId,
               const std::vector<std::string>& arguments,
               bool accepted, std::string_view result,
               std::string& error) const;

    AdministratorDaemonConfig config_;
    int serverDescriptor_{-1};
    int shellDescriptor_{-1};
    bool ownsSocketPath_{false};
    std::vector<Client> clients_;
    std::optional<PendingCommand> pending_;
    std::optional<RunningCommand> running_;
    AdministratorPermissionLease cachedPermission_;
    std::uint32_t cachedRequestId_{0};
    std::vector<std::string> cachedArguments_;
    AdministratorAuditStore audit_;
};

} // namespace lcl::security
