#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <sys/types.h>
#include <sys/socket.h>

namespace lcl::protocol {

constexpr uint32_t LCL_PROTOCOL_MAGIC = 0x4C434C50; // "LCLP"
constexpr uint32_t LCL_PROTOCOL_VERSION = 1;

enum class LCLRole : uint32_t {
    Unspecified = 0,
    WindowManager = 1,
    ShellPanel = 2,
    DesktopWallpaper = 3,
    ClientApp = 4
};

enum class LCLOpcode : uint32_t {
    RegisterRole = 1,
    SurfaceCreate = 2,
    SurfaceDestroy = 3,
    ConfigureBounds = 4,
    AttachBuffer = 5,
    InputEvent = 6,
    AckResponse = 7
};

#pragma pack(push, 1)

struct LCLHeader {
    uint32_t magic{LCL_PROTOCOL_MAGIC};
    uint32_t version{LCL_PROTOCOL_VERSION};
    LCLOpcode opcode{LCLOpcode::AckResponse};
    uint32_t payloadSize{0};
};

struct LCLMsgRegisterRole {
    LCLRole role{LCLRole::ClientApp};
    char clientName[64]{0};
};

struct LCLMsgSurfaceCreate {
    uint32_t surfaceId{0};
    int32_t x{0};
    int32_t y{0};
    uint32_t width{0};
    uint32_t height{0};
    char title[128]{0};
};

struct LCLMsgSurfaceDestroy {
    uint32_t surfaceId{0};
};

struct LCLMsgConfigureBounds {
    uint32_t surfaceId{0};
    int32_t x{0};
    int32_t y{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t headerColor{0};
    uint8_t isFocused{0};
    char title[128]{0};
};

struct LCLMsgAttachBuffer {
    uint32_t surfaceId{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride{0};
    uint32_t format{0}; // e.g. ARGB8888
};

struct LCLMsgAckResponse {
    uint32_t status{0}; // 0 = OK, >0 = error code
    char message[128]{0};
};

struct LCLMsgInputEvent {
    uint32_t surfaceId{0};
    uint32_t type{0};      // 1 = KeyboardKey, 2 = PointerMotion, 3 = PointerButton
    uint32_t key{0};       // Linux evdev keycode (e.g. KEY_A, KEY_ENTER)
    uint8_t  pressed{0};   // 1 = Down, 0 = Up
    uint8_t  modifiers{0};
    float    x{0.0f};
    float    y{0.0f};
};

#pragma pack(pop)

/**
 * @brief Send an IPC packet with optional shared memory file descriptor (SCM_RIGHTS).
 */
bool sendMsgWithFd(int socketFd, const LCLHeader& header, const void* payload, int passedFd = -1);

/**
 * @brief Receive an IPC packet with optional shared memory file descriptor (SCM_RIGHTS).
 */
bool recvMsgWithFd(int socketFd, LCLHeader& header, std::vector<uint8_t>& payload, int& receivedFd);

} // namespace lcl::protocol
