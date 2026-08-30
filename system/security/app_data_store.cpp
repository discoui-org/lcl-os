#include "system/security/app_data_store.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

namespace lcl::security {
namespace {

namespace fs = std::filesystem;

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

bool ensureDirectoryAt(int parentDescriptor, const char* name, uid_t uid, gid_t gid,
                       ScopedFd& directory, std::string& error) {
    if (mkdirat(parentDescriptor, name, 0700) != 0 && errno != EEXIST) {
        error = std::string("could not create app storage directory '") + name + "': " +
                std::strerror(errno);
        return false;
    }

    ScopedFd opened(openat(parentDescriptor, name,
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat status {};
    if (!opened.valid() || fstat(opened.get(), &status) != 0 || !S_ISDIR(status.st_mode)) {
        error = std::string("app storage directory '") + name + "' is not a safe directory";
        return false;
    }
    if ((status.st_uid != uid || status.st_gid != gid) && fchown(opened.get(), uid, gid) != 0) {
        error = std::string("could not assign app storage directory '") + name + "': " +
                std::strerror(errno);
        return false;
    }
    if (fchmod(opened.get(), 0700) != 0) {
        error = std::string("could not secure app storage directory '") + name + "': " +
                std::strerror(errno);
        return false;
    }
    directory = std::move(opened);
    return true;
}

} // namespace

AppDataStore::AppDataStore(AppDataStoreConfig config) : config_(std::move(config)) {}

bool AppDataStore::validateContainersRoot(std::string& error) const {
    if (config_.containersRoot.empty() || !fs::path(config_.containersRoot).is_absolute()) {
        error = "app containers root is empty";
        return false;
    }
    struct stat status {};
    if (lstat(config_.containersRoot.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode) || status.st_uid != config_.ownerUid ||
        status.st_gid != config_.ownerGid || (status.st_mode & 0022) != 0) {
        error = "app containers root is not an owner-controlled directory";
        return false;
    }
    return true;
}

bool AppDataStore::ensurePersistentDirectories(const AppIdentity& identity,
                                               AppPersistentDirectories& directories,
                                               std::string& error) const {
    error.clear();
    directories = {};
    if (!AppIdentityRegistry::isValidAppId(identity.appId) || identity.uid == 0 || identity.gid == 0 ||
        identity.uid != identity.gid) {
        error = "invalid app identity for persistent storage";
        return false;
    }
    if (!validateContainersRoot(error)) {
        return false;
    }

    ScopedFd root(open(config_.containersRoot.c_str(),
                       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!root.valid()) {
        error = std::string("could not open app containers root: ") + std::strerror(errno);
        return false;
    }

    ScopedFd container;
    ScopedFd data;
    ScopedFd cache;
    ScopedFd preferences;
    if (!ensureDirectoryAt(root.get(), identity.appId.c_str(), identity.uid, identity.gid, container, error) ||
        !ensureDirectoryAt(container.get(), "Data", identity.uid, identity.gid, data, error) ||
        !ensureDirectoryAt(container.get(), "Cache", identity.uid, identity.gid, cache, error) ||
        !ensureDirectoryAt(container.get(), "Preferences", identity.uid, identity.gid, preferences, error)) {
        return false;
    }

    const fs::path containerPath = fs::path(config_.containersRoot) / identity.appId;
    directories.container = containerPath.string();
    directories.data = (containerPath / "Data").string();
    directories.cache = (containerPath / "Cache").string();
    directories.preferences = (containerPath / "Preferences").string();
    return true;
}

} // namespace lcl::security
