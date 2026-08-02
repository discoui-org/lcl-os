#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <sys/types.h>
#include <sys/socket.h>

namespace lcl::protocol {

constexpr uint32_t LCL_PROTOCOL_MAGIC = 0x4C434C50; // "LCLP"
constexpr uint32_t LCL_PROTOCOL_VERSION = 2;

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
    AckResponse = 7,
    SetDecorationMode = 8,
    SetWindowLayer = 9,
    SetReservedZone = 10,
    SetEffectGraph = 11,
    ClearEffectGraph = 12,
    BeginWindowMove = 13,
    RequestSurfaceClose = 14,
    SetInsetBorder = 15,
    SetWindowCornerRadius = 16,
    WindowListUpdate = 17
};

enum class LCLDecorationMode : uint32_t {
    SSD = 0, // Server-Side Decoration
    CSD = 1, // Client-Side Decoration
    None = 2 // Frameless / No Decoration
};

enum class LCLWindowLayer : uint32_t {
    Bottom = 0,  // Wallpaper / Background
    Normal = 1,  // Standard Application Windows
    TopMost = 2  // Menu Bar, Dock, System Overlays
};

enum class FilterType : uint8_t {
    None = 0,
    Blur = 1,
    Brightness = 2,
    Contrast = 3,
    Saturation = 4,
    Grayscale = 5,
    Invert = 6,
    Glass = 7
};

enum class GlassProfile : uint8_t {
    Auto = 0,
    Clear = 1,
    Frosted = 2,
    Dense = 3
};

enum class EffectSourceType : uint8_t {
    Backdrop = 0,
    Foreground = 1
};

enum class EffectBlendMode : uint8_t {
    Normal = 0,
    Screen = 1,
    Multiply = 2,
    Overlay = 3,
    Plus = 4
};

#pragma pack(push, 1)

struct FilterOp {
    FilterType type{FilterType::None};
    float value{0.0f};
    uint8_t profile{static_cast<uint8_t>(GlassProfile::Auto)};

    // Reserved for future per-filter metadata without protocol reshaping.
    uint8_t reserved0{0};
    uint16_t reserved1{0};

    // Optional custom parameters (used by advanced filters like Glass).
    // Glass mapping:
    // params[0] = thicknessPx
    // params[1] = refractionFactor
    // params[2] = dispersionGain
    float params[3]{0.0f, 0.0f, 0.0f};
};

struct EffectRegion {
    int32_t x{0};
    int32_t y{0};
    uint32_t width{0};
    uint32_t height{0};
    float cornerRadius{0.0f};
    EffectSourceType source{EffectSourceType::Backdrop};
    EffectBlendMode blendMode{EffectBlendMode::Normal};
    uint16_t filterCount{0};
    uint32_t filterOffset{0};
    float opacity{1.0f};
};

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
    // Logical-to-buffer scale. 1.0 keeps the legacy raw-pixel protocol.
    float bufferScale{1.0f};
    char title[128]{0};
    char appId[64]{0};
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
    // Width/height and input coordinates are in logical pixels at this scale.
    float bufferScale{1.0f};
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
    uint32_t type{0};      // 1 = KeyDown, 2 = KeyUp, 3 = PointerMotion, 4 = PointerButton, 5 = KeyPress/TextInput
    uint32_t key{0};       // Linux evdev keycode (e.g. KEY_A, KEY_ENTER)
    uint8_t  pressed{0};   // 1 = Down, 0 = Up
    uint8_t  modifiers{0}; // Bitmask: 0x01=Shift, 0x02=Ctrl, 0x04=Alt, 0x08=CapsLock, 0x10=Super
    uint32_t codepoint{0}; // Translated UTF-8 / ASCII codepoint (e.g. 'A', 'a', '1', '\n')
    float    x{0.0f};
    float    y{0.0f};
};

struct LCLMsgSetDecorationMode {
    uint32_t surfaceId{0};
    LCLDecorationMode mode{LCLDecorationMode::SSD};
};

struct LCLMsgSetWindowLayer {
    uint32_t surfaceId{0};
    LCLWindowLayer layer{LCLWindowLayer::Normal};
    uint8_t unfocusable{0}; // 1 = unfocusable (does not steal focus), 0 = focusable
};

struct LCLMsgSetReservedZone {
    uint32_t surfaceId{0};
    uint32_t top{0};    // Reserved inset from top of screen (Menu bar height, e.g. 32px)
    uint32_t bottom{0}; // Reserved inset from bottom of screen (Dock height)
    uint32_t left{0};   // Reserved inset from left edge
    uint32_t right{0};  // Reserved inset from right edge
};

struct LCLMsgSetEffectGraphHeader {
    uint32_t surfaceId{0};
    uint32_t regionCount{0}; // Followed by EffectRegion array payload
    uint32_t filterCount{0}; // Followed by flattened FilterOp array payload
};

struct LCLMsgClearEffectGraph {
    uint32_t surfaceId{0};
};

struct LCLMsgBeginWindowMove {
    uint32_t surfaceId{0};
    float localX{0.0f};
    float localY{0.0f};
};

struct LCLMsgRequestSurfaceClose {
    uint32_t surfaceId{0};
};

struct LCLMsgSetInsetBorder {
    uint32_t surfaceId{0};
    uint8_t enabled{1};
};

struct LCLMsgSetWindowCornerRadius {
    uint32_t surfaceId{0};
    float radiusPx{0.0f};
};

struct LCLMsgWindowListHeader {
    uint32_t windowCount{0};
};

struct LCLMsgWindowListEntry {
    uint32_t windowId{0};
    uint8_t isFocused{0};
    char title[128]{0};
    char appId[64]{0};
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
