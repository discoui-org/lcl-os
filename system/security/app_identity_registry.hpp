#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <sys/types.h>

namespace lcl::security {

struct AppIdentity {
    std::string appId;
    uid_t uid{0};
    gid_t gid{0};
};

/**
 * The privileged service chooses this range after platform policy validation.
 * Keeping it explicit prevents the canonical userspace ABI from assuming an
 * Android AID range.
 */
struct AppIdentityRegistryConfig {
    std::string registryPath;
    uid_t firstAppUid{0};
    uid_t lastAppUid{0};
    uid_t ownerUid{0};
    gid_t ownerGid{0};
};

/**
 * Root-owned, atomically updated app-id to uid/gid mapping.
 *
 * This class is for sandboxd/installer only. Session and application processes
 * must never receive write access to its directory or registry file.
 */
class AppIdentityRegistry final {
public:
    explicit AppIdentityRegistry(AppIdentityRegistryConfig config);

    AppIdentityRegistry(const AppIdentityRegistry&) = delete;
    AppIdentityRegistry& operator=(const AppIdentityRegistry&) = delete;

    /** Reloads and validates the root-owned mapping from disk. */
    bool load(std::string& error);

    /** Returns an existing mapping or atomically allocates and persists one. */
    std::optional<AppIdentity> getOrCreate(const std::string& appId, std::string& error);

    std::optional<AppIdentity> find(const std::string& appId) const;
    std::size_t size() const noexcept { return identities_.size(); }

    static bool isValidAppId(const std::string& appId);

private:
    bool loadUnlocked(std::string& error);
    bool persistUnlocked(std::string& error) const;
    bool validateConfig(std::string& error) const;
    bool validateRegistryParent(std::string& error) const;
    std::optional<uid_t> allocateUid() const;

    AppIdentityRegistryConfig config_;
    std::map<std::string, AppIdentity> identities_;
};

} // namespace lcl::security
