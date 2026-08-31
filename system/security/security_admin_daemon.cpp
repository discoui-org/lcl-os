#include "system/security/security_admin_daemon.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

namespace lcl::security {
namespace {

constexpr std::size_t kReceiveBufferSize =
    kSecurityAdminWireHeaderSize + kSecurityAdminMaxPayload;

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

void closeTransferredDescriptors(const msghdr& message) {
    for (cmsghdr* control = CMSG_FIRSTHDR(const_cast<msghdr*>(&message)); control;
         control = CMSG_NXTHDR(const_cast<msghdr*>(&message), control)) {
        if (control->cmsg_level != SOL_SOCKET || control->cmsg_type != SCM_RIGHTS ||
            control->cmsg_len < CMSG_LEN(0)) {
            continue;
        }
        const std::size_t byteCount = control->cmsg_len - CMSG_LEN(0);
        if (byteCount % sizeof(int) != 0) {
            continue;
        }
        const int* descriptors = reinterpret_cast<const int*>(CMSG_DATA(control));
        for (std::size_t index = 0; index < byteCount / sizeof(int); ++index) {
            if (descriptors[index] >= 0) {
                close(descriptors[index]);
            }
        }
    }
}

bool containsAncillaryData(const msghdr& message) {
    return (message.msg_flags & MSG_CTRUNC) != 0 || CMSG_FIRSTHDR(const_cast<msghdr*>(&message)) != nullptr;
}

} // namespace

SecurityAdminDaemon::SecurityAdminDaemon(SecurityAdminDaemonConfig config)
    : authority_(config.bundleApprovals), config_(std::move(config)) {}

SecurityAdminDaemon::~SecurityAdminDaemon() { shutdown(); }

bool SecurityAdminDaemon::validateConfig(std::string& error) const {
    error.clear();
    if (!isSafeSocketPath(config_.socketPath) ||
        config_.socketPath.size() >= sizeof(sockaddr_un{}.sun_path)) {
        error = "securityd socket path is not a safe Unix socket path";
        return false;
    }
    if (config_.ownerUid != 0 || config_.ownerGid != 0 || config_.sessionUid == 0 ||
        config_.sessionGid == 0) {
        error = "securityd requires root ownership and one non-root session identity";
        return false;
    }
    return true;
}

bool SecurityAdminDaemon::validateSocketParent(std::string& error) const {
    const std::filesystem::path parent = std::filesystem::path(config_.socketPath).parent_path();
    struct stat status {};
    if (parent.empty() || lstat(parent.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode) || status.st_uid != config_.ownerUid ||
        status.st_gid != config_.ownerGid || (status.st_mode & 0022) != 0) {
        error = "securityd socket parent is not a root-owned non-writable directory";
        return false;
    }
    return true;
}

bool SecurityAdminDaemon::initialize(std::string& error) {
    error.clear();
    if (serverDescriptor_ >= 0) {
        return true;
    }
    if (geteuid() != 0) {
        error = "lcl-securityd must run as root";
        return false;
    }
    if (!validateConfig(error) || !validateSocketParent(error)) {
        return false;
    }

    struct stat existing {};
    if (lstat(config_.socketPath.c_str(), &existing) == 0) {
        const bool rootOwned = existing.st_uid == config_.ownerUid &&
                               existing.st_gid == config_.ownerGid;
        const bool expectedSessionSocket = existing.st_uid == config_.sessionUid &&
                                           existing.st_gid == config_.sessionGid &&
                                           (existing.st_mode & 0777) == 0600;
        if (!S_ISSOCK(existing.st_mode) || (!rootOwned && !expectedSessionSocket)) {
            error = "securityd refuses to replace an unexpected socket path";
            return false;
        }
        if (unlink(config_.socketPath.c_str()) != 0) {
            error = std::string("could not remove stale securityd socket: ") + std::strerror(errno);
            return false;
        }
    } else if (errno != ENOENT) {
        error = std::string("could not inspect securityd socket path: ") + std::strerror(errno);
        return false;
    }

    const mode_t previousUmask = umask(0077);
    const int descriptor = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    umask(previousUmask);
    if (descriptor < 0) {
        error = std::string("could not create securityd socket: ") + std::strerror(errno);
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
        error = std::string("could not initialize securityd socket: ") + std::strerror(savedErrno);
        return false;
    }
    serverDescriptor_ = descriptor;
    ownsSocketPath_ = true;
    return true;
}

void SecurityAdminDaemon::shutdown() {
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
}

bool SecurityAdminDaemon::peerIsTrustedSessionUser(int descriptor) const {
    struct ucred credential {};
    socklen_t length = sizeof(credential);
    return getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credential, &length) == 0 &&
           length == sizeof(credential) && credential.uid == config_.sessionUid &&
           credential.gid == config_.sessionGid;
}

bool SecurityAdminDaemon::sendPacket(int descriptor, SecurityAdminOpcode opcode,
                                     std::uint32_t requestId,
                                     const std::vector<std::uint8_t>& payload) {
    SecurityAdminHeader header{};
    header.opcode = opcode;
    header.requestId = requestId;
    header.payloadSize = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> packet;
    return encodeSecurityAdminPacket(header, payload, packet) &&
           send(descriptor, packet.data(), packet.size(), MSG_NOSIGNAL) ==
               static_cast<ssize_t>(packet.size());
}

void SecurityAdminDaemon::acceptConnections() {
    while (true) {
        const int descriptor = accept4(serverDescriptor_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (descriptor >= 0) {
            if (peerIsTrustedSessionUser(descriptor)) {
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

void SecurityAdminDaemon::sendErrorAndClose(int descriptor, std::uint32_t requestId,
                                            const std::string& message) {
    std::vector<std::uint8_t> payload;
    if (encodeSecurityAdminError(message, payload)) {
        sendPacket(descriptor, SecurityAdminOpcode::ErrorResponse, requestId, payload);
    }
    removeClient(descriptor);
}

void SecurityAdminDaemon::serviceClient(int descriptor) {
    std::array<std::uint8_t, kReceiveBufferSize> bytes{};
    while (true) {
        std::array<char, CMSG_SPACE(sizeof(int) * 4)> control{};
        iovec vector{.iov_base = bytes.data(), .iov_len = bytes.size()};
        msghdr message{};
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        const ssize_t count = recvmsg(descriptor, &message, MSG_DONTWAIT | MSG_TRUNC | MSG_CMSG_CLOEXEC);
        const bool hasAncillaryData = containsAncillaryData(message);
        closeTransferredDescriptors(message);
        if (count == 0) {
            removeClient(descriptor);
            return;
        }
        if (count < 0) {
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
            sendErrorAndClose(descriptor, 1, "securityd packet exceeds the protocol limit");
            return;
        }
        DecodedSecurityAdminPacket packet{};
        if (hasAncillaryData ||
            !decodeSecurityAdminPacket(bytes.data(), static_cast<std::size_t>(count), packet)) {
            const std::uint32_t requestId = packet.header.requestId == 0 ? 1 : packet.header.requestId;
            sendErrorAndClose(descriptor, requestId,
                              "securityd packet or descriptor transfer is invalid");
            return;
        }

        if (packet.header.opcode == SecurityAdminOpcode::PendingBundleApprovalsRequest) {
            if (!packet.payload.empty()) {
                sendErrorAndClose(descriptor, packet.header.requestId,
                                  "pending bundle approvals request must be empty");
                return;
            }
            std::string error;
            const std::vector<PendingBundleApproval> pending =
                authority_.pendingFor(config_.sessionUid, error);
            std::vector<std::uint8_t> payload;
            if (!error.empty() || !encodePendingBundleApprovals(pending, payload) ||
                !sendPacket(descriptor, SecurityAdminOpcode::PendingBundleApprovalsResponse,
                            packet.header.requestId, payload)) {
                sendErrorAndClose(descriptor, packet.header.requestId,
                                  error.empty() ? "could not encode pending bundle approvals" : error);
                return;
            }
            continue;
        }

        const bool isApprove = packet.header.opcode == SecurityAdminOpcode::ApproveBundleRequest;
        const bool isRevoke = packet.header.opcode == SecurityAdminOpcode::RevokeBundleApprovalRequest;
        if (!isApprove && !isRevoke) {
            sendErrorAndClose(descriptor, packet.header.requestId,
                              "securityd opcode is not accepted");
            return;
        }
        SecurityAdminBundleIdentity identity{};
        if (!decodeSecurityAdminBundleIdentity(packet.payload, identity)) {
            sendErrorAndClose(descriptor, packet.header.requestId,
                              "securityd bundle approval identity is invalid");
            return;
        }
        std::string error;
        const bool accepted = isApprove
            ? authority_.approvePending(config_.sessionUid, identity.appId,
                                        identity.bundleRecordDigest, error)
            : authority_.revoke(config_.sessionUid, identity.appId,
                                identity.bundleRecordDigest, error);
        SecurityAdminApprovalResult result{
            .accepted = accepted,
            .message = accepted ? (isApprove ? "approved" : "revoked")
                                : (error.empty() ? "bundle approval request was rejected" : error),
        };
        std::vector<std::uint8_t> payload;
        const SecurityAdminOpcode responseOpcode = isApprove
            ? SecurityAdminOpcode::ApproveBundleResponse
            : SecurityAdminOpcode::RevokeBundleApprovalResponse;
        if (!encodeSecurityAdminApprovalResult(result, payload) ||
            !sendPacket(descriptor, responseOpcode, packet.header.requestId, payload)) {
            removeClient(descriptor);
            return;
        }
    }
}

void SecurityAdminDaemon::removeClient(int descriptor) {
    std::erase(clientDescriptors_, descriptor);
    close(descriptor);
}

void SecurityAdminDaemon::poll() {
    if (serverDescriptor_ < 0) {
        return;
    }
    acceptConnections();
    const auto clients = clientDescriptors_;
    for (const int descriptor : clients) {
        if (std::find(clientDescriptors_.begin(), clientDescriptors_.end(), descriptor) !=
            clientDescriptors_.end()) {
            serviceClient(descriptor);
        }
    }
}

} // namespace lcl::security
