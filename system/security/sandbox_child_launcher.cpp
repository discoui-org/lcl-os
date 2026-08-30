#include "system/security/sandbox_child_launcher.hpp"

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

constexpr int kExecutableDescriptor = 3;
constexpr int kRuntimeDescriptor = 4;
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
    const SandboxKernelEnforcement& enforcement = spec.kernelEnforcement;
    return (!profile.privateMountNamespace || enforcement.mountNamespaceReady) &&
           (!profile.privatePidNamespace || enforcement.pidNamespaceReady) &&
           (!profile.privateIpcNamespace || enforcement.ipcNamespaceReady) &&
           (!profile.privateNetworkNamespace || enforcement.networkNamespaceReady) &&
           (!profile.requireSeccomp || enforcement.seccompInstalled) &&
           (!profile.requireLandlock || enforcement.landlockInstalled) &&
           (!profile.denyDirectDeviceAccess || enforcement.directDeviceAccessDenied);
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

bool closeInheritedDescriptors() {
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
    return set("HOME", "/Data") && set("TMPDIR", "/Temporary") &&
           set("PATH", "/System/Core:/usr/bin") && set("USER", spec.identity.appId) &&
           set("LOGNAME", spec.identity.appId) && set("LCL_APP_ID", spec.identity.appId) &&
           set("LCL_APP_INSTANCE_ID", instanceId);
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

[[noreturn]] void execSandboxChild(const SandboxChildLaunchSpec& spec) {
    if (setpgid(0, 0) != 0) {
        childExit(ChildFailureStage::Setup, "could not create app process group");
    }
    if (prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0) != 0 || setgroups(0, nullptr) != 0 ||
        setresgid(spec.identity.gid, spec.identity.gid, spec.identity.gid) != 0 ||
        setresuid(spec.identity.uid, spec.identity.uid, spec.identity.uid) != 0 || !clearCapabilities() ||
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0 ||
        !clearAndSetEnvironment(spec)) {
        childExit(ChildFailureStage::Setup, "could not apply sandbox child credentials or environment");
    }

    if (!duplicateDescriptor(spec.executableDescriptor, kExecutableDescriptor, false) ||
        (spec.plan.profile.runtime == SandboxRuntime::JavaScript &&
         !duplicateDescriptor(spec.runtimeDescriptor, kRuntimeDescriptor, false)) ||
        !duplicateDescriptor(kFailureDescriptor, kFailureDescriptor, true) ||
        !replaceStandardStreams() || !closeInheritedDescriptors()) {
        childExit(ChildFailureStage::Setup, "could not prepare sandbox child file descriptors");
    }
    umask(0077);

    const std::string executablePath = "/proc/self/fd/" + std::to_string(kExecutableDescriptor);
    std::vector<std::string> arguments;
    if (spec.plan.profile.runtime == SandboxRuntime::JavaScript) {
        arguments.push_back("/proc/self/fd/" + std::to_string(kRuntimeDescriptor));
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
    execv(argv.front(), argv.data());
    childExit(ChildFailureStage::Execution,
              std::string("could not execute verified app descriptor: ") + std::strerror(errno));
}

} // namespace

bool validateSandboxChildLaunchSpec(const SandboxChildLaunchSpec& spec, std::string& error) {
    error.clear();
    if (!validateSandboxLaunchRequest(spec.plan.request, error) ||
        !validateSandboxProfile(spec.plan.profile, error) ||
        spec.plan.request.appId != spec.identity.appId || spec.identity.uid == 0 || spec.identity.gid == 0 ||
        spec.identity.uid != spec.identity.gid || !isRegularDescriptor(spec.executableDescriptor,
                                                                        spec.plan.profile.runtime == SandboxRuntime::Native) ||
        spec.executableDescriptor < kExecutableDescriptor ||
        spec.arguments.size() > kMaximumArgumentCount || !hasRequiredKernelEnforcement(spec)) {
        if (error.empty()) {
            error = "sandbox child launch spec is incomplete or unsafe";
        }
        return false;
    }
    if (spec.plan.profile.runtime == SandboxRuntime::JavaScript &&
        (!isRegularDescriptor(spec.runtimeDescriptor, true) || spec.runtimeDescriptor < kRuntimeDescriptor)) {
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

    int failurePipe[2] = {-1, -1};
    if (pipe2(failurePipe, O_CLOEXEC) != 0) {
        result.status = SandboxLaunchStatus::SetupFailed;
        result.message = std::string("could not create sandbox child status pipe: ") + std::strerror(errno);
        return result;
    }
    const pid_t child = fork();
    if (child < 0) {
        close(failurePipe[0]);
        close(failurePipe[1]);
        result.status = SandboxLaunchStatus::SetupFailed;
        result.message = std::string("could not fork sandbox child: ") + std::strerror(errno);
        return result;
    }
    if (child == 0) {
        close(failurePipe[0]);
        SandboxChildLaunchSpec childSpec = spec;
        if (!duplicateDescriptor(childSpec.executableDescriptor, kExecutableDescriptor, false) ||
            (childSpec.plan.profile.runtime == SandboxRuntime::JavaScript &&
             !duplicateDescriptor(childSpec.runtimeDescriptor, kRuntimeDescriptor, false))) {
            _exit(127);
        }
        childSpec.executableDescriptor = kExecutableDescriptor;
        if (childSpec.plan.profile.runtime == SandboxRuntime::JavaScript) {
            childSpec.runtimeDescriptor = kRuntimeDescriptor;
        }
        if (!duplicateDescriptor(failurePipe[1], kFailureDescriptor, true)) {
            _exit(127);
        }
        if (failurePipe[1] != kFailureDescriptor) {
            close(failurePipe[1]);
        }
        execSandboxChild(childSpec);
    }

    close(failurePipe[1]);
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
        result.status = SandboxLaunchStatus::SetupFailed;
        result.message = std::string("could not read sandbox child status: ") + std::strerror(readError);
        return result;
    }

    int waitStatus = 0;
    while (waitpid(child, &waitStatus, 0) < 0 && errno == EINTR) {
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
