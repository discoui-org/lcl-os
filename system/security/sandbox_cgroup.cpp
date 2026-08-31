#include "system/security/sandbox_cgroup.hpp"

#include <fcntl.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string_view>
#include <utility>

namespace lcl::security {
namespace {

constexpr const char* kCgroupMountPath = "/sys/fs/cgroup";
constexpr const char* kControllerRequest = "+memory +pids +cpu";

bool writeAll(int descriptor, std::string_view value) {
    std::size_t offset = 0;
    while (offset < value.size()) {
        const ssize_t count = write(descriptor, value.data() + offset, value.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool writeControlFile(const std::string& path, std::string_view value, std::string& error) {
    const int descriptor = open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0 || !writeAll(descriptor, value)) {
        const int savedErrno = errno;
        if (descriptor >= 0) close(descriptor);
        error = std::string("could not write cgroup control '") + path + "': " +
                std::strerror(savedErrno);
        return false;
    }
    close(descriptor);
    return true;
}

bool readControlFile(const std::string& path, std::string& output, std::string& error) {
    output.clear();
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        error = std::string("could not read cgroup control '") + path + "': " +
                std::strerror(errno);
        return false;
    }
    std::array<char, 256> buffer{};
    while (true) {
        const ssize_t count = read(descriptor, buffer.data(), buffer.size());
        if (count > 0) {
            output.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            close(descriptor);
            return true;
        }
        if (errno == EINTR) continue;
        const int savedErrno = errno;
        close(descriptor);
        error = std::string("could not read cgroup control '") + path + "': " +
                std::strerror(savedErrno);
        return false;
    }
}

bool containsController(const std::string& controls, const char* required) {
    std::istringstream input(controls);
    std::string controller;
    while (input >> controller) {
        if (controller == required) return true;
    }
    return false;
}

bool ensureRootOwnedDirectory(const std::string& path, std::string& error) {
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        error = std::string("could not create sandbox cgroup directory '") + path + "': " +
                std::strerror(errno);
        return false;
    }
    struct stat status {};
    if (lstat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode) ||
        status.st_uid != 0 || status.st_gid != 0 || (status.st_mode & 0022) != 0) {
        error = std::string("sandbox cgroup directory is unsafe: ") + path;
        return false;
    }
    return true;
}

bool enableControllers(const std::string& path, std::string& error) {
    const std::string subtreeControl = path + "/cgroup.subtree_control";
    std::string current;
    if (!readControlFile(subtreeControl, current, error)) return false;
    if (!containsController(current, "memory") || !containsController(current, "pids") ||
        !containsController(current, "cpu")) {
        if (!writeControlFile(subtreeControl, kControllerRequest, error)) return false;
        if (!readControlFile(subtreeControl, current, error)) return false;
    }
    if (!containsController(current, "memory") || !containsController(current, "pids") ||
        !containsController(current, "cpu")) {
        error = "cgroup v2 does not expose the required memory, pids, and cpu controllers";
        return false;
    }
    return true;
}

bool isSandboxInstanceName(std::string_view name) {
    constexpr std::string_view kPrefix = "instance-";
    if (!name.starts_with(kPrefix) || name.size() == kPrefix.size()) {
        return false;
    }
    std::uint64_t instanceId = 0;
    const char* const first = name.data() + kPrefix.size();
    const char* const last = name.data() + name.size();
    const auto [end, parseError] = std::from_chars(first, last, instanceId);
    return parseError == std::errc{} && end == last && instanceId != 0;
}

bool removeStaleInstanceCgroups(const std::string& rootPath, std::string& error) {
    std::error_code iteratorError;
    std::filesystem::directory_iterator iterator(rootPath, iteratorError);
    const std::filesystem::directory_iterator end;
    for (; !iteratorError && iterator != end; iterator.increment(iteratorError)) {
        const std::filesystem::directory_entry& entry = *iterator;
        if (iteratorError) {
            error = "could not enumerate sandbox cgroup hierarchy: " + iteratorError.message();
            return false;
        }
        std::error_code typeError;
        if (!entry.is_directory(typeError)) {
            if (typeError) {
                error = "could not inspect a sandbox cgroup hierarchy entry: " + typeError.message();
                return false;
            }
            continue;
        }

        const std::string name = entry.path().filename().string();
        struct stat status {};
        if (!isSandboxInstanceName(name) || lstat(entry.path().c_str(), &status) != 0 ||
            !S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode) || status.st_uid != 0 ||
            status.st_gid != 0 || (status.st_mode & 0022) != 0) {
            error = "sandbox cgroup hierarchy contains an unsafe stale entry";
            return false;
        }
        if (rmdir(entry.path().c_str()) != 0) {
            error = std::string("sandboxd cannot recover a stale app cgroup '") + name +
                    "': " + std::strerror(errno);
            return false;
        }
    }
    if (iteratorError) {
        error = "could not enumerate sandbox cgroup hierarchy: " + iteratorError.message();
        return false;
    }
    return true;
}

} // namespace

SandboxCgroupManager::SandboxCgroupManager(std::string rootPath) : rootPath_(std::move(rootPath)) {}

bool SandboxCgroupManager::initialize(std::string& error) {
    error.clear();
    if (initialized_) return true;
    if (geteuid() != 0) {
        error = "sandbox cgroup manager requires root sandboxd";
        return false;
    }
    const std::filesystem::path root(rootPath_);
    if (!root.is_absolute() || root.lexically_normal() != root ||
        rootPath_.rfind(std::string(kCgroupMountPath) + '/', 0) != 0) {
        error = "sandbox cgroup root must be a normalized path below /sys/fs/cgroup";
        return false;
    }
    struct statfs filesystemStatus {};
    if (statfs(kCgroupMountPath, &filesystemStatus) != 0 ||
        static_cast<unsigned long>(filesystemStatus.f_type) != CGROUP2_SUPER_MAGIC) {
        error = "cgroup v2 is not mounted at /sys/fs/cgroup";
        return false;
    }
    const std::string lclRoot = std::string(kCgroupMountPath) + "/lcl";
    if (!enableControllers(kCgroupMountPath, error) || !ensureRootOwnedDirectory(lclRoot, error) ||
        !enableControllers(lclRoot, error) || !ensureRootOwnedDirectory(rootPath_, error) ||
        !enableControllers(rootPath_, error) || !removeStaleInstanceCgroups(rootPath_, error)) {
        return false;
    }
    initialized_ = true;
    return true;
}

bool SandboxCgroupManager::isSafeInstancePath(const SandboxCgroup& cgroup) const {
    const std::string prefix = rootPath_ + "/instance-";
    return cgroup.instanceId != 0 && cgroup.path == prefix + std::to_string(cgroup.instanceId);
}

std::optional<SandboxCgroup> SandboxCgroupManager::createInstance(const SandboxLaunchPlan& plan,
                                                                    std::string& error) {
    error.clear();
    if (!initialized_ || !validateSandboxProfile(plan.profile, error) ||
        !validateSandboxLaunchRequest(plan.request, error) ||
        plan.request.profileDigest != digestSandboxProfile(plan.profile)) {
        if (error.empty()) error = "sandbox cgroup received an unverified launch plan";
        return std::nullopt;
    }
    SandboxCgroup cgroup{};
    cgroup.instanceId = plan.request.instanceId;
    cgroup.path = rootPath_ + "/instance-" + std::to_string(cgroup.instanceId);
    if (mkdir(cgroup.path.c_str(), 0755) != 0) {
        error = std::string("could not create sandbox instance cgroup: ") + std::strerror(errno);
        return std::nullopt;
    }
    if (!ensureRootOwnedDirectory(cgroup.path, error) ||
        !writeControlFile(cgroup.path + "/memory.max",
                          std::to_string(static_cast<std::uint64_t>(plan.profile.memoryMaxMiB) * 1024U * 1024U), error) ||
        !writeControlFile(cgroup.path + "/pids.max", std::to_string(plan.profile.pidsMax), error) ||
        !writeControlFile(cgroup.path + "/cpu.weight", std::to_string(plan.profile.cpuWeight), error)) {
        rmdir(cgroup.path.c_str());
        return std::nullopt;
    }
    return cgroup;
}

bool SandboxCgroupManager::moveProcess(const SandboxCgroup& cgroup, pid_t processId,
                                       std::string& error) const {
    error.clear();
    if (!initialized_ || !isSafeInstancePath(cgroup) || processId <= 0) {
        error = "sandbox cgroup process placement is invalid";
        return false;
    }
    return writeControlFile(cgroup.path + "/cgroup.procs", std::to_string(processId), error);
}

bool SandboxCgroupManager::removeInstance(const SandboxCgroup& cgroup, std::string& error) const {
    error.clear();
    if (!initialized_ || !isSafeInstancePath(cgroup) || rmdir(cgroup.path.c_str()) != 0) {
        error = std::string("could not remove sandbox instance cgroup: ") + std::strerror(errno);
        return false;
    }
    return true;
}

} // namespace lcl::security
