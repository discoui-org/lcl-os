#include "system/security/sandbox_landlock.hpp"

#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

#if __has_include(<linux/landlock.h>)
#include <linux/landlock.h>
#define LCL_HAS_LANDLOCK_HEADERS 1
#else
#define LCL_HAS_LANDLOCK_HEADERS 0
#endif

namespace lcl::security {
namespace {

constexpr long kMinimumLandlockAbi = 3;

bool isDirectoryDescriptor(int descriptor, struct stat& status) {
    return descriptor >= 0 && fstat(descriptor, &status) == 0 && S_ISDIR(status.st_mode);
}

bool isRegularDescriptor(int descriptor) {
    struct stat status {};
    return descriptor >= 0 && fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode) &&
           status.st_nlink == 1;
}

bool isPrivateAppDirectory(int descriptor, const AppIdentity& identity) {
    struct stat status {};
    return isDirectoryDescriptor(descriptor, status) && status.st_uid == identity.uid &&
           status.st_gid == identity.gid && (status.st_mode & 0777) == 0700;
}

bool isRootControlledDirectory(int descriptor) {
    struct stat status {};
    return isDirectoryDescriptor(descriptor, status) && status.st_uid == 0 && status.st_gid == 0 &&
           (status.st_mode & 0022) == 0;
}

#if LCL_HAS_LANDLOCK_HEADERS && defined(SYS_landlock_create_ruleset) && \
    defined(SYS_landlock_add_rule) && defined(SYS_landlock_restrict_self) && \
    defined(LANDLOCK_CREATE_RULESET_VERSION) && defined(LANDLOCK_ACCESS_FS_TRUNCATE)

constexpr __u64 kReadExecuteAccess = LANDLOCK_ACCESS_FS_EXECUTE |
                                      LANDLOCK_ACCESS_FS_READ_FILE |
                                      LANDLOCK_ACCESS_FS_READ_DIR;
constexpr __u64 kExecutableAccess = LANDLOCK_ACCESS_FS_EXECUTE |
                                      LANDLOCK_ACCESS_FS_READ_FILE;
constexpr __u64 kWritableAppAccess = LANDLOCK_ACCESS_FS_READ_FILE |
                                      LANDLOCK_ACCESS_FS_READ_DIR |
                                      LANDLOCK_ACCESS_FS_WRITE_FILE |
                                      LANDLOCK_ACCESS_FS_TRUNCATE |
                                      LANDLOCK_ACCESS_FS_REMOVE_DIR |
                                      LANDLOCK_ACCESS_FS_REMOVE_FILE |
                                      LANDLOCK_ACCESS_FS_MAKE_DIR |
                                      LANDLOCK_ACCESS_FS_MAKE_REG |
                                      LANDLOCK_ACCESS_FS_MAKE_SOCK |
                                      LANDLOCK_ACCESS_FS_MAKE_FIFO |
                                      LANDLOCK_ACCESS_FS_MAKE_SYM |
                                      LANDLOCK_ACCESS_FS_REFER;
// The private /dev tree contains /dev/dri/renderD*.  READ_DIR is required
// for Landlock to permit traversal of that nested directory; it exposes no
// additional host devices because the sandbox's /dev is its own tmpfs.
constexpr __u64 kDeviceAccess = LANDLOCK_ACCESS_FS_READ_FILE |
                                 LANDLOCK_ACCESS_FS_READ_DIR |
                                 LANDLOCK_ACCESS_FS_WRITE_FILE;
constexpr __u64 kReadOnlyAccess = LANDLOCK_ACCESS_FS_READ_FILE |
                                  LANDLOCK_ACCESS_FS_READ_DIR;
constexpr __u64 kHandledAccess = kReadExecuteAccess | kWritableAppAccess | kDeviceAccess;

bool addPathRule(int rulesetDescriptor, int parentDescriptor, __u64 allowedAccess,
                 std::string& error) {
    landlock_path_beneath_attr rule{};
    rule.allowed_access = allowedAccess;
    rule.parent_fd = parentDescriptor;
    if (syscall(SYS_landlock_add_rule, rulesetDescriptor, LANDLOCK_RULE_PATH_BENEATH, &rule, 0) ==
        0) {
        return true;
    }
    error = std::string("could not add Landlock path rule: ") + std::strerror(errno);
    return false;
}

#endif

} // namespace

bool validateSandboxLandlockRules(const SandboxLandlockRules& rules, std::string& error) {
    error.clear();
    if (!validateSandboxFilesystemSources(rules.filesystemSources, rules.identity, error)) {
        return false;
    }
    if (!isPrivateAppDirectory(rules.temporaryDescriptor, rules.identity)) {
        error = "sandbox temporary source is not app-owned 0700";
        return false;
    }
    if (!isRegularDescriptor(rules.executableDescriptor)) {
        error = "sandbox executable source is not a regular descriptor";
        return false;
    }
    if (!isRootControlledDirectory(rules.deviceDescriptor)) {
        error = "sandbox device source is not a root-controlled directory";
        return false;
    }
    const bool hasRenderNode = rules.filesystemSources.renderNodeDescriptor >= 0;
    if (hasRenderNode != (rules.sysfsDescriptor >= 0) ||
        (rules.sysfsDescriptor >= 0 && !isRootControlledDirectory(rules.sysfsDescriptor))) {
        error = "sandbox graphics sysfs Landlock source is invalid";
        return false;
    }
    return true;
}

bool installSandboxLandlockRules(const SandboxLandlockRules& rules, std::string& error) {
    error.clear();
    if (!validateSandboxLandlockRules(rules, error)) {
        return false;
    }
    if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) {
        error = "Landlock requires PR_SET_NO_NEW_PRIVS before ruleset installation";
        return false;
    }

#if LCL_HAS_LANDLOCK_HEADERS && defined(SYS_landlock_create_ruleset) && \
    defined(SYS_landlock_add_rule) && defined(SYS_landlock_restrict_self) && \
    defined(LANDLOCK_CREATE_RULESET_VERSION) && defined(LANDLOCK_ACCESS_FS_TRUNCATE)
    errno = 0;
    const long abi = syscall(SYS_landlock_create_ruleset, nullptr, 0,
                             LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0) {
        error = std::string("could not query Landlock ABI: ") + std::strerror(errno);
        return false;
    }
    if (abi < kMinimumLandlockAbi) {
        error = std::string("Landlock ABI ") + std::to_string(abi) +
                " is below the required ABI 3";
        return false;
    }

    landlock_ruleset_attr attributes{};
    attributes.handled_access_fs = kHandledAccess;
    const int rulesetDescriptor = static_cast<int>(
        syscall(SYS_landlock_create_ruleset, &attributes, sizeof(attributes), 0));
    if (rulesetDescriptor < 0) {
        error = std::string("could not create Landlock ruleset: ") + std::strerror(errno);
        return false;
    }

    const bool rulesAdded =
        addPathRule(rulesetDescriptor, rules.filesystemSources.appBundleDescriptor,
                    kReadExecuteAccess, error) &&
        addPathRule(rulesetDescriptor, rules.filesystemSources.systemDescriptor,
                    kReadExecuteAccess, error) &&
        addPathRule(rulesetDescriptor, rules.filesystemSources.dataDescriptor,
                    kWritableAppAccess, error) &&
        addPathRule(rulesetDescriptor, rules.filesystemSources.cacheDescriptor,
                    kWritableAppAccess, error) &&
        addPathRule(rulesetDescriptor, rules.filesystemSources.preferencesDescriptor,
                    kWritableAppAccess, error) &&
        addPathRule(rulesetDescriptor, rules.executableDescriptor, kExecutableAccess, error) &&
        addPathRule(rulesetDescriptor, rules.temporaryDescriptor, kWritableAppAccess, error) &&
        addPathRule(rulesetDescriptor, rules.deviceDescriptor, kDeviceAccess, error) &&
        (rules.sysfsDescriptor < 0 ||
         addPathRule(rulesetDescriptor, rules.sysfsDescriptor, kReadOnlyAccess, error));
    if (!rulesAdded) {
        close(rulesetDescriptor);
        return false;
    }
    const bool restricted = syscall(SYS_landlock_restrict_self, rulesetDescriptor, 0) == 0;
    const int savedErrno = errno;
    close(rulesetDescriptor);
    if (!restricted) {
        error = std::string("could not restrict sandbox child with Landlock: ") +
                std::strerror(savedErrno);
        return false;
    }
    return true;
#else
    error = "kernel headers do not expose the required Landlock ABI";
    return false;
#endif
}

} // namespace lcl::security
