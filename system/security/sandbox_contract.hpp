#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "system/security/app_identity_registry.hpp"
#include "system/security/permission_store.hpp"
#include "system/security/sha256.hpp"

namespace lcl::security {

inline constexpr std::uint32_t kSandboxContractVersion = 2;

enum class SandboxRuntime : std::uint8_t {
    Native = 1,
    JavaScript = 2,
};

enum class SandboxLaunchStatus : std::uint32_t {
    Launched = 0,
    InvalidRequest = 1,
    PolicyRejected = 2,
    SetupFailed = 3,
    ExecutionFailed = 4,
};

/**
 * Immutable, platform-independent LCL capability contract.
 *
 * This is the only profile which crosses the sessiond -> sandboxd protocol
 * boundary.  It deliberately says nothing about Linux-specific mechanisms
 * such as a PID namespace, Landlock, or cgroup v2.  Those mechanisms are
 * sandboxd implementation details below the common LCL userspace ABI.
 *
 * `network.client` remains a broker permission: it must never mean that an
 * app receives the host network namespace or a direct network capability.
 */
struct SandboxProfile {
    std::uint32_t contractVersion{kSandboxContractVersion};
    std::string profileId;
    SandboxRuntime runtime{SandboxRuntime::Native};
    bool noNewPrivileges{true};
    bool requireSeccomp{true};
    /** The app receives a private mounted filesystem view, never the host root. */
    bool privateFilesystemView{true};
    /** The app receives no host network interfaces or routes. */
    bool isolatedNetworkView{true};
    bool denyDirectDeviceAccess{true};
    /** Only declared and currently granted LCL capabilities. */
    std::vector<std::string> effectivePermissions;
};

/**
 * Daemon-owned resource limits.  They are not part of the application ABI or
 * its capability digest: Linux may use cgroup v2 while Android applies the
 * portable rlimit subset before dropping the app UID.
 */
struct SandboxResourceLimits {
    std::uint32_t memoryMaxMiB{512};
    std::uint32_t pidsMax{64};
    /** cgroup v2 cpu.weight when the platform provides it. */
    std::uint32_t cpuWeight{100};
};

/**
 * Platform-local enforcement selected by root sandboxd after capability
 * negotiation.  This object is never serialized in a launch request.
 */
struct SandboxPlatformHardening {
    bool privateMountNamespace{true};
    bool privatePidNamespace{true};
    bool privateIpcNamespace{true};
    bool privateNetworkNamespace{true};
    bool requireLandlock{true};
    bool requireCgroupResourceAccounting{true};
    bool requirePortableResourceLimits{true};
    SandboxResourceLimits resourceLimits{};
};

Sha256Digest digestSandboxProfile(const SandboxProfile& profile);
bool validateSandboxProfile(const SandboxProfile& profile, std::string& error);
bool validateSandboxPlatformHardening(const SandboxPlatformHardening& hardening,
                                      std::string& error);

/**
 * Internal output of the verifier + identity registry. It is not IPC input:
 * only trusted sessiond/sandboxd code can construct it.
 */
struct VerifiedApplication {
    std::string appId;
    AppIdentity identity;
    SandboxRuntime runtime{SandboxRuntime::Native};
    std::vector<std::string> requestedPermissions;
    Sha256Digest bundleRecordDigest{};
    /** Present only when rootfs policy owns the live permission decisions. */
    std::optional<PermissionSubject> permissionSubject;
};

/**
 * The only launch data sandboxd receives. It deliberately has no executable
 * path, uid/gid, mount path, capability mask, command-line arguments or
 * environment. Sandboxd resolves each of those from its protected registry.
 */
struct SandboxLaunchRequest {
    std::uint32_t contractVersion{kSandboxContractVersion};
    std::string appId;
    std::uint64_t instanceId{0};
    Sha256Digest bundleRecordDigest{};
    Sha256Digest profileDigest{};
};

struct SandboxLaunchResult {
    SandboxLaunchStatus status{SandboxLaunchStatus::InvalidRequest};
    std::uint64_t instanceId{0};
    std::int32_t pid{0};
    std::int32_t processGroupId{0};
    std::string message;
};

struct SandboxLaunchPlan {
    SandboxProfile profile;
    SandboxLaunchRequest request;
};

bool validateSandboxLaunchRequest(const SandboxLaunchRequest& request, std::string& error);

/** Builds a policy profile without exposing app identity or execution material. */
std::optional<SandboxProfile> makeThirdPartySandboxProfile(
    SandboxRuntime runtime,
    const std::vector<std::string>& requestedPermissions,
    const std::vector<std::string>& grantedPermissions,
    std::string& error);

/** Builds a default-deny third-party profile from verified, trusted inputs. */
std::optional<SandboxLaunchPlan> makeThirdPartySandboxLaunchPlan(
    const VerifiedApplication& application,
    const std::vector<std::string>& grantedPermissions,
    std::uint64_t instanceId,
    std::string& error);

} // namespace lcl::security
