#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/ipc/lcl_protocol.hpp"

namespace lcl::shell {

struct ShellScene {
    uint64_t sceneId{0};
    uint64_t appInstanceId{0};
    uint32_t windowId{0};
    int32_t clientPid{0};
    uint32_t displayId{0};
    uint32_t workspaceId{0};
    int32_t x{0};
    int32_t y{0};
    int32_t width{0};
    int32_t height{0};
    protocol::LCLSceneVisibility visibility{protocol::LCLSceneVisibility::Visible};
    protocol::LCLDecorationMode decorationMode{protocol::LCLDecorationMode::SSD};
    bool edgeToEdge{false};
    std::string appId;
    std::string title;
};

struct ShellStateSnapshot {
    uint64_t revision{0};
    uint32_t seatId{0};
    uint32_t displayId{0};
    uint32_t workspaceId{0};
    uint64_t activeSceneId{0};
    std::vector<ShellScene> scenes;
};

struct ShellStateDelta {
    uint64_t revision{0};
    protocol::LCLShellStateDeltaKind kind{protocol::LCLShellStateDeltaKind::SceneUpdated};
    uint32_t seatId{0};
    uint32_t displayId{0};
    uint32_t workspaceId{0};
    uint64_t activeSceneId{0};
    ShellScene scene{};
};

/**
 * Typed, revision-aware consumer of compositor shell state.  It is deliberately
 * independent of widgets and may be shared by desktop and mobile shells.
 */
class ShellStateClient {
public:
    using SnapshotCallback = std::function<void(const ShellStateSnapshot&)>;
    using DeltaCallback = std::function<void(const ShellStateDelta&)>;

    ShellStateClient() = default;
    ~ShellStateClient();
    ShellStateClient(const ShellStateClient&) = delete;
    ShellStateClient& operator=(const ShellStateClient&) = delete;

    bool connect(const std::string& socketPath = "/Runtime/lcl-compositor.sock");
    void setOnSnapshot(SnapshotCallback callback) { m_onSnapshot = std::move(callback); }
    void setOnDelta(DeltaCallback callback) { m_onDelta = std::move(callback); }
    /** Drain available state packets. Returns false after disconnect/protocol failure. */
    bool poll();
    bool isConnected() const noexcept { return m_socketFd >= 0; }
    uint64_t revision() const noexcept { return m_revision; }

private:
    bool subscribe(uint64_t lastKnownRevision);
    bool send(protocol::LCLOpcode opcode, const void* payload, uint32_t payloadSize);
    void requestSnapshot();

    int m_socketFd{-1};
    uint32_t m_nextRequestId{1};
    uint64_t m_revision{0};
    SnapshotCallback m_onSnapshot;
    DeltaCallback m_onDelta;
};

} // namespace lcl::shell
