#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>

namespace lcl::security {

struct AppLaunchIdentity {
    std::uint64_t instanceId{0};
    std::string appId;
    pid_t processGroupId{0};
    uid_t uid{0};
    gid_t gid{0};
};

struct AppLaunchRegistryConfig {
    std::string directoryPath{"/var/lib/lcl-security/app-launches"};
    uid_t firstAppUid{61000};
    uid_t lastAppUid{61999};
    uid_t ownerUid{0};
    gid_t ownerGid{0};
};

/** Root-owned live launch records. Cleared whenever sandboxd starts. */
class AppLaunchRegistry final {
public:
    explicit AppLaunchRegistry(AppLaunchRegistryConfig config = {});

    bool initializeAndReset(std::string& error) const;
    bool registerLaunch(const AppLaunchIdentity& identity, std::string& error) const;
    std::optional<AppLaunchIdentity> find(std::uint64_t instanceId,
                                          std::string& error) const;
    bool remove(std::uint64_t instanceId, std::string& error) const;

private:
    bool validateDirectory(std::string& error) const;
    std::string recordPath(std::uint64_t instanceId) const;

    AppLaunchRegistryConfig config_;
};

} // namespace lcl::security
