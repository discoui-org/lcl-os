#include "system/security/sandbox_daemon.hpp"
#include "system/security/sandbox_platform_probe.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <signal.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>

namespace lcl::security {
namespace {

constexpr std::size_t kReceiveBufferSize = kSandboxWireHeaderSize + kSandboxMaxPayload;

bool isSafeSocketPath(const std::string& path) {
    const std::filesystem::path candidate(path);
    if (!candidate.is_absolute() || candidate.filename() == "." || candidate.filename() == ".." ||
        candidate.parent_path() == candidate) {
        return false;
    }
    for (const auto& component : candidate) {
        if (component == "." || component == "..") {
            return false;
        }
    }
    return true;
}

SandboxLaunchResult rejectedLaunch(const SandboxLaunchRequest& request, SandboxLaunchStatus status,
                                   std::string message) {
    SandboxLaunchResult result{};
    result.status = status;
    result.instanceId = request.instanceId;
    result.message = std::move(message);
    return result;
}

} // namespace

SandboxDaemon::SandboxDaemon(SandboxDaemonConfig config)
    : authorizer_(config.permissionStore), externalApplications_(config.externalApplications),
      launchRegistry_(config.launches),
      config_(std::move(config)) {}

SandboxDaemon::~SandboxDaemon() { shutdown(); }

bool SandboxDaemon::validateConfig(std::string& error) const {
    error.clear();
    if (!isSafeSocketPath(config_.socketPath) ||
        config_.socketPath.size() >= sizeof(sockaddr_un{}.sun_path)) {
        error = "sandboxd socket path is not a safe Unix socket path";
        return false;
    }
    const bool rootSessionAuthority = config_.sessionUid == 0 && config_.sessionGid == 0;
    const bool dedicatedUnprivilegedAuthority = config_.sessionUid != 0 && config_.sessionGid != 0;
    if (config_.ownerUid != 0 || config_.ownerGid != 0 ||
        (!rootSessionAuthority && !dedicatedUnprivilegedAuthority)) {
        error = "sandboxd requires root ownership and one complete launch-authority identity";
        return false;
    }
    return true;
}

bool SandboxDaemon::validateSocketParent(std::string& error) const {
    const std::filesystem::path parent = std::filesystem::path(config_.socketPath).parent_path();
    struct stat status {};
    if (parent.empty() || lstat(parent.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode) || status.st_uid != config_.ownerUid ||
        status.st_gid != config_.ownerGid || (status.st_mode & 0022) != 0) {
        error = "sandboxd socket parent is not a root-owned non-writable directory";
        return false;
    }
    return true;
}

bool SandboxDaemon::initialize(std::string& error) {
    error.clear();
    if (serverDescriptor_ >= 0) {
        return true;
    }
    if (geteuid() != 0) {
        error = "lcl-sandboxd must run as root";
        return false;
    }
    if (!validateConfig(error) || !validateSocketParent(error)) {
        return false;
    }
    if (!launchRegistry_.initializeAndReset(error)) {
        return false;
    }
    const SandboxPlatformCapabilities capabilities = probeSandboxPlatformCapabilities();
    const auto hardening = makeSandboxPlatformHardening(capabilities, config_.platformMode, error);
    if (!hardening) {
        error = "mandatory common app capability profile is unavailable: " + error + "\n" +
                formatSandboxPlatformCapabilities(capabilities);
        return false;
    }
    // Linux uses cgroup v2 for accounting before the socket becomes reachable.
    // Android keeps the same capability profile but uses the portable rlimit
    // subset inside the child because the direct-deploy chroot cannot own its
    // host task profiles.
    if (hardening->requireCgroupResourceAccounting && !cgroupManager_.initialize(error)) {
        return false;
    }
    hardening_ = *hardening;
    hardeningReady_ = true;

    struct stat existing {};
    if (lstat(config_.socketPath.c_str(), &existing) == 0) {
        const bool rootOwned = existing.st_uid == config_.ownerUid &&
                               existing.st_gid == config_.ownerGid;
        const bool expectedPreviousSocket = existing.st_uid == config_.sessionUid &&
                                            existing.st_gid == config_.sessionGid &&
                                            (existing.st_mode & 0777) == 0600;
        // The directory itself is root-owned and not writable by the session
        // identity, so this expected session-owned stale socket cannot have
        // been replaced by that peer between inspection and unlink.
        if (!S_ISSOCK(existing.st_mode) || (!rootOwned && !expectedPreviousSocket)) {
            error = "sandboxd refuses to replace an unexpected socket path";
            return false;
        }
        if (unlink(config_.socketPath.c_str()) != 0) {
            error = std::string("could not remove stale sandboxd socket: ") + std::strerror(errno);
            return false;
        }
    } else if (errno != ENOENT) {
        error = std::string("could not inspect sandboxd socket path: ") + std::strerror(errno);
        return false;
    }

    const mode_t previousUmask = umask(0077);
    const int descriptor = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    umask(previousUmask);
    if (descriptor < 0) {
        error = std::string("could not create sandboxd socket: ") + std::strerror(errno);
        return false;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, config_.socketPath.c_str(), sizeof(address.sun_path) - 1);
    if (bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        lchown(config_.socketPath.c_str(), config_.sessionUid, config_.sessionGid) != 0 ||
        chmod(config_.socketPath.c_str(), 0600) != 0 || listen(descriptor, 8) != 0) {
        const int savedErrno = errno;
        close(descriptor);
        unlink(config_.socketPath.c_str());
        error = std::string("could not initialize sandboxd socket: ") + std::strerror(savedErrno);
        return false;
    }
    serverDescriptor_ = descriptor;
    ownsSocketPath_ = true;
    return true;
}

void SandboxDaemon::shutdown() {
    childReaper_.terminateAll(SIGTERM);
    if (hardeningReady_) {
        std::string cleanupError;
        launchRegistry_.initializeAndReset(cleanupError);
    }
    for (const int descriptor : clientDescriptors_) {
        close(descriptor);
    }
    clientDescriptors_.clear();
    if (serverDescriptor_ >= 0) {
        close(serverDescriptor_);
        serverDescriptor_ = -1;
    }
    if (ownsSocketPath_ && !config_.socketPath.empty()) {
        struct stat status {};
        if (lstat(config_.socketPath.c_str(), &status) == 0 && S_ISSOCK(status.st_mode) &&
            status.st_uid == config_.sessionUid && status.st_gid == config_.sessionGid) {
            unlink(config_.socketPath.c_str());
        }
    }
    ownsSocketPath_ = false;
    hardeningReady_ = false;
}

bool SandboxDaemon::registerVerifiedApplication(const VerifiedApplication& application,
                                                const std::vector<std::string>& grantedPermissions,
                                                std::string& error) {
    return authorizer_.registerVerifiedApplication(application, grantedPermissions, error);
}

bool SandboxDaemon::registerVerifiedLaunchMaterial(const SandboxLaunchMaterialInput& input,
                                                   std::string& error) {
    return materialRegistry_.registerMaterial(input, error);
}

bool SandboxDaemon::registerExternalApplication(
    const SandboxApplicationRegistration& registration, int appBundleDescriptor,
    int executableDescriptor, std::string& error) {
    return externalApplications_.registerSnapshot(*this, registration, appBundleDescriptor,
                                                  executableDescriptor, error);
}

void SandboxDaemon::removeVerifiedApplication(const std::string& appId) {
    authorizer_.remove(appId);
    materialRegistry_.remove(appId);
}

bool SandboxDaemon::hasRegisteredApplication(const std::string& appId) const {
    return authorizer_.contains(appId);
}

std::size_t SandboxDaemon::registeredApplicationCount() const {
    return authorizer_.size();
}

bool SandboxDaemon::recordLaunchedChild(const std::string& appId,
                                        const SandboxLaunchResult& launch,
                                        std::string& error) {
    return childReaper_.track(appId, launch, error);
}

bool SandboxDaemon::recordLaunchedChild(const std::string& appId,
                                        const SandboxLaunchResult& launch,
                                        const SandboxCgroup& cgroup,
                                        std::string& error) {
    return childReaper_.track(appId, launch, cgroup, error);
}

std::size_t SandboxDaemon::runningChildCount() const {
    return childReaper_.size();
}

SandboxLaunchResult SandboxDaemon::handleLaunchRequest(const SandboxLaunchRequest& request) {
    std::string error;
    refreshRuntimeEndpointState();
    if (!validateSandboxLaunchRequest(request, error)) {
        return rejectedLaunch(request, SandboxLaunchStatus::InvalidRequest, std::move(error));
    }
    if (!hardeningReady_) {
        return rejectedLaunch(request, SandboxLaunchStatus::SetupFailed,
                              "sandboxd platform hardening is not initialized");
    }
    if (runtimeEndpointRefreshRequired_) {
        return rejectedLaunch(request, SandboxLaunchStatus::SetupFailed,
                              "sandbox graphics endpoints changed; application registration is refreshing");
    }
    const auto plan = authorizer_.authorize(request, error);
    if (!plan) {
        return rejectedLaunch(request, SandboxLaunchStatus::PolicyRejected, std::move(error));
    }
    // Material must exist before resource allocation.  In particular, a stale
    // session request cannot leave an empty instance cgroup behind or turn
    // filesystem verification into a side effect of attacker-controlled IPC.
    if (!materialRegistry_.hasMaterialFor(*plan)) {
        return rejectedLaunch(request, SandboxLaunchStatus::SetupFailed,
                              "verified sandbox launch material is unavailable for this request");
    }

    std::optional<SandboxCgroup> cgroup;
    std::optional<SandboxCgroupBinding> cgroupBinding;
    if (hardening_.requireCgroupResourceAccounting) {
        cgroup = cgroupManager_.createInstance(*plan, hardening_, error);
        if (!cgroup) {
            return rejectedLaunch(request, SandboxLaunchStatus::SetupFailed,
                                  "could not create mandatory sandbox cgroup: " + error);
        }
        cgroupBinding = SandboxCgroupBinding{.manager = &cgroupManager_, .cgroup = *cgroup};
    }
    SandboxChildLaunchSpec spec{};
    if (!materialRegistry_.makeChildLaunchSpec(*plan, hardening_, std::move(cgroupBinding), spec,
                                               error)) {
        if (cgroup) {
            std::string cleanupError;
            cgroupManager_.removeInstance(*cgroup, cleanupError);
        }
        return rejectedLaunch(request, SandboxLaunchStatus::SetupFailed, std::move(error));
    }
    bool launchIdentityRegistered = false;
    SandboxLaunchResult launch = spawnSandboxChild(
        spec, [&](pid_t processGroupId, std::string& registrationError) {
            launchIdentityRegistered = launchRegistry_.registerLaunch(
                {
                    .instanceId = plan->request.instanceId,
                    .appId = plan->request.appId,
                    .processGroupId = processGroupId,
                    .uid = spec.identity.uid,
                    .gid = spec.identity.gid,
                },
                registrationError);
            return launchIdentityRegistered;
        });
    if (launch.status != SandboxLaunchStatus::Launched) {
        if (launchIdentityRegistered) {
            std::string cleanupError;
            launchRegistry_.remove(plan->request.instanceId, cleanupError);
        }
        if (cgroup) {
            // Child-side setup normally removes this itself.  Repeating the
            // root-owned removal is harmless when it already disappeared and
            // closes the early-failure path before any child was created.
            std::string cleanupError;
            cgroupManager_.removeInstance(*cgroup, cleanupError);
        }
        return launch;
    }
    if (cgroup) {
        if (!recordLaunchedChild(plan->request.appId, launch, *cgroup, error)) {
            kill(-launch.processGroupId, SIGKILL);
            kill(launch.pid, SIGKILL);
            while (waitpid(launch.pid, nullptr, 0) < 0 && errno == EINTR) {
            }
            std::string cleanupError;
            cgroupManager_.removeInstance(*cgroup, cleanupError);
            launchRegistry_.remove(plan->request.instanceId, cleanupError);
            return rejectedLaunch(request, SandboxLaunchStatus::SetupFailed,
                                  "could not track sandbox child ownership: " + error);
        }
    } else if (!recordLaunchedChild(plan->request.appId, launch, error)) {
        kill(-launch.processGroupId, SIGKILL);
        kill(launch.pid, SIGKILL);
        while (waitpid(launch.pid, nullptr, 0) < 0 && errno == EINTR) {
        }
        std::string cleanupError;
        launchRegistry_.remove(plan->request.instanceId, cleanupError);
        return rejectedLaunch(request, SandboxLaunchStatus::SetupFailed,
                              "could not track sandbox child ownership: " + error);
    }
    return launch;
}

bool SandboxDaemon::peerIsAuthorized(int descriptor) const {
    struct ucred credential {};
    socklen_t length = sizeof(credential);
    return getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credential, &length) == 0 &&
           length == sizeof(credential) && credential.uid == config_.sessionUid &&
           credential.gid == config_.sessionGid;
}

bool SandboxDaemon::sendPacket(int descriptor, SandboxOpcode opcode, std::uint32_t requestId,
                               const std::vector<std::uint8_t>& payload) {
    SandboxHeader header{};
    header.opcode = opcode;
    header.requestId = requestId;
    header.payloadSize = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> packet;
    return encodeSandboxPacket(header, payload, packet) &&
           send(descriptor, packet.data(), packet.size(), MSG_NOSIGNAL) ==
               static_cast<ssize_t>(packet.size());
}

void SandboxDaemon::acceptConnections() {
    while (true) {
        const int descriptor = accept4(serverDescriptor_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (descriptor >= 0) {
            if (peerIsAuthorized(descriptor)) {
                clientDescriptors_.push_back(descriptor);
            } else {
                close(descriptor);
            }
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        return;
    }
}

void SandboxDaemon::sendErrorAndClose(int descriptor, std::uint32_t requestId,
                                      const std::string& message) {
    std::vector<std::uint8_t> payload;
    if (encodeSandboxError(message, payload)) {
        sendPacket(descriptor, SandboxOpcode::ErrorResponse, requestId, payload);
    }
    removeClient(descriptor);
}

void SandboxDaemon::serviceClient(int descriptor) {
    std::array<std::uint8_t, kReceiveBufferSize> bytes{};
    while (true) {
        std::array<char, CMSG_SPACE(sizeof(int) * 2)> control{};
        iovec vector{.iov_base = bytes.data(), .iov_len = bytes.size()};
        msghdr message{};
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        const ssize_t count = recvmsg(descriptor, &message,
                                      MSG_DONTWAIT | MSG_TRUNC | MSG_CMSG_CLOEXEC);
        std::array<int, 2> receivedDescriptors{-1, -1};
        std::size_t receivedDescriptorCount = 0;
        bool invalidDescriptorTransfer = (message.msg_flags & MSG_CTRUNC) != 0;
        for (cmsghdr* controlMessage = CMSG_FIRSTHDR(&message); controlMessage;
             controlMessage = CMSG_NXTHDR(&message, controlMessage)) {
            const bool rightsMessage = controlMessage->cmsg_level == SOL_SOCKET &&
                                       controlMessage->cmsg_type == SCM_RIGHTS &&
                                       controlMessage->cmsg_len >= CMSG_LEN(0);
            const std::size_t descriptorBytes = rightsMessage
                ? controlMessage->cmsg_len - CMSG_LEN(0)
                : 0;
            if (!rightsMessage || descriptorBytes % sizeof(int) != 0 ||
                descriptorBytes != sizeof(int) * receivedDescriptors.size() ||
                receivedDescriptorCount != 0) {
                invalidDescriptorTransfer = true;
                if (rightsMessage && descriptorBytes % sizeof(int) == 0) {
                    const int* unexpected =
                        reinterpret_cast<const int*>(CMSG_DATA(controlMessage));
                    for (std::size_t index = 0; index < descriptorBytes / sizeof(int); ++index) {
                        close(unexpected[index]);
                    }
                }
                continue;
            }
            std::memcpy(receivedDescriptors.data(), CMSG_DATA(controlMessage),
                        sizeof(int) * receivedDescriptors.size());
            receivedDescriptorCount = receivedDescriptors.size();
        }
        const auto closeReceivedDescriptors = [&] {
            for (const int received : receivedDescriptors) {
                if (received >= 0) {
                    close(received);
                }
            }
        };
        if (count == 0) {
            closeReceivedDescriptors();
            removeClient(descriptor);
            return;
        }
        if (count < 0) {
            closeReceivedDescriptors();
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            removeClient(descriptor);
            return;
        }
        if (static_cast<std::size_t>(count) > bytes.size()) {
            closeReceivedDescriptors();
            sendErrorAndClose(descriptor, 1, "sandbox packet exceeds the protocol limit");
            return;
        }

        DecodedSandboxPacket packet{};
        if (invalidDescriptorTransfer ||
            !decodeSandboxPacket(bytes.data(), static_cast<std::size_t>(count), packet)) {
            const std::uint32_t requestId = packet.header.requestId == 0 ? 1 : packet.header.requestId;
            closeReceivedDescriptors();
            sendErrorAndClose(descriptor, requestId, "sandbox packet or descriptor transfer is invalid");
            return;
        }
        if (packet.header.opcode == SandboxOpcode::RegisterApplication) {
            SandboxApplicationRegistration registration{};
            std::string registrationError;
            const bool registered = receivedDescriptorCount == receivedDescriptors.size() &&
                                    decodeSandboxApplicationRegistration(packet.payload, registration) &&
                                    registerExternalApplication(registration, receivedDescriptors[0],
                                                                receivedDescriptors[1], registrationError);
            closeReceivedDescriptors();
            SandboxRegistrationResult result{
                .registered = registered,
                .message = registered ? "registered" :
                    (registrationError.empty() ? "sandbox application registration was rejected"
                                               : registrationError),
            };
            std::vector<std::uint8_t> payload;
            if (!encodeSandboxRegistrationResult(result, payload) ||
                !sendPacket(descriptor, SandboxOpcode::RegistrationResult,
                            packet.header.requestId, payload)) {
                removeClient(descriptor);
                return;
            }
            continue;
        }
        if (packet.header.opcode != SandboxOpcode::LaunchRequest || receivedDescriptorCount != 0) {
            closeReceivedDescriptors();
            sendErrorAndClose(descriptor, packet.header.requestId,
                              "sandbox opcode or descriptor transfer is not accepted");
            return;
        }
        SandboxLaunchRequest request{};
        if (!decodeSandboxLaunchRequest(packet.payload, request)) {
            closeReceivedDescriptors();
            sendErrorAndClose(descriptor, packet.header.requestId, "sandbox launch request is invalid");
            return;
        }
        closeReceivedDescriptors();
        const SandboxLaunchResult result = handleLaunchRequest(request);
        std::vector<std::uint8_t> payload;
        if (!encodeSandboxLaunchResult(result, payload) ||
            !sendPacket(descriptor, SandboxOpcode::LaunchResult, packet.header.requestId, payload)) {
            removeClient(descriptor);
            return;
        }
    }
}

void SandboxDaemon::removeClient(int descriptor) {
    std::erase(clientDescriptors_, descriptor);
    close(descriptor);
}

void SandboxDaemon::reapChildren() {
    for (const SandboxChildExit& exited : childReaper_.reap()) {
        std::string launchCleanupError;
        launchRegistry_.remove(exited.instanceId, launchCleanupError);
        if (exited.cgroup.has_value()) {
            std::string cleanupError;
            // The process has been reaped before its cgroup is removed.  A
            // cleanup failure cannot revive the app or change the exit event;
            // startup will still fail closed if the hierarchy is unsafe.
            cgroupManager_.removeInstance(*exited.cgroup, cleanupError);
        }
        SandboxProcessExited event{};
        event.instanceId = exited.instanceId;
        event.exitCode = exited.exitCode;
        std::vector<std::uint8_t> payload;
        if (!encodeSandboxProcessExited(event, payload)) {
            continue;
        }
        const auto clients = clientDescriptors_;
        for (const int descriptor : clients) {
            if (!sendPacket(descriptor, SandboxOpcode::ProcessExited, 1, payload)) {
                removeClient(descriptor);
            }
        }
    }
}

void SandboxDaemon::refreshRuntimeEndpointState() {
    std::string error;
    if (materialRegistry_.runtimeEndpointsCurrent(error)) {
        return;
    }
    if (!runtimeEndpointRefreshRequired_) {
        // A child holds its private bind mount even after its source socket is
        // unlinked.  Kill the complete process group immediately so no app
        // can keep talking to the retired endpoint generation.
        childReaper_.terminateAll(SIGKILL);
    }
    runtimeEndpointRefreshRequired_ = true;
    // Never retain a descriptor for an unlinked socket generation.  The
    // authorizer may retain its policy record, but it cannot produce a child
    // until the root-owned registry supplies current launch material again.
    materialRegistry_.removeStaleRuntimeEndpointMaterials();
}

bool SandboxDaemon::completeRuntimeEndpointRefresh(std::string& error) {
    error.clear();
    if (!runtimeEndpointRefreshRequired_) {
        return true;
    }
    if (!materialRegistry_.runtimeEndpointsCurrent(error)) {
        return false;
    }
    runtimeEndpointRefreshRequired_ = false;
    return true;
}

void SandboxDaemon::poll() {
    if (serverDescriptor_ < 0) {
        return;
    }
    refreshRuntimeEndpointState();
    acceptConnections();
    const auto clients = clientDescriptors_;
    for (const int descriptor : clients) {
        if (std::find(clientDescriptors_.begin(), clientDescriptors_.end(), descriptor) !=
            clientDescriptors_.end()) {
            serviceClient(descriptor);
        }
    }
    reapChildren();
}

} // namespace lcl::security
