#include "core/session/session_service.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace lcl::session {
namespace {

constexpr size_t kReceiveBufferSize = kSessionWireHeaderSize + kSessionMaxPayload;

std::vector<std::string> executableArgs(const core::AppBundleMetadata& app) {
    if (app.runtime == "org.lcl.javascript" ||
        (app.executablePath.size() >= 3 &&
         app.executablePath.substr(app.executablePath.size() - 3) == ".js")) {
        std::string jsRuntime = "/System/Core/lcl-js";
        std::error_code ec;
        if (!std::filesystem::exists(jsRuntime, ec)) {
            jsRuntime = "/usr/bin/lcl-js";
        }
        return {jsRuntime, app.executablePath};
    }
    return {app.executablePath};
}

std::string resolvedIconPath(const core::AppBundleMetadata& app) {
    if (app.icon.empty()) return {};
    std::filesystem::path icon(app.icon);
    if (icon.is_relative()) icon = std::filesystem::path(app.bundlePath) / icon;
    std::error_code error;
    if (!std::filesystem::is_regular_file(icon, error) || error) return {};
    return icon.string();
}

void exportLaunchOrigin(const LaunchRequest::Origin& origin) {
    if (!origin.valid) return;
    setenv("LCL_LAUNCH_ORIGIN_X", std::to_string(origin.x).c_str(), 1);
    setenv("LCL_LAUNCH_ORIGIN_Y", std::to_string(origin.y).c_str(), 1);
    setenv("LCL_LAUNCH_ORIGIN_WIDTH", std::to_string(origin.width).c_str(), 1);
    setenv("LCL_LAUNCH_ORIGIN_HEIGHT", std::to_string(origin.height).c_str(), 1);
    setenv("LCL_LAUNCH_ORIGIN_RADIUS", std::to_string(origin.cornerRadius).c_str(), 1);
}

} // namespace

SessionService::SessionService(std::vector<std::string> appSearchPaths)
    : m_registry(std::move(appSearchPaths)) {}

SessionService::~SessionService() { shutdown(); }

bool SessionService::initialize(const std::string& socketPath) {
    if (m_serverFd >= 0) return true;
    m_socketPath = socketPath;
    const std::filesystem::path parent = std::filesystem::path(socketPath).parent_path();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) {
            std::cerr << "[LCL Session ERROR] Could not create runtime directory " << parent
                      << ": " << error.message() << "\n";
            return false;
        }
        chmod(parent.c_str(), 0700);
    }
    unlink(socketPath.c_str());
    m_serverFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (m_serverFd < 0) return false;

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof(address.sun_path)) {
        close(m_serverFd);
        m_serverFd = -1;
        errno = ENAMETOOLONG;
        return false;
    }
    std::strncpy(address.sun_path, socketPath.c_str(), sizeof(address.sun_path) - 1);
    if (bind(m_serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(m_serverFd, 32) != 0) {
        const int error = errno;
        close(m_serverFd);
        m_serverFd = -1;
        unlink(socketPath.c_str());
        errno = error;
        return false;
    }
    chmod(socketPath.c_str(), 0600);
    refreshCatalog();
    std::cout << "[LCL Session] Session authority active on " << socketPath << "\n";
    return true;
}

void SessionService::shutdown() {
    for (const auto& [_, instance] : m_instances) {
        if (instance.running && instance.pid > 0) {
            // Sessiond creates one process group per app instance, so shutdown
            // can reclaim the app and any children it owns without touching an
            // unrelated session process.
            kill(-instance.pid, SIGTERM);
        }
    }
    for (const int fd : m_clientFds) close(fd);
    m_clientFds.clear();
    m_waiters.clear();
    if (m_serverFd >= 0) {
        close(m_serverFd);
        m_serverFd = -1;
    }
    if (!m_socketPath.empty()) unlink(m_socketPath.c_str());
}

void SessionService::refreshCatalog() {
    m_registry.refresh();
    std::cout << "[LCL Session] Catalog refreshed: " << m_registry.entries().size()
              << " application bundle(s).\n";
}

LaunchResponse SessionService::launch(const LaunchRequest& request) {
    LaunchResponse response{};
    const auto app = m_registry.find(request.target);
    if (!app || !app->valid) {
        response.status = 1;
        response.message = "unknown application: " + request.target;
        return response;
    }
    if (access(app->executablePath.c_str(), X_OK) != 0 &&
        !(app->executablePath.size() >= 3 &&
          app->executablePath.substr(app->executablePath.size() - 3) == ".js")) {
        response.status = 2;
        response.message = "application executable is unavailable: " + app->executablePath;
        return response;
    }

    const auto args = executableArgs(*app);
    if (args.empty()) {
        response.status = 3;
        response.message = "application has no executable";
        return response;
    }
    const pid_t child = fork();
    if (child < 0) {
        response.status = 4;
        response.message = std::string("fork failed: ") + std::strerror(errno);
        return response;
    }
    if (child == 0) {
        setsid();
        exportLaunchOrigin(request.origin);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execv(argv.front(), argv.data());
        _exit(127);
    }

    AppInstance instance;
    instance.instanceId = m_nextInstanceId++;
    instance.appId = app->appId;
    instance.pid = static_cast<int32_t>(child);
    instance.running = true;
    m_instanceByPid.emplace(instance.pid, instance.instanceId);
    m_instances.emplace(instance.instanceId, instance);

    response.instanceId = instance.instanceId;
    response.pid = instance.pid;
    response.appId = instance.appId;
    response.message = "launched";
    std::cout << "[LCL Session] Launched " << app->appId << " as instance "
              << instance.instanceId << " (PID " << child << ")\n";
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

bool SessionService::sendPacket(int fd, SessionOpcode opcode, uint32_t requestId,
                                const std::vector<uint8_t>& payload) {
    SessionHeader header{};
    header.opcode = opcode;
    header.requestId = requestId;
    header.payloadSize = static_cast<uint32_t>(payload.size());
    std::vector<uint8_t> packet;
    if (!encodePacket(header, payload, packet)) return false;
    return send(fd, packet.data(), packet.size(), MSG_NOSIGNAL) ==
           static_cast<ssize_t>(packet.size());
}

void SessionService::acceptConnections() {
    while (true) {
        const int fd = accept4(m_serverFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0) {
            m_clientFds.push_back(fd);
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            std::cerr << "[LCL Session ERROR] accept failed: " << std::strerror(errno) << "\n";
        }
        return;
    }
}

void SessionService::serviceClient(int fd) {
    std::array<uint8_t, kReceiveBufferSize> bytes{};
    while (true) {
        const ssize_t count = recv(fd, bytes.data(), bytes.size(), MSG_DONTWAIT);
        if (count == 0) {
            removeClient(fd);
            return;
        }
        if (count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
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
        if (packet.header.opcode == SessionOpcode::CatalogRequest && packet.payload.empty()) {
            std::vector<CatalogEntry> entries;
            entries.reserve(m_registry.entries().size());
            for (const auto& app : m_registry.entries()) {
                entries.push_back({app.appId, app.name, app.version, resolvedIconPath(app), app.type});
            }
            std::vector<uint8_t> payload;
            encodeCatalogSnapshot(entries, payload);
            sendPacket(fd, SessionOpcode::CatalogSnapshot, packet.header.requestId, payload);
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
            if (!sendPacket(fd, SessionOpcode::LaunchResponse, packet.header.requestId, payload)) {
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
        sendPacket(fd, SessionOpcode::ErrorResponse, packet.header.requestId, error);
    }
}

void SessionService::removeClient(int fd) {
    std::erase(m_clientFds, fd);
    for (auto& [_, waiters] : m_waiters) std::erase(waiters, fd);
    close(fd);
}

int SessionService::exitCodeFromStatus(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

void SessionService::notifyExit(const ProcessExited& event) {
    const auto found = m_waiters.find(event.instanceId);
    if (found == m_waiters.end()) return;
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
        if (pid <= 0) return;
        const auto found = m_instanceByPid.find(static_cast<int32_t>(pid));
        if (found == m_instanceByPid.end()) continue;
        const uint64_t instanceId = found->second;
        m_instanceByPid.erase(found);
        auto instance = m_instances.find(instanceId);
        if (instance == m_instances.end()) continue;
        instance->second.running = false;
        instance->second.exitCode = exitCodeFromStatus(status);
        notifyExit({instanceId, instance->second.exitCode});
        std::cout << "[LCL Session] Instance " << instanceId << " exited with "
                  << instance->second.exitCode << "\n";
    }
}

void SessionService::poll() {
    if (m_serverFd >= 0) {
        acceptConnections();
        const auto clients = m_clientFds;
        for (const int fd : clients) {
            if (std::find(m_clientFds.begin(), m_clientFds.end(), fd) != m_clientFds.end()) {
                serviceClient(fd);
            }
        }
    }
    reapChildren();
}

} // namespace lcl::session
