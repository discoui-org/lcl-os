#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <netinet/in.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdio>

#include "producer_grant_probe.hpp"
#include "system/ipc/lcl_protocol.hpp"

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

bool pidNamespaceMatchesTrustedSandboxMode() {
  const char *const selected = std::getenv("LCL_SANDBOX_PID_NAMESPACE");
  if (selected == nullptr) {
    return false;
  }
  if (std::strcmp(selected, "1") == 0) {
    return getpid() == 1;
  }
  if (std::strcmp(selected, "0") == 0) {
    return getpid() != 1;
  }
  return false;
}

bool hasExpectedPortableResourceLimits() {
  constexpr rlim_t kExpectedAddressSpace = 512U * 1024U * 1024U;
  constexpr rlim_t kExpectedProcessCount = 64U;
  rlimit addressSpace{};
  rlimit processCount{};
  return getrlimit(RLIMIT_AS, &addressSpace) == 0 &&
         addressSpace.rlim_cur == kExpectedAddressSpace &&
         addressSpace.rlim_max == kExpectedAddressSpace &&
         getrlimit(RLIMIT_NPROC, &processCount) == 0 &&
         processCount.rlim_cur == kExpectedProcessCount &&
         processCount.rlim_max == kExpectedProcessCount;
}

bool producerGrantIsProcessBound() {
  namespace protocol = lcl::protocol;
  namespace raster = lcl::raster_protocol;
  using namespace lcl::sandbox_probe;
  const ProbeFd report(open("/Data/producer-grant-probe.txt",
                           O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
  if (report.get() < 0) return false;
  const auto record = [&](const char* name, bool passed) {
    dprintf(report.get(), "%s %s\n", passed ? "PASS" : "FAIL", name);
    return passed;
  };
  const ProbeFd compositor(connectRaster("/Runtime/lcl-compositor.sock"));
  if (!record("compositor connection", compositor.get() >= 0)) return false;
  protocol::LCLMsgSurfaceCreate create{};
  create.surfaceId = 1;
  create.width = create.height = 64.0f;
  std::strcpy(create.appId, "org.lcl.sandbox-probe");
  std::strcpy(create.title, "Producer grant probe");
  const char* instance = std::getenv("LCL_APP_INSTANCE_ID");
  if (!instance) return record("launch instance", false);
  create.appInstanceId = std::strtoull(instance, nullptr, 10);
  protocol::LCLHeader request{};
  request.opcode = protocol::LCLOpcode::SurfaceCreate;
  request.requestId = 1;
  request.payloadSize = sizeof(create);
  if (!record("surface request", protocol::sendMsgWithFd(compositor.get(), request, &create)))
    return false;
  raster::SurfaceGrant grant{};
  for (int attempt = 0; attempt < 8 && grant.surfaceId == 0; ++attempt) {
    pollfd waiter{compositor.get(), POLLIN, 0};
    if (poll(&waiter, 1, 2000) <= 0) break;
    protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    int receivedFd = -1;
    const auto status = protocol::recvPacketWithFd(compositor.get(), header, payload, receivedFd);
    const ProbeFd received(receivedFd);
    if (status != protocol::ReceiveStatus::Received) break;
    if (header.opcode == protocol::LCLOpcode::SurfaceProducerGrant &&
        payload.size() == sizeof(protocol::LCLMsgSurfaceProducerGrant)) {
      protocol::LCLMsgSurfaceProducerGrant message{};
      std::memcpy(&message, payload.data(), sizeof(message));
      grant = {message.surfaceId, message.ownerPid, message.flags, message.tokenHigh, message.tokenLow};
    }
  }
  if (!record("authenticated surface grant", grant.surfaceId == create.surfaceId &&
              (grant.tokenHigh != 0 || grant.tokenLow != 0))) return false;
  constexpr const char* rasterPath = "/Runtime/lcl-raster.sock";
  const ProbeFd owner(connectRaster(rasterPath));
  if (!record("owner grant accepted", checkGrant(owner.get(), grant, raster::DiscardReason::InvalidFrame)))
    return false;
  bool passed = true;
  passed &= record("copied grant denied", rejectsChildGrant(rasterPath, grant, GrantAttack::NewConnection));
  passed &= record("rewritten owner denied", rejectsChildGrant(rasterPath, grant, GrantAttack::RewrittenOwner));
  passed &= record("inherited socket denied", rejectsChildGrant(rasterPath, grant, GrantAttack::InheritedConnection));
  passed &= record("owner survives attacks", checkGrant(owner.get(), grant, raster::DiscardReason::InvalidFrame));
  // Closing the compositor connection revokes the grant and removes the
  // never-mapped probe surface through the normal client cleanup path.
  return passed;
}

} // namespace

int main() {
  // Each nonzero result identifies the first failed boundary to a terminal
  // caller of `open -w org.lcl.sandbox-probe`; the process emits no data
  // outside its private namespace.
  if (geteuid() == 0 || getegid() == 0 ||
      !pidNamespaceMatchesTrustedSandboxMode())
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
  if (!pathIsInaccessible(
          "/Users/Rei/Library/Containers/org.lcl.sandbox-test/Data"))
    return 21;
  if (!systemPathIsNotWritable())
    return 17;
  if (!cannotReachLoopback())
    return 18;
  if (!hasExpectedPortableResourceLimits())
    return 19;
  if (!producerGrantIsProcessBound())
    return 20;
  return 0;
}
