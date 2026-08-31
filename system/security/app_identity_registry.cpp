#include "system/security/app_identity_registry.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace lcl::security {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kRegistryHeader = "LCL_APP_IDENTITIES_V1";
constexpr std::size_t kMaxRegistryBytes = 1024 * 1024;

class ScopedFd final {
public:
    explicit ScopedFd(int descriptor = -1) noexcept : descriptor_(descriptor) {}
    ~ScopedFd() {
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            if (descriptor_ >= 0) {
                close(descriptor_);
            }
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    int get() const noexcept { return descriptor_; }
    bool valid() const noexcept { return descriptor_ >= 0; }

private:
    int descriptor_{-1};
};

bool isSafeRegistryFile(const fs::path& path, uid_t ownerUid, gid_t ownerGid, struct stat& status) {
    if (lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != ownerUid || status.st_gid != ownerGid || (status.st_mode & 0077) != 0 ||
        status.st_nlink != 1) {
        return false;
    }
    return true;
}

ScopedFd openRegistryLock(const AppIdentityRegistryConfig& config, int lockOperation,
                          std::string& error) {
    ScopedFd lock(open((config.registryPath + ".lock").c_str(),
                       O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    struct stat status {};
    if (!lock.valid() || fstat(lock.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != config.ownerUid || status.st_gid != config.ownerGid ||
        (status.st_mode & 0077) != 0 || status.st_nlink != 1) {
        error = "app identity registry lock has unsafe ownership, mode, link count, or type";
        return ScopedFd{};
    }
    if (flock(lock.get(), lockOperation) != 0) {
        error = "could not acquire app identity registry lock";
        return ScopedFd{};
    }
    return lock;
}

bool writeAll(int descriptor, std::string_view contents) {
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t count = write(descriptor, contents.data() + offset, contents.size() - offset);
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

bool readAll(int descriptor, std::string& contents) {
    struct stat status {};
    if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
        static_cast<std::uintmax_t>(status.st_size) > kMaxRegistryBytes) {
        return false;
    }
    contents.resize(static_cast<std::size_t>(status.st_size));
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t count = read(descriptor, contents.data() + offset, contents.size() - offset);
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

bool parseUnsigned(std::string_view value, uid_t& output) {
    std::uintmax_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() ||
        parsed > static_cast<std::uintmax_t>(std::numeric_limits<uid_t>::max())) {
        return false;
    }
    output = static_cast<uid_t>(parsed);
    return true;
}

std::vector<std::string_view> splitFields(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start < line.size()) {
        const std::size_t separator = line.find(' ', start);
        fields.push_back(line.substr(start, separator - start));
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1;
    }
    return fields;
}

} // namespace

AppIdentityRegistry::AppIdentityRegistry(AppIdentityRegistryConfig config)
    : config_(std::move(config)) {}

bool AppIdentityRegistry::isValidAppId(const std::string& appId) {
    if (appId.empty() || appId.size() > 128) {
        return false;
    }

    bool labelStart = true;
    bool hasDot = false;
    char previous = '\0';
    for (char character : appId) {
        if (character == '.') {
            if (labelStart || previous == '-') {
                return false;
            }
            labelStart = true;
            hasDot = true;
        } else if (labelStart) {
            if (character < 'a' || character > 'z') {
                return false;
            }
            labelStart = false;
        } else if (!((character >= 'a' && character <= 'z') ||
                     (character >= '0' && character <= '9') || character == '-')) {
            return false;
        }
        previous = character;
    }
    return hasDot && !labelStart && previous != '-';
}

bool AppIdentityRegistry::validateConfig(std::string& error) const {
    if (config_.registryPath.empty() || !fs::path(config_.registryPath).is_absolute() ||
        config_.firstAppUid == 0 ||
        config_.firstAppUid > config_.lastAppUid) {
        error = "invalid app identity registry configuration";
        return false;
    }
    return true;
}

bool AppIdentityRegistry::validateRegistryParent(std::string& error) const {
    const fs::path parent = fs::path(config_.registryPath).parent_path();
    struct stat status {};
    if (parent.empty() || lstat(parent.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode) || status.st_uid != config_.ownerUid ||
        status.st_gid != config_.ownerGid || (status.st_mode & 0022) != 0) {
        error = "app identity registry parent is not a private owner-controlled directory";
        return false;
    }
    return true;
}

bool AppIdentityRegistry::loadUnlocked(std::string& error) {
    identities_.clear();
    struct stat pathStatus {};
    if (lstat(config_.registryPath.c_str(), &pathStatus) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        error = std::string("could not inspect app identity registry: ") + std::strerror(errno);
        return false;
    }
    if (!isSafeRegistryFile(config_.registryPath, config_.ownerUid, config_.ownerGid, pathStatus)) {
        error = "app identity registry has unsafe ownership, mode, link count, or type";
        return false;
    }

    ScopedFd registry(open(config_.registryPath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct stat openedStatus {};
    if (!registry.valid() || fstat(registry.get(), &openedStatus) != 0 ||
        openedStatus.st_dev != pathStatus.st_dev || openedStatus.st_ino != pathStatus.st_ino ||
        openedStatus.st_uid != config_.ownerUid || openedStatus.st_gid != config_.ownerGid ||
        (openedStatus.st_mode & 0077) != 0 || openedStatus.st_nlink != 1) {
        error = "app identity registry changed while opening";
        return false;
    }

    std::string contents;
    if (!readAll(registry.get(), contents)) {
        error = "could not read app identity registry";
        return false;
    }

    std::istringstream input(contents);
    std::string line;
    if (!std::getline(input, line) || line != kRegistryHeader) {
        error = "app identity registry has an unknown format";
        return false;
    }

    std::map<uid_t, std::string> seenUids;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        const std::vector<std::string_view> fields = splitFields(line);
        if (fields.size() != 3 || !isValidAppId(std::string(fields[0]))) {
            error = "app identity registry contains an invalid record";
            return false;
        }
        uid_t uid = 0;
        gid_t gid = 0;
        if (!parseUnsigned(fields[1], uid) || !parseUnsigned(fields[2], gid) || uid != gid ||
            uid < config_.firstAppUid || uid > config_.lastAppUid || identities_.contains(std::string(fields[0])) ||
            seenUids.contains(uid)) {
            error = "app identity registry contains duplicate or out-of-range identities";
            return false;
        }
        const std::string appId(fields[0]);
        identities_.emplace(appId, AppIdentity{appId, uid, gid});
        seenUids.emplace(uid, appId);
    }
    return true;
}

bool AppIdentityRegistry::load(std::string& error) {
    error.clear();
    if (!validateConfig(error) || !validateRegistryParent(error)) {
        return false;
    }

    ScopedFd lock = openRegistryLock(config_, LOCK_SH, error);
    if (!lock.valid()) {
        return false;
    }
    return loadUnlocked(error);
}

std::optional<uid_t> AppIdentityRegistry::allocateUid() const {
    for (uid_t candidate = config_.firstAppUid; candidate <= config_.lastAppUid; ++candidate) {
        bool used = false;
        for (const auto& [_, identity] : identities_) {
            if (identity.uid == candidate) {
                used = true;
                break;
            }
        }
        if (!used) {
            return candidate;
        }
        if (candidate == std::numeric_limits<uid_t>::max()) {
            break;
        }
    }
    return std::nullopt;
}

bool AppIdentityRegistry::persistUnlocked(std::string& error) const {
    const fs::path registryPath(config_.registryPath);
    const fs::path parent = registryPath.parent_path();
    const std::string temporaryName = registryPath.filename().string() + ".tmp." +
                                      std::to_string(getpid());
    const fs::path temporaryPath = parent / temporaryName;
    unlink(temporaryPath.c_str());

    ScopedFd temporary(open(temporaryPath.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!temporary.valid()) {
        error = std::string("could not create app identity registry transaction: ") + std::strerror(errno);
        return false;
    }
    struct stat temporaryStatus {};
    if (fstat(temporary.get(), &temporaryStatus) != 0 ||
        ((temporaryStatus.st_uid != config_.ownerUid || temporaryStatus.st_gid != config_.ownerGid) &&
         fchown(temporary.get(), config_.ownerUid, config_.ownerGid) != 0) ||
        fchmod(temporary.get(), 0600) != 0) {
        error = std::string("could not secure app identity registry transaction: ") + std::strerror(errno);
        unlink(temporaryPath.c_str());
        return false;
    }

    std::ostringstream output;
    output << kRegistryHeader << '\n';
    for (const auto& [_, identity] : identities_) {
        output << identity.appId << ' ' << identity.uid << ' ' << identity.gid << '\n';
    }
    const std::string contents = output.str();
    if (!writeAll(temporary.get(), contents) || fsync(temporary.get()) != 0) {
        error = std::string("could not write app identity registry transaction: ") + std::strerror(errno);
        unlink(temporaryPath.c_str());
        return false;
    }
    if (rename(temporaryPath.c_str(), registryPath.c_str()) != 0) {
        error = std::string("could not commit app identity registry transaction: ") + std::strerror(errno);
        unlink(temporaryPath.c_str());
        return false;
    }

    ScopedFd parentDescriptor(open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!parentDescriptor.valid() || fsync(parentDescriptor.get()) != 0) {
        error = "app identity registry committed but parent directory could not be synced";
        return false;
    }
    return true;
}

std::optional<AppIdentity> AppIdentityRegistry::getOrCreate(const std::string& appId, std::string& error) {
    error.clear();
    if (!isValidAppId(appId)) {
        error = "invalid canonical app ID";
        return std::nullopt;
    }
    if (!validateConfig(error) || !validateRegistryParent(error)) {
        return std::nullopt;
    }

    ScopedFd lock = openRegistryLock(config_, LOCK_EX, error);
    if (!lock.valid()) {
        return std::nullopt;
    }
    if (!loadUnlocked(error)) {
        return std::nullopt;
    }
    if (const auto existing = find(appId)) {
        return existing;
    }

    const auto allocatedUid = allocateUid();
    if (!allocatedUid) {
        error = "app identity UID range is exhausted";
        return std::nullopt;
    }
    const AppIdentity identity{appId, *allocatedUid, static_cast<gid_t>(*allocatedUid)};
    identities_.emplace(appId, identity);
    if (!persistUnlocked(error)) {
        identities_.erase(appId);
        return std::nullopt;
    }
    return identity;
}

std::optional<AppIdentity> AppIdentityRegistry::find(const std::string& appId) const {
    const auto found = identities_.find(appId);
    if (found == identities_.end()) {
        return std::nullopt;
    }
    return found->second;
}

} // namespace lcl::security
