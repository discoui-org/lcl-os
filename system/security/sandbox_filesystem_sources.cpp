#include "system/security/sandbox_filesystem_sources.hpp"
#include "system/security/session_user.hpp"

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <linux/magic.h>
#include <array>
#include <cerrno>
#include <cstring>
namespace lcl::security {
namespace {

constexpr std::size_t kMaximumPinnedEndpointPathBytes = 4096;

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

bool isApplicationRuntimeSocket(int descriptor) {
    struct stat status {};
    return descriptor >= 0 && fstat(descriptor, &status) == 0 && S_ISSOCK(status.st_mode) &&
           status.st_uid == kSessionUserUid && status.st_gid == kApplicationRuntimeGid &&
           (status.st_mode & 0777) == 0660;
}

bool isDrmRenderNode(int descriptor) {
    struct stat status {};
    // DRM assigns render nodes to major 226 and reserves minor 128..255 for
    // non-KMS render access. The descriptor itself is opened by sandboxd.
    return descriptor >= 0 && fstat(descriptor, &status) == 0 &&
           S_ISCHR(status.st_mode) && major(status.st_rdev) == 226 &&
           minor(status.st_rdev) >= 128 && minor(status.st_rdev) <= 255;
}

bool isSysfsDirectory(int descriptor) {
    struct stat status {};
    struct statfs filesystem {};
    return descriptor >= 0 && fstat(descriptor, &status) == 0 &&
           fstatfs(descriptor, &filesystem) == 0 && S_ISDIR(status.st_mode) &&
           status.st_uid == 0 && status.st_gid == 0 &&
           filesystem.f_type == SYSFS_MAGIC;
}

bool endpointDescriptorStillNamesLinkedSocket(int descriptor, std::string& error) {
    struct stat expected {};
    if (!isApplicationRuntimeSocket(descriptor) || fstat(descriptor, &expected) != 0) {
        error = "sandbox graphics endpoint descriptor is unsafe";
        return false;
    }

    const std::string descriptorLink = "/proc/self/fd/" + std::to_string(descriptor);
    std::array<char, kMaximumPinnedEndpointPathBytes> bytes{};
    const ssize_t count = readlink(descriptorLink.c_str(), bytes.data(), bytes.size() - 1);
    if (count <= 0 || static_cast<std::size_t>(count) >= bytes.size() - 1) {
        error = std::string("could not resolve protected graphics endpoint: ") +
                std::strerror(errno);
        return false;
    }
    const std::string path(bytes.data(), static_cast<std::size_t>(count));
    if (path.ends_with(" (deleted)")) {
        // unlink(2) leaves the retained descriptor usable but the endpoint has
        // moved to a new socket generation. Treat this as the same stale
        // generation condition as an inode mismatch.
        error = "protected graphics endpoint generation changed";
        return false;
    }
    if (path.empty() || path.front() != '/') {
        error = "protected graphics endpoint no longer has a stable path";
        return false;
    }

    struct stat current {};
    if (lstat(path.c_str(), &current) != 0 || !S_ISSOCK(current.st_mode) ||
        current.st_dev != expected.st_dev || current.st_ino != expected.st_ino) {
        error = "protected graphics endpoint generation changed";
        return false;
    }
    return true;
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
    if (!isApplicationRuntimeSocket(sources.compositorSocketDescriptor) ||
        !isApplicationRuntimeSocket(sources.rasterSocketDescriptor)) {
        error = "sandbox graphics source is not a protected application runtime socket";
        return false;
    }
    if (sources.gpuSocketDescriptor >= 0 &&
        !isApplicationRuntimeSocket(sources.gpuSocketDescriptor)) {
        error = "sandbox GPU source is not a protected application runtime socket";
        return false;
    }
    const bool hasRenderNode = sources.renderNodeDescriptor >= 0;
    if (hasRenderNode && !isDrmRenderNode(sources.renderNodeDescriptor)) {
        error = "sandbox render-node source is not a DRM render device";
        return false;
    }
    if (hasRenderNode != (sources.sysfsDescriptor >= 0)) {
        error = "sandbox render-node and sysfs sources must be paired";
        return false;
    }
    if (sources.sysfsDescriptor >= 0 && !isSysfsDirectory(sources.sysfsDescriptor)) {
        error = "sandbox graphics sysfs source is not the trusted sysfs mount";
        return false;
    }
    return true;
}

bool validateSandboxRuntimeEndpointGeneration(const SandboxFilesystemSources& sources,
                                              std::string& error) {
    error.clear();
    return endpointDescriptorStillNamesLinkedSocket(sources.compositorSocketDescriptor, error) &&
           endpointDescriptorStillNamesLinkedSocket(sources.rasterSocketDescriptor, error) &&
           (sources.gpuSocketDescriptor < 0 ||
            endpointDescriptorStillNamesLinkedSocket(sources.gpuSocketDescriptor, error));
}

} // namespace lcl::security
