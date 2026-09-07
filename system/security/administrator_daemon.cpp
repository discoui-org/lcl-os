#include "system/security/administrator_daemon.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <string_view>

namespace lcl::security {
namespace {

constexpr std::size_t kPacketCapacity =
    kAdministratorWireHeaderSize + kAdministratorMaxPayload;
constexpr std::chrono::minutes kPermissionLifetime{5};

std::string executableName(pid_t pid) {
    std::array<char, 64> path{};
    std::snprintf(path.data(), path.size(), "/proc/%d/exe", static_cast<int>(pid));
    std::array<char, 4096> target{};
    const ssize_t length = readlink(path.data(), target.data(), target.size() - 1);
    if (length <= 0) return {};
    target[static_cast<std::size_t>(length)] = '\0';
    return std::filesystem::path(target.data()).filename().string();
}

pid_t parentPid(pid_t pid) {
    std::array<char, 64> path{};
    std::snprintf(path.data(), path.size(), "/proc/%d/status", static_cast<int>(pid));
    FILE* file = std::fopen(path.data(), "re");
    if (!file) return 0;
    std::array<char, 256> line{};
    pid_t parent = 0;
    while (std::fgets(line.data(), static_cast<int>(line.size()), file)) {
        if (std::sscanf(line.data(), "PPid:%d", &parent) == 1) break;
    }
    std::fclose(file);
    return parent;
}

bool isTrustedShell(pid_t pid) {
    const auto name = executableName(pid);
    return name == "lcl-desktop-shell" || name == "lcl-mobile-shell";
}

std::uint64_t processStartTime(pid_t pid) {
    std::array<char, 64> path{};
    std::snprintf(path.data(), path.size(), "/proc/%d/stat", static_cast<int>(pid));
    std::ifstream file(path.data());
    std::string record;
    if (!file || !std::getline(file, record)) return 0;
    const auto commandEnd = record.rfind(')');
    if (commandEnd == std::string::npos || commandEnd + 2 >= record.size()) return 0;
    std::istringstream fields(record.substr(commandEnd + 2));
    std::string field;
    for (unsigned index = 0; index <= 19; ++index) {
        if (!(fields >> field)) return 0;
        if (index == 19) {
            try {
                return std::stoull(field);
            } catch (...) {
                return 0;
            }
        }
    }
    return 0;
}

bool terminalIdentityForSudo(pid_t pid, pid_t& terminalPid,
                             std::uint64_t& terminalStartTime) {
    terminalPid = 0;
    terminalStartTime = 0;
    if (executableName(pid) != "lcl-sudo") return false;
    pid_t ancestor = parentPid(pid);
    for (unsigned depth = 0; depth < 8 && ancestor > 1; ++depth) {
        const auto name = executableName(ancestor);
        if (name == "Terminal" || name == "lcl-terminal") {
            const auto startTime = processStartTime(ancestor);
            if (startTime == 0) return false;
            terminalPid = ancestor;
            terminalStartTime = startTime;
            return true;
        }
        ancestor = parentPid(ancestor);
    }
    return false;
}

bool safeSocketPath(const std::string& value) {
    const std::filesystem::path path(value);
    if (!path.is_absolute() || path.parent_path().empty()) return false;
    return std::none_of(path.begin(), path.end(), [](const auto& component) {
        return component == "." || component == "..";
    });
}

bool receiveAuthenticatedPacket(int descriptor, const ucred& expected,
                                DecodedAdministratorPacket& packet) {
    std::array<std::uint8_t, kPacketCapacity> bytes{};
    std::array<unsigned char, CMSG_SPACE(sizeof(ucred)) + CMSG_SPACE(sizeof(int))> control{};
    iovec vector{.iov_base = bytes.data(), .iov_len = bytes.size()};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    const ssize_t count = recvmsg(descriptor, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (count <= 0 || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) return false;

    const ucred* credentials = nullptr;
    for (cmsghdr* item = CMSG_FIRSTHDR(&message); item;
         item = CMSG_NXTHDR(&message, item)) {
        if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_RIGHTS) {
            const std::size_t countBytes = item->cmsg_len >= CMSG_LEN(0)
                ? item->cmsg_len - CMSG_LEN(0) : 0;
            const int* descriptors = reinterpret_cast<const int*>(CMSG_DATA(item));
            for (std::size_t index = 0; index < countBytes / sizeof(int); ++index) {
                if (descriptors[index] >= 0) close(descriptors[index]);
            }
            return false;
        }
        if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_CREDENTIALS &&
            item->cmsg_len == CMSG_LEN(sizeof(ucred))) {
            if (credentials) return false;
            credentials = reinterpret_cast<const ucred*>(CMSG_DATA(item));
        }
    }
    return credentials && credentials->pid == expected.pid &&
           credentials->uid == expected.uid && credentials->gid == expected.gid &&
           decodeAdministratorPacket(bytes.data(), static_cast<std::size_t>(count), packet);
}

std::string resolveExecutable(const std::string& command) {
    std::vector<std::string> candidates;
    if (command.find('/') == std::string::npos) {
        for (const char* directory : {"/System/Tools", "/System/Core", "/usr/bin", "/bin",
                                      "/usr/sbin", "/sbin"}) {
            candidates.push_back(std::string(directory) + "/" + command);
        }
    } else if (!command.empty() && command.front() == '/') {
        candidates.push_back(command);
    }

    for (const auto& candidate : candidates) {
        std::array<char, 4096> resolved{};
        if (!realpath(candidate.c_str(), resolved.data())) continue;
        const std::string path(resolved.data());
        const bool allowed = path.starts_with("/System/Tools/") ||
                             path.starts_with("/System/Core/") ||
                             path.starts_with("/usr/bin/") ||
                             path.starts_with("/usr/sbin/");
        struct stat status {};
        if (!allowed || lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
            status.st_uid != 0 || (status.st_mode & 0022) != 0 ||
            (status.st_mode & 0111) == 0) {
            continue;
        }
        return path;
    }
    return {};
}

int exitCodeForStatus(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return std::min(255, 128 + WTERMSIG(status));
    return 125;
}

} // namespace

AdministratorDaemon::AdministratorDaemon(AdministratorDaemonConfig config)
    : config_(std::move(config)) {}

AdministratorDaemon::~AdministratorDaemon() { shutdown(); }

bool AdministratorDaemon::initialize(std::string& error) {
    shutdown();
    error.clear();
    if (geteuid() != 0 || config_.sessionUid == 0 || config_.sessionGid == 0 ||
        !safeSocketPath(config_.socketPath) ||
        config_.socketPath.size() >= sizeof(sockaddr_un{}.sun_path)) {
        error = "lcl-admind requires root and a valid session identity";
        return false;
    }
    const auto parent = std::filesystem::path(config_.socketPath).parent_path();
    struct stat parentStatus {};
    if (lstat(parent.c_str(), &parentStatus) != 0 || !S_ISDIR(parentStatus.st_mode) ||
        parentStatus.st_uid != 0 || parentStatus.st_gid != 0 ||
        (parentStatus.st_mode & 0022) != 0) {
        error = "lcl-admind socket parent is not root-controlled";
        return false;
    }
    struct stat existing {};
    if (lstat(config_.socketPath.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode) || existing.st_uid != 0 || unlink(config_.socketPath.c_str()) != 0) {
            error = "lcl-admind refused an unsafe existing endpoint";
            return false;
        }
    } else if (errno != ENOENT) {
        error = std::strerror(errno);
        return false;
    }

    serverDescriptor_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (serverDescriptor_ < 0) {
        error = std::strerror(errno);
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, config_.socketPath.c_str(), sizeof(address.sun_path) - 1);
    if (bind(serverDescriptor_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        chown(config_.socketPath.c_str(), 0, config_.sessionGid) != 0 ||
        chmod(config_.socketPath.c_str(), 0660) != 0 || listen(serverDescriptor_, 8) != 0) {
        error = std::strerror(errno);
        shutdown();
        return false;
    }
    ownsSocketPath_ = true;
    return true;
}

void AdministratorDaemon::acceptConnections() {
    while (true) {
        const int descriptor = accept4(serverDescriptor_, nullptr, nullptr, SOCK_CLOEXEC);
        if (descriptor < 0) {
            if (errno == EINTR) continue;
            return;
        }
        ucred peer{};
        socklen_t size = sizeof(peer);
        int passCredentials = 1;
        if (getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 ||
            size != sizeof(peer) || peer.uid != config_.sessionUid ||
            peer.gid != config_.sessionGid ||
            setsockopt(descriptor, SOL_SOCKET, SO_PASSCRED, &passCredentials,
                       sizeof(passCredentials)) != 0) {
            close(descriptor);
            continue;
        }
        clients_.push_back({descriptor, peer.pid, peer.uid, peer.gid,
                            ClientRole::Unknown, 0, 0});
    }
}

AdministratorDaemon::Client* AdministratorDaemon::findClient(int descriptor) {
    const auto found = std::find_if(clients_.begin(), clients_.end(),
        [descriptor](const Client& client) { return client.descriptor == descriptor; });
    return found == clients_.end() ? nullptr : &*found;
}

bool AdministratorDaemon::sendPacket(
        int descriptor, AdministratorOpcode opcode, std::uint32_t requestId,
        const std::vector<std::uint8_t>& payload) {
    AdministratorHeader header{};
    header.opcode = opcode;
    header.requestId = requestId;
    header.payloadSize = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> packet;
    return encodeAdministratorPacket(header, payload, packet) &&
           send(descriptor, packet.data(), packet.size(), MSG_NOSIGNAL) ==
               static_cast<ssize_t>(packet.size());
}

void AdministratorDaemon::sendError(int descriptor, std::uint32_t requestId,
                                    const std::string& message) {
    std::vector<std::uint8_t> payload;
    if (encodeAdministratorError(message, payload)) {
        (void)sendPacket(descriptor, AdministratorOpcode::ErrorResponse, requestId, payload);
    }
}

void AdministratorDaemon::serviceClient(int descriptor) {
    Client* client = findClient(descriptor);
    if (!client) return;
    const ucred expected{.pid = client->pid, .uid = client->uid, .gid = client->gid};
    DecodedAdministratorPacket packet{};
    if (!receiveAuthenticatedPacket(descriptor, expected, packet)) {
        removeClient(descriptor);
        return;
    }

    if (client->role == ClientRole::Unknown &&
        packet.header.opcode == AdministratorOpcode::ShellHello &&
        packet.payload.empty() && isTrustedShell(client->pid) && shellDescriptor_ < 0) {
        client->role = ClientRole::Shell;
        shellDescriptor_ = descriptor;
        return;
    }

    if (client->role == ClientRole::Unknown &&
        packet.header.opcode == AdministratorOpcode::ExecuteRequest &&
        terminalIdentityForSudo(client->pid, client->terminalPid,
                                client->terminalStartTime)) {
        AdministratorExecuteRequest request{};
        if (!decodeAdministratorExecuteRequest(packet.payload, request)) {
            sendError(descriptor, packet.header.requestId, "Invalid administrator command.");
            removeClient(descriptor);
            return;
        }
        client->role = ClientRole::Command;
        if (pending_ || running_) {
            sendError(descriptor, packet.header.requestId, "Another administrator request is active.");
            removeClient(descriptor);
            return;
        }
        pending_ = PendingCommand{descriptor, packet.header.requestId,
                                  std::move(request), client->terminalPid,
                                  client->terminalStartTime};
        pruneCachedPermission();
        if (cachedPermission_ &&
            cachedPermission_->terminalPid == client->terminalPid &&
            cachedPermission_->terminalStartTime == client->terminalStartTime) {
            PendingCommand pending = std::move(*pending_);
            pending_.reset();
            std::string error;
            if (!startCommand(std::move(pending), error)) {
                sendError(descriptor, packet.header.requestId, error);
                removeClient(descriptor);
            }
            return;
        }
        if (shellDescriptor_ < 0) {
            sendError(descriptor, packet.header.requestId, "The permission UI is unavailable.");
            pending_.reset();
            removeClient(descriptor);
            return;
        }
        const AdministratorPrompt prompt{
            .appName = "Terminal",
            .title = "Wants administrator privileges",
            .description = "Allow Terminal to perform this protected command?",
        };
        std::vector<std::uint8_t> payload;
        if (!encodeAdministratorPrompt(prompt, payload) ||
            !sendPacket(shellDescriptor_, AdministratorOpcode::PermissionPrompt,
                        packet.header.requestId, payload)) {
            sendError(descriptor, packet.header.requestId, "The permission UI is unavailable.");
            pending_.reset();
            removeClient(shellDescriptor_);
            removeClient(descriptor);
        }
        return;
    }

    if (client->role == ClientRole::Shell &&
        packet.header.opcode == AdministratorOpcode::PermissionDecision && pending_ &&
        pending_->requestId == packet.header.requestId) {
        bool allowed = false;
        if (!decodeAdministratorDecision(packet.payload, allowed)) {
            removeClient(descriptor);
            return;
        }
        PendingCommand pending = std::move(*pending_);
        pending_.reset();
        if (!allowed) {
            sendError(pending.clientDescriptor, pending.requestId,
                      "Administrator permission was denied.");
            removeClient(pending.clientDescriptor);
            return;
        }
        std::string error;
        const int commandClient = pending.clientDescriptor;
        const std::uint32_t commandRequestId = pending.requestId;
        const pid_t terminalPid = pending.terminalPid;
        const std::uint64_t terminalStartTime = pending.terminalStartTime;
        if (!startCommand(std::move(pending), error)) {
            sendError(commandClient, commandRequestId, error);
            removeClient(commandClient);
        } else {
            cachedPermission_ = CachedPermission{
                terminalPid,
                terminalStartTime,
                std::chrono::steady_clock::now() + kPermissionLifetime,
            };
        }
        return;
    }

    sendError(descriptor, packet.header.requestId, "Administrator request is not allowed.");
    removeClient(descriptor);
}

bool AdministratorDaemon::startCommand(PendingCommand pending, std::string& error) {
    const std::string executable = resolveExecutable(pending.request.arguments.front());
    if (executable.empty()) {
        error = "Command is outside the administrator allow path.";
        return false;
    }
    int stdoutPipe[2]{-1, -1};
    int stderrPipe[2]{-1, -1};
    if (pipe2(stdoutPipe, O_CLOEXEC | O_NONBLOCK) != 0 ||
        pipe2(stderrPipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        error = std::strerror(errno);
        if (stdoutPipe[0] >= 0) close(stdoutPipe[0]);
        if (stdoutPipe[1] >= 0) close(stdoutPipe[1]);
        if (stderrPipe[0] >= 0) close(stderrPipe[0]);
        if (stderrPipe[1] >= 0) close(stderrPipe[1]);
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        error = std::strerror(errno);
        close(stdoutPipe[0]); close(stdoutPipe[1]);
        close(stderrPipe[0]); close(stderrPipe[1]);
        return false;
    }
    if (child == 0) {
        setpgid(0, 0);
        const int nullInput = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (nullInput < 0 || dup2(nullInput, STDIN_FILENO) < 0 ||
            dup2(stdoutPipe[1], STDOUT_FILENO) < 0 ||
            dup2(stderrPipe[1], STDERR_FILENO) < 0 ||
            chdir(pending.request.workingDirectory.c_str()) != 0) {
            _exit(125);
        }
        if (nullInput > STDERR_FILENO) close(nullInput);
        close(stdoutPipe[0]); close(stdoutPipe[1]);
        close(stderrPipe[0]); close(stderrPipe[1]);
        clearenv();
        setenv("HOME", "/root", 1);
        setenv("USER", "root", 1);
        setenv("LOGNAME", "root", 1);
        setenv("PATH", "/System/Tools:/System/Core:/usr/bin:/bin:/usr/sbin:/sbin", 1);
        setenv("TERM", "xterm-256color", 1);
        std::vector<char*> arguments;
        arguments.reserve(pending.request.arguments.size() + 1);
        for (auto& argument : pending.request.arguments) arguments.push_back(argument.data());
        arguments.push_back(nullptr);
        execv(executable.c_str(), arguments.data());
        _exit(errno == ENOENT ? 127 : 126);
    }
    (void)setpgid(child, child);
    close(stdoutPipe[1]);
    close(stderrPipe[1]);
    running_ = RunningCommand{pending.clientDescriptor, pending.requestId, child,
                              stdoutPipe[0], stderrPipe[0], false, 125};
    return true;
}

void AdministratorDaemon::serviceOutput(int descriptor,
                                        AdministratorOutputStream stream) {
    if (!running_ || descriptor < 0) return;
    std::array<std::uint8_t, 4096> bytes{};
    while (true) {
        const ssize_t count = read(descriptor, bytes.data(), bytes.size());
        if (count > 0) {
            AdministratorCommandOutput output{
                .stream = stream,
                .bytes = std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + count),
            };
            std::vector<std::uint8_t> payload;
            if (!encodeAdministratorCommandOutput(output, payload) ||
                !sendPacket(running_->clientDescriptor, AdministratorOpcode::CommandOutput,
                            running_->requestId, payload)) {
                removeClient(running_->clientDescriptor);
                return;
            }
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count == 0) {
            close(descriptor);
            if (running_ && running_->stdoutDescriptor == descriptor)
                running_->stdoutDescriptor = -1;
            if (running_ && running_->stderrDescriptor == descriptor)
                running_->stderrDescriptor = -1;
        }
        return;
    }
}

void AdministratorDaemon::finishCommandIfReady() {
    if (!running_) return;
    if (!running_->childExited) {
        int status = 0;
        const pid_t result = waitpid(running_->pid, &status, WNOHANG);
        if (result == running_->pid) {
            running_->childExited = true;
            running_->exitCode = exitCodeForStatus(status);
        }
    }
    if (!running_ || !running_->childExited || running_->stdoutDescriptor >= 0 ||
        running_->stderrDescriptor >= 0) {
        return;
    }
    std::vector<std::uint8_t> payload;
    const int client = running_->clientDescriptor;
    const auto requestId = running_->requestId;
    const int exitCode = running_->exitCode;
    running_.reset();
    if (encodeAdministratorCommandResult(exitCode, payload)) {
        (void)sendPacket(client, AdministratorOpcode::CommandResult, requestId, payload);
    }
    removeClient(client);
}

void AdministratorDaemon::pruneCachedPermission() {
    if (!cachedPermission_) return;
    const auto name = executableName(cachedPermission_->terminalPid);
    if (std::chrono::steady_clock::now() >= cachedPermission_->expiresAt ||
        (name != "Terminal" && name != "lcl-terminal") ||
        processStartTime(cachedPermission_->terminalPid) !=
            cachedPermission_->terminalStartTime) {
        cachedPermission_.reset();
    }
}

void AdministratorDaemon::removeClient(int descriptor) {
    if (descriptor < 0) return;
    if (shellDescriptor_ == descriptor) {
        shellDescriptor_ = -1;
        if (pending_) {
            sendError(pending_->clientDescriptor, pending_->requestId,
                      "The permission UI disconnected.");
            const int pendingClient = pending_->clientDescriptor;
            pending_.reset();
            if (pendingClient != descriptor) removeClient(pendingClient);
        }
    }
    if (pending_ && pending_->clientDescriptor == descriptor) pending_.reset();
    if (running_ && running_->clientDescriptor == descriptor) {
        if (running_->pid > 0) kill(-running_->pid, SIGKILL);
        if (running_->stdoutDescriptor >= 0) close(running_->stdoutDescriptor);
        if (running_->stderrDescriptor >= 0) close(running_->stderrDescriptor);
        running_.reset();
    }
    const auto found = std::find_if(clients_.begin(), clients_.end(),
        [descriptor](const Client& client) { return client.descriptor == descriptor; });
    if (found != clients_.end()) {
        close(found->descriptor);
        clients_.erase(found);
    }
}

void AdministratorDaemon::poll() {
    if (serverDescriptor_ < 0) return;
    pruneCachedPermission();
    std::vector<pollfd> descriptors;
    descriptors.push_back({serverDescriptor_, POLLIN, 0});
    for (const auto& client : clients_) {
        descriptors.push_back({client.descriptor, POLLIN | POLLHUP | POLLERR, 0});
    }
    if (running_ && running_->stdoutDescriptor >= 0)
        descriptors.push_back({running_->stdoutDescriptor, POLLIN | POLLHUP | POLLERR, 0});
    if (running_ && running_->stderrDescriptor >= 0)
        descriptors.push_back({running_->stderrDescriptor, POLLIN | POLLHUP | POLLERR, 0});
    if (::poll(descriptors.data(), descriptors.size(), 0) < 0) return;
    if ((descriptors.front().revents & POLLIN) != 0) acceptConnections();

    std::vector<int> readableClients;
    for (std::size_t index = 1; index < descriptors.size(); ++index) {
        const int descriptor = descriptors[index].fd;
        if (running_ && (descriptor == running_->stdoutDescriptor ||
                         descriptor == running_->stderrDescriptor)) {
            continue;
        }
        if ((descriptors[index].revents & POLLIN) != 0) readableClients.push_back(descriptor);
        else if ((descriptors[index].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
            removeClient(descriptor);
    }
    for (const int descriptor : readableClients) {
        if (findClient(descriptor)) serviceClient(descriptor);
    }
    if (running_ && running_->stdoutDescriptor >= 0)
        serviceOutput(running_->stdoutDescriptor, AdministratorOutputStream::StandardOutput);
    if (running_ && running_->stderrDescriptor >= 0)
        serviceOutput(running_->stderrDescriptor, AdministratorOutputStream::StandardError);
    finishCommandIfReady();
}

void AdministratorDaemon::shutdown() {
    if (running_) {
        if (running_->pid > 0) kill(-running_->pid, SIGKILL);
        if (running_->stdoutDescriptor >= 0) close(running_->stdoutDescriptor);
        if (running_->stderrDescriptor >= 0) close(running_->stderrDescriptor);
        running_.reset();
    }
    pending_.reset();
    cachedPermission_.reset();
    for (const auto& client : clients_) close(client.descriptor);
    clients_.clear();
    shellDescriptor_ = -1;
    if (serverDescriptor_ >= 0) close(serverDescriptor_);
    serverDescriptor_ = -1;
    if (ownsSocketPath_) unlink(config_.socketPath.c_str());
    ownsSocketPath_ = false;
}

} // namespace lcl::security
