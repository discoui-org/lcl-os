#include "system/security/sandbox_child_reaper.hpp"

#include <signal.h>
#include <sys/wait.h>

#include <cerrno>
#include <utility>

namespace lcl::security {
namespace {

std::int32_t exitCodeFromStatus(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

} // namespace

bool SandboxChildReaper::track(const std::string& appId, const SandboxLaunchResult& launch,
                               std::string& error) {
    return track(appId, launch, SandboxCgroup{}, error);
}

bool SandboxChildReaper::track(const std::string& appId, const SandboxLaunchResult& launch,
                               const SandboxCgroup& cgroup, std::string& error) {
    error.clear();
    if (!AppIdentityRegistry::isValidAppId(appId) || launch.status != SandboxLaunchStatus::Launched ||
        launch.instanceId == 0 || launch.pid <= 0 || launch.processGroupId <= 0 ||
        launch.pid != launch.processGroupId) {
        error = "sandbox child reaper received invalid launch ownership";
        return false;
    }

    std::lock_guard lock(mutex_);
    if (byInstance_.contains(launch.instanceId) || instanceByPid_.contains(launch.pid)) {
        error = "sandbox child is already tracked";
        return false;
    }
    TrackedChild child{};
    child.instanceId = launch.instanceId;
    child.appId = appId;
    child.pid = launch.pid;
    child.processGroupId = launch.processGroupId;
    if (cgroup.instanceId != 0 || !cgroup.path.empty()) {
        if (cgroup.instanceId != launch.instanceId || cgroup.path.empty()) {
            error = "sandbox child reaper received invalid cgroup ownership";
            return false;
        }
        child.cgroup = cgroup;
    }
    instanceByPid_.emplace(child.pid, child.instanceId);
    byInstance_.emplace(child.instanceId, std::move(child));
    return true;
}

std::vector<SandboxChildExit> SandboxChildReaper::reap() {
    std::vector<TrackedChild> children;
    {
        std::lock_guard lock(mutex_);
        children.reserve(byInstance_.size());
        for (const auto& [_, child] : byInstance_) {
            children.push_back(child);
        }
    }

    std::vector<SandboxChildExit> exited;
    for (const TrackedChild& child : children) {
        int status = 0;
        pid_t result = 0;
        do {
            result = waitpid(child.pid, &status, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == 0) {
            continue;
        }
        if (result < 0 && errno != ECHILD) {
            continue;
        }

        std::lock_guard lock(mutex_);
        const auto current = byInstance_.find(child.instanceId);
        if (current == byInstance_.end() || current->second.pid != child.pid) {
            continue;
        }
        const TrackedChild finished = current->second;
        byInstance_.erase(current);
        instanceByPid_.erase(finished.pid);
        // ECHILD is not a normal exit: sandboxd was expected to be the only
        // waiter. Surface it as a failed instance rather than silently losing
        // lifecycle authority.
        exited.push_back({finished.instanceId, finished.appId, finished.pid,
                          finished.processGroupId, result == -1 ? 1 : exitCodeFromStatus(status),
                          finished.cgroup});
    }
    return exited;
}

void SandboxChildReaper::terminateAll(int signalNumber) {
    std::vector<std::int32_t> processGroups;
    {
        std::lock_guard lock(mutex_);
        processGroups.reserve(byInstance_.size());
        for (const auto& [_, child] : byInstance_) {
            processGroups.push_back(child.processGroupId);
        }
    }
    for (const std::int32_t processGroupId : processGroups) {
        if (processGroupId > 0) {
            kill(-processGroupId, signalNumber);
        }
    }
}

std::size_t SandboxChildReaper::size() const {
    std::lock_guard lock(mutex_);
    return byInstance_.size();
}

} // namespace lcl::security
