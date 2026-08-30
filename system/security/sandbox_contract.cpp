#include "system/security/sandbox_contract.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace lcl::security {
namespace {

constexpr std::uint32_t kDefaultMemoryMaxMiB = 512;
constexpr std::uint32_t kDefaultPidsMax = 64;

void appendU32(std::string& output, std::uint32_t value) {
    for (int index = 3; index >= 0; --index) {
        output.push_back(static_cast<char>(value >> (index * 8)));
    }
}

void appendString(std::string& output, std::string_view value) {
    appendU32(output, static_cast<std::uint32_t>(value.size()));
    output.append(value);
}

void appendBoolean(std::string& output, bool value) {
    output.push_back(value ? '\x01' : '\x00');
}

bool containsPermission(const std::vector<std::string>& permissions, std::string_view permission) {
    return std::binary_search(permissions.begin(), permissions.end(), std::string(permission));
}

bool normalizePermissions(const std::vector<std::string>& input, std::vector<std::string>& output,
                          std::string& error) {
    output = input;
    std::sort(output.begin(), output.end());
    for (std::size_t index = 0; index < output.size(); ++index) {
        if (!AppIdentityRegistry::isValidAppId(output[index])) {
            error = "sandbox profile contains an invalid permission identifier";
            return false;
        }
        if (index > 0 && output[index - 1] == output[index]) {
            error = "sandbox profile contains a duplicate permission identifier";
            return false;
        }
    }
    return true;
}

bool isProfileIdForRuntime(const std::string& profileId, SandboxRuntime runtime) {
    switch (runtime) {
        case SandboxRuntime::Native:
            return profileId == "lcl.third-party.native.v1";
        case SandboxRuntime::JavaScript:
            return profileId == "lcl.third-party.javascript.v1";
    }
    return false;
}

} // namespace

bool validateSandboxProfile(const SandboxProfile& profile, std::string& error) {
    error.clear();
    if (profile.contractVersion != kSandboxContractVersion ||
        !isProfileIdForRuntime(profile.profileId, profile.runtime)) {
        error = "sandbox profile has an unknown ABI or profile ID";
        return false;
    }
    if (!profile.privateMountNamespace || !profile.privatePidNamespace ||
        !profile.privateIpcNamespace || !profile.noNewPrivileges || !profile.requireSeccomp ||
        !profile.requireLandlock || !profile.denyDirectDeviceAccess || profile.memoryMaxMiB == 0 ||
        profile.pidsMax == 0) {
        error = "third-party sandbox profile weakens a mandatory security boundary";
        return false;
    }
    if (profile.allowNetworkClient && !profile.privateNetworkNamespace) {
        error = "network-capable profile must keep an isolated network namespace";
        return false;
    }

    std::vector<std::string> normalized;
    if (!normalizePermissions(profile.effectivePermissions, normalized, error) ||
        normalized != profile.effectivePermissions) {
        if (error.empty()) {
            error = "sandbox profile permissions are not canonical";
        }
        return false;
    }
    if (profile.allowNetworkClient != containsPermission(profile.effectivePermissions, "network.client")) {
        error = "network profile does not match effective permissions";
        return false;
    }
    return true;
}

Sha256Digest digestSandboxProfile(const SandboxProfile& profile) {
    std::string canonical;
    canonical.reserve(128 + profile.profileId.size() + profile.effectivePermissions.size() * 32);
    canonical.append("LCL_SANDBOX_PROFILE_V1", 22);
    appendU32(canonical, profile.contractVersion);
    appendString(canonical, profile.profileId);
    canonical.push_back(static_cast<char>(profile.runtime));
    appendBoolean(canonical, profile.privateMountNamespace);
    appendBoolean(canonical, profile.privatePidNamespace);
    appendBoolean(canonical, profile.privateIpcNamespace);
    appendBoolean(canonical, profile.privateNetworkNamespace);
    appendBoolean(canonical, profile.noNewPrivileges);
    appendBoolean(canonical, profile.requireSeccomp);
    appendBoolean(canonical, profile.requireLandlock);
    appendBoolean(canonical, profile.denyDirectDeviceAccess);
    appendBoolean(canonical, profile.allowNetworkClient);
    appendU32(canonical, profile.memoryMaxMiB);
    appendU32(canonical, profile.pidsMax);
    appendU32(canonical, static_cast<std::uint32_t>(profile.effectivePermissions.size()));
    for (const std::string& permission : profile.effectivePermissions) {
        appendString(canonical, permission);
    }
    return sha256(canonical);
}

bool validateSandboxLaunchRequest(const SandboxLaunchRequest& request, std::string& error) {
    error.clear();
    if (request.contractVersion != kSandboxContractVersion ||
        !AppIdentityRegistry::isValidAppId(request.appId) || request.instanceId == 0 ||
        isZeroDigest(request.bundleRecordDigest) || isZeroDigest(request.profileDigest)) {
        error = "sandbox launch request is incomplete or invalid";
        return false;
    }
    return true;
}

std::optional<SandboxLaunchPlan> makeThirdPartySandboxLaunchPlan(
    const VerifiedApplication& application,
    const std::vector<std::string>& grantedPermissions,
    std::uint64_t instanceId,
    std::string& error) {
    error.clear();
    if (!AppIdentityRegistry::isValidAppId(application.appId) ||
        application.identity.appId != application.appId || application.identity.uid == 0 ||
        application.identity.gid == 0 || application.identity.uid != application.identity.gid ||
        isZeroDigest(application.bundleRecordDigest) || instanceId == 0) {
        error = "verified application has invalid identity or bundle digest";
        return std::nullopt;
    }

    std::vector<std::string> requested;
    std::vector<std::string> granted;
    if (!normalizePermissions(application.requestedPermissions, requested, error) ||
        !normalizePermissions(grantedPermissions, granted, error)) {
        return std::nullopt;
    }
    for (const std::string& permission : granted) {
        if (!containsPermission(requested, permission)) {
            error = "sandbox policy tried to grant a permission absent from the manifest";
            return std::nullopt;
        }
    }

    SandboxProfile profile{};
    profile.runtime = application.runtime;
    profile.profileId = application.runtime == SandboxRuntime::JavaScript
        ? "lcl.third-party.javascript.v1"
        : "lcl.third-party.native.v1";
    profile.privateNetworkNamespace = true;
    profile.allowNetworkClient = containsPermission(granted, "network.client");
    profile.memoryMaxMiB = kDefaultMemoryMaxMiB;
    profile.pidsMax = kDefaultPidsMax;
    profile.effectivePermissions = std::move(granted);
    if (!validateSandboxProfile(profile, error)) {
        return std::nullopt;
    }

    SandboxLaunchRequest request{};
    request.appId = application.appId;
    request.instanceId = instanceId;
    request.bundleRecordDigest = application.bundleRecordDigest;
    request.profileDigest = digestSandboxProfile(profile);
    if (!validateSandboxLaunchRequest(request, error)) {
        return std::nullopt;
    }
    return SandboxLaunchPlan{std::move(profile), std::move(request)};
}

} // namespace lcl::security
