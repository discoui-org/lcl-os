#include "system/security/sandbox_external_application_registry.hpp"

#include "system/security/sandbox_daemon.hpp"
#include "system/security/sandbox_launch_material.hpp"
#include "system/session/app_bundle_parser.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

namespace lcl::security {
namespace {

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

bool isRootControlledDirectory(int descriptor) {
    struct stat status {};
    return descriptor >= 0 && fstat(descriptor, &status) == 0 && S_ISDIR(status.st_mode) &&
           status.st_uid == 0 && status.st_gid == 0 && (status.st_mode & 0022) == 0;
}

bool isRootControlledDirectory(const std::string& path) {
    struct stat status {};
    return lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode) &&
           !S_ISLNK(status.st_mode) && status.st_uid == 0 && status.st_gid == 0 &&
           (status.st_mode & 0022) == 0;
}

bool isReservedSystemApplicationId(const SandboxExternalApplicationRegistryConfig& config,
                                   const std::string& appId, std::string& error) {
    if (!isRootControlledDirectory(config.systemApplicationsPath)) {
        error = "system application registry source is not root-controlled";
        return true;
    }
    const auto systemBundles =
        lcl::core::AppBundleParser::scanDirectory(config.systemApplicationsPath);
    for (const lcl::core::AppBundleMetadata& systemBundle : systemBundles) {
        if (systemBundle.appId == appId) {
            return true;
        }
    }
    return false;
}

ScopedFd openDirectory(const std::string& path, std::string& error) {
    ScopedFd descriptor(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!descriptor.valid()) {
        error = std::string("could not open protected sandbox directory '") + path +
                "': " + std::strerror(errno);
    }
    return descriptor;
}

ScopedFd openRuntimeSocket(const std::string& path, std::string& error) {
    ScopedFd descriptor(open(path.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW));
    if (!descriptor.valid()) {
        error = std::string("could not open protected graphics endpoint '") + path +
                "': " + std::strerror(errno);
    }
    return descriptor;
}

ScopedFd openOptionalRuntimeSocket(const std::string& path) {
    if (path.empty()) return ScopedFd{};
    return ScopedFd(open(path.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW));
}

bool requestsPermission(const std::vector<std::string>& permissions,
                        std::string_view permission) {
    return std::binary_search(permissions.begin(), permissions.end(), permission);
}

ScopedFd openExecutable(const std::string& path, std::string& error) {
    ScopedFd descriptor(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct stat status {};
    if (!descriptor.valid() || fstat(descriptor.get(), &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_nlink != 1 ||
        (status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) {
        error = std::string("could not open protected JavaScript runtime '") + path + "'";
        return ScopedFd{};
    }
    return descriptor;
}

SandboxRuntime runtimeForRegistration(const SandboxApplicationRegistration& registration,
                                      std::string& error) {
    switch (registration.runtime) {
        case SandboxRuntime::Native:
        case SandboxRuntime::JavaScript:
            return registration.runtime;
    }
    error = "external bundle has an unsupported sandbox runtime";
    return static_cast<SandboxRuntime>(0);
}

} // namespace

SandboxExternalApplicationRegistry::SandboxExternalApplicationRegistry(
    SandboxExternalApplicationRegistryConfig config)
    : config_(std::move(config)) {}

bool SandboxExternalApplicationRegistry::registerSnapshot(
    SandboxDaemon& daemon, const SandboxApplicationRegistration& registration,
    int appBundleDescriptor, int executableDescriptor, std::string& error) {
    error.clear();
    if (geteuid() != 0 || !validateSandboxApplicationRegistration(registration, error) ||
        !isRootControlledDirectory(appBundleDescriptor)) {
        if (error.empty()) {
            error = "external sandbox registration requires a root-controlled bundle snapshot";
        }
        return false;
    }
    const SandboxRuntime runtime = runtimeForRegistration(registration, error);
    if (error.size() != 0) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (isReservedSystemApplicationId(config_, registration.appId, error)) {
        if (error.empty()) {
            error = "external bundle cannot use a system application ID";
        }
        return false;
    }
    if (!externalAppIds_.contains(registration.appId) &&
        daemon.hasRegisteredApplication(registration.appId)) {
        error = "external bundle cannot replace a system or separately registered app ID";
        return false;
    }

    AppIdentityRegistry identities(config_.identities);
    if (!identities.load(error)) {
        return false;
    }
    const auto identity = identities.getOrCreate(registration.appId, error);
    if (!identity) {
        return false;
    }
    const AppDataStore dataStore(config_.dataStore);
    AppPersistentDirectories directories;
    if (!dataStore.ensurePersistentDirectories(*identity, directories, error)) {
        return false;
    }
    ScopedFd system = openDirectory(config_.systemPath, error);
    ScopedFd data = openDirectory(directories.data, error);
    ScopedFd cache = openDirectory(directories.cache, error);
    ScopedFd preferences = openDirectory(directories.preferences, error);
    ScopedFd compositorSocket = openRuntimeSocket(config_.compositorSocketPath, error);
    ScopedFd rasterSocket = openRuntimeSocket(config_.rasterSocketPath, error);
    ScopedFd gpuSocket = openOptionalRuntimeSocket(config_.gpuSocketPath);
    if (!system.valid() || !data.valid() || !cache.valid() || !preferences.valid() ||
        !compositorSocket.valid() || !rasterSocket.valid() ||
        !isRootControlledDirectory(system.get())) {
        if (error.empty()) {
            error = "external sandbox registration could not prepare protected filesystem sources";
        }
        return false;
    }
    ScopedFd runtimeDescriptor;
    if (runtime == SandboxRuntime::JavaScript) {
        runtimeDescriptor = openExecutable(config_.javascriptRuntimePath, error);
        if (!runtimeDescriptor.valid()) {
            return false;
        }
    }

    VerifiedApplication application{};
    application.appId = registration.appId;
    application.identity = *identity;
    application.runtime = runtime;
    application.requestedPermissions = registration.requestedPermissions;
    application.bundleRecordDigest = registration.bundleRecordDigest;
    application.permissionSubject = PermissionSubject{
        .userUid = registration.userUid,
        .appId = registration.appId,
        .bundleRecordDigest = registration.bundleRecordDigest,
        .publisherIdentity = registration.publisherIdentity,
        .permissionVersion = kPermissionDecisionVersion,
    };
    SandboxLaunchMaterialInput material{};
    material.application = application;
    material.filesystemSources = {
        .appBundleDescriptor = appBundleDescriptor,
        .systemDescriptor = system.get(),
        .dataDescriptor = data.get(),
        .cacheDescriptor = cache.get(),
        .preferencesDescriptor = preferences.get(),
        .compositorSocketDescriptor = compositorSocket.get(),
        .rasterSocketDescriptor = rasterSocket.get(),
    };
    // Permission-store decisions are evaluated again at each launch. Retain
    // a pinned endpoint only for bundles that explicitly declare graphics.gpu;
    // makeChildLaunchSpec removes it unless that decision is effective.
    if (requestsPermission(registration.requestedPermissions, "graphics.gpu")) {
        material.filesystemSources.gpuSocketDescriptor = gpuSocket.get();
    }
    material.executableBundlePath = registration.executableBundlePath;
    material.executableDescriptor = executableDescriptor;
    material.runtimeDescriptor = runtimeDescriptor.get();

    // A new approved version replaces only future launches. Existing children
    // already own their private mount and executable descriptors.
    daemon.removeVerifiedApplication(application.appId);
    if (!daemon.registerVerifiedApplication(application, {}, error)) {
        return false;
    }
    if (!daemon.registerVerifiedLaunchMaterial(material, error)) {
        daemon.removeVerifiedApplication(application.appId);
        return false;
    }
    externalAppIds_.insert(application.appId);
    return true;
}

} // namespace lcl::security
