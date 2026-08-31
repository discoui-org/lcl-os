#include "system/security/desktop_user.hpp"

#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
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

bool ensureDesktopDirectoryAt(int parentDescriptor, const char* name, ScopedFd& directory,
                              std::string& error) {
    if (mkdirat(parentDescriptor, name, 0700) != 0 && errno != EEXIST) {
        error = std::string("could not create desktop directory '") + name + "': " +
                std::strerror(errno);
        return false;
    }
    ScopedFd opened(openat(parentDescriptor, name,
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat status {};
    if (!opened.valid() || fstat(opened.get(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode)) {
        error = std::string("desktop path '") + name + "' is not a safe directory";
        return false;
    }
    if (fchown(opened.get(), kDesktopUserUid, kDesktopUserGid) != 0 ||
        fchmod(opened.get(), 0700) != 0) {
        error = std::string("could not secure desktop directory '") + name + "': " +
                std::strerror(errno);
        return false;
    }
    directory = std::move(opened);
    return true;
}

bool secureOptionalBashrc(int homeDescriptor, std::string& error) {
    ScopedFd bashrc(openat(homeDescriptor, ".bashrc", O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!bashrc.valid()) {
        if (errno == ENOENT) {
            return true;
        }
        error = std::string("could not open desktop .bashrc safely: ") + std::strerror(errno);
        return false;
    }
    struct stat status {};
    if (fstat(bashrc.get(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_nlink != 1 ||
        fchown(bashrc.get(), kDesktopUserUid, kDesktopUserGid) != 0 ||
        fchmod(bashrc.get(), 0600) != 0) {
        error = "desktop .bashrc is not a safe private regular file";
        return false;
    }
    return true;
}

} // namespace

bool dropToDesktopUser(std::string& error) {
    error.clear();
    if (geteuid() != 0) {
        return true;
    }
    if (prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0) != 0 || setgroups(0, nullptr) != 0 ||
        setresgid(kDesktopUserGid, kDesktopUserGid, kDesktopUserGid) != 0 ||
        setresuid(kDesktopUserUid, kDesktopUserUid, kDesktopUserUid) != 0 || !clearCapabilities() ||
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
        error = std::string("could not drop to the desktop user: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool provisionDesktopUserHome(std::string& error) {
    error.clear();
    if (geteuid() != 0) {
        error = "desktop home provisioning requires root";
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
    if (!ensureDesktopDirectoryAt(users.get(), kDesktopUserName, home, error) ||
        !ensureDesktopDirectoryAt(home.get(), "Applications", applications, error) ||
        !ensureDesktopDirectoryAt(home.get(), "Desktop", desktop, error) ||
        !ensureDesktopDirectoryAt(home.get(), "Documents", documents, error) ||
        !ensureDesktopDirectoryAt(home.get(), "Downloads", downloads, error) ||
        !ensureDesktopDirectoryAt(home.get(), "Library", library, error) ||
        !ensureDesktopDirectoryAt(library.get(), "Containers", containers, error) ||
        !ensureDesktopDirectoryAt(containers.get(), "org.lcl.terminal", terminalContainer, error) ||
        !ensureDesktopDirectoryAt(terminalContainer.get(), "Data", data, error) ||
        !ensureDesktopDirectoryAt(terminalContainer.get(), "Cache", cache, error) ||
        !ensureDesktopDirectoryAt(terminalContainer.get(), "Preferences", preferences, error) ||
        !ensureDesktopDirectoryAt(terminalContainer.get(), "Temporary", temporary, error) ||
        !secureOptionalBashrc(home.get(), error)) {
        return false;
    }
    return true;
}

bool assignDesktopUserOwnership(const std::string& path, mode_t mode, std::string& error) {
    error.clear();
    if (geteuid() != 0) {
        if (chmod(path.c_str(), mode) != 0) {
            error = std::string("could not secure desktop runtime object '") + path + "': " +
                    std::strerror(errno);
            return false;
        }
        return true;
    }
    struct stat status {};
    if (lstat(path.c_str(), &status) != 0 || S_ISLNK(status.st_mode) ||
        lchown(path.c_str(), kDesktopUserUid, kDesktopUserGid) != 0 ||
        chmod(path.c_str(), mode) != 0) {
        error = std::string("could not assign desktop runtime object '") + path + "': " +
                std::strerror(errno);
        return false;
    }
    return true;
}

} // namespace lcl::security
