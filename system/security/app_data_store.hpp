#pragma once

#include <string>
#include <sys/types.h>

#include "system/security/app_identity_registry.hpp"

namespace lcl::security {

struct AppPersistentDirectories {
    std::string container;
    std::string data;
    std::string cache;
    std::string preferences;
};

struct AppDataStoreConfig {
    std::string containersRoot;
    uid_t ownerUid{0};
    gid_t ownerGid{0};
};

/**
 * Privileged installer/sandboxd helper for an app's persistent storage.
 *
 * It deliberately has no Temporary directory: every sandbox instance creates
 * that directory as a tmpfs mount, so it cannot retain data across launches.
 */
class AppDataStore final {
public:
    explicit AppDataStore(AppDataStoreConfig config);

    AppDataStore(const AppDataStore&) = delete;
    AppDataStore& operator=(const AppDataStore&) = delete;

    /** Creates or repairs the app-owned 0700 persistent directories. */
    bool ensurePersistentDirectories(const AppIdentity& identity,
                                     AppPersistentDirectories& directories,
                                     std::string& error) const;

private:
    bool validateContainersRoot(std::string& error) const;

    AppDataStoreConfig config_;
};

} // namespace lcl::security
