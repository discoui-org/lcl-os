#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <netinet/in.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>

namespace {

bool canWritePrivateFile(const char *path) {
  const int descriptor =
      open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    return false;
  }
  constexpr char marker[] = "sandbox-private\n";
  const bool wrote = write(descriptor, marker, sizeof(marker) - 1) ==
                     static_cast<ssize_t>(sizeof(marker) - 1);
  close(descriptor);
  return wrote;
}

bool pathIsInaccessible(const char *path) {
  const int descriptor = open(path, O_RDONLY | O_CLOEXEC);
  if (descriptor >= 0) {
    close(descriptor);
    return false;
  }
  // A private tmpfs may make an excluded path absent, while Landlock may
  // deny traversal before the VFS observes that it is absent.  Either is a
  // valid kernel-enforced result for an app-visible inaccessible endpoint.
  return errno == ENOENT || errno == EACCES;
}

bool systemPathIsNotWritable() {
  const int descriptor = open("/System/Applications/Terminal.app/Manifest.json",
                              O_WRONLY | O_CLOEXEC);
  if (descriptor >= 0) {
    close(descriptor);
    return false;
  }
  return errno == EACCES || errno == EROFS;
}

bool hasNoCapabilities() {
  __user_cap_header_struct header{};
  header.version = _LINUX_CAPABILITY_VERSION_3;
  std::array<__user_cap_data_struct, 2> capabilities{};
  if (syscall(SYS_capget, &header, capabilities.data()) != 0) {
    return false;
  }
  for (const __user_cap_data_struct &set : capabilities) {
    if (set.effective != 0 || set.permitted != 0 || set.inheritable != 0) {
      return false;
    }
  }
  return true;
}

bool cannotReachLoopback() {
  const int descriptor = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (descriptor < 0) {
    return false;
  }
  sockaddr_in loopback{};
  loopback.sin_family = AF_INET;
  loopback.sin_port = htons(9);
  loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const bool blocked =
      connect(descriptor, reinterpret_cast<const sockaddr *>(&loopback),
              sizeof(loopback)) != 0;
  close(descriptor);
  return blocked;
}

} // namespace

int main() {
  // Each nonzero result identifies the first failed boundary to a terminal
  // caller of `open -w org.lcl.sandbox-probe`; the process emits no data
  // outside its private namespace.
  if (geteuid() == 0 || getegid() == 0 || getpid() != 1)
    return 10;
  if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1)
    return 11;
  if (!hasNoCapabilities())
    return 12;
  if (!canWritePrivateFile("/Data/sandbox-probe-data"))
    return 13;
  if (!canWritePrivateFile("/Temporary/sandbox-probe-temporary"))
    return 14;
  if (!pathIsInaccessible("/dev/dri/card0") ||
      !pathIsInaccessible("/dev/input/event0"))
    return 15;
  if (!pathIsInaccessible("/Runtime/lcl-sessiond.sock"))
    return 16;
  if (!systemPathIsNotWritable())
    return 17;
  if (!cannotReachLoopback())
    return 18;
  return 0;
}
