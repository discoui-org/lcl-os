#include "system/security/sandbox_launch_material.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace lcl::security {
namespace {

constexpr int kMinimumRetainedDescriptor = 8;
constexpr std::size_t kMaximumArgumentCount = 64;
constexpr std::size_t kMaximumArgumentBytes = 4096;

bool isSafeArgument(const std::string& argument) {
    return argument.size() <= kMaximumArgumentBytes && argument.find('\0') == std::string::npos;
}

bool isRegularDescriptor(int descriptor, bool requireExecutable) {
    struct stat status {};
    return descriptor >= 0 && fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode) &&
           status.st_nlink == 1 && (!requireExecutable ||
                                    (status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0);
}

bool descriptorMatchesBundlePath(int bundleDescriptor, const std::string& relativePath,
                                 int expectedDescriptor) {
    if (!isSafeSandboxBundleRelativePath(relativePath)) {
        return false;
    }
    int parent = fcntl(bundleDescriptor, F_DUPFD_CLOEXEC, kMinimumRetainedDescriptor);
    if (parent < 0) {
        return false;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t end = relativePath.find('/', start);
        const std::string component = relativePath.substr(start, end - start);
        const bool finalComponent = end == std::string::npos;
        const int opened = openat(parent, component.c_str(),
                                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                                      (finalComponent ? 0 : O_DIRECTORY));
        close(parent);
        if (opened < 0) {
            return false;
        }
        if (finalComponent) {
            struct stat openedStatus {};
            struct stat expectedStatus {};
            const bool matches = fstat(opened, &openedStatus) == 0 &&
                                 fstat(expectedDescriptor, &expectedStatus) == 0 &&
                                 S_ISREG(openedStatus.st_mode) &&
                                 openedStatus.st_dev == expectedStatus.st_dev &&
                                 openedStatus.st_ino == expectedStatus.st_ino;
            close(opened);
            return matches;
        }
        parent = opened;
        start = end + 1;
    }
}

int duplicateDescriptor(int descriptor, std::string& error) {
    const int duplicate = fcntl(descriptor, F_DUPFD_CLOEXEC, kMinimumRetainedDescriptor);
    if (duplicate < 0) {
        error = std::string("could not retain verified sandbox descriptor: ") + std::strerror(errno);
    }
    return duplicate;
}

void closeSources(SandboxFilesystemSources& sources) {
    for (int* descriptor : {&sources.appBundleDescriptor, &sources.systemDescriptor,
                            &sources.dataDescriptor, &sources.cacheDescriptor,
                            &sources.preferencesDescriptor, &sources.compositorSocketDescriptor,
                            &sources.rasterSocketDescriptor}) {
        if (*descriptor >= 0) {
            close(*descriptor);
            *descriptor = -1;
        }
    }
}

} // namespace

struct SandboxLaunchMaterialRegistry::StoredMaterial final {
    ~StoredMaterial() {
        closeSources(filesystemSources);
        if (executableDescriptor >= 0) {
            close(executableDescriptor);
        }
        if (runtimeDescriptor >= 0) {
            close(runtimeDescriptor);
        }
    }

    VerifiedApplication application;
    SandboxFilesystemSources filesystemSources;
    std::string executableBundlePath;
    int executableDescriptor{-1};
    int runtimeDescriptor{-1};
    std::vector<std::string> arguments;
};

SandboxLaunchMaterialRegistry::SandboxLaunchMaterialRegistry() = default;
SandboxLaunchMaterialRegistry::~SandboxLaunchMaterialRegistry() = default;

bool SandboxLaunchMaterialRegistry::registerMaterial(const SandboxLaunchMaterialInput& input,
                                                      std::string& error) {
    error.clear();
    if (!AppIdentityRegistry::isValidAppId(input.application.appId) ||
        input.application.identity.appId != input.application.appId ||
        input.application.identity.uid == 0 || input.application.identity.gid == 0 ||
        input.application.identity.uid != input.application.identity.gid ||
        (input.application.runtime != SandboxRuntime::Native &&
         input.application.runtime != SandboxRuntime::JavaScript) ||
        isZeroDigest(input.application.bundleRecordDigest) ||
        !validateSandboxFilesystemSources(input.filesystemSources, input.application.identity, error) ||
        !validateSandboxRuntimeEndpointGeneration(input.filesystemSources, error) ||
        !isSafeSandboxBundleRelativePath(input.executableBundlePath) ||
        !isRegularDescriptor(input.executableDescriptor,
                             input.application.runtime == SandboxRuntime::Native) ||
        !descriptorMatchesBundlePath(input.filesystemSources.appBundleDescriptor,
                                     input.executableBundlePath, input.executableDescriptor) ||
        input.arguments.size() > kMaximumArgumentCount) {
        if (error.empty()) {
            error = "verified sandbox launch material is incomplete or unsafe";
        }
        return false;
    }
    if (input.application.runtime == SandboxRuntime::JavaScript &&
        !isRegularDescriptor(input.runtimeDescriptor, true)) {
        error = "verified JavaScript runtime descriptor is unavailable";
        return false;
    }
    for (const std::string& argument : input.arguments) {
        if (!isSafeArgument(argument)) {
            error = "verified sandbox argument is unsafe or too long";
            return false;
        }
    }
    const auto existing = materials_.find(input.application.appId);
    if (existing != materials_.end() &&
        existing->second->application.bundleRecordDigest != input.application.bundleRecordDigest) {
        error = "sandbox launch material refuses to replace a different verified bundle";
        return false;
    }

    auto material = std::make_unique<StoredMaterial>();
    material->application = input.application;
    material->executableBundlePath = input.executableBundlePath;
    material->arguments = input.arguments;
    material->filesystemSources.appBundleDescriptor =
        duplicateDescriptor(input.filesystemSources.appBundleDescriptor, error);
    material->filesystemSources.systemDescriptor =
        duplicateDescriptor(input.filesystemSources.systemDescriptor, error);
    material->filesystemSources.dataDescriptor =
        duplicateDescriptor(input.filesystemSources.dataDescriptor, error);
    material->filesystemSources.cacheDescriptor =
        duplicateDescriptor(input.filesystemSources.cacheDescriptor, error);
    material->filesystemSources.preferencesDescriptor =
        duplicateDescriptor(input.filesystemSources.preferencesDescriptor, error);
    material->filesystemSources.compositorSocketDescriptor =
        duplicateDescriptor(input.filesystemSources.compositorSocketDescriptor, error);
    material->filesystemSources.rasterSocketDescriptor =
        duplicateDescriptor(input.filesystemSources.rasterSocketDescriptor, error);
    material->executableDescriptor = duplicateDescriptor(input.executableDescriptor, error);
    if (input.application.runtime == SandboxRuntime::JavaScript) {
        material->runtimeDescriptor = duplicateDescriptor(input.runtimeDescriptor, error);
    }
    if (error.empty() &&
        (!validateSandboxFilesystemSources(material->filesystemSources, material->application.identity,
                                            error) ||
         !validateSandboxRuntimeEndpointGeneration(material->filesystemSources, error) ||
         !isSafeSandboxBundleRelativePath(material->executableBundlePath) ||
         !isRegularDescriptor(material->executableDescriptor,
                              material->application.runtime == SandboxRuntime::Native) ||
         !descriptorMatchesBundlePath(material->filesystemSources.appBundleDescriptor,
                                      material->executableBundlePath, material->executableDescriptor) ||
         (material->application.runtime == SandboxRuntime::JavaScript &&
          !isRegularDescriptor(material->runtimeDescriptor, true)))) {
        if (error.empty()) {
            error = "retained sandbox launch material is unsafe";
        }
    }
    if (!error.empty()) {
        return false;
    }
    materials_[input.application.appId] = std::move(material);
    return true;
}

void SandboxLaunchMaterialRegistry::remove(const std::string& appId) {
    materials_.erase(appId);
}

bool SandboxLaunchMaterialRegistry::hasMaterialFor(const SandboxLaunchPlan& plan) const {
    std::string ignoredError;
    if (!validateSandboxProfile(plan.profile, ignoredError) ||
        !validateSandboxLaunchRequest(plan.request, ignoredError) ||
        plan.request.profileDigest != digestSandboxProfile(plan.profile)) {
        return false;
    }
    const auto found = materials_.find(plan.request.appId);
    return found != materials_.end() &&
           found->second->application.bundleRecordDigest == plan.request.bundleRecordDigest &&
           found->second->application.identity.appId == plan.request.appId &&
           found->second->application.runtime == plan.profile.runtime;
}

bool SandboxLaunchMaterialRegistry::runtimeEndpointsCurrent(std::string& error) const {
    error.clear();
    for (const auto& [_, material] : materials_) {
        if (!validateSandboxRuntimeEndpointGeneration(material->filesystemSources, error)) {
            return false;
        }
    }
    return true;
}

void SandboxLaunchMaterialRegistry::removeStaleRuntimeEndpointMaterials() {
    for (auto iterator = materials_.begin(); iterator != materials_.end();) {
        std::string error;
        if (!validateSandboxRuntimeEndpointGeneration(iterator->second->filesystemSources, error)) {
            iterator = materials_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

bool SandboxLaunchMaterialRegistry::makeChildLaunchSpec(const SandboxLaunchPlan& plan,
                                                         const SandboxPlatformHardening& hardening,
                                                         std::optional<SandboxCgroupBinding> cgroup,
                                                         SandboxChildLaunchSpec& spec,
                                                         std::string& error) const {
    error.clear();
    spec = {};
    if (!validateSandboxProfile(plan.profile, error) || !validateSandboxLaunchRequest(plan.request, error) ||
        !validateSandboxPlatformHardening(hardening, error) ||
        plan.request.profileDigest != digestSandboxProfile(plan.profile) ||
        (hardening.requireCgroupResourceAccounting != cgroup.has_value()) ||
        (cgroup.has_value() &&
         (!cgroup->manager || cgroup->cgroup.instanceId != plan.request.instanceId ||
          cgroup->cgroup.path.empty()))) {
        if (error.empty()) {
            error = "sandbox launch plan or cgroup binding is invalid";
        }
        return false;
    }
    if (!hasMaterialFor(plan)) {
        error = "verified sandbox launch material is unavailable for this request";
        return false;
    }
    const auto found = materials_.find(plan.request.appId);
    const StoredMaterial& material = *found->second;
    if (!validateSandboxFilesystemSources(material.filesystemSources, material.application.identity, error) ||
        !validateSandboxRuntimeEndpointGeneration(material.filesystemSources, error) ||
        !isSafeSandboxBundleRelativePath(material.executableBundlePath) ||
        !isRegularDescriptor(material.executableDescriptor,
                             material.application.runtime == SandboxRuntime::Native) ||
        !descriptorMatchesBundlePath(material.filesystemSources.appBundleDescriptor,
                                     material.executableBundlePath, material.executableDescriptor) ||
        (material.application.runtime == SandboxRuntime::JavaScript &&
         !isRegularDescriptor(material.runtimeDescriptor, true))) {
        if (error.empty()) {
            error = "retained sandbox launch material changed or became unsafe";
        }
        return false;
    }

    spec.plan = plan;
    spec.hardening = hardening;
    spec.identity = material.application.identity;
    spec.cgroup = std::move(cgroup);
    spec.filesystemSources = material.filesystemSources;
    spec.executableBundlePath = material.executableBundlePath;
    spec.executableDescriptor = material.executableDescriptor;
    spec.runtimeDescriptor = material.runtimeDescriptor;
    spec.arguments = material.arguments;
    return validateSandboxChildLaunchSpec(spec, error);
}

} // namespace lcl::security
