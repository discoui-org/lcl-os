#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "system/security/sandbox_filesystem_namespace.hpp"

#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace lcl::security {
namespace {

constexpr const char* kSandboxRootMountpoint = "/Runtime/.lcl-sandbox-root";
constexpr const char* kSandboxRuntimeParent = "/Runtime";
constexpr const char* kSandboxTemporarySize = "size=64M";
constexpr std::size_t kMaximumPinnedSourcePathBytes = 4096;

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
    int release() noexcept { return std::exchange(descriptor_, -1); }
    bool valid() const noexcept { return descriptor_ >= 0; }

private:
    int descriptor_{-1};
};

bool isRootControlledDirectory(const char* path, std::string& error) {
    struct stat status {};
    if (lstat(path, &status) != 0 || !S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode) ||
        status.st_uid != 0 || status.st_gid != 0 || (status.st_mode & 0022) != 0) {
        error = std::string("sandbox mount parent is not root-controlled: ") + path;
        return false;
    }
    return true;
}

bool ensureDirectory(const std::string& path, mode_t mode, std::string& error) {
    if (mkdir(path.c_str(), mode) != 0 && errno != EEXIST) {
        error = std::string("could not create sandbox mount target '") + path + "': " +
                std::strerror(errno);
        return false;
    }
    struct stat status {};
    if (lstat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode) ||
        chmod(path.c_str(), mode) != 0) {
        error = std::string("sandbox mount target is unsafe: ") + path;
        return false;
    }
    return true;
}

bool createSystemLibraryLink(const std::string& path, const char* target, std::string& error) {
    if (symlink(target, path.c_str()) == 0) {
        return true;
    }
    error = std::string("could not create sandbox dynamic-library link '") + path + "': " +
            std::strerror(errno);
    return false;
}

bool mountTmpfs(const std::string& target, std::string_view options, bool allowDeviceNodes,
                std::string& error) {
    unsigned long mountFlags = MS_NOSUID | MS_NOEXEC;
    if (!allowDeviceNodes) {
        mountFlags |= MS_NODEV;
    }
    if (mount("tmpfs", target.c_str(), "tmpfs", mountFlags, std::string(options).c_str()) == 0) {
        return true;
    }
    error = std::string("could not mount sandbox tmpfs at '") + target + "': " +
            std::strerror(errno);
    return false;
}

bool bindMountFromDirectoryDescriptor(int sourceDescriptor, const std::string& target,
                                      bool readOnly, bool noExecute, std::string& error) {
    // mount(2) accepts a relative source path.  Resolve "." after temporarily
    // changing to the already verified directory descriptor, so the source
    // stays descriptor-pinned without relying on the QEMU kernel's procfs
    // magic-link or empty-path mount variants.
    ScopedFd previousDirectory(open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!previousDirectory.valid()) {
        error = std::string("could not preserve sandbox working directory: ") + std::strerror(errno);
        return false;
    }
    if (fchdir(sourceDescriptor) != 0) {
        error = std::string("could not select protected sandbox mount source: ") + std::strerror(errno);
        return false;
    }
    const bool mounted = mount(".", target.c_str(), nullptr, MS_BIND, nullptr) == 0;
    const int mountError = errno;
    if (fchdir(previousDirectory.get()) != 0) {
        error = std::string("could not restore sandbox working directory: ") + std::strerror(errno);
        return false;
    }
    if (!mounted) {
        error = std::string("could not bind protected sandbox source at '") + target + "': " +
                std::strerror(mountError);
        return false;
    }
    unsigned long flags = MS_BIND | MS_REMOUNT | MS_NOSUID | MS_NODEV;
    if (readOnly) {
        flags |= MS_RDONLY;
    }
    if (noExecute) {
        flags |= MS_NOEXEC;
    }
    if (mount(nullptr, target.c_str(), nullptr, flags, nullptr) == 0) {
        return true;
    }
    error = std::string("could not harden sandbox bind mount at '") + target + "': " +
            std::strerror(errno);
    return false;
}

bool bindMountTrustedSysfs(int sourceDescriptor, const std::string& target,
                           std::string& error) {
    // sysfs rejects the fchdir(".") bind pattern used for ordinary
    // directories on the QEMU kernel.  The descriptor was opened and
    // filesystem-type validated by sandboxd before namespace isolation; bind
    // the immutable kernel mountpoint only after inode-matching it to that FD.
    struct stat pinned {};
    struct stat current {};
    if (fstat(sourceDescriptor, &pinned) != 0 || stat("/sys", &current) != 0 ||
        !S_ISDIR(pinned.st_mode) || pinned.st_dev != current.st_dev ||
        pinned.st_ino != current.st_ino) {
        error = "trusted graphics sysfs mount changed before sandbox bind";
        return false;
    }
    if (mount("/sys", target.c_str(), nullptr, MS_BIND, nullptr) != 0) {
        error = std::string("could not bind trusted graphics sysfs: ") +
                std::strerror(errno);
        return false;
    }
    const unsigned long flags = MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID |
                                MS_NODEV | MS_NOEXEC;
    if (mount(nullptr, target.c_str(), nullptr, flags, nullptr) == 0) {
        return true;
    }
    error = std::string("could not harden trusted graphics sysfs bind: ") +
            std::strerror(errno);
    return false;
}

struct SandboxDirectorySourcePaths final {
    std::string appBundle;
    std::string system;
    std::string data;
    std::string cache;
    std::string preferences;
    std::string compositorSocket;
    std::string rasterSocket;
    std::string gpuSocket;
};

bool capturePinnedDirectoryPath(int descriptor, std::string& path, std::string& error) {
    const std::string descriptorLink = "/proc/self/fd/" + std::to_string(descriptor);
    std::array<char, kMaximumPinnedSourcePathBytes> bytes{};
    const ssize_t count = readlink(descriptorLink.c_str(), bytes.data(), bytes.size() - 1);
    if (count <= 0 || static_cast<std::size_t>(count) >= bytes.size() - 1) {
        error = std::string("could not resolve protected sandbox source: ") + std::strerror(errno);
        return false;
    }
    path.assign(bytes.data(), static_cast<std::size_t>(count));
    if (path.front() != '/' || path.ends_with(" (deleted)")) {
        error = "protected sandbox source does not have a stable absolute path";
        return false;
    }
    return true;
}

bool captureSandboxDirectorySourcePaths(const SandboxFilesystemSources& sources,
                                        SandboxDirectorySourcePaths& paths, std::string& error) {
    return capturePinnedDirectoryPath(sources.appBundleDescriptor, paths.appBundle, error) &&
           capturePinnedDirectoryPath(sources.systemDescriptor, paths.system, error) &&
           capturePinnedDirectoryPath(sources.dataDescriptor, paths.data, error) &&
           capturePinnedDirectoryPath(sources.cacheDescriptor, paths.cache, error) &&
           capturePinnedDirectoryPath(sources.preferencesDescriptor, paths.preferences, error) &&
           capturePinnedDirectoryPath(sources.compositorSocketDescriptor, paths.compositorSocket, error) &&
           capturePinnedDirectoryPath(sources.rasterSocketDescriptor, paths.rasterSocket, error) &&
           (sources.gpuSocketDescriptor < 0 ||
            capturePinnedDirectoryPath(sources.gpuSocketDescriptor, paths.gpuSocket, error));
}

bool bindMountRuntimeSocket(int sourceDescriptor, const std::string& sourcePath,
                            const std::string& target, std::string& error) {
    struct stat expected {};
    struct stat current {};
    if (fstat(sourceDescriptor, &expected) != 0 || lstat(sourcePath.c_str(), &current) != 0 ||
        !S_ISSOCK(expected.st_mode) || !S_ISSOCK(current.st_mode) ||
        expected.st_dev != current.st_dev || expected.st_ino != current.st_ino) {
        error = "protected graphics endpoint changed before sandbox mount";
        return false;
    }
    ScopedFd placeholder(open(target.c_str(), O_CREAT | O_EXCL | O_RDONLY | O_CLOEXEC, 0600));
    if (!placeholder.valid()) {
        error = std::string("could not create sandbox graphics endpoint target: ") +
                std::strerror(errno);
        return false;
    }
    if (mount(sourcePath.c_str(), target.c_str(), nullptr, MS_BIND, nullptr) != 0) {
        error = std::string("could not bind sandbox graphics endpoint: ") + std::strerror(errno);
        return false;
    }
    return true;
}

ScopedFd reopenPinnedDirectoryInCurrentMountNamespace(int originalDescriptor,
                                                       const std::string& sourcePath,
                                                       std::string& error) {
    ScopedFd reopened(open(sourcePath.c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!reopened.valid()) {
        error = std::string("could not reopen verified sandbox source in its private mount namespace: ") +
                std::strerror(errno);
        return ScopedFd{};
    }
    struct stat originalStatus {};
    struct stat reopenedStatus {};
    if (fstat(originalDescriptor, &originalStatus) != 0 ||
        fstat(reopened.get(), &reopenedStatus) != 0) {
        error = std::string("could not inspect reopened sandbox source: ") +
                std::strerror(errno);
        return ScopedFd{};
    }
    if (originalStatus.st_dev != reopenedStatus.st_dev ||
        originalStatus.st_ino != reopenedStatus.st_ino || !S_ISDIR(reopenedStatus.st_mode)) {
        error = "reopened sandbox source no longer matches its verified descriptor";
        return ScopedFd{};
    }
    return reopened;
}

bool createMinimalDeviceTree(const std::string& target, std::string& error) {
    // /dev/null is the only device node created here.  Its private /dev
    // tmpfs must not be nodev or the kernel rejects opening that node, which
    // prevents the child from redirecting stdin, stdout, and stderr.
    if (!mountTmpfs(target, "mode=0755", true, error)) {
        return false;
    }
    const std::string nullDevice = target + "/null";
    if (mknod(nullDevice.c_str(), S_IFCHR | 0666, makedev(1, 3)) != 0 && errno != EEXIST) {
        error = std::string("could not create sandbox /dev/null: ") + std::strerror(errno);
        return false;
    }
    struct stat status {};
    if (lstat(nullDevice.c_str(), &status) != 0 || !S_ISCHR(status.st_mode) ||
        major(status.st_rdev) != 1 || minor(status.st_rdev) != 3 || status.st_uid != 0 ||
        status.st_gid != 0 || chmod(nullDevice.c_str(), 0666) != 0) {
        error = "sandbox /dev/null is unsafe";
        return false;
    }
    return true;
}

bool addPrivateRenderNode(int descriptor, const std::string& devices,
                          const AppIdentity& identity, std::string& error) {
    if (descriptor < 0) return true;
    struct stat source {};
    if (fstat(descriptor, &source) != 0 || !S_ISCHR(source.st_mode) ||
        major(source.st_rdev) != 226 || minor(source.st_rdev) < 128 ||
        minor(source.st_rdev) > 255) {
        error = "trusted DRM render-node descriptor is invalid";
        return false;
    }
    const std::string dri = devices + "/dri";
    if (!ensureDirectory(dri, 0755, error)) return false;
    const std::string node = dri + "/renderD" + std::to_string(minor(source.st_rdev));
    if (mknod(node.c_str(), S_IFCHR | 0600, source.st_rdev) != 0) {
        error = std::string("could not create private DRM render node: ") + std::strerror(errno);
        return false;
    }
    if (chown(node.c_str(), identity.uid, identity.gid) != 0 ||
        chmod(node.c_str(), 0600) != 0) {
        error = std::string("could not assign private DRM render node: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool mountPrivateProc(const std::string& target, std::string& error) {
    if (mount("proc", target.c_str(), "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) == 0) {
        return true;
    }
    error = std::string("could not mount private sandbox /proc: ") + std::strerror(errno);
    return false;
}

ScopedFd openSandboxDirectory(const char* path, std::string& error) {
    ScopedFd descriptor(open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!descriptor.valid()) {
        error = std::string("could not open prepared sandbox directory '") + path + "': " +
                std::strerror(errno);
    }
    return descriptor;
}

} // namespace

bool enterSandboxFilesystemNamespace(const SandboxFilesystemSources& sources,
                                    const AppIdentity& identity,
                                    int executableDescriptor,
                                    bool executableMayRun,
                                    bool requirePidNamespaceInit,
                                    SandboxLandlockRules& landlockRules,
                                    std::string& error) {
    error.clear();
    landlockRules = {};
    if (!validateSandboxFilesystemSources(sources, identity, error)) {
        return false;
    }
    struct stat executableStatus {};
    if (executableDescriptor < 0 || fstat(executableDescriptor, &executableStatus) != 0 ||
        !S_ISREG(executableStatus.st_mode) || executableStatus.st_nlink != 1 ||
        (executableMayRun &&
         (executableStatus.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)) {
        error = "sandbox executable descriptor is not a verified regular file";
        return false;
    }
    if (geteuid() != 0 || (requirePidNamespaceInit && getpid() != 1)) {
        error = "sandbox filesystem setup requires root and, when selected, the PID-namespace init child";
        return false;
    }
    // A file descriptor opened before unshare(CLONE_NEWNS) keeps a reference
    // to its originating mount namespace.  Record each protected path now,
    // then reopen and inode-match it below after the new namespace exists.
    SandboxDirectorySourcePaths sourcePaths{};
    if (!captureSandboxDirectorySourcePaths(sources, sourcePaths, error)) {
        return false;
    }
    if (unshare(CLONE_NEWNS) != 0 || mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        error = std::string("could not isolate sandbox mount propagation: ") + std::strerror(errno);
        return false;
    }
    ScopedFd appSource = reopenPinnedDirectoryInCurrentMountNamespace(
        sources.appBundleDescriptor, sourcePaths.appBundle, error);
    ScopedFd systemSource = reopenPinnedDirectoryInCurrentMountNamespace(
        sources.systemDescriptor, sourcePaths.system, error);
    ScopedFd dataSource = reopenPinnedDirectoryInCurrentMountNamespace(
        sources.dataDescriptor, sourcePaths.data, error);
    ScopedFd cacheSource = reopenPinnedDirectoryInCurrentMountNamespace(
        sources.cacheDescriptor, sourcePaths.cache, error);
    ScopedFd preferencesSource = reopenPinnedDirectoryInCurrentMountNamespace(
        sources.preferencesDescriptor, sourcePaths.preferences, error);
    if (!appSource.valid() || !systemSource.valid() || !dataSource.valid() ||
        !cacheSource.valid() || !preferencesSource.valid()) {
        return false;
    }
    if (!isRootControlledDirectory(kSandboxRuntimeParent, error) ||
        !ensureDirectory(kSandboxRootMountpoint, 0700, error) ||
        !mountTmpfs(kSandboxRootMountpoint, "mode=0755,size=32M", false, error)) {
        return false;
    }

    const std::string root(kSandboxRootMountpoint);
    const std::string app = root + "/App";
    const std::string system = root + "/System";
    const std::string data = root + "/Data";
    const std::string cache = root + "/Cache";
    const std::string preferences = root + "/Preferences";
    const std::string temporary = root + "/Temporary";
    const std::string user = root + "/usr";
    const std::string devices = root + "/dev";
    const std::string proc = root + "/proc";
    const std::string sysfs = root + "/sys";
    const std::string share = user + "/share";
    const std::string etc = root + "/etc";
    const std::string runtime = root + "/Runtime";
    for (const std::string* target : {&app, &system, &data, &cache, &preferences, &temporary,
                                      &user, &devices, &proc, &runtime}) {
        if (!ensureDirectory(*target, 0755, error)) {
            return false;
        }
    }
    if (sources.sysfsDescriptor >= 0 && !ensureDirectory(sysfs, 0755, error)) {
        return false;
    }

    if (!bindMountFromDirectoryDescriptor(appSource.get(), app, true, false, error) ||
        !bindMountFromDirectoryDescriptor(systemSource.get(), system, true, false, error) ||
        !bindMountFromDirectoryDescriptor(dataSource.get(), data, false, true, error) ||
        !bindMountFromDirectoryDescriptor(cacheSource.get(), cache, false, true, error) ||
        !bindMountFromDirectoryDescriptor(preferencesSource.get(), preferences, false, true, error)) {
        return false;
    }
    if (!bindMountRuntimeSocket(sources.compositorSocketDescriptor, sourcePaths.compositorSocket,
                                runtime + "/lcl-compositor.sock", error) ||
        !bindMountRuntimeSocket(sources.rasterSocketDescriptor, sourcePaths.rasterSocket,
                                runtime + "/lcl-raster.sock", error)) {
        return false;
    }
    if (sources.gpuSocketDescriptor >= 0 &&
        !bindMountRuntimeSocket(sources.gpuSocketDescriptor, sourcePaths.gpuSocket,
                                runtime + "/lcl-gpu.sock", error)) {
        return false;
    }
    // Mesa/libdrm discovers the DRM device topology through sysfs.  This is a
    // non-recursive bind of the verified sysfs root: it deliberately excludes
    // subordinate mounts such as cgroup2, is read-only, and exists only for a
    // bundle that holds graphics.render-node.
    if (sources.sysfsDescriptor >= 0 &&
        !bindMountTrustedSysfs(sources.sysfsDescriptor, sysfs, error)) {
        return false;
    }
    // The kernel resolves an ELF interpreter before it starts the app.  LCL's
    // canonical rootfs keeps that loader and shared objects under /System;
    // expose only root-owned links to that tree instead of bind-mounting the
    // host's /lib or /usr hierarchy.
    if (!createSystemLibraryLink(root + "/lib", "System/Library/Libraries", error) ||
        !createSystemLibraryLink(root + "/lib64", "System/Library/Libraries", error) ||
        !createSystemLibraryLink(user + "/lib", "../System/Library/Libraries", error) ||
        !createSystemLibraryLink(user + "/lib64", "../System/Library/Libraries", error)) {
        return false;
    }
    if (sources.renderNodeDescriptor >= 0) {
        // GLVND discovers Mesa's EGL vendor JSON at /usr/share/glvnd.  The
        // sandbox builds its own /usr, so restore only this root-controlled
        // metadata path for the explicit graphics capability.
        if (!ensureDirectory(share, 0755, error) || !ensureDirectory(etc, 0755, error) ||
            !createSystemLibraryLink(share + "/glvnd", "../../System/Library/EGL/glvnd", error) ||
            !createSystemLibraryLink(etc + "/glvnd", "../System/Library/EGL/glvnd", error)) {
            return false;
        }
    }
    const std::string temporaryOptions = "mode=0700,uid=" + std::to_string(identity.uid) +
                                         ",gid=" + std::to_string(identity.gid) + "," +
                                         kSandboxTemporarySize;
    if (!mountTmpfs(temporary, temporaryOptions, false, error) ||
        !createMinimalDeviceTree(devices, error) ||
        !addPrivateRenderNode(sources.renderNodeDescriptor, devices, identity, error) ||
        !mountPrivateProc(proc, error)) {
        return false;
    }

    if (chdir(root.c_str()) != 0 || chroot(".") != 0 || chdir("/") != 0) {
        error = std::string("could not enter private sandbox filesystem: ") + std::strerror(errno);
        return false;
    }
    ScopedFd temporaryDescriptor = openSandboxDirectory("/Temporary", error);
    if (!temporaryDescriptor.valid()) {
        return false;
    }
    ScopedFd deviceDescriptor = openSandboxDirectory("/dev", error);
    if (!deviceDescriptor.valid()) {
        return false;
    }
    ScopedFd sysfsDescriptor;
    if (sources.sysfsDescriptor >= 0) {
        sysfsDescriptor = openSandboxDirectory("/sys", error);
        if (!sysfsDescriptor.valid()) {
            return false;
        }
    }

    SandboxLandlockRules prepared{};
    prepared.identity = identity;
    prepared.filesystemSources = sources;
    prepared.executableDescriptor = executableDescriptor;
    prepared.temporaryDescriptor = temporaryDescriptor.get();
    prepared.deviceDescriptor = deviceDescriptor.get();
    prepared.sysfsDescriptor = sysfsDescriptor.get();
    if (!validateSandboxLandlockRules(prepared, error)) {
        return false;
    }
    landlockRules = prepared;
    landlockRules.temporaryDescriptor = temporaryDescriptor.release();
    landlockRules.deviceDescriptor = deviceDescriptor.release();
    landlockRules.sysfsDescriptor = sysfsDescriptor.release();
    return true;
}

} // namespace lcl::security
