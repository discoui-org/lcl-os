#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "system/security/sandbox_contract.hpp"

namespace lcl::security {

struct SandboxChildExit {
    std::uint64_t instanceId{0};
    std::string appId;
    std::int32_t pid{0};
    std::int32_t processGroupId{0};
    std::int32_t exitCode{0};
};

/**
 * sandboxd's child ownership table.
 *
 * Only a successful, protected SandboxLaunchResult may be registered. Every
 * tracked app is required to lead its own process group, allowing orderly
 * daemon shutdown without signalling unrelated session processes. Reaping is
 * per-known PID rather than waitpid(-1), so sandboxd cannot consume a child
 * owned by another future daemon responsibility.
 */
class SandboxChildReaper final {
public:
    SandboxChildReaper() = default;

    SandboxChildReaper(const SandboxChildReaper&) = delete;
    SandboxChildReaper& operator=(const SandboxChildReaper&) = delete;

    bool track(const std::string& appId, const SandboxLaunchResult& launch, std::string& error);
    std::vector<SandboxChildExit> reap();
    void terminateAll(int signalNumber);
    std::size_t size() const;

private:
    struct TrackedChild {
        std::uint64_t instanceId{0};
        std::string appId;
        std::int32_t pid{0};
        std::int32_t processGroupId{0};
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, TrackedChild> byInstance_;
    std::unordered_map<std::int32_t, std::uint64_t> instanceByPid_;
};

} // namespace lcl::security
