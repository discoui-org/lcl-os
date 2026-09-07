#include "system/session/session_service.hpp"

#include "system/security/security_admin_client.hpp"
#include "system/security/security_admin_protocol.hpp"

#include "system/security/bundle_record.hpp"
#include "system/security/bundle_launch_gate.hpp"
#include "system/security/bundle_signature_backend.hpp"
#include "system/security/bundle_signature_verifier.hpp"
#include "system/security/session_user.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace lcl::session {
namespace {

constexpr size_t kReceiveBufferSize =
    kSessionWireHeaderSize + kSessionMaxPayload;

std::vector<std::string>
executableArgs(const core::AppBundleMetadata &app,
               const std::string &executableDescriptorPath) {
  if (app.runtime == "org.lcl.javascript" ||
      (app.executablePath.size() >= 3 &&
       app.executablePath.substr(app.executablePath.size() - 3) == ".js")) {
    std::string jsRuntime = "/System/Core/lcl-js";
    std::error_code ec;
    if (!std::filesystem::exists(jsRuntime, ec)) {
      jsRuntime = "/usr/bin/lcl-js";
    }
    return {jsRuntime, executableDescriptorPath};
  }
  return {executableDescriptorPath};
}

constexpr int kAppExecutableDescriptor = 3;
constexpr int kSecurityAdminCapabilityDescriptor = 4;

bool pinAndIsolateExecutableDescriptor(int descriptor,
                                       int securityCapabilityDescriptor = -1) {
  if (descriptor < 0 || securityCapabilityDescriptor == kAppExecutableDescriptor ||
      securityCapabilityDescriptor < -1) {
    return false;
  }
  // Preserve both sources before assigning their fixed exec descriptors: an
  // open executable can legitimately occupy fd 4 while the connected
  // capability occupies fd 3, so assigning either target first could clobber
  // the other source.
  const bool hasSecurityCapability = securityCapabilityDescriptor >= 0;
  const int preservedExecutable = hasSecurityCapability
      ? fcntl(descriptor, F_DUPFD_CLOEXEC, kSecurityAdminCapabilityDescriptor + 1)
      : descriptor;
  const int preservedCapability = hasSecurityCapability
      ? fcntl(securityCapabilityDescriptor, F_DUPFD_CLOEXEC,
              kSecurityAdminCapabilityDescriptor + 1)
      : -1;
  if (preservedExecutable < 0 || (hasSecurityCapability && preservedCapability < 0)) {
    if (preservedExecutable >= 0 && preservedExecutable != descriptor) close(preservedExecutable);
    if (preservedCapability >= 0 && preservedCapability != securityCapabilityDescriptor) {
      close(preservedCapability);
    }
    return false;
  }
  if (preservedExecutable != kAppExecutableDescriptor &&
      dup3(preservedExecutable, kAppExecutableDescriptor, 0) < 0) {
    if (preservedExecutable != descriptor) close(preservedExecutable);
    if (preservedCapability >= 0 && preservedCapability != securityCapabilityDescriptor) {
      close(preservedCapability);
    }
    return false;
  }
  const int flags = fcntl(kAppExecutableDescriptor, F_GETFD);
  if (flags < 0 ||
      fcntl(kAppExecutableDescriptor, F_SETFD, flags & ~FD_CLOEXEC) != 0) {
    return false;
  }
  if (hasSecurityCapability) {
    if (preservedCapability != kSecurityAdminCapabilityDescriptor &&
        dup3(preservedCapability, kSecurityAdminCapabilityDescriptor, 0) < 0) {
      if (preservedExecutable != descriptor) close(preservedExecutable);
      if (preservedCapability != securityCapabilityDescriptor) close(preservedCapability);
      return false;
    }
    const int capabilityFlags = fcntl(kSecurityAdminCapabilityDescriptor, F_GETFD);
    if (capabilityFlags < 0 ||
        fcntl(kSecurityAdminCapabilityDescriptor, F_SETFD,
              capabilityFlags & ~FD_CLOEXEC) != 0) {
      if (preservedExecutable != descriptor) close(preservedExecutable);
      if (preservedCapability != securityCapabilityDescriptor) close(preservedCapability);
      return false;
    }
  }
  if (preservedExecutable != descriptor) close(preservedExecutable);
  if (hasSecurityCapability && preservedCapability != securityCapabilityDescriptor) {
    close(preservedCapability);
  }

  struct rlimit limit{};
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0 ||
      limit.rlim_cur == RLIM_INFINITY ||
      limit.rlim_cur > static_cast<rlim_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  const int firstDescriptorToClose = hasSecurityCapability
      ? kSecurityAdminCapabilityDescriptor + 1
      : kAppExecutableDescriptor + 1;
  for (int inherited = firstDescriptorToClose;
       inherited < static_cast<int>(limit.rlim_cur); ++inherited) {
    close(inherited);
  }
  return true;
}

std::string resolvedIconPath(const core::AppBundleMetadata &app) {
  if (app.icon.empty())
    return {};
  std::filesystem::path icon(app.icon);
  if (icon.is_relative())
    icon = std::filesystem::path(app.bundlePath) / icon;
  std::error_code error;
  if (!std::filesystem::is_regular_file(icon, error) || error)
    return {};
  return icon.string();
}

void exportLaunchContext(const LaunchRequest &request, uint64_t instanceId) {
  setenv("LCL_APP_INSTANCE_ID", std::to_string(instanceId).c_str(), 1);
  if (request.launchToken != 0) {
    setenv("LCL_LAUNCH_TOKEN", std::to_string(request.launchToken).c_str(), 1);
  }
  const auto &origin = request.origin;
  if (!origin.valid)
    return;
  setenv("LCL_LAUNCH_ORIGIN_X", std::to_string(origin.x).c_str(), 1);
  setenv("LCL_LAUNCH_ORIGIN_Y", std::to_string(origin.y).c_str(), 1);
  setenv("LCL_LAUNCH_ORIGIN_WIDTH", std::to_string(origin.width).c_str(), 1);
  setenv("LCL_LAUNCH_ORIGIN_HEIGHT", std::to_string(origin.height).c_str(), 1);
  setenv("LCL_LAUNCH_ORIGIN_RADIUS",
         std::to_string(origin.cornerRadius).c_str(), 1);
}

std::optional<lcl::security::SandboxRuntime>
sandboxRuntimeForApp(const core::AppBundleMetadata &app, std::string &error) {
  error.clear();
  if (app.runtime.empty()) {
    const bool executableIsJavaScript =
        app.executable.size() >= 3 && app.executable.ends_with(".js");
    return executableIsJavaScript ? lcl::security::SandboxRuntime::JavaScript
                                  : lcl::security::SandboxRuntime::Native;
  }
  if (app.runtime == "org.lcl.native") {
    return lcl::security::SandboxRuntime::Native;
  }
  if (app.runtime == "org.lcl.javascript") {
    return lcl::security::SandboxRuntime::JavaScript;
  }
  error = "application declares an unsupported sandbox runtime: " + app.runtime;
  return std::nullopt;
}

bool isSystemImageApplication(const core::AppBundleMetadata &app) {
  const std::filesystem::path bundlePath(app.bundlePath);
  return bundlePath.is_absolute() &&
         bundlePath.parent_path() == std::filesystem::path("/System/Applications") &&
         bundlePath.extension() == ".app";
}

bool conflictsWithSystemImageApplication(const AppRegistry &registry,
                                         const core::AppBundleMetadata &app) {
  const auto systemApplication = registry.find(app.appId);
  return systemApplication && isSystemImageApplication(*systemApplication) &&
         !isSystemImageApplication(app);
}

lcl::security::BundleSourceScope bundleSourceScope(
    const core::AppBundleMetadata &app) {
  const std::filesystem::path bundlePath(app.bundlePath);
  if (isSystemImageApplication(app)) {
    return lcl::security::BundleSourceScope::System;
  }
  if (bundlePath.parent_path() == std::filesystem::path("/Applications")) {
    return lcl::security::BundleSourceScope::Machine;
  }
  if (bundlePath.parent_path() ==
      std::filesystem::path(lcl::security::kSessionUserHome) / "Applications") {
    return lcl::security::BundleSourceScope::User;
  }
  return lcl::security::BundleSourceScope::Direct;
}

lcl::security::PermissionSubject makeBundlePermissionSubject(
    const lcl::core::AppBundleMetadata &app,
    const lcl::security::Sha256Digest &bundleRecordDigest,
    std::string publisherIdentity) {
  return {
      .userUid = lcl::security::kSessionUserUid,
      .appId = app.appId,
      .bundleRecordDigest = bundleRecordDigest,
      .publisherIdentity = std::move(publisherIdentity),
      .permissionVersion = lcl::security::kPermissionDecisionVersion,
  };
}

} // namespace

SessionService::SessionService(std::vector<std::string> appSearchPaths,
                               std::string sandboxSocketPath,
                               lcl::security::PermissionStoreConfig permissionStoreConfig,
                               lcl::security::BundleApprovalStoreConfig bundleApprovalStoreConfig,
                               lcl::security::BundleSnapshotStoreConfig bundleSnapshotStoreConfig,
                               lcl::security::PendingBundleApprovalStoreConfig
                                   pendingBundleApprovalStoreConfig)
    : m_registry(std::move(appSearchPaths)),
      m_permissionStore(std::move(permissionStoreConfig)),
      m_bundleApprovals(std::move(bundleApprovalStoreConfig)),
      m_bundleSnapshots(std::move(bundleSnapshotStoreConfig)),
      m_pendingBundleApprovals(std::move(pendingBundleApprovalStoreConfig)),
      m_sandboxSocketPath(std::move(sandboxSocketPath)) {}

SessionService::~SessionService() { shutdown(); }

bool SessionService::initialize(const std::string &socketPath) {
  if (m_serverFd >= 0)
    return true;
  m_socketPath = socketPath;
  const std::filesystem::path parent =
      std::filesystem::path(socketPath).parent_path();
  if (!parent.empty()) {
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
      std::cerr << "[LCL Session ERROR] Could not create runtime directory "
                << parent << ": " << error.message() << "\n";
      return false;
    }
    // Runtime endpoint names must remain traversable by the session user:
    // the sockets themselves are the authorization boundary (0600 and
    // owned by that user).  A root-only 0700 /Runtime would make an
    // otherwise authorized session client fail before it can reach either
    // lcl-sessiond or the compositor socket.
    if (chmod(parent.c_str(), 0711) != 0) {
      std::cerr << "[LCL Session ERROR] Could not secure runtime directory "
                << parent << ": " << std::strerror(errno) << "\n";
      return false;
    }
  }
  unlink(socketPath.c_str());
  m_serverFd =
      socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (m_serverFd < 0)
    return false;
  // Each received request must identify its actual sending process. This
  // closes the otherwise persistent authority transfer path where a trusted
  // shell's connected session socket is inherited by or passed to an app.
  const int passCredentials = 1;
  if (setsockopt(m_serverFd, SOL_SOCKET, SO_PASSCRED, &passCredentials,
                 sizeof(passCredentials)) != 0) {
    close(m_serverFd);
    m_serverFd = -1;
    return false;
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (socketPath.size() >= sizeof(address.sun_path)) {
    close(m_serverFd);
    m_serverFd = -1;
    errno = ENAMETOOLONG;
    return false;
  }
  std::strncpy(address.sun_path, socketPath.c_str(),
               sizeof(address.sun_path) - 1);
  if (bind(m_serverFd, reinterpret_cast<sockaddr *>(&address),
           sizeof(address)) != 0 ||
      listen(m_serverFd, 32) != 0) {
    const int error = errno;
    close(m_serverFd);
    m_serverFd = -1;
    unlink(socketPath.c_str());
    errno = error;
    return false;
  }
  std::string ownershipError;
  if (!lcl::security::assignSessionUserOwnership(socketPath, 0600,
                                                 ownershipError)) {
    close(m_serverFd);
    m_serverFd = -1;
    unlink(socketPath.c_str());
    std::cerr << "[LCL Session ERROR] " << ownershipError << "\n";
    return false;
  }
  refreshCatalog();
  std::cout << "[LCL Session] Session authority active on " << socketPath
            << "\n";
  return true;
}

void SessionService::shutdown() {
  for (const auto &[_, instance] : m_instances) {
    if (instance.running && instance.pid > 0) {
      // Sessiond creates one process group per app instance, so shutdown
      // can reclaim the app and any children it owns without touching an
      // unrelated session process.
      kill(-instance.pid, SIGTERM);
    }
  }
  for (const auto &connection : m_clientConnections)
    close(connection.fd);
  m_clientConnections.clear();
  m_waiters.clear();
  m_sandboxClient.close();
  if (m_serverFd >= 0) {
    close(m_serverFd);
    m_serverFd = -1;
  }
  if (!m_socketPath.empty())
    unlink(m_socketPath.c_str());
}

void SessionService::refreshCatalog() {
  m_registry.refresh();
  std::cout << "[LCL Session] Catalog refreshed: "
            << m_registry.entries().size() << " application bundle(s).\n";
}

LaunchResponse SessionService::launch(const LaunchRequest &request) {
  LaunchResponse response{};
  auto app = m_registry.find(request.target);
  if (!app && std::filesystem::path(request.target).is_absolute() &&
      std::filesystem::path(request.target).extension() == ".app") {
    // Direct Finder/open selection still enters the exact same descriptor,
    // approval and snapshot chain. It is not a direct exec escape hatch.
    app = core::AppBundleParser::parseBundle(request.target);
  }
  if (!app || !app->valid) {
    response.status = 1;
    response.message = "unknown application: " + request.target;
    return response;
  }
  if (conflictsWithSystemImageApplication(m_registry, *app)) {
    response.status = 3;
    response.appId = app->appId;
    response.message = "external bundle cannot use a system application ID";
    return response;
  }
  // This identifier is capability-bearing: even when the system catalog is
  // unavailable, a user or machine bundle must never be able to claim it and
  // inherit Settings' direct-exec path.
  if (app->appId == lcl::security::kSystemSettingsAppId &&
      !lcl::security::isSystemSettingsBundle(app->appId, app->bundlePath)) {
    response.status = 3;
    response.appId = app->appId;
    response.message = "external bundle cannot use the Settings application ID";
    return response;
  }
  if (request.singleInstance) {
    const auto running = m_runningInstanceByAppId.find(app->appId);
    if (running != m_runningInstanceByAppId.end()) {
      const auto instance = m_instances.find(running->second);
      if (instance != m_instances.end() && instance->second.running) {
        response.instanceId = instance->second.instanceId;
        response.pid = instance->second.pid;
        response.reused = true;
        response.appId = instance->second.appId;
        response.message = "reused";
        return response;
      }
      m_runningInstanceByAppId.erase(running);
    }
  }

  const uint64_t instanceId = m_nextInstanceId++;
  const bool isTrustedUserShell =
      lcl::security::isTrustedUserShellBundle(app->appId, app->bundlePath);
  const bool isSystemSettings =
      lcl::security::isSystemSettingsBundle(app->appId, app->bundlePath);
  // Direct exec is limited to the immutable Terminal shell and Settings.
  // Settings receives one pre-connected, purpose-limited security capability;
  // every other bundle reaches root-owned sandboxd.
  if (!isTrustedUserShell && !isSystemSettings) {
    return launchSandboxed(*app, instanceId);
  }
  if (!app->executableHandle || !app->executableHandle->valid()) {
    response.status = 2;
    response.message =
        "application executable handle is unavailable: " + app->executablePath;
    return response;
  }

  lcl::security::SecurityAdminClient securityCapability;
  if (isSystemSettings) {
    std::string capabilityError;
    if (!securityCapability.connectAsSessionAuthority(
            lcl::security::kSecurityAdminSocket, capabilityError)) {
      response.status = 3;
      response.appId = app->appId;
      response.message = "Settings security capability is unavailable: " + capabilityError;
      return response;
    }
  }

  const pid_t child = fork();
  if (child < 0) {
    response.status = 4;
    response.message = std::string("fork failed: ") + std::strerror(errno);
    return response;
  }
  if (child == 0) {
    setsid();
    if (isTrustedUserShell) {
      std::string profileError;
      if (!lcl::security::prepareTrustedUserShellEnvironment(instanceId,
                                                             profileError)) {
        _exit(127);
      }
    } else if (isSystemSettings) {
      std::string profileError;
      if (!lcl::security::prepareSystemSettingsEnvironment(instanceId, profileError)) {
        _exit(127);
      }
    }
    exportLaunchContext(request, instanceId);
    std::string identityError;
    if (!lcl::security::dropToSessionUser(identityError)) {
      _exit(127);
    }
    // Keep only the pinned executable, plus Settings' single pre-connected
    // capability when applicable. No listener, store, control, or arbitrary
    // inherited descriptor can cross this exec boundary.
    if (!pinAndIsolateExecutableDescriptor(
            app->executableHandle->descriptor(),
            isSystemSettings ? securityCapability.capabilityDescriptor() : -1)) {
      _exit(127);
    }
    const auto args = executableArgs(
        *app, "/proc/self/fd/" + std::to_string(kAppExecutableDescriptor));
    if (args.empty()) {
      _exit(127);
    }
    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const auto &arg : args)
      argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
    execv(argv.front(), argv.data());
    _exit(127);
  }

  AppInstance instance;
  instance.instanceId = instanceId;
  instance.appId = app->appId;
  instance.pid = static_cast<int32_t>(child);
  instance.sandboxed = false;
  instance.running = true;
  m_instanceByPid.emplace(instance.pid, instance.instanceId);
  m_instances.emplace(instance.instanceId, instance);
  m_runningInstanceByAppId[instance.appId] = instance.instanceId;

  response.instanceId = instance.instanceId;
  response.pid = instance.pid;
  response.appId = instance.appId;
  response.message = "launched";
  std::cout << "[LCL Session] Launched " << app->appId << " as instance "
            << instance.instanceId << " (PID " << child << ")\n";
  return response;
}

LaunchResponse
SessionService::launchSandboxed(const core::AppBundleMetadata &app,
                                uint64_t instanceId) {
  LaunchResponse response{};
  response.appId = app.appId;
  if (!app.executableHandle || !app.executableHandle->valid()) {
    response.status = 2;
    response.message =
        "application executable handle is unavailable: " + app.executablePath;
    return response;
  }

  std::string error;
  const auto record = lcl::security::makeBundleRecord(app, error);
  if (!record || record->appId != app.appId) {
    response.status = 3;
    response.message =
        "lcl-sandboxd launch rejected because bundle verification failed";
    if (!error.empty()) {
      response.message += ": " + error;
    }
    return response;
  }
  const auto runtime = sandboxRuntimeForApp(app, error);
  if (!runtime) {
    response.status = 3;
    response.message = "lcl-sandboxd launch rejected: " + error;
    return response;
  }
  core::AppBundleMetadata protectedBundle = app;
  lcl::security::PermissionSubject permissionSubject{};
  if (isSystemImageApplication(app)) {
    permissionSubject = lcl::security::makeSystemImagePermissionSubject(
        lcl::security::kSessionUserUid, app.appId, record->digest);
  } else {
    lcl::security::OpenSslEd25519Verifier ed25519;
    lcl::security::RootPublisherTrustStore publisherTrust;
    lcl::security::BundleSignatureVerifier signatureVerifier(ed25519,
                                                               publisherTrust);
    const lcl::security::BundleSignatureVerification signature =
        signatureVerifier.verify(app, *record);
    const lcl::security::BundlePublisherState publisherState =
        signature.publisherState;
    const std::string publisherIdentity =
        publisherState == lcl::security::BundlePublisherState::SignatureVerified
            ? "ed25519:" +
                  lcl::security::hexEncodeDigest(signature.publisherFingerprint)
            : "unverified";
    lcl::security::BundleLaunchGate gate(m_bundleApprovals);
    const lcl::security::BundleLaunchAssessment assessment = gate.assess(
        lcl::security::kSessionUserUid, bundleSourceScope(app), *record,
        publisherState);
    if (!assessment.allowed()) {
      response.status = 3;
      if (assessment.decision ==
          lcl::security::BundleLaunchDecision::NeedsUserApproval) {
        if (!m_pendingBundleApprovals.record(
                lcl::security::kSessionUserUid, *record,
                publisherState,
                bundleSourceScope(app), app.bundlePath, app.name, error)) {
          response.message =
              "lcl-sandboxd launch rejected because the pending bundle approval could not be saved: " +
              error;
          return response;
        }
      }
      response.message =
          "lcl-sandboxd launch rejected because bundle is not accepted: " +
          assessment.reason;
      return response;
    }
    if (!m_bundleSnapshots.stage(app, *record, protectedBundle, error)) {
      response.status = 3;
      response.message = "lcl-sandboxd launch rejected because protected bundle staging failed: " +
                         error;
      return response;
    }
    if (!protectedBundle.bundleHandle || !protectedBundle.bundleHandle->valid() ||
        !protectedBundle.executableHandle || !protectedBundle.executableHandle->valid()) {
      response.status = 3;
      response.message = "lcl-sandboxd launch rejected because protected bundle descriptors are unavailable";
      return response;
    }
    if (!m_sandboxClient.connected() &&
        !m_sandboxClient.connect(m_sandboxSocketPath, error)) {
      response.status = 3;
      response.message = "lcl-sandboxd is unavailable: " + error;
      return response;
    }
    permissionSubject = makeBundlePermissionSubject(
        app, record->digest, publisherIdentity);
    const lcl::security::SandboxApplicationRegistration registration{
        .userUid = permissionSubject.userUid,
        .appId = app.appId,
        .runtime = *runtime,
        .requestedPermissions = app.requestedPermissions,
        .bundleRecordDigest = record->digest,
        .publisherIdentity = permissionSubject.publisherIdentity,
        .executableBundlePath = protectedBundle.executable,
    };
    if (!m_sandboxClient.registerExternalApplication(
            registration,
            {protectedBundle.bundleHandle->descriptor(),
             protectedBundle.executableHandle->descriptor()},
            error)) {
      response.status = 3;
      response.message = "lcl-sandboxd launch rejected because protected bundle registration failed: " +
                         error;
      return response;
    }
  }
  const std::vector<std::string> grantedPermissions =
      m_permissionStore.grantedPermissions(permissionSubject,
                                           app.requestedPermissions, error);
  if (!error.empty()) {
    response.status = 3;
    response.message = "lcl-sandboxd launch rejected because permission policy is unavailable: " +
                       error;
    return response;
  }
  const auto profile = lcl::security::makeThirdPartySandboxProfile(
      *runtime, app.requestedPermissions, grantedPermissions, error);
  if (!profile) {
    response.status = 3;
    response.message =
        "lcl-sandboxd launch rejected because sandbox policy is invalid: " +
        error;
    return response;
  }
  lcl::security::SandboxLaunchRequest sandboxRequest{};
  sandboxRequest.appId = app.appId;
  sandboxRequest.instanceId = instanceId;
  sandboxRequest.bundleRecordDigest = record->digest;
  sandboxRequest.profileDigest = lcl::security::digestSandboxProfile(*profile);
  if (!lcl::security::validateSandboxLaunchRequest(sandboxRequest, error)) {
    response.status = 3;
    response.message = "lcl-sandboxd launch request is invalid: " + error;
    return response;
  }
  if (!m_sandboxClient.connected() &&
      !m_sandboxClient.connect(m_sandboxSocketPath, error)) {
    response.status = 3;
    response.message = "lcl-sandboxd is unavailable: " + error;
    return response;
  }

  lcl::security::SandboxLaunchResult sandboxResult{};
  if (!m_sandboxClient.launch(sandboxRequest, sandboxResult, error)) {
    m_sandboxClient.close();
    response.status = 3;
    response.message = "lcl-sandboxd launch failed: " + error;
    return response;
  }
  if (sandboxResult.status != lcl::security::SandboxLaunchStatus::Launched ||
      sandboxResult.instanceId != instanceId || sandboxResult.pid <= 0 ||
      sandboxResult.processGroupId != sandboxResult.pid) {
    response.status = 3;
    response.message = "lcl-sandboxd rejected launch";
    if (!sandboxResult.message.empty()) {
      response.message += ": " + sandboxResult.message;
    }
    return response;
  }

  AppInstance instance{};
  instance.instanceId = instanceId;
  instance.appId = app.appId;
  instance.pid = sandboxResult.pid;
  instance.sandboxed = true;
  instance.running = true;
  m_instanceByPid.emplace(instance.pid, instance.instanceId);
  m_instances.emplace(instance.instanceId, instance);
  m_runningInstanceByAppId[instance.appId] = instance.instanceId;

  response.instanceId = instance.instanceId;
  response.pid = instance.pid;
  response.message = "launched";
  std::cout << "[LCL Session] Requested sandbox launch of " << app.appId
            << " as instance " << instance.instanceId << " (PID "
            << instance.pid << ")\n";
  return response;
}

bool SessionService::launchDefaultProfile() {
  const LaunchResponse response = launch({"org.lcl.terminal", false});
  if (response.status != 0) {
    std::cerr << "[LCL Session ERROR] Default profile launch failed: "
              << response.message << "\n";
    return false;
  }
  return true;
}

bool SessionService::sendPacket(int fd, SessionOpcode opcode,
                                uint32_t requestId,
                                const std::vector<uint8_t> &payload) {
  SessionHeader header{};
  header.opcode = opcode;
  header.requestId = requestId;
  header.payloadSize = static_cast<uint32_t>(payload.size());
  std::vector<uint8_t> packet;
  if (!encodePacket(header, payload, packet))
    return false;
  return send(fd, packet.data(), packet.size(), MSG_NOSIGNAL) ==
         static_cast<ssize_t>(packet.size());
}

void SessionService::acceptConnections() {
  while (true) {
    const int fd =
        accept4(m_serverFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd >= 0) {
      ClientConnection connection{};
      if (peerIsTrustedSessionAuthority(fd, connection)) {
        m_clientConnections.push_back(connection);
      } else {
        close(fd);
      }
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      std::cerr << "[LCL Session ERROR] accept failed: " << std::strerror(errno)
                << "\n";
    }
    return;
  }
}

bool SessionService::peerIsTrustedSessionAuthority(
    int fd, ClientConnection &connection) const {
  ucred credentials{};
  socklen_t length = sizeof(credentials);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0 ||
      length != sizeof(credentials) || credentials.pid <= 0 ||
      credentials.uid != lcl::security::kSessionUserUid ||
      credentials.gid != lcl::security::kSessionUserGid) {
    return false;
  }
  connection = {fd, credentials.pid, credentials.uid, credentials.gid};
  return true;
}

void SessionService::serviceClient(const ClientConnection &connection) {
  const int fd = connection.fd;
  std::array<uint8_t, kReceiveBufferSize> bytes{};
  while (true) {
    std::array<char, CMSG_SPACE(sizeof(ucred)) + CMSG_SPACE(sizeof(int) * 4)>
        control{};
    iovec vector{.iov_base = bytes.data(), .iov_len = bytes.size()};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    const ssize_t count = recvmsg(fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (count == 0) {
      removeClient(fd);
      return;
    }
    if (count < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;
      removeClient(fd);
      return;
    }
    bool validSender = (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) == 0;
    bool sawCredentials = false;
    for (cmsghdr *header = CMSG_FIRSTHDR(&message); header;
         header = CMSG_NXTHDR(&message, header)) {
      if (header->cmsg_level == SOL_SOCKET &&
          header->cmsg_type == SCM_CREDENTIALS &&
          header->cmsg_len == CMSG_LEN(sizeof(ucred)) && !sawCredentials) {
        ucred credentials{};
        std::memcpy(&credentials, CMSG_DATA(header), sizeof(credentials));
        sawCredentials = true;
        validSender = validSender && credentials.pid == connection.pid &&
                      credentials.uid == connection.uid &&
                      credentials.gid == connection.gid;
      } else if (header->cmsg_level == SOL_SOCKET &&
                 header->cmsg_type == SCM_RIGHTS &&
                 header->cmsg_len >= CMSG_LEN(0)) {
        const auto count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t index = 0; index < count; ++index) {
          int received = -1;
          std::memcpy(&received, CMSG_DATA(header) + index * sizeof(int),
                      sizeof(received));
          if (received >= 0) close(received);
        }
        validSender = false;
      } else {
        validSender = false;
      }
    }
    if (!validSender || !sawCredentials) {
      removeClient(fd);
      return;
    }
    DecodedPacket packet{};
    if (!decodePacket(bytes.data(), static_cast<size_t>(count), packet)) {
      std::vector<uint8_t> error;
      encodeError("invalid session packet", error);
      sendPacket(fd, SessionOpcode::ErrorResponse, 1, error);
      removeClient(fd);
      return;
    }
    if (packet.header.opcode == SessionOpcode::CatalogRequest &&
        packet.payload.empty()) {
      std::vector<CatalogEntry> entries;
      entries.reserve(m_registry.entries().size());
      for (const auto &app : m_registry.entries()) {
        entries.push_back({app.appId, app.name, app.version,
                           resolvedIconPath(app), app.type});
      }
      std::vector<uint8_t> payload;
      encodeCatalogSnapshot(entries, payload);
      sendPacket(fd, SessionOpcode::CatalogSnapshot, packet.header.requestId,
                 payload);
      continue;
    }
    if (packet.header.opcode == SessionOpcode::LaunchRequest) {
      LaunchRequest request;
      LaunchResponse response;
      if (!decodeLaunchRequest(packet.payload, request)) {
        response.status = 5;
        response.message = "invalid launch request";
      } else {
        response = launch(request);
      }
      std::vector<uint8_t> payload;
      encodeLaunchResponse(response, payload);
      if (!sendPacket(fd, SessionOpcode::LaunchResponse,
                      packet.header.requestId, payload)) {
        removeClient(fd);
        return;
      }
      if (response.status == 0 && request.waitForExit) {
        m_waiters[response.instanceId].push_back(fd);
      }
      continue;
    }
    std::vector<uint8_t> error;
    encodeError("session opcode not accepted", error);
    sendPacket(fd, SessionOpcode::ErrorResponse, packet.header.requestId,
               error);
  }
}

void SessionService::removeClient(int fd) {
  std::erase_if(m_clientConnections,
                [fd](const ClientConnection &connection) { return connection.fd == fd; });
  for (auto &[_, waiters] : m_waiters)
    std::erase(waiters, fd);
  close(fd);
}

int SessionService::exitCodeFromStatus(int status) {
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return 1;
}

void SessionService::notifyExit(const ProcessExited &event) {
  const auto found = m_waiters.find(event.instanceId);
  if (found == m_waiters.end())
    return;
  std::vector<uint8_t> payload;
  encodeProcessExited(event, payload);
  for (const int fd : found->second) {
    sendPacket(fd, SessionOpcode::ProcessExited, 1, payload);
  }
  m_waiters.erase(found);
}

void SessionService::reapChildren() {
  while (true) {
    int status = 0;
    const pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid <= 0)
      return;
    const auto found = m_instanceByPid.find(static_cast<int32_t>(pid));
    if (found == m_instanceByPid.end())
      continue;
    const uint64_t instanceId = found->second;
    if (const auto instance = m_instances.find(instanceId);
        instance != m_instances.end() && !instance->second.sandboxed) {
      completeInstanceExit(instanceId, exitCodeFromStatus(status));
    }
  }
}

void SessionService::reapSandboxChildren() {
  if (!m_sandboxClient.connected()) {
    return;
  }
  while (true) {
    lcl::security::SandboxProcessExited exited{};
    std::string error;
    if (m_sandboxClient.pollExit(exited, error)) {
      const auto instance = m_instances.find(exited.instanceId);
      if (instance != m_instances.end() && instance->second.sandboxed &&
          instance->second.running) {
        completeInstanceExit(exited.instanceId, exited.exitCode);
      }
      continue;
    }
    if (error.empty()) {
      return;
    }

    // A lost control connection must not preserve a stale running entry.
    // sandboxd also gives every child a parent-death kill relationship;
    // the explicit group kill closes the same boundary for protocol or
    // socket failures while sandboxd is still alive.
    std::cerr << "[LCL Session ERROR] lcl-sandboxd lifecycle channel failed: "
              << error << "\n";
    m_sandboxClient.close();
    std::vector<uint64_t> sandboxedInstances;
    for (const auto &[instanceId, instance] : m_instances) {
      if (instance.sandboxed && instance.running) {
        sandboxedInstances.push_back(instanceId);
      }
    }
    for (const uint64_t instanceId : sandboxedInstances) {
      const auto instance = m_instances.find(instanceId);
      if (instance != m_instances.end() && instance->second.pid > 0) {
        kill(-instance->second.pid, SIGKILL);
      }
      completeInstanceExit(instanceId, 128 + SIGKILL);
    }
    return;
  }
}

void SessionService::completeInstanceExit(uint64_t instanceId, int exitCode) {
  const auto instance = m_instances.find(instanceId);
  if (instance == m_instances.end() || !instance->second.running) {
    return;
  }
  m_instanceByPid.erase(instance->second.pid);
  instance->second.running = false;
  instance->second.exitCode = exitCode;
  const auto running = m_runningInstanceByAppId.find(instance->second.appId);
  if (running != m_runningInstanceByAppId.end() &&
      running->second == instanceId) {
    m_runningInstanceByAppId.erase(running);
  }
  notifyExit({instanceId, exitCode});
  std::cout << "[LCL Session] Instance " << instanceId << " exited with "
            << exitCode << "\n";
}

void SessionService::poll() {
  if (m_serverFd >= 0) {
    acceptConnections();
    const auto clients = m_clientConnections;
    for (const auto &connection : clients) {
      const auto found = std::find_if(
          m_clientConnections.begin(), m_clientConnections.end(),
          [&connection](const ClientConnection &current) {
            return current.fd == connection.fd;
          });
      if (found != m_clientConnections.end()) {
        serviceClient(*found);
      }
    }
  }
  reapChildren();
  reapSandboxChildren();
}

} // namespace lcl::session
