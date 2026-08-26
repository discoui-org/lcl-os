#include "core/shell/shell_state_client.hpp"

#include <cstring>
#include <iostream>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace lcl::shell {
namespace {

ShellScene fromWire(const protocol::LCLMsgShellScene& wire) {
    ShellScene scene{};
    scene.sceneId = wire.sceneId;
    scene.appInstanceId = wire.appInstanceId;
    scene.windowId = wire.windowId;
    scene.clientPid = wire.clientPid;
    scene.displayId = wire.displayId;
    scene.workspaceId = wire.workspaceId;
    scene.x = wire.x;
    scene.y = wire.y;
    scene.width = wire.width;
    scene.height = wire.height;
    scene.visibility = wire.visibility;
    scene.decorationMode = wire.decorationMode;
    scene.edgeToEdge = wire.edgeToEdge != 0;
    scene.appId = wire.appId;
    scene.title = wire.title;
    return scene;
}

} // namespace

ShellStateClient::~ShellStateClient() {
    if (m_socketFd >= 0) {
        protocol::discardPendingWrites(m_socketFd);
        close(m_socketFd);
    }
}

bool ShellStateClient::connect(const std::string& socketPath) {
    if (m_socketFd >= 0) return true;
    m_socketFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (m_socketFd < 0) return false;

    std::string effectiveSocketPath = socketPath;
    if (const char* envSocket = std::getenv("LCL_COMPOSITOR_SOCKET")) {
        if (envSocket[0] != '\0') {
            effectiveSocketPath = envSocket;
        }
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (effectiveSocketPath.size() >= sizeof(address.sun_path)) {
        close(m_socketFd);
        m_socketFd = -1;
        return false;
    }
    std::strncpy(address.sun_path, effectiveSocketPath.c_str(), sizeof(address.sun_path) - 1);
    if (::connect(m_socketFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(m_socketFd);
        m_socketFd = -1;
        return false;
    }
    const int flags = fcntl(m_socketFd, F_GETFL, 0);
    if (flags >= 0) fcntl(m_socketFd, F_SETFL, flags | O_NONBLOCK);

    if (!subscribe(0)) {
        close(m_socketFd);
        m_socketFd = -1;
        return false;
    }
    return true;
}

bool ShellStateClient::poll() {
    if (m_socketFd < 0) return false;
    while (true) {
        protocol::LCLHeader header{};
        std::vector<uint8_t> payload;
        int receivedFd = -1;
        const auto status = protocol::recvPacketWithFd(m_socketFd, header, payload, receivedFd);
        if (receivedFd >= 0) close(receivedFd);
        if (status == protocol::ReceiveStatus::WouldBlock) return true;
        if (status != protocol::ReceiveStatus::Received) {
            close(m_socketFd);
            m_socketFd = -1;
            return false;
        }
        if (header.opcode == protocol::LCLOpcode::AckResponse) {
            if (payload.size() == sizeof(protocol::LCLMsgAckResponse)) {
                const auto* ack = reinterpret_cast<const
                    protocol::LCLMsgAckResponse*>(payload.data());
                if (ack->status != 0) {
                    std::cerr << "[LCL Shell State ERROR] Compositor rejected request "
                              << header.requestId << " (status " << ack->status
                              << "): " << ack->message << "\n";
                }
            }
            continue;
        }

        if (header.opcode == protocol::LCLOpcode::ShellStateSnapshot) {
            if (payload.size() < sizeof(protocol::LCLMsgShellStateSnapshot)) {
                requestSnapshot();
                continue;
            }
            const auto* wire = reinterpret_cast<const protocol::LCLMsgShellStateSnapshot*>(payload.data());
            const size_t expected = sizeof(*wire) +
                static_cast<size_t>(wire->sceneCount) * sizeof(protocol::LCLMsgShellScene);
            if (payload.size() != expected) {
                requestSnapshot();
                continue;
            }
            ShellStateSnapshot snapshot{};
            snapshot.revision = wire->revision;
            snapshot.seatId = wire->seatId;
            snapshot.displayId = wire->displayId;
            snapshot.workspaceId = wire->workspaceId;
            snapshot.activeSceneId = wire->activeSceneId;
            const auto* scenes = reinterpret_cast<const protocol::LCLMsgShellScene*>(payload.data() + sizeof(*wire));
            snapshot.scenes.reserve(wire->sceneCount);
            for (uint32_t index = 0; index < wire->sceneCount; ++index) {
                snapshot.scenes.push_back(fromWire(scenes[index]));
            }
            m_revision = snapshot.revision;
            if (m_onSnapshot) m_onSnapshot(snapshot);
            continue;
        }

        if (header.opcode == protocol::LCLOpcode::ShellStateDelta &&
            payload.size() == sizeof(protocol::LCLMsgShellStateDelta)) {
            const auto* wire = reinterpret_cast<const protocol::LCLMsgShellStateDelta*>(payload.data());
            if (wire->revision != m_revision + 1) {
                requestSnapshot();
                continue;
            }
            ShellStateDelta delta{};
            delta.revision = wire->revision;
            delta.kind = wire->kind;
            delta.seatId = wire->seatId;
            delta.displayId = wire->displayId;
            delta.workspaceId = wire->workspaceId;
            delta.activeSceneId = wire->activeSceneId;
            delta.scene = fromWire(wire->scene);
            m_revision = delta.revision;
            if (m_onDelta) m_onDelta(delta);
        }
    }
}

bool ShellStateClient::subscribe(uint64_t lastKnownRevision) {
    protocol::LCLMsgSubscribeShellState request{};
    request.lastKnownRevision = lastKnownRevision;
    return send(protocol::LCLOpcode::SubscribeShellState, &request, sizeof(request));
}

bool ShellStateClient::send(protocol::LCLOpcode opcode, const void* payload, uint32_t payloadSize) {
    if (m_socketFd < 0) return false;
    protocol::LCLHeader header{};
    header.opcode = opcode;
    header.requestId = m_nextRequestId++;
    if (m_nextRequestId == 0) m_nextRequestId = 1;
    header.payloadSize = payloadSize;
    return protocol::sendMsgWithFd(m_socketFd, header, payload);
}

void ShellStateClient::requestSnapshot() {
    m_revision = 0;
    subscribe(0);
}

} // namespace lcl::shell
