#pragma once

#include <cstddef>
#include <string>
#include <sys/types.h>
#include <vector>

#include "system/security/sandbox_launch_authorizer.hpp"
#include "system/security/sandbox_protocol.hpp"

namespace lcl::security {

inline constexpr const char* kSandboxSocket = "/Runtime/lcl-sandboxd.sock";

/**
 * Root-owned sandboxd endpoint configuration.
 *
 * The permitted peer must be a dedicated, unprivileged session-service
 * identity. It deliberately cannot be UID 0 or the interactive desktop user:
 * otherwise any process sharing that identity could request application
 * launch. The socket is chowned 0600 to that identity and every accepted
 * connection is checked again with SO_PEERCRED.
 */
struct SandboxDaemonConfig {
    std::string socketPath{kSandboxSocket};
    uid_t sessionUid{0};
    gid_t sessionGid{0};
    uid_t ownerUid{0};
    gid_t ownerGid{0};
};

/**
 * Small root sandbox control plane.
 *
 * Registration is an in-process, trusted verifier/registry operation only;
 * there is intentionally no wire opcode that can add an app, executable path,
 * UID/GID, mount, environment or capability request. A session client can
 * submit only SandboxLaunchRequest, which is reconstructed from the protected
 * verified record by SandboxLaunchAuthorizer.
 *
 * This initial service deliberately does not launch before a child-side kernel
 * setup implementation (namespaces, seccomp, Landlock and device policy) is
 * installed. Therefore an otherwise authorized request returns SetupFailed
 * rather than falling back to sessiond's old direct exec path.
 */
class SandboxDaemon final {
public:
    explicit SandboxDaemon(SandboxDaemonConfig config);
    ~SandboxDaemon();

    SandboxDaemon(const SandboxDaemon&) = delete;
    SandboxDaemon& operator=(const SandboxDaemon&) = delete;

    /** Opens the root-owned server socket. Must be called by root. */
    bool initialize(std::string& error);
    void shutdown();
    void poll();

    /** Trusted, in-process verifier/registry API; never exposed over IPC. */
    bool registerVerifiedApplication(const VerifiedApplication& application,
                                     const std::vector<std::string>& grantedPermissions,
                                     std::string& error);
    void removeVerifiedApplication(const std::string& appId);
    std::size_t registeredApplicationCount() const;

    /** Handles only an already-decoded narrow request; useful to trusted tests. */
    SandboxLaunchResult handleLaunchRequest(const SandboxLaunchRequest& request) const;

private:
    bool validateConfig(std::string& error) const;
    bool validateSocketParent(std::string& error) const;
    bool peerIsAuthorized(int descriptor) const;
    bool sendPacket(int descriptor, SandboxOpcode opcode, std::uint32_t requestId,
                    const std::vector<std::uint8_t>& payload);
    void acceptConnections();
    void serviceClient(int descriptor);
    void removeClient(int descriptor);
    void sendErrorAndClose(int descriptor, std::uint32_t requestId, const std::string& message);

    SandboxDaemonConfig config_;
    SandboxLaunchAuthorizer authorizer_;
    int serverDescriptor_{-1};
    bool ownsSocketPath_{false};
    std::vector<int> clientDescriptors_;
};

} // namespace lcl::security
