#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>

#include "system/security/sandbox_contract.hpp"

namespace lcl::security {

/** Root-owned cgroup v2 location allocated for exactly one app instance. */
struct SandboxCgroup {
    std::uint64_t instanceId{0};
    std::string path;
};

/**
 * Applies daemon-owned platform resource limits to cgroup v2. No
 * application-controlled path or limit enters this API. The sandboxd launch
 * path creates an instance cgroup, moves its child into it before exec, and
 * removes it only after the reaper observed process exit.
 */
class SandboxCgroupManager final {
public:
    explicit SandboxCgroupManager(std::string rootPath = "/sys/fs/cgroup/lcl/apps");

    SandboxCgroupManager(const SandboxCgroupManager&) = delete;
    SandboxCgroupManager& operator=(const SandboxCgroupManager&) = delete;

    bool initialize(std::string& error);
    std::optional<SandboxCgroup> createInstance(const SandboxLaunchPlan& plan,
                                                 const SandboxPlatformHardening& hardening,
                                                 std::string& error);
    bool moveProcess(const SandboxCgroup& cgroup, pid_t processId, std::string& error) const;
    bool removeInstance(const SandboxCgroup& cgroup, std::string& error) const;

private:
    bool isSafeInstancePath(const SandboxCgroup& cgroup) const;

    std::string rootPath_;
    bool initialized_{false};
};

} // namespace lcl::security
