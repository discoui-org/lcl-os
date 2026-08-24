#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lcl::session {

inline constexpr uint32_t kSessionProtocolMagic = 0x4C435353; // "LCSS"
inline constexpr uint32_t kSessionProtocolVersion = 3;
inline constexpr uint32_t kSessionWireHeaderSize = 20;
inline constexpr uint32_t kSessionMaxPayload = 64u * 1024u;
inline constexpr const char* kSessionSocket = "/Runtime/lcl-sessiond.sock";

enum class SessionOpcode : uint32_t {
    CatalogRequest = 1,
    CatalogSnapshot = 2,
    LaunchRequest = 3,
    LaunchResponse = 4,
    ProcessExited = 5,
    ErrorResponse = 6,
};

struct SessionHeader {
    uint32_t magic{kSessionProtocolMagic};
    uint32_t version{kSessionProtocolVersion};
    SessionOpcode opcode{SessionOpcode::ErrorResponse};
    uint32_t requestId{0};
    uint32_t payloadSize{0};
};

struct CatalogEntry {
    std::string appId;
    std::string name;
    std::string version;
    std::string icon;
    std::string type;
};

struct LaunchRequest {
    std::string target;
    bool waitForExit{false};
    /** The caller requests reuse instead of spawning a second app process. */
    bool singleInstance{false};
    /** Caller-owned identity joining placeholder, process and first surface. */
    uint64_t launchToken{0};
    struct Origin {
        bool valid{false};
        float x{0.0f};
        float y{0.0f};
        float width{0.0f};
        float height{0.0f};
        float cornerRadius{0.0f};
    } origin;
};

struct LaunchResponse {
    uint32_t status{0};
    uint64_t instanceId{0};
    int32_t pid{0};
    bool reused{false};
    std::string appId;
    std::string message;
};

struct ProcessExited {
    uint64_t instanceId{0};
    int32_t exitCode{0};
};

struct DecodedPacket {
    SessionHeader header;
    std::vector<uint8_t> payload;
};

bool encodePacket(const SessionHeader& header, const std::vector<uint8_t>& payload,
                  std::vector<uint8_t>& packet);
bool decodePacket(const uint8_t* packet, size_t packetSize, DecodedPacket& decoded);

bool encodeCatalogSnapshot(const std::vector<CatalogEntry>& entries,
                           std::vector<uint8_t>& payload);
bool decodeCatalogSnapshot(const std::vector<uint8_t>& payload,
                           std::vector<CatalogEntry>& entries);
bool encodeLaunchRequest(const LaunchRequest& request, std::vector<uint8_t>& payload);
bool decodeLaunchRequest(const std::vector<uint8_t>& payload, LaunchRequest& request);
bool encodeLaunchResponse(const LaunchResponse& response,
                          std::vector<uint8_t>& payload);
bool decodeLaunchResponse(const std::vector<uint8_t>& payload,
                          LaunchResponse& response);
bool encodeProcessExited(const ProcessExited& event, std::vector<uint8_t>& payload);
bool decodeProcessExited(const std::vector<uint8_t>& payload, ProcessExited& event);
bool encodeError(const std::string& message, std::vector<uint8_t>& payload);
bool decodeError(const std::vector<uint8_t>& payload, std::string& message);

} // namespace lcl::session
