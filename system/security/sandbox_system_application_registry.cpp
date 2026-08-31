#include "system/security/sandbox_system_application_registry.hpp"

#include "system/security/bundle_record.hpp"
#include "system/security/sandbox_daemon.hpp"
#include "system/security/sandbox_launch_material.hpp"
#include "system/security/session_user.hpp"
#include "system/session/app_bundle_parser.hpp"

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

class ScopedFd final {
public:
  explicit ScopedFd(int descriptor = -1) noexcept : descriptor_(descriptor) {}
  ~ScopedFd() {
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
  }

  ScopedFd(const ScopedFd &) = delete;
  ScopedFd &operator=(const ScopedFd &) = delete;
  ScopedFd(ScopedFd &&other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}
  ScopedFd &operator=(ScopedFd &&other) noexcept {
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

bool isRootControlledDirectory(const std::string &path, std::string &error) {
  struct stat status{};
  if (lstat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
      S_ISLNK(status.st_mode) || status.st_uid != 0 || status.st_gid != 0 ||
      (status.st_mode & 0022) != 0) {
    error =
        "system sandbox registry source is not a root-controlled directory: " +
        path;
    return false;
  }
  return true;
}

ScopedFd openDirectory(const std::string &path, std::string &error) {
  ScopedFd descriptor(
      open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!descriptor.valid()) {
    error = std::string("could not open protected sandbox directory '") + path +
            "': " + std::strerror(errno);
  }
  return descriptor;
}

SandboxRuntime runtimeForBundle(const lcl::core::AppBundleMetadata &metadata,
                                std::string &error) {
  if (metadata.runtime.empty()) {
    const bool executableIsJavaScript =
        metadata.executable.size() >= 3 && metadata.executable.ends_with(".js");
    return executableIsJavaScript ? SandboxRuntime::JavaScript
                                  : SandboxRuntime::Native;
  }
  if (metadata.runtime == "org.lcl.native") {
    return SandboxRuntime::Native;
  }
  if (metadata.runtime == "org.lcl.javascript") {
    return SandboxRuntime::JavaScript;
  }
  error =
      "system bundle has an unsupported sandbox runtime: " + metadata.runtime;
  return static_cast<SandboxRuntime>(0);
}

bool bundleIsInsideSystemApplications(
    const lcl::core::AppBundleMetadata &metadata,
    const SandboxSystemApplicationRegistryConfig &config) {
  const std::filesystem::path expectedParent(config.systemApplicationsPath);
  const std::filesystem::path bundlePath(metadata.bundlePath);
  return bundlePath.is_absolute() &&
         bundlePath.parent_path() == expectedParent &&
         bundlePath.extension() == ".app";
}

bool registerOneSystemApplication(
    SandboxDaemon &daemon, const SandboxSystemApplicationRegistryConfig &config,
    AppIdentityRegistry &identities, const AppDataStore &dataStore,
    const ScopedFd &systemDescriptor,
    const lcl::core::AppBundleMetadata &metadata, std::string &error) {
  if (!metadata.valid || !bundleIsInsideSystemApplications(metadata, config) ||
      !metadata.bundleHandle || !metadata.bundleHandle->valid() ||
      !metadata.executableHandle || !metadata.executableHandle->valid()) {
    error = "system sandbox registry received invalid bundle metadata";
    return false;
  }
  if (isTrustedUserShellBundle(metadata.appId, metadata.bundlePath)) {
    return true;
  }

  const auto record = makeBundleRecord(metadata, error);
  if (!record || record->appId != metadata.appId) {
    if (error.empty()) {
      error =
          "system sandbox bundle record does not match its catalog identity";
    }
    return false;
  }
  const SandboxRuntime runtime = runtimeForBundle(metadata, error);
  if (error.size() != 0) {
    return false;
  }
  const auto identity = identities.getOrCreate(metadata.appId, error);
  if (!identity) {
    return false;
  }
  AppPersistentDirectories directories;
  if (!dataStore.ensurePersistentDirectories(*identity, directories, error)) {
    return false;
  }
  ScopedFd data = openDirectory(directories.data, error);
  if (!data.valid())
    return false;
  ScopedFd cache = openDirectory(directories.cache, error);
  if (!cache.valid())
    return false;
  ScopedFd preferences = openDirectory(directories.preferences, error);
  if (!preferences.valid())
    return false;
  ScopedFd runtimeDescriptor;
  if (runtime == SandboxRuntime::JavaScript) {
    runtimeDescriptor = ScopedFd(open(config.javascriptRuntimePath.c_str(),
                                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!runtimeDescriptor.valid()) {
      error = std::string("could not open protected JavaScript runtime: ") +
              std::strerror(errno);
      return false;
    }
  }

  VerifiedApplication application{};
  application.appId = metadata.appId;
  application.identity = *identity;
  application.runtime = runtime;
  application.requestedPermissions = metadata.requestedPermissions;
  application.bundleRecordDigest = record->digest;
  application.permissionSubject = makeSystemImagePermissionSubject(
      kSessionUserUid, application.appId, application.bundleRecordDigest);
  SandboxLaunchMaterialInput material{};
  material.application = application;
  material.filesystemSources = {
      .appBundleDescriptor = metadata.bundleHandle->descriptor(),
      .systemDescriptor = systemDescriptor.get(),
      .dataDescriptor = data.get(),
      .cacheDescriptor = cache.get(),
      .preferencesDescriptor = preferences.get(),
  };
  material.executableBundlePath = metadata.executable;
  material.executableDescriptor = metadata.executableHandle->descriptor();
  material.runtimeDescriptor = runtimeDescriptor.get();

  if (!daemon.registerVerifiedApplication(application, {}, error)) {
    return false;
  }
  if (!daemon.registerVerifiedLaunchMaterial(material, error)) {
    daemon.removeVerifiedApplication(application.appId);
    return false;
  }
  return true;
}

} // namespace

SandboxSystemApplicationRegistry::SandboxSystemApplicationRegistry(
    SandboxSystemApplicationRegistryConfig config)
    : config_(std::move(config)) {}

bool SandboxSystemApplicationRegistry::registerSystemApplications(
    SandboxDaemon &daemon, std::string &error) const {
  error.clear();
  if (geteuid() != 0 ||
      !isRootControlledDirectory(config_.systemApplicationsPath, error) ||
      !isRootControlledDirectory(config_.systemPath, error)) {
    if (error.empty()) {
      error = "system sandbox registry requires root";
    }
    return false;
  }

  AppIdentityRegistry identities(config_.identities);
  if (!identities.load(error)) {
    return false;
  }
  const AppDataStore dataStore(config_.dataStore);
  ScopedFd systemDescriptor = openDirectory(config_.systemPath, error);
  if (!systemDescriptor.valid()) {
    return false;
  }
  const auto bundles =
      lcl::core::AppBundleParser::scanDirectory(config_.systemApplicationsPath);
  for (const lcl::core::AppBundleMetadata &bundle : bundles) {
    if (!registerOneSystemApplication(daemon, config_, identities, dataStore,
                                      systemDescriptor, bundle, error)) {
      return false;
    }
  }
  return true;
}

} // namespace lcl::security
