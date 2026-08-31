#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "system/security/sandbox_platform_probe.hpp"

#include <fcntl.h>
#include <linux/filter.h>
#include <linux/magic.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>

#if __has_include(<linux/landlock.h>)
#include <linux/landlock.h>
#define LCL_HAS_LANDLOCK_HEADERS 1
#else
#define LCL_HAS_LANDLOCK_HEADERS 0
#endif

namespace lcl::security {
namespace {

struct ChildProbeResult {
    std::int32_t result{0};
    std::int32_t error{0};
};

template <typename Probe>
bool runInChild(Probe&& probe, std::string& error) {
    error.clear();
    int pipeDescriptors[2] = {-1, -1};
    if (pipe2(pipeDescriptors, O_CLOEXEC) != 0) {
        error = std::string("could not create diagnostic pipe: ") + std::strerror(errno);
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        const int savedErrno = errno;
        close(pipeDescriptors[0]);
        close(pipeDescriptors[1]);
        error = std::string("could not fork diagnostic child: ") + std::strerror(savedErrno);
        return false;
    }
    if (child == 0) {
        close(pipeDescriptors[0]);
        ChildProbeResult result{};
        result.result = probe() ? 1 : 0;
        result.error = result.result == 0 ? errno : 0;
        const auto* bytes = reinterpret_cast<const char*>(&result);
        std::size_t remaining = sizeof(result);
        while (remaining != 0) {
            const ssize_t written = write(pipeDescriptors[1], bytes, remaining);
            if (written > 0) {
                bytes += written;
                remaining -= static_cast<std::size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        _exit(result.result == 1 ? 0 : 1);
    }

    close(pipeDescriptors[1]);
    ChildProbeResult result{};
    auto* bytes = reinterpret_cast<char*>(&result);
    std::size_t remaining = sizeof(result);
    while (remaining != 0) {
        const ssize_t readCount = read(pipeDescriptors[0], bytes, remaining);
        if (readCount > 0) {
            bytes += readCount;
            remaining -= static_cast<std::size_t>(readCount);
            continue;
        }
        if (readCount < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    close(pipeDescriptors[0]);
    int waitStatus = 0;
    while (waitpid(child, &waitStatus, 0) < 0 && errno == EINTR) {
    }
    if (remaining != 0 || !WIFEXITED(waitStatus)) {
        error = "diagnostic child did not return a complete result";
        return false;
    }
    if (result.result == 1) {
        return true;
    }
    error = std::strerror(result.error == 0 ? EIO : result.error);
    return false;
}

std::string availability(bool available, const std::string& error) {
    return available ? "available" : "unavailable (" + error + ')';
}

bool cgroupControllerIsAvailable(const std::string& controllers, const char* required) {
    std::istringstream values(controllers);
    std::string controller;
    while (values >> controller) {
        if (controller == required) {
            return true;
        }
    }
    return false;
}

bool readCgroupControllers(std::string& controllers, std::string& error) {
    controllers.clear();
    error.clear();
    const int descriptor = open("/sys/fs/cgroup/cgroup.controllers", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        error = std::strerror(errno);
        return false;
    }
    std::array<char, 256> buffer{};
    while (true) {
        const ssize_t count = read(descriptor, buffer.data(), buffer.size());
        if (count > 0) {
            controllers.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            close(descriptor);
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        const int savedErrno = errno;
        close(descriptor);
        error = std::strerror(savedErrno);
        return false;
    }
}

} // namespace

SandboxPlatformCapabilities probeSandboxPlatformCapabilities() {
    SandboxPlatformCapabilities capabilities{};
    capabilities.mountNamespace = runInChild(
        [] { return unshare(CLONE_NEWNS) == 0; }, capabilities.mountNamespaceError);
    capabilities.pidNamespace = runInChild(
        [] { return unshare(CLONE_NEWPID) == 0; }, capabilities.pidNamespaceError);
    capabilities.ipcNamespace = runInChild(
        [] { return unshare(CLONE_NEWIPC) == 0; }, capabilities.ipcNamespaceError);
    capabilities.networkNamespace = runInChild(
        [] { return unshare(CLONE_NEWNET) == 0; }, capabilities.networkNamespaceError);
    capabilities.namespaceCombination = runInChild(
        [] { return unshare(CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWNET) == 0; },
        capabilities.namespaceCombinationError);

    capabilities.noNewPrivileges = runInChild(
        [] { return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0; },
        capabilities.noNewPrivilegesError);
    capabilities.seccompFilter = runInChild(
        [] {
            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
                return false;
            }
            const std::array<sock_filter, 1> filter = {
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
            };
            sock_fprog program{};
            program.len = static_cast<unsigned short>(filter.size());
            program.filter = const_cast<sock_filter*>(filter.data());
            return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
        },
        capabilities.seccompError);

#if LCL_HAS_LANDLOCK_HEADERS && defined(SYS_landlock_create_ruleset) && \
    defined(LANDLOCK_CREATE_RULESET_VERSION)
    errno = 0;
    const long landlockVersion = syscall(SYS_landlock_create_ruleset, nullptr, 0,
                                         LANDLOCK_CREATE_RULESET_VERSION);
    if (landlockVersion >= 1) {
        capabilities.landlockAbi = static_cast<std::uint32_t>(landlockVersion);
    } else {
        capabilities.landlockError = std::strerror(errno == 0 ? EOPNOTSUPP : errno);
    }
#else
    capabilities.landlockError = "kernel headers do not expose the Landlock ABI";
#endif

    struct statfs cgroupStatus {};
    if (statfs("/sys/fs/cgroup", &cgroupStatus) == 0) {
        if (static_cast<unsigned long>(cgroupStatus.f_type) == CGROUP2_SUPER_MAGIC) {
            capabilities.cgroupV2 = true;
            std::string controllers;
            if (readCgroupControllers(controllers, capabilities.cgroupError)) {
                capabilities.cgroupMemoryController =
                    cgroupControllerIsAvailable(controllers, "memory");
                capabilities.cgroupPidsController =
                    cgroupControllerIsAvailable(controllers, "pids");
                capabilities.cgroupCpuController =
                    cgroupControllerIsAvailable(controllers, "cpu");
            }
        } else {
            capabilities.cgroupError = "cgroup v2 is not mounted";
        }
    } else {
        capabilities.cgroupError = std::strerror(errno);
    }
    return capabilities;
}

std::string formatSandboxPlatformCapabilities(const SandboxPlatformCapabilities& capabilities) {
    std::ostringstream output;
    output << "sandbox kernel capability report\n"
           << "  mount namespace: "
           << availability(capabilities.mountNamespace, capabilities.mountNamespaceError) << '\n'
           << "  pid namespace: "
           << availability(capabilities.pidNamespace, capabilities.pidNamespaceError) << '\n'
           << "  ipc namespace: "
           << availability(capabilities.ipcNamespace, capabilities.ipcNamespaceError) << '\n'
           << "  network namespace: "
           << availability(capabilities.networkNamespace, capabilities.networkNamespaceError) << '\n'
           << "  combined namespace setup: "
           << availability(capabilities.namespaceCombination, capabilities.namespaceCombinationError) << '\n'
           << "  no_new_privs: "
           << availability(capabilities.noNewPrivileges, capabilities.noNewPrivilegesError) << '\n'
           << "  seccomp filter: "
           << availability(capabilities.seccompFilter,
                           capabilities.seccompError) << '\n'
           << "  Landlock ABI: ";
    if (capabilities.landlockAbi != 0) {
        output << capabilities.landlockAbi;
    } else {
        output << "unavailable (" << capabilities.landlockError << ')';
    }
    output << '\n' << "  cgroup v2: "
           << availability(capabilities.cgroupV2, capabilities.cgroupError) << '\n'
           << "  cgroup memory controller: "
           << availability(capabilities.cgroupMemoryController,
                           "not available in cgroup.controllers") << '\n'
           << "  cgroup pids controller: "
           << availability(capabilities.cgroupPidsController,
                           "not available in cgroup.controllers") << '\n'
           << "  cgroup cpu controller: "
           << availability(capabilities.cgroupCpuController,
                           "not available in cgroup.controllers") << '\n'
           << "  third-party profile: "
           << (capabilities.supportsMandatoryThirdPartyProfile() ? "supported" : "NOT supported")
           << '\n';
    return output.str();
}

} // namespace lcl::security
