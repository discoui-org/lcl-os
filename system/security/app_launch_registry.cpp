#include "system/security/app_launch_registry.hpp"

#include "system/security/app_identity_registry.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace lcl::security {
namespace {

namespace fs = std::filesystem;
constexpr std::string_view kHeader = "LCL_APP_LAUNCH_V1";

bool safeDirectory(const std::string& path, uid_t uid, gid_t gid) {
    struct stat status {};
    return lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode) &&
        !S_ISLNK(status.st_mode) && status.st_uid == uid &&
        status.st_gid == gid && (status.st_mode & 0777) == 0700;
}

bool safeRecord(const std::string& path, uid_t uid, gid_t gid,
                struct stat& status) {
    return lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
        status.st_uid == uid && status.st_gid == gid &&
        (status.st_mode & 0777) == 0600 && status.st_nlink == 1;
}

bool writeAll(int descriptor, std::string_view contents) {
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t count = write(
            descriptor, contents.data() + offset, contents.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

template <typename T>
bool parseUnsigned(std::string_view value, T& output) {
    std::uintmax_t parsed = 0;
    const auto [end, ec] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc{} || end != value.data() + value.size() ||
        parsed > static_cast<std::uintmax_t>(std::numeric_limits<T>::max())) {
        return false;
    }
    output = static_cast<T>(parsed);
    return true;
}

} // namespace

AppLaunchRegistry::AppLaunchRegistry(AppLaunchRegistryConfig config)
    : config_(std::move(config)) {}

bool AppLaunchRegistry::validateDirectory(std::string& error) const {
    const fs::path directory(config_.directoryPath);
    const fs::path parent = directory.parent_path();
    struct stat parentStatus {};
    if (!directory.is_absolute() || config_.firstAppUid == 0 ||
        config_.firstAppUid > config_.lastAppUid || parent.empty() ||
        lstat(parent.c_str(), &parentStatus) != 0 ||
        !S_ISDIR(parentStatus.st_mode) || S_ISLNK(parentStatus.st_mode) ||
        parentStatus.st_uid != config_.ownerUid ||
        parentStatus.st_gid != config_.ownerGid ||
        (parentStatus.st_mode & 0022) != 0 ||
        !safeDirectory(config_.directoryPath, config_.ownerUid,
                       config_.ownerGid)) {
        error = "app launch registry is not an owner-controlled private directory";
        return false;
    }
    return true;
}

std::string AppLaunchRegistry::recordPath(std::uint64_t instanceId) const {
    return (fs::path(config_.directoryPath) /
            ("launch-" + std::to_string(instanceId) + ".v1")).string();
}

bool AppLaunchRegistry::initializeAndReset(std::string& error) const {
    error.clear();
    const fs::path directory(config_.directoryPath);
    const fs::path parent = directory.parent_path();
    struct stat parentStatus {};
    if (!directory.is_absolute() || parent.empty() ||
        lstat(parent.c_str(), &parentStatus) != 0 ||
        !S_ISDIR(parentStatus.st_mode) || S_ISLNK(parentStatus.st_mode) ||
        parentStatus.st_uid != config_.ownerUid ||
        parentStatus.st_gid != config_.ownerGid ||
        (parentStatus.st_mode & 0022) != 0) {
        error = "app launch registry parent is unsafe";
        return false;
    }
    bool created = false;
    if (mkdir(directory.c_str(), 0700) == 0) {
        created = true;
    } else if (errno != EEXIST) {
        error = std::string("could not create app launch registry: ") +
            std::strerror(errno);
        return false;
    }
    const int descriptor = open(directory.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat directoryStatus {};
    const bool secured = descriptor >= 0 &&
        (!created || (fchown(descriptor, config_.ownerUid, config_.ownerGid) == 0 &&
                      fchmod(descriptor, 0700) == 0)) &&
        fstat(descriptor, &directoryStatus) == 0 &&
        S_ISDIR(directoryStatus.st_mode) &&
        directoryStatus.st_uid == config_.ownerUid &&
        directoryStatus.st_gid == config_.ownerGid &&
        (directoryStatus.st_mode & 0777) == 0700;
    if (descriptor >= 0) close(descriptor);
    if (!secured || !validateDirectory(error)) {
        if (error.empty()) {
            error = "app launch registry is not an owner-controlled private directory";
        }
        return false;
    }
    std::error_code iteratorError;
    fs::directory_iterator iterator(directory, iteratorError);
    const fs::directory_iterator end;
    for (; !iteratorError && iterator != end; iterator.increment(iteratorError)) {
        const auto& entry = *iterator;
        const std::string name = entry.path().filename().string();
        if (!name.starts_with("launch-") || !name.ends_with(".v1")) continue;
        struct stat status {};
        if (!safeRecord(entry.path().string(), config_.ownerUid,
                        config_.ownerGid, status) ||
            unlink(entry.path().c_str()) != 0) {
            error = "could not safely clear a stale app launch record";
            return false;
        }
    }
    if (iteratorError) {
        error = "could not enumerate stale app launch records";
        return false;
    }
    return true;
}

bool AppLaunchRegistry::registerLaunch(const AppLaunchIdentity& identity,
                                       std::string& error) const {
    error.clear();
    if (!validateDirectory(error) || identity.instanceId == 0 ||
        !AppIdentityRegistry::isValidAppId(identity.appId) ||
        identity.processGroupId <= 0 || identity.uid != identity.gid ||
        identity.uid < config_.firstAppUid ||
        identity.uid > config_.lastAppUid) {
        if (error.empty()) error = "invalid app launch identity";
        return false;
    }
    const std::string target = recordPath(identity.instanceId);
    struct stat existing {};
    if (lstat(target.c_str(), &existing) == 0 || errno != ENOENT) {
        error = "app launch instance is already registered";
        return false;
    }
    const std::string temporary = target + ".tmp." + std::to_string(getpid());
    unlink(temporary.c_str());
    const int descriptor = open(temporary.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) {
        error = "could not create app launch record";
        return false;
    }
    std::ostringstream output;
    output << kHeader << '\n' << identity.instanceId << ' ' << identity.appId
           << ' ' << identity.processGroupId << ' ' << identity.uid << ' '
           << identity.gid << '\n';
    const std::string contents = output.str();
    const bool secured = fchown(descriptor, config_.ownerUid, config_.ownerGid) == 0 &&
        fchmod(descriptor, 0600) == 0;
    const bool written = secured && writeAll(descriptor, contents) &&
        fsync(descriptor) == 0;
    close(descriptor);
    if (!written || rename(temporary.c_str(), target.c_str()) != 0) {
        unlink(temporary.c_str());
        error = "could not commit app launch record";
        return false;
    }
    const int parent = open(config_.directoryPath.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0 || fsync(parent) != 0) {
        if (parent >= 0) close(parent);
        unlink(target.c_str());
        error = "app launch registry could not be synced";
        return false;
    }
    close(parent);
    return true;
}

std::optional<AppLaunchIdentity> AppLaunchRegistry::find(
        std::uint64_t instanceId, std::string& error) const {
    error.clear();
    if (instanceId == 0 || !validateDirectory(error)) return std::nullopt;
    const std::string path = recordPath(instanceId);
    struct stat pathStatus {};
    if (!safeRecord(path, config_.ownerUid, config_.ownerGid, pathStatus)) {
        error = "app launch record is missing or unsafe";
        return std::nullopt;
    }
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat opened {};
    if (descriptor < 0 || fstat(descriptor, &opened) != 0 ||
        opened.st_dev != pathStatus.st_dev || opened.st_ino != pathStatus.st_ino) {
        if (descriptor >= 0) close(descriptor);
        error = "app launch record changed while opening";
        return std::nullopt;
    }
    std::array<char, 512> bytes{};
    const ssize_t count = read(descriptor, bytes.data(), bytes.size() - 1);
    close(descriptor);
    if (count <= 0 || count >= static_cast<ssize_t>(bytes.size() - 1)) {
        error = "could not read app launch record";
        return std::nullopt;
    }
    std::istringstream input(std::string(bytes.data(), static_cast<size_t>(count)));
    std::string header;
    std::string line;
    if (!std::getline(input, header) || header != kHeader ||
        !std::getline(input, line)) {
        error = "app launch record has an unknown format";
        return std::nullopt;
    }
    std::istringstream fields(line);
    std::string rawInstance, appId, rawGroup, rawUid, rawGid, extra;
    AppLaunchIdentity identity{};
    pid_t rawProcessGroup = 0;
    if (!(fields >> rawInstance >> appId >> rawGroup >> rawUid >> rawGid) ||
        (fields >> extra) || !parseUnsigned(rawInstance, identity.instanceId) ||
        !parseUnsigned(rawGroup, rawProcessGroup) ||
        !parseUnsigned(rawUid, identity.uid) ||
        !parseUnsigned(rawGid, identity.gid)) {
        error = "app launch record contains invalid fields";
        return std::nullopt;
    }
    identity.appId = std::move(appId);
    identity.processGroupId = static_cast<pid_t>(rawProcessGroup);
    if (identity.instanceId != instanceId ||
        !AppIdentityRegistry::isValidAppId(identity.appId) ||
        identity.processGroupId <= 0 || identity.uid != identity.gid ||
        identity.uid < config_.firstAppUid ||
        identity.uid > config_.lastAppUid) {
        error = "app launch record identity is invalid";
        return std::nullopt;
    }
    return identity;
}

bool AppLaunchRegistry::remove(std::uint64_t instanceId, std::string& error) const {
    error.clear();
    if (instanceId == 0 || !validateDirectory(error)) return false;
    const std::string path = recordPath(instanceId);
    struct stat status {};
    if (lstat(path.c_str(), &status) != 0) {
        if (errno == ENOENT) return true;
        error = "could not inspect app launch record";
        return false;
    }
    if (!S_ISREG(status.st_mode) || status.st_uid != config_.ownerUid ||
        status.st_gid != config_.ownerGid || (status.st_mode & 0777) != 0600 ||
        status.st_nlink != 1) {
        error = "app launch record is unsafe";
        return false;
    }
    if (unlink(path.c_str()) != 0) {
        error = "could not remove app launch record";
        return false;
    }
    return true;
}

} // namespace lcl::security
