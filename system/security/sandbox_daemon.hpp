#pragma once

#include <cstddef>
#include <string>
#include <sys/types.h>
#include <vector>

#include "system/security/sandbox_child_reaper.hpp"
#include "system/security/sandbox_cgroup.hpp"
#include "system/security/sandbox_external_application_registry.hpp"
#include "system/security/sandbox_launch_authorizer.hpp"
#include "system/security/sandbox_launch_material.hpp"
#include "system/security/sandbox_platform_probe.hpp"
#include "system/security/sandbox_protocol.hpp"

namespace lcl::security {

/**
 * Root-owned sandboxd endpoint configuration.
 *
 * The permitted peer is either the root-owned session authority used by the
 * current canonical boot chain or a future dedicated non-interactive service
 * identity. The interactive session UID is never an accepted peer. The
 * socket is chowned 0600 to that authority and every connection is checked
 * again with SO_PEERCRED.
 */
struct SandboxDaemonConfig {
    std::string socketPath{kSandboxSocket};
    uid_t sessionUid{0};
    gid_t sessionGid{0};
    uid_t ownerUid{0};
    gid_t ownerGid{0};
    SandboxPlatformMode platformMode{SandboxPlatformMode::LinuxFull};
    PermissionStoreConfig permissionStore{};
    SandboxExternalApplicationRegistryConfig externalApplications{};
};

/**
 * Small root sandbox control plane.
 *
 * System-image registration is an in-process verifier/registry operation.
 * The only registration wire operation is restricted by SO_PEERCRED to the
 * root session authority and accepts exactly one root-owned immutable bundle
 * snapshot plus its executable descriptor. It cannot carry an app-selected
 * UID/GID, mount, environment, capability grant, argument or runtime path.
 * A normal application can submit neither registration nor anything beyond a
 * narrow SandboxLaunchRequest, which sandboxd reconstructs from protected
 * verified material.
 *
 * An authorized request launches only after sandboxd has a verifier-retained
 * descriptor snapshot for that exact bundle record.  The daemon creates the
 * mandatory cgroup, then applies namespaces, private mounts, Landlock,
 * credential drop and seccomp in its child path.  It never falls back to
 * sessiond's historical direct exec path.
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
    bool registerVerifiedLaunchMaterial(const SandboxLaunchMaterialInput& input, std::string& error);
    /** Root-sessiond-only snapshot registration path; normal apps never reach it. */
    bool registerExternalApplication(const SandboxApplicationRegistration& registration,
                                     int appBundleDescriptor, int executableDescriptor,
                                     std::string& error);
    void removeVerifiedApplication(const std::string& appId);
    bool hasRegisteredApplication(const std::string& appId) const;
    std::size_t registeredApplicationCount() const;

    /** Trusted child-launch path records only a successful daemon-owned child. */
    bool recordLaunchedChild(const std::string& appId, const SandboxLaunchResult& launch,
                             std::string& error);
    bool recordLaunchedChild(const std::string& appId, const SandboxLaunchResult& launch,
                             const SandboxCgroup& cgroup, std::string& error);
    std::size_t runningChildCount() const;

    /** Handles only an already-decoded narrow request; useful to trusted tests. */
    SandboxLaunchResult handleLaunchRequest(const SandboxLaunchRequest& request);

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
    void reapChildren();

    SandboxLaunchAuthorizer authorizer_;
    SandboxExternalApplicationRegistry externalApplications_;
    SandboxDaemonConfig config_;
    SandboxLaunchMaterialRegistry materialRegistry_;
    // The daemon owns the only cgroup v2 allocator.  It is initialized before
    // the launch socket becomes reachable, so no request can observe a daemon
    // that would fall back to unconstrained process creation on Linux.
    SandboxCgroupManager cgroupManager_;
    /** Selected locally after probing the actual kernel; never protocol input. */
    SandboxPlatformHardening hardening_{};
    bool hardeningReady_{false};
    SandboxChildReaper childReaper_;
    int serverDescriptor_{-1};
    bool ownsSocketPath_{false};
    std::vector<int> clientDescriptors_;
};

} // namespace lcl::security
