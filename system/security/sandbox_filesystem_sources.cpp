#include "system/security/sandbox_filesystem_sources.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <array>
namespace lcl::security {
namespace {

bool descriptorStatus(int descriptor, struct stat& status) {
    return descriptor >= 0 && fstat(descriptor, &status) == 0 && S_ISDIR(status.st_mode);
}

bool isRootControlledSystemDirectory(int descriptor) {
    struct stat status {};
    return descriptorStatus(descriptor, status) && status.st_uid == 0 && status.st_gid == 0 &&
           (status.st_mode & 0022) == 0;
}

bool isPrivateAppDirectory(int descriptor, const AppIdentity& identity) {
    struct stat status {};
    return descriptorStatus(descriptor, status) && status.st_uid == identity.uid &&
           status.st_gid == identity.gid && (status.st_mode & 0777) == 0700;
}

} // namespace

bool isSafeSandboxBundleRelativePath(const std::string& path) noexcept {
    if (path.empty() || path.size() > 1024 || path.front() == '/' ||
        path.find('\\') != std::string::npos || path.find('\0') != std::string::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start < path.size()) {
        const std::size_t end = path.find('/', start);
        const std::size_t componentSize = (end == std::string::npos ? path.size() : end) - start;
        if (componentSize == 0 ||
            (componentSize == 1 && path[start] == '.') ||
            (componentSize == 2 && path[start] == '.' && path[start + 1] == '.')) {
            return false;
        }
        for (std::size_t index = start; index < start + componentSize; ++index) {
            const unsigned char character = static_cast<unsigned char>(path[index]);
            if (character < 0x20 || character == 0x7F) {
                return false;
            }
        }
        if (end == std::string::npos) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

bool validateSandboxFilesystemSources(const SandboxFilesystemSources& sources,
                                     const AppIdentity& identity, std::string& error) {
    error.clear();
    if (!AppIdentityRegistry::isValidAppId(identity.appId) || identity.uid == 0 || identity.gid == 0 ||
        identity.uid != identity.gid) {
        error = "sandbox filesystem sources have an invalid app identity";
        return false;
    }

    struct stat bundleStatus {};
    if (!descriptorStatus(sources.appBundleDescriptor, bundleStatus)) {
        error = "sandbox app bundle source is not a directory descriptor";
        return false;
    }
    if (!isRootControlledSystemDirectory(sources.systemDescriptor)) {
        error = "sandbox system source is not a root-controlled directory";
        return false;
    }
    constexpr std::array<int SandboxFilesystemSources::*, 3> kPrivateDirectories = {
        &SandboxFilesystemSources::dataDescriptor,
        &SandboxFilesystemSources::cacheDescriptor,
        &SandboxFilesystemSources::preferencesDescriptor,
    };
    for (int SandboxFilesystemSources::*member : kPrivateDirectories) {
        if (!isPrivateAppDirectory(sources.*member, identity)) {
            error = "sandbox app storage source is not app-owned 0700";
            return false;
        }
    }
    return true;
}

} // namespace lcl::security
