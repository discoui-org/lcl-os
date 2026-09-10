#include "system/security/session_user.hpp"

#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <linux/securebits.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <cstdlib>
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

bool clearCapabilities() {
    __user_cap_header_struct header{};
    header.version = _LINUX_CAPABILITY_VERSION_3;
    std::array<__user_cap_data_struct, 2> capabilities{};
    return syscall(SYS_capset, &header, capabilities.data()) == 0;
}

bool clearAmbientCapabilities() {
#if defined(PR_CAP_AMBIENT) && defined(PR_CAP_AMBIENT_CLEAR_ALL)
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) == 0) {
        return true;
    }
    // Kernels predating ambient capabilities report EINVAL and therefore have
    // no ambient set to clear.
    return errno == EINVAL;
#else
    return true;
#endif
}

bool credentialDropFailed(std::string& error, const char* operation,
                          int operationError = errno) {
    error = std::string("could not drop to the session user (") + operation + "): " +
            std::strerror(operationError);
    return false;
}

bool ensureSessionDirectoryAt(int parentDescriptor, const char* name, ScopedFd& directory,
                              std::string& error) {
    if (mkdirat(parentDescriptor, name, 0700) != 0 && errno != EEXIST) {
        error = std::string("could not create session directory '") + name + "': " +
                std::strerror(errno);
        return false;
    }
    ScopedFd opened(openat(parentDescriptor, name,
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat status {};
    if (!opened.valid() || fstat(opened.get(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode)) {
        error = std::string("session path '") + name + "' is not a safe directory";
        return false;
    }
    if (fchown(opened.get(), kSessionUserUid, kSessionUserGid) != 0 ||
        fchmod(opened.get(), 0700) != 0) {
        error = std::string("could not secure session directory '") + name + "': " +
                std::strerror(errno);
        return false;
    }
    directory = std::move(opened);
    return true;
}

/**
 * The per-user Containers root is an authority boundary, not user storage:
 * sandboxd creates and repairs every child app container below it.  The
 * interactive session user may traverse it to reach its trusted Terminal
 * container, but cannot list, create, rename, or replace another app's root.
 */
bool ensureRootContainerDirectoryAt(int parentDescriptor, ScopedFd& directory, std::string& error) {
    constexpr const char* kContainersName = "Containers";
    if (mkdirat(parentDescriptor, kContainersName, 0711) != 0 && errno != EEXIST) {
        error = std::string("could not create app containers root: ") + std::strerror(errno);
        return false;
    }
    ScopedFd opened(openat(parentDescriptor, kContainersName,
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat status {};
    if (!opened.valid() || fstat(opened.get(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode)) {
        error = "app containers root is not a safe directory";
        return false;
    }
    if (fchown(opened.get(), 0, 0) != 0 || fchmod(opened.get(), 0711) != 0) {
        error = std::string("could not secure app containers root: ") + std::strerror(errno);
        return false;
    }
    directory = std::move(opened);
    return true;
}

bool secureOptionalSessionProfile(int homeDescriptor, const char* fileName, std::string& error) {
    ScopedFd profile(openat(homeDescriptor, fileName, O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!profile.valid()) {
        if (errno == ENOENT) {
            return true;
        }
        error = std::string("could not open session ") + fileName + " safely: " +
                std::strerror(errno);
        return false;
    }
    struct stat status {};
    if (fstat(profile.get(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_nlink != 1 ||
        fchown(profile.get(), kSessionUserUid, kSessionUserGid) != 0 ||
        fchmod(profile.get(), 0600) != 0) {
        error = std::string("session ") + fileName + " is not a safe private regular file";
        return false;
    }
    return true;
}

bool setEnvironmentVariable(const char* name, const std::string& value, std::string& error) {
    if (setenv(name, value.c_str(), 1) == 0) {
        return true;
    }
    error = std::string("could not set trusted shell environment variable '") + name + "': " +
            std::strerror(errno);
    return false;
}

} // namespace

bool dropToSessionUser(std::string& error) {
    error.clear();
    if (geteuid() != 0) {
        return true;
    }

    const int secureBits = prctl(PR_GET_SECUREBITS, 0, 0, 0, 0);
    if (secureBits < 0) {
        return credentialDropFailed(error, "PR_GET_SECUREBITS");
    }

    // Android root brokers can enter with KEEP_CAPS locked. In that state a
    // redundant PR_SET_KEEPCAPS(0) fails with EPERM before the UID transition.
    // Continue through the transition and explicitly clear every capability
    // set below; KEEP_CAPS itself is cleared by execve.
    if ((secureBits & SECBIT_KEEP_CAPS_LOCKED) == 0 &&
        prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0) != 0) {
        return credentialDropFailed(error, "PR_SET_KEEPCAPS");
    }
    if (setgroups(0, nullptr) != 0) {
        return credentialDropFailed(error, "setgroups");
    }
    if (setresgid(kSessionUserGid, kSessionUserGid, kSessionUserGid) != 0) {
        return credentialDropFailed(error, "setresgid");
    }
    if (setresuid(kSessionUserUid, kSessionUserUid, kSessionUserUid) != 0) {
        return credentialDropFailed(error, "setresuid");
    }
    if (!clearAmbientCapabilities()) {
        return credentialDropFailed(error, "PR_CAP_AMBIENT_CLEAR_ALL");
    }
    if (!clearCapabilities()) {
        return credentialDropFailed(error, "capset");
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return credentialDropFailed(error, "PR_SET_NO_NEW_PRIVS");
    }
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
        return credentialDropFailed(error, "PR_SET_DUMPABLE");
    }
    if (geteuid() != kSessionUserUid || getegid() != kSessionUserGid ||
        getgroups(0, nullptr) != 0) {
        error = "could not drop to the session user (credential verification failed)";
        return false;
    }
    return true;
}

bool isTrustedUserShellBundle(std::string_view appId, std::string_view bundlePath) noexcept {
    return appId == kTrustedUserShellAppId && bundlePath == kTrustedUserShellBundlePath;
}

bool isSystemSettingsBundle(std::string_view appId, std::string_view bundlePath) noexcept {
    return appId == kSystemSettingsAppId && bundlePath == kSystemSettingsBundlePath;
}

bool prepareTrustedUserShellEnvironment(uint64_t instanceId, std::string& error) {
    error.clear();
    if (clearenv() != 0) {
        error = std::string("could not clear inherited process environment: ") +
                std::strerror(errno);
        return false;
    }

    // Never inherit loader, PATH, HOME or authority-related variables from
    // init/sessiond. Every executable location below is rootfs-controlled.
    if (!setEnvironmentVariable("HOME", kSessionUserHome, error) ||
        !setEnvironmentVariable("USER", kSessionUserName, error) ||
        !setEnvironmentVariable("LOGNAME", kSessionUserName, error) ||
        !setEnvironmentVariable("SHELL", "/System/Tools/bash", error) ||
        !setEnvironmentVariable("PATH", "/System/Core:/System/Tools", error) ||
        !setEnvironmentVariable("TERM", "xterm-256color", error) ||
        !setEnvironmentVariable("LCL_APP_ID", kTrustedUserShellAppId, error) ||
        !setEnvironmentVariable("LCL_APP_INSTANCE_ID", std::to_string(instanceId), error) ||
        !setEnvironmentVariable("LCL_LAUNCH_PROFILE", kTrustedUserShellProfileId, error)) {
        return false;
    }
    umask(0077);
    return true;
}

bool prepareSystemSettingsEnvironment(uint64_t instanceId, std::string& error) {
    error.clear();
    // Settings normally receives a fixed, capability-free environment. Frame
    // tracing is the one developer-only diagnostic admitted from sessiond;
    // accept exactly the boolean enable value, never arbitrary variables.
    const char* traceFrames = std::getenv("LCL_TRACE_FRAMES");
    const bool frameTraceEnabled = traceFrames &&
        std::strcmp(traceFrames, "1") == 0;
    if (clearenv() != 0) {
        error = std::string("could not clear inherited process environment: ") +
                std::strerror(errno);
        return false;
    }
    if (!setEnvironmentVariable("HOME", kSessionUserHome, error) ||
        !setEnvironmentVariable("USER", kSessionUserName, error) ||
        !setEnvironmentVariable("LOGNAME", kSessionUserName, error) ||
        !setEnvironmentVariable("PATH", "/System/Core", error) ||
        !setEnvironmentVariable("LCL_APP_ID", kSystemSettingsAppId, error) ||
        !setEnvironmentVariable("LCL_APP_INSTANCE_ID", std::to_string(instanceId), error) ||
        !setEnvironmentVariable("LCL_LAUNCH_PROFILE", kSystemSettingsProfileId, error) ||
        !setEnvironmentVariable("LCL_SECURITY_ADMIN_FD", "4", error)) {
        return false;
    }
    if (frameTraceEnabled &&
        !setEnvironmentVariable("LCL_TRACE_FRAMES", "1", error)) {
        return false;
    }
    umask(0077);
    return true;
}

bool provisionSessionUserHome(std::string& error) {
    error.clear();
    if (geteuid() != 0) {
        error = "session home provisioning requires root";
        return false;
    }
    ScopedFd users(open("/Users", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat usersStatus {};
    if (!users.valid() || fstat(users.get(), &usersStatus) != 0 || !S_ISDIR(usersStatus.st_mode) ||
        usersStatus.st_uid != 0 || usersStatus.st_gid != 0 || (usersStatus.st_mode & 0022) != 0) {
        error = "/Users is not a root-controlled directory";
        return false;
    }

    ScopedFd home;
    ScopedFd applications;
    ScopedFd desktop;
    ScopedFd documents;
    ScopedFd downloads;
    ScopedFd library;
    ScopedFd containers;
    ScopedFd terminalContainer;
    ScopedFd data;
    ScopedFd cache;
    ScopedFd preferences;
    ScopedFd temporary;
    if (!ensureSessionDirectoryAt(users.get(), kSessionUserName, home, error) ||
        !ensureSessionDirectoryAt(home.get(), "Applications", applications, error) ||
        !ensureSessionDirectoryAt(home.get(), "Desktop", desktop, error) ||
        !ensureSessionDirectoryAt(home.get(), "Documents", documents, error) ||
        !ensureSessionDirectoryAt(home.get(), "Downloads", downloads, error) ||
        !ensureSessionDirectoryAt(home.get(), "Library", library, error) ||
        !ensureRootContainerDirectoryAt(library.get(), containers, error) ||
        !ensureSessionDirectoryAt(containers.get(), "org.lcl.terminal", terminalContainer, error) ||
        !ensureSessionDirectoryAt(terminalContainer.get(), "Data", data, error) ||
        !ensureSessionDirectoryAt(terminalContainer.get(), "Cache", cache, error) ||
        !ensureSessionDirectoryAt(terminalContainer.get(), "Preferences", preferences, error) ||
        !ensureSessionDirectoryAt(terminalContainer.get(), "Temporary", temporary, error) ||
        !secureOptionalSessionProfile(home.get(), ".bashrc", error) ||
        !secureOptionalSessionProfile(home.get(), ".profile", error)) {
        return false;
    }
    return true;
}

bool assignSessionUserOwnership(const std::string& path, mode_t mode, std::string& error) {
    error.clear();
    if (geteuid() != 0) {
        if (chmod(path.c_str(), mode) != 0) {
            error = std::string("could not secure session runtime object '") + path + "': " +
                    std::strerror(errno);
            return false;
        }
        return true;
    }
    struct stat status {};
    if (lstat(path.c_str(), &status) != 0 || S_ISLNK(status.st_mode) ||
        lchown(path.c_str(), kSessionUserUid, kSessionUserGid) != 0 ||
        chmod(path.c_str(), mode) != 0) {
        error = std::string("could not assign session runtime object '") + path + "': " +
                std::strerror(errno);
        return false;
    }
    return true;
}

bool assignApplicationRuntimeOwnership(const std::string& path, std::string& error) {
    error.clear();
    constexpr mode_t kEndpointMode = 0660;
    if (geteuid() != 0) {
        if (chmod(path.c_str(), kEndpointMode) != 0) {
            error = std::string("could not secure application runtime endpoint '") + path +
                    "': " + std::strerror(errno);
            return false;
        }
        return true;
    }
    struct stat status {};
    if (lstat(path.c_str(), &status) != 0 || !S_ISSOCK(status.st_mode) ||
        lchown(path.c_str(), kSessionUserUid, kApplicationRuntimeGid) != 0 ||
        chmod(path.c_str(), kEndpointMode) != 0) {
        error = std::string("could not assign application runtime endpoint '") + path +
                "': " + std::strerror(errno);
        return false;
    }
    return true;
}

} // namespace lcl::security
