#include <gtest/gtest.h>

#include "system/security/session_user.hpp"
#include "system/security/security_admin_client.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace lcl::security {

TEST(SessionUserTest, DefinesASeparateUnprivilegedSessionIdentity) {
    EXPECT_NE(kSessionUserUid, 0U);
    EXPECT_NE(kSessionUserGid, 0U);
    EXPECT_EQ(std::string(kSessionUserHome), "/Users/Rei");
}

TEST(SessionUserTest, TrustedUserShellProfileIsBoundToTheSystemTerminalBundle) {
    EXPECT_TRUE(isTrustedUserShellBundle(kTrustedUserShellAppId, kTrustedUserShellBundlePath));
    EXPECT_FALSE(isTrustedUserShellBundle(kTrustedUserShellAppId,
                                          "/Users/Rei/Applications/Terminal.app"));
    EXPECT_FALSE(isTrustedUserShellBundle("com.example.terminal", kTrustedUserShellBundlePath));
}

TEST(SessionUserTest, SystemSettingsProfileIsBoundToTheCanonicalBundle) {
    EXPECT_TRUE(isSystemSettingsBundle(kSystemSettingsAppId, kSystemSettingsBundlePath));
    EXPECT_FALSE(isSystemSettingsBundle(kSystemSettingsAppId,
                                        "/Users/Rei/Applications/Settings.app"));
    EXPECT_FALSE(isSystemSettingsBundle("org.lcl.settings-copy", kSystemSettingsBundlePath));
}

TEST(SessionUserTest, TrustedUserShellEnvironmentIsMinimalAndCanonical) {
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        setenv("LD_PRELOAD", "/untrusted/libinject.so", 1);
        setenv("HOME", "/untrusted/home", 1);
        std::string error;
        if (!prepareTrustedUserShellEnvironment(42, error) || getenv("LD_PRELOAD") != nullptr ||
            std::string(getenv("HOME") ? getenv("HOME") : "") != kSessionUserHome ||
            std::string(getenv("PATH") ? getenv("PATH") : "") != "/System/Core:/System/Tools" ||
            std::string(getenv("LCL_APP_ID") ? getenv("LCL_APP_ID") : "") !=
                kTrustedUserShellAppId ||
            std::string(getenv("LCL_APP_INSTANCE_ID") ? getenv("LCL_APP_INSTANCE_ID") : "") !=
                "42" ||
            std::string(getenv("LCL_LAUNCH_PROFILE") ? getenv("LCL_LAUNCH_PROFILE") : "") !=
                kTrustedUserShellProfileId) {
            _exit(1);
        }
        _exit(0);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(SessionUserTest, SystemSettingsEnvironmentCarriesOnlyTheFixedCapabilitySlot) {
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        setenv("LD_PRELOAD", "/untrusted/libinject.so", 1);
        std::string error;
        if (!prepareSystemSettingsEnvironment(91, error) || getenv("LD_PRELOAD") != nullptr ||
            std::string(getenv("PATH") ? getenv("PATH") : "") != "/System/Core" ||
            std::string(getenv("LCL_APP_ID") ? getenv("LCL_APP_ID") : "") !=
                kSystemSettingsAppId ||
            std::string(getenv("LCL_APP_INSTANCE_ID") ? getenv("LCL_APP_INSTANCE_ID") : "") !=
                "91" ||
            std::string(getenv("LCL_SECURITY_ADMIN_FD") ? getenv("LCL_SECURITY_ADMIN_FD") : "") !=
                "4") {
            _exit(1);
        }
        _exit(0);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(SessionUserTest, UnprivilegedProcessCannotOpenTheSecurityAdminEndpoint) {
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        std::string error;
        if (!dropToSessionUser(error)) {
            _exit(2);
        }
        SecurityAdminClient client;
        if (client.connectAsSessionAuthority("/Runtime/lcl-securityd.sock", error)) {
            _exit(1);
        }
        _exit(0);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(SessionUserTest, RootChildDropsAllTheWayToTheSessionIdentity) {
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        std::string error;
        if (!dropToSessionUser(error) || geteuid() == 0 || getegid() == 0) {
            _exit(1);
        }
        if (geteuid() == kSessionUserUid && getegid() == kSessionUserGid) {
            _exit(0);
        }
        // A non-root test runner stays non-root; it must never gain an ID.
        _exit(0);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(SessionUserTest, SecuresRootCreatedRuntimeSocketForTheSessionUser) {
    const std::string path = "/tmp/lcl-session-user-" + std::to_string(getpid()) + ".sock";
    unlink(path.c_str());
    const int socketFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(socketFd, 0);

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    ASSERT_LT(path.size(), sizeof(address.sun_path));
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    ASSERT_EQ(bind(socketFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);

    std::string error;
    ASSERT_TRUE(assignSessionUserOwnership(path, 0600, error)) << error;
    struct stat status {};
    ASSERT_EQ(lstat(path.c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0600);
    if (geteuid() == 0) {
        EXPECT_EQ(status.st_uid, kSessionUserUid);
        EXPECT_EQ(status.st_gid, kSessionUserGid);
    }

    close(socketFd);
    unlink(path.c_str());
}

TEST(SessionUserTest, SecuresGraphicsSocketForSandboxApplicationGroup) {
    const std::string path = "/tmp/lcl-app-runtime-" + std::to_string(getpid()) + ".sock";
    unlink(path.c_str());
    const int socketFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(socketFd, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    ASSERT_LT(path.size(), sizeof(address.sun_path));
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    ASSERT_EQ(bind(socketFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);

    std::string error;
    ASSERT_TRUE(assignApplicationRuntimeOwnership(path, error)) << error;
    struct stat status {};
    ASSERT_EQ(lstat(path.c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0660);
    if (geteuid() == 0) {
        EXPECT_EQ(status.st_uid, kSessionUserUid);
        EXPECT_EQ(status.st_gid, kApplicationRuntimeGid);
    }
    close(socketFd);
    unlink(path.c_str());
}

} // namespace lcl::security
