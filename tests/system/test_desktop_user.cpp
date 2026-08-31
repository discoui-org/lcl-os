#include <gtest/gtest.h>

#include "system/security/desktop_user.hpp"

#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace lcl::security {

TEST(DesktopUserTest, DefinesASeparateUnprivilegedDesktopIdentity) {
    EXPECT_NE(kDesktopUserUid, 0U);
    EXPECT_NE(kDesktopUserGid, 0U);
    EXPECT_EQ(std::string(kDesktopUserHome), "/Users/Rei");
}

TEST(DesktopUserTest, RootChildDropsAllTheWayToTheDesktopIdentity) {
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        std::string error;
        if (!dropToDesktopUser(error) || geteuid() == 0 || getegid() == 0) {
            _exit(1);
        }
        if (geteuid() == kDesktopUserUid && getegid() == kDesktopUserGid) {
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

TEST(DesktopUserTest, SecuresRootCreatedRuntimeSocketForTheDesktopUser) {
    const std::string path = "/tmp/lcl-desktop-user-" + std::to_string(getpid()) + ".sock";
    unlink(path.c_str());
    const int socketFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(socketFd, 0);

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    ASSERT_LT(path.size(), sizeof(address.sun_path));
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    ASSERT_EQ(bind(socketFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);

    std::string error;
    ASSERT_TRUE(assignDesktopUserOwnership(path, 0600, error)) << error;
    struct stat status {};
    ASSERT_EQ(lstat(path.c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0600);
    if (geteuid() == 0) {
        EXPECT_EQ(status.st_uid, kDesktopUserUid);
        EXPECT_EQ(status.st_gid, kDesktopUserGid);
    }

    close(socketFd);
    unlink(path.c_str());
}

} // namespace lcl::security
