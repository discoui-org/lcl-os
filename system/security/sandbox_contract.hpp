#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "system/security/app_identity_registry.hpp"
#include "system/security/permission_store.hpp"
#include "system/security/sha256.hpp"

namespace lcl::security {

inline constexpr std::uint32_t kSandboxContractVersion = 1;

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
 * Immutable kernel-enforcement intent. The platform sandboxd translates this
 * ABI into namespaces, credentials, seccomp, Landlock and cgroup calls.
 */
struct SandboxProfile {
    std::uint32_t contractVersion{kSandboxContractVersion};
    std::string profileId;
    SandboxRuntime runtime{SandboxRuntime::Native};
    bool privateMountNamespace{true};
    bool privatePidNamespace{true};
    bool privateIpcNamespace{true};
    bool privateNetworkNamespace{true};
    bool noNewPrivileges{true};
    bool requireSeccomp{true};
    bool requireLandlock{true};
    bool denyDirectDeviceAccess{true};
    bool allowNetworkClient{false};
    std::uint32_t memoryMaxMiB{512};
    std::uint32_t pidsMax{64};
    /** cgroup v2 cpu.weight; Linux permits values in the inclusive 1..10000 range. */
    std::uint32_t cpuWeight{100};
    std::vector<std::string> effectivePermissions;
};

Sha256Digest digestSandboxProfile(const SandboxProfile& profile);
bool validateSandboxProfile(const SandboxProfile& profile, std::string& error);

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
