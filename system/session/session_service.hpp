#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include "system/security/sandbox_client.hpp"
#include "system/security/bundle_approval_store.hpp"
#include "system/security/bundle_snapshot_store.hpp"
#include "system/security/pending_bundle_approval_store.hpp"
#include "system/security/permission_store.hpp"
#include "system/security/session_user.hpp"
#include "system/session/app_registry.hpp"
#include "system/session/session_protocol.hpp"

namespace lcl::session {

struct AppInstance {
  uint64_t instanceId{0};
  std::string appId;
  int32_t pid{0};
  // The process group leader returned by sandboxd is not a child of
  // sessiond; sandboxd reports its eventual exit over its control socket.
  bool sandboxed{false};
  bool running{false};
  int32_t exitCode{0};
};

/**
 * The session authority for application catalog, launch and process lifecycle.
 * It deliberately has no compositor, surface, scene, or focus dependency.
 */
class SessionService {
public:
  explicit SessionService(
      std::vector<std::string> appSearchPaths =
          {"/System/Applications", "/Applications",
           (std::getenv("HOME")
                ? std::string(std::getenv("HOME")) + "/Applications"
                : "/Users/Rei/Applications")},
      std::string sandboxSocketPath = lcl::security::kSandboxSocket,
      lcl::security::PermissionStoreConfig permissionStoreConfig = {},
      lcl::security::BundleApprovalStoreConfig bundleApprovalStoreConfig = {
          .storePath = "/var/lib/lcl-security/bundle-approvals.v1",
          .ownerUid = 0,
          .ownerGid = 0,
      },
      lcl::security::BundleSnapshotStoreConfig bundleSnapshotStoreConfig = {},
      lcl::security::PendingBundleApprovalStoreConfig
          pendingBundleApprovalStoreConfig = {});
  ~SessionService();

  SessionService(const SessionService &) = delete;
  SessionService &operator=(const SessionService &) = delete;

  bool initialize(const std::string &socketPath = kSessionSocket);
  void shutdown();
  void poll();

  void refreshCatalog();
  const AppRegistry &registry() const noexcept { return m_registry; }
  const std::unordered_map<uint64_t, AppInstance> &instances() const noexcept {
    return m_instances;
  }

  LaunchResponse launch(const LaunchRequest &request);
  bool launchDefaultProfile();

private:
  struct ClientConnection {
    int fd{-1};
    pid_t pid{0};
    uid_t uid{0};
    gid_t gid{0};
  };

  bool sendPacket(int fd, SessionOpcode opcode, uint32_t requestId,
                  const std::vector<uint8_t> &payload);
  LaunchResponse launchSandboxed(const core::AppBundleMetadata &app,
                                 uint64_t instanceId);
  void acceptConnections();
  bool peerIsTrustedSessionAuthority(int fd, ClientConnection &connection) const;
  void serviceClient(const ClientConnection &connection);
  void removeClient(int fd);
  void reapChildren();
  void reapSandboxChildren();
  void completeInstanceExit(uint64_t instanceId, int exitCode);
  void notifyExit(const ProcessExited &event);
  static int exitCodeFromStatus(int status);

  AppRegistry m_registry;
  lcl::security::SandboxClient m_sandboxClient;
  lcl::security::PermissionStore m_permissionStore;
  lcl::security::BundleApprovalStore m_bundleApprovals;
  lcl::security::BundleSnapshotStore m_bundleSnapshots;
  lcl::security::PendingBundleApprovalStore m_pendingBundleApprovals;
  std::string m_sandboxSocketPath;
  std::string m_socketPath;
  int m_serverFd{-1};
  std::vector<ClientConnection> m_clientConnections;
  std::unordered_map<uint64_t, AppInstance> m_instances;
  std::unordered_map<int32_t, uint64_t> m_instanceByPid;
  std::unordered_map<std::string, uint64_t> m_runningInstanceByAppId;
  std::unordered_map<uint64_t, std::vector<int>> m_waiters;
  uint64_t m_nextInstanceId{1};
};

} // namespace lcl::session
