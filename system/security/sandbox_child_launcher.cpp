#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "system/security/sandbox_child_launcher.hpp"
#include "system/security/sandbox_filesystem_namespace.hpp"
#include "system/security/sandbox_landlock.hpp"
#include "system/security/sandbox_namespace.hpp"
#include "system/security/sandbox_seccomp.hpp"
#include "system/security/session_user.hpp"

#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

namespace lcl::security {
namespace {

constexpr int kProgramDescriptor = 3;
constexpr int kUnusedDescriptor = 4;
constexpr int kFailureDescriptor = 5;
constexpr std::size_t kMaximumArgumentCount = 64;
constexpr std::size_t kMaximumArgumentBytes = 4096;
constexpr std::size_t kFailureMessageBytes = 512;

enum class ChildFailureStage : char {
    Setup = 'S',
    Execution = 'E',
};

bool isSafeArgument(const std::string& argument) {
    return argument.size() <= kMaximumArgumentBytes && argument.find('\0') == std::string::npos;
}

bool isRegularDescriptor(int descriptor, bool requireExecutable) {
    struct stat status {};
    return descriptor >= 0 && fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode) &&
           status.st_nlink == 1 && (!requireExecutable ||
                                    (status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0);
}

bool hasRequiredKernelEnforcement(const SandboxChildLaunchSpec& spec) {
    const SandboxProfile& profile = spec.plan.profile;
    const SandboxPlatformHardening& hardening = spec.hardening;
    const SandboxKernelEnforcement& enforcement = spec.kernelEnforcement;
    return (!hardening.privateMountNamespace || enforcement.mountNamespaceReady) &&
           (!hardening.privatePidNamespace || enforcement.pidNamespaceReady) &&
           (!hardening.privateIpcNamespace || enforcement.ipcNamespaceReady) &&
           (!hardening.privateNetworkNamespace || enforcement.networkNamespaceReady) &&
           (!profile.noNewPrivileges || enforcement.noNewPrivilegesInstalled) &&
           (!profile.requireSeccomp || enforcement.seccompInstalled) &&
           (!hardening.requireLandlock || enforcement.landlockInstalled) &&
           (!profile.denyDirectDeviceAccess || enforcement.directDeviceAccessDenied) &&
           (!hardening.requirePortableResourceLimits ||
            enforcement.portableResourceLimitsInstalled);
}

bool duplicateDescriptor(int source, int target, bool closeOnExec) {
    if (source == target) {
        const int flags = fcntl(target, F_GETFD);
        return flags >= 0 && fcntl(target, F_SETFD,
                                   closeOnExec ? (flags | FD_CLOEXEC) : (flags & ~FD_CLOEXEC)) == 0;
    }
    if (dup3(source, target, closeOnExec ? O_CLOEXEC : 0) < 0) {
        return false;
    }
    return true;
}

bool clearCapabilities() {
    __user_cap_header_struct header{};
    header.version = _LINUX_CAPABILITY_VERSION_3;
    header.pid = 0;
    std::array<__user_cap_data_struct, 2> data{};
    return syscall(SYS_capset, &header, data.data()) == 0;
}

bool applyPortableResourceLimits(const SandboxResourceLimits& limits) {
    const rlim_t maximumAddressSpace =
        static_cast<rlim_t>(limits.memoryMaxMiB) * 1024U * 1024U;
    struct rlimit addressSpaceLimit {
        .rlim_cur = maximumAddressSpace,
        .rlim_max = maximumAddressSpace,
    };
    if (setrlimit(RLIMIT_AS, &addressSpaceLimit) != 0) {
        return false;
    }
#ifdef RLIMIT_NPROC
    const rlim_t maximumProcesses = static_cast<rlim_t>(limits.pidsMax);
    struct rlimit processLimit {
        .rlim_cur = maximumProcesses,
        .rlim_max = maximumProcesses,
    };
    if (setrlimit(RLIMIT_NPROC, &processLimit) != 0) {
        return false;
    }
#endif
    return true;
}

bool closeInheritedDescriptors() {
    // FDs 0-2 are replaced with /dev/null, fd 3 is the one program image and
    // fd 5 is the private failure channel.  FD 4 has no sandbox role and
    // must not accidentally retain a daemon socket or verifier handle.
    if (close(kUnusedDescriptor) != 0 && errno != EBADF) {
        return false;
    }
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 6U, std::numeric_limits<unsigned int>::max(), 0U) == 0) {
        return true;
    }
#endif
    struct rlimit limit {};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur == RLIM_INFINITY ||
        limit.rlim_cur > static_cast<rlim_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    for (int descriptor = 6; descriptor < static_cast<int>(limit.rlim_cur); ++descriptor) {
        close(descriptor);
    }
    return true;
}

bool clearAndSetEnvironment(const SandboxChildLaunchSpec& spec) {
    if (clearenv() != 0) {
        return false;
    }
    const auto set = [](const char* name, const std::string& value) {
        return setenv(name, value.c_str(), 1) == 0;
    };
    const std::string instanceId = std::to_string(spec.plan.request.instanceId);
    const std::string pidNamespace = spec.hardening.privatePidNamespace ? "1" : "0";
    return set("HOME", "/Data") && set("TMPDIR", "/Temporary") &&
           set("PATH", "/System/Core") && set("USER", spec.identity.appId) &&
           set("LOGNAME", spec.identity.appId) && set("LCL_APP_ID", spec.identity.appId) &&
           set("LCL_APP_INSTANCE_ID", instanceId) &&
           set("LCL_SANDBOX_PID_NAMESPACE", pidNamespace);
}

bool replaceStandardStreams() {
    const int nullDescriptor = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (nullDescriptor < 0) {
        return false;
    }
    const bool duplicated = duplicateDescriptor(nullDescriptor, STDIN_FILENO, false) &&
                            duplicateDescriptor(nullDescriptor, STDOUT_FILENO, false) &&
                            duplicateDescriptor(nullDescriptor, STDERR_FILENO, false);
    if (nullDescriptor > STDERR_FILENO) {
        close(nullDescriptor);
    }
    return duplicated;
}

void writeChildFailure(ChildFailureStage stage, std::string_view message) {
    std::array<char, kFailureMessageBytes> payload{};
    payload[0] = static_cast<char>(stage);
    const std::size_t length = std::min(message.size(), payload.size() - 2);
    std::memcpy(payload.data() + 1, message.data(), length);
    payload[length + 1] = '\0';
    std::size_t offset = 0;
    const std::size_t total = length + 2;
    while (offset < total) {
        const ssize_t written = write(kFailureDescriptor, payload.data() + offset, total - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

[[noreturn]] void childExit(ChildFailureStage stage, const std::string& message) {
    writeChildFailure(stage, message);
    _exit(127);
}

void closeLandlockDescriptors(SandboxLandlockRules& rules) {
    if (rules.temporaryDescriptor >= 0) {
        close(rules.temporaryDescriptor);
        rules.temporaryDescriptor = -1;
    }
    if (rules.deviceDescriptor >= 0) {
        close(rules.deviceDescriptor);
        rules.deviceDescriptor = -1;
    }
}

[[noreturn]] void execSandboxChild(SandboxChildLaunchSpec spec, SandboxLandlockRules landlockRules) {
    std::string error;
    const pid_t expectedParent = getppid();
    const gid_t runtimeGroup = kApplicationRuntimeGid;
    if (!applyPortableResourceLimits(spec.hardening.resourceLimits) ||
        prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0) != 0 || setgroups(1, &runtimeGroup) != 0 ||
        setresgid(spec.identity.gid, spec.identity.gid, spec.identity.gid) != 0 ||
        setresuid(spec.identity.uid, spec.identity.uid, spec.identity.uid) != 0 ||
        prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != expectedParent ||
        !clearCapabilities() || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0 || !clearAndSetEnvironment(spec)) {
        childExit(ChildFailureStage::Setup, "could not apply sandbox child credentials or environment");
    }
    spec.kernelEnforcement.portableResourceLimitsInstalled = true;
    spec.kernelEnforcement.noNewPrivilegesInstalled = true;
    // /dev/null is a minimal filesystem object prepared by sandboxd.  Bind
    // stdio before Landlock so the setup path itself never depends on a
    // capability it is about to remove.
    if (!replaceStandardStreams()) {
        childExit(ChildFailureStage::Setup,
                  std::string("could not redirect sandbox standard streams: ") +
                      std::strerror(errno));
    }
    if (spec.hardening.requireLandlock && !installSandboxLandlockRules(landlockRules, error)) {
        childExit(ChildFailureStage::Setup, "could not install sandbox Landlock rules: " + error);
    }
    closeLandlockDescriptors(landlockRules);
    spec.kernelEnforcement.landlockInstalled = spec.hardening.requireLandlock;
    if (!installSandboxBaselineSeccomp(spec.plan.profile.runtime, error)) {
        childExit(ChildFailureStage::Setup, "could not install sandbox seccomp rules: " + error);
    }
    spec.kernelEnforcement.seccompInstalled = true;
    if (!hasRequiredKernelEnforcement(spec)) {
        childExit(ChildFailureStage::Setup, "sandbox kernel enforcement is incomplete");
    }

    if (!duplicateDescriptor(kFailureDescriptor, kFailureDescriptor, true)) {
        childExit(ChildFailureStage::Setup,
                  std::string("could not protect sandbox failure channel: ") + std::strerror(errno));
    }
    if (!closeInheritedDescriptors()) {
        childExit(ChildFailureStage::Setup,
                  std::string("could not close inherited sandbox descriptors: ") + std::strerror(errno));
    }
    umask(0077);

    const std::string executablePath = "/App/" + spec.executableBundlePath;
    std::vector<std::string> arguments;
    if (spec.plan.profile.runtime == SandboxRuntime::JavaScript) {
        arguments.push_back("lcl-js");
        arguments.push_back(executablePath);
    } else {
        arguments.push_back(executablePath);
    }
    arguments.insert(arguments.end(), spec.arguments.begin(), spec.arguments.end());
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
#ifdef SYS_execveat
    syscall(SYS_execveat, kProgramDescriptor, "", argv.data(), environ, AT_EMPTY_PATH);
#else
    errno = ENOSYS;
#endif
    childExit(ChildFailureStage::Execution,
              std::string("could not execute verified app descriptor: ") + std::strerror(errno));
}

bool waitForSetupGate(int descriptor) {
    char token = '\0';
    ssize_t count = -1;
    do {
        count = read(descriptor, &token, sizeof(token));
    } while (count < 0 && errno == EINTR);
    return count == static_cast<ssize_t>(sizeof(token)) && token == 'G';
}

void waitForInnerSandboxChildAndExit(pid_t child) {
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) {
        _exit(WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
        _exit(128 + WTERMSIG(status));
    }
    _exit(127);
}

} // namespace

bool validateSandboxChildLaunchSpec(const SandboxChildLaunchSpec& spec, std::string& error) {
    error.clear();
    if (!validateSandboxLaunchRequest(spec.plan.request, error) ||
        !validateSandboxProfile(spec.plan.profile, error) ||
        !validateSandboxPlatformHardening(spec.hardening, error) ||
        spec.plan.request.appId != spec.identity.appId || spec.identity.uid == 0 || spec.identity.gid == 0 ||
        spec.identity.uid != spec.identity.gid || !isRegularDescriptor(spec.executableDescriptor,
                                                                        spec.plan.profile.runtime == SandboxRuntime::Native) ||
        !isSafeSandboxBundleRelativePath(spec.executableBundlePath) ||
        spec.executableDescriptor < kProgramDescriptor ||
        spec.arguments.size() > kMaximumArgumentCount ||
        (spec.hardening.requireCgroupResourceAccounting != spec.cgroup.has_value()) ||
        (spec.cgroup.has_value() &&
         (!spec.cgroup->manager || spec.cgroup->cgroup.instanceId != spec.plan.request.instanceId))) {
        if (error.empty()) {
            error = "sandbox child launch spec is incomplete or unsafe";
        }
        return false;
    }
    if (!validateSandboxFilesystemSources(spec.filesystemSources, spec.identity, error)) {
        return false;
    }
    if (spec.plan.profile.runtime == SandboxRuntime::JavaScript &&
        (!isRegularDescriptor(spec.runtimeDescriptor, true) ||
         spec.runtimeDescriptor < kProgramDescriptor)) {
        error = "sandbox JavaScript runtime descriptor is unavailable";
        return false;
    }
    for (const std::string& argument : spec.arguments) {
        if (!isSafeArgument(argument)) {
            error = "sandbox child argument is unsafe or too long";
            return false;
        }
    }
    return true;
}

SandboxLaunchResult spawnSandboxChild(const SandboxChildLaunchSpec& spec) {
    SandboxLaunchResult result{};
    result.instanceId = spec.plan.request.instanceId;
    std::string error;
    if (!validateSandboxChildLaunchSpec(spec, error)) {
        result.status = SandboxLaunchStatus::InvalidRequest;
        result.message = std::move(error);
        return result;
    }
    if (geteuid() != 0) {
        result.status = SandboxLaunchStatus::SetupFailed;
        result.message = "sandbox child launcher requires root sandboxd";
        return result;
    }
    if (spec.hardening.requireCgroupResourceAccounting &&
        (!spec.cgroup.has_value() || !spec.cgroup->manager)) {
        result.status = SandboxLaunchStatus::SetupFailed;
        result.message = "sandbox child launcher requires a daemon-owned cgroup allocation";
        return result;
    }

    int failurePipe[2] = {-1, -1};
    if (pipe2(failurePipe, O_CLOEXEC) != 0) {
        result.status = SandboxLaunchStatus::SetupFailed;
        result.message = std::string("could not create sandbox child status pipe: ") + std::strerror(errno);
        return result;
    }
    int setupGate[2] = {-1, -1};
    if (spec.hardening.requireCgroupResourceAccounting && pipe2(setupGate, O_CLOEXEC) != 0) {
        const int savedErrno = errno;
        close(failurePipe[0]);
        close(failurePipe[1]);
        result.status = SandboxLaunchStatus::SetupFailed;
        result.message = std::string("could not create sandbox cgroup gate: ") + std::strerror(savedErrno);
        return result;
    }
    const pid_t child = fork();
    if (child < 0) {
        close(failurePipe[0]);
        close(failurePipe[1]);
        if (setupGate[0] >= 0) close(setupGate[0]);
        if (setupGate[1] >= 0) close(setupGate[1]);
        result.status = SandboxLaunchStatus::SetupFailed;
        result.message = std::string("could not fork sandbox child: ") + std::strerror(errno);
        return result;
    }
    if (child == 0) {
        const pid_t expectedDaemon = getppid();
        close(failurePipe[0]);
        if (setupGate[1] >= 0) close(setupGate[1]);
        if (!duplicateDescriptor(failurePipe[1], kFailureDescriptor, true)) {
            _exit(127);
        }
        if (failurePipe[1] != kFailureDescriptor) {
            close(failurePipe[1]);
        }
        // If sandboxd dies (including an unclean crash), the outer reaper
        // process must die before it can release its child toward init.  The
        // final app process installs the same relationship after dropping
        // credentials because Linux clears a parent-death signal on UID/GID
        // changes.
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != expectedDaemon) {
            childExit(ChildFailureStage::Setup, "sandboxd parent process disappeared during launch");
        }
        if (spec.hardening.requireCgroupResourceAccounting && !waitForSetupGate(setupGate[0])) {
            close(setupGate[0]);
            childExit(ChildFailureStage::Setup, "sandbox cgroup placement gate was not released");
        }
        if (setupGate[0] >= 0) close(setupGate[0]);
        if (setpgid(0, 0) != 0) {
            childExit(ChildFailureStage::Setup, "could not create app process group");
        }

        SandboxKernelEnforcement enforcement{};
        if (!beginSandboxNamespaces(spec.hardening, enforcement, error)) {
            childExit(ChildFailureStage::Setup, error);
        }
        if (spec.hardening.privatePidNamespace) {
            const pid_t innerChild = fork();
            if (innerChild < 0) {
                childExit(ChildFailureStage::Setup,
                          std::string("could not fork PID-namespace init child: ") + std::strerror(errno));
            }
            if (innerChild > 0) {
                close(kFailureDescriptor);
                waitForInnerSandboxChildAndExit(innerChild);
            }
        }

        if (!finalizeSandboxPidNamespace(spec.hardening, enforcement, error)) {
            childExit(ChildFailureStage::Setup, error);
        }
        SandboxLandlockRules landlockRules{};
        if (!enterSandboxFilesystemNamespace(spec.filesystemSources, spec.identity,
                                             spec.executableDescriptor,
                                             spec.plan.profile.runtime == SandboxRuntime::Native,
                                             spec.hardening.privatePidNamespace,
                                             landlockRules, error)) {
            childExit(ChildFailureStage::Setup, error);
        }
        enforcement.mountNamespaceReady = true;
        enforcement.directDeviceAccessDenied = true;

        SandboxChildLaunchSpec childSpec = spec;
        childSpec.kernelEnforcement = enforcement;
        const int programDescriptor = childSpec.plan.profile.runtime == SandboxRuntime::JavaScript
            ? childSpec.runtimeDescriptor
            : childSpec.executableDescriptor;
        if (!duplicateDescriptor(programDescriptor, kProgramDescriptor, false)) {
            childExit(ChildFailureStage::Setup, "could not preserve verified executable descriptors");
        }
        execSandboxChild(std::move(childSpec), std::move(landlockRules));
    }

    close(failurePipe[1]);
    if (setupGate[0] >= 0) close(setupGate[0]);
    if (spec.hardening.requireCgroupResourceAccounting &&
        !spec.cgroup->manager->moveProcess(spec.cgroup->cgroup, child, error)) {
        close(setupGate[1]);
        kill(child, SIGKILL);
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
        }
        std::string cleanupError;
        spec.cgroup->manager->removeInstance(spec.cgroup->cgroup, cleanupError);
        close(failurePipe[0]);
        result.status = SandboxLaunchStatus::SetupFailed;
        result.message = "could not place sandbox child into its cgroup: " + error;
        return result;
    }
    const char gateToken = 'G';
    if (spec.hardening.requireCgroupResourceAccounting &&
        write(setupGate[1], &gateToken, sizeof(gateToken)) != static_cast<ssize_t>(sizeof(gateToken))) {
        const int savedErrno = errno;
        close(setupGate[1]);
        kill(child, SIGKILL);
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
        }
        std::string cleanupError;
        spec.cgroup->manager->removeInstance(spec.cgroup->cgroup, cleanupError);
        close(failurePipe[0]);
        result.status = SandboxLaunchStatus::SetupFailed;
        result.message = std::string("could not release sandbox cgroup gate: ") +
                         std::strerror(savedErrno);
        return result;
    }
    if (setupGate[1] >= 0) close(setupGate[1]);
    std::array<char, kFailureMessageBytes> failure{};
    ssize_t count = -1;
    do {
        count = read(failurePipe[0], failure.data(), failure.size() - 1);
    } while (count < 0 && errno == EINTR);
    close(failurePipe[0]);
    if (count == 0) {
        result.status = SandboxLaunchStatus::Launched;
        result.pid = static_cast<std::int32_t>(child);
        result.processGroupId = static_cast<std::int32_t>(child);
        result.message = "launched";
        return result;
    }
    if (count < 0) {
        const int readError = errno;
        kill(-child, SIGKILL);
        kill(child, SIGKILL);
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
        }
        if (spec.hardening.requireCgroupResourceAccounting) {
            std::string cleanupError;
            spec.cgroup->manager->removeInstance(spec.cgroup->cgroup, cleanupError);
        }
        result.status = SandboxLaunchStatus::SetupFailed;
        result.message = std::string("could not read sandbox child status: ") + std::strerror(readError);
        return result;
    }

    int waitStatus = 0;
    while (waitpid(child, &waitStatus, 0) < 0 && errno == EINTR) {
    }
    if (spec.hardening.requireCgroupResourceAccounting) {
        std::string cleanupError;
        spec.cgroup->manager->removeInstance(spec.cgroup->cgroup, cleanupError);
    }
    result.status = failure[0] == static_cast<char>(ChildFailureStage::Execution)
        ? SandboxLaunchStatus::ExecutionFailed
        : SandboxLaunchStatus::SetupFailed;
    if (count > 1) {
        result.message.assign(failure.data() + 1, static_cast<std::size_t>(count - 2));
    } else {
        result.message = "sandbox child failed without a diagnostic";
    }
    return result;
}

} // namespace lcl::security
