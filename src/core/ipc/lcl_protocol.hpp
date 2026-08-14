#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <sys/types.h>
#include <sys/socket.h>

namespace lcl::protocol {

constexpr uint32_t LCL_PROTOCOL_MAGIC = 0x4C434C50; // "LCLP"
constexpr uint32_t LCL_PROTOCOL_VERSION = 10;
constexpr uint32_t LCL_BUFFER_FORMAT_ARGB8888 = 1;
constexpr uint32_t LCL_PROTOCOL_MAX_PAYLOAD = 1024u * 1024u;
constexpr uint32_t LCL_PROTOCOL_WIRE_HEADER_SIZE = 24u;

enum class LCLOpcode : uint32_t {
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
    RequestWindowAction = 18,
    SubscribeShellState = 19,
    ShellStateSnapshot = 20,
    ShellStateDelta = 21,
    SetSystemSurfaceKind = 22,
    SetWindowCornerStyle = 23,
    // A single-plane DRM/GBM buffer. The file descriptor travels through
    // SCM_RIGHTS, while this message carries the metadata needed for import.
    AttachDmaBuf = 24,
    // Sent by the compositor only after it has stopped sampling a DMA-BUF.
    // Clients must not render into that pool slot before this message arrives.
    ReleaseDmaBuf = 25,
    // Presentation timing is separate from DMA-BUF ownership. A release makes
    // a pool slot writable; this callback paces the next interactive frame.
    FramePresented = 26
};

enum class LCLSystemSurfaceKind : uint32_t {
    None = 0,
    Wallpaper = 1,
    MenuBar = 2,
    Dock = 3,
};

enum class LCLSceneVisibility : uint8_t {
    Visible = 0,
    Minimized = 1,
    Closing = 2,
};

enum class LCLShellStateDeltaKind : uint32_t {
    SceneAdded = 1,
    SceneUpdated = 2,
    SceneRemoved = 3,
    FocusChanged = 4,
};

/** Client-originated requests for compositor-owned window state. */
enum class LCLWindowAction : uint32_t {
    BeginDrag = 1,
    Minimize = 2,
    Maximize = 3,
    Restore = 4,
    ToggleMaximize = 5,
    Close = 6
};

enum class LCLDecorationMode : uint32_t {
    SSD = 0, // Server-Side Decoration
    CSD = 1, // Client-Side Decoration
    None = 2 // Frameless / No Decoration
};

/**
 * How the compositor presents a surface while its configured size changes.
 * Live advances only after each matching client buffer commit; CompositorMorph
 * animates the compositor presentation while a replacement buffer is pending.
 */
enum class LCLResizePresentationMode : uint8_t {
    Live = 0,
    CompositorMorph = 1,
};

/** Why the compositor is issuing a size configure. */
enum class LCLConfigureResizeReason : uint8_t {
    Initial = 0,
    Interactive = 1,
    WindowStateTransition = 2,
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

enum class EffectBoundsPolicy : uint8_t {
    Local = 0,
    WindowGroup = 1
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
    float cornerRoundness{2.0f};
    EffectBoundsPolicy boundsPolicy{EffectBoundsPolicy::Local};
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
    uint32_t flags{0};
    uint32_t requestId{0};
    uint32_t payloadSize{0};
};

struct LCLMsgSurfaceCreate {
    uint32_t surfaceId{0};
    int32_t x{0};
    int32_t y{0};
    uint32_t width{0};
    uint32_t height{0};
    char title[128]{0};
    char appId[64]{0};
    float bufferScale{1.0f}; // Required v3 logical-to-buffer scale.
    LCLResizePresentationMode resizePresentation{LCLResizePresentationMode::CompositorMorph};
};

struct LCLMsgSurfaceDestroy {
    uint32_t surfaceId{0};
};

struct LCLMsgConfigureBounds {
    uint32_t surfaceId{0};
    uint64_t configureSerial{0};
    int32_t x{0};
    int32_t y{0};
    uint32_t width{0};
    uint32_t height{0};
    // Logical capacity hint for a GPU backing allocation. Content remains
    // width x height and must fit inside this extent without being scaled.
    uint32_t backingWidth{0};
    uint32_t backingHeight{0};
    uint32_t headerColor{0};
    uint8_t isFocused{0};
    char title[128]{0};
    float bufferScale{1.0f}; // Required v3 scale; bounds and input are logical.
    LCLConfigureResizeReason resizeReason{LCLConfigureResizeReason::Initial};
};

struct LCLMsgAttachBuffer {
    uint32_t surfaceId{0};
    uint64_t configureSerial{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride{0};
    uint32_t format{0}; // LCL_BUFFER_FORMAT_ARGB8888
};

struct LCLMsgAttachDmaBuf {
    uint32_t surfaceId{0};
    uint64_t configureSerial{0};
    uint32_t bufferId{0};
    // Physical pixels containing valid newly rendered content.
    uint32_t width{0};
    uint32_t height{0};
    // Physical dimensions of the exported GBM allocation.
    uint32_t backingWidth{0};
    uint32_t backingHeight{0};
    uint32_t stride{0};
    uint32_t format{0}; // LCL_BUFFER_FORMAT_ARGB8888
    uint64_t modifier{~uint64_t{0}}; // DRM_FORMAT_MOD_INVALID when unspecified
};

struct LCLMsgReleaseDmaBuf {
    uint32_t surfaceId{0};
    uint32_t bufferId{0};
};

struct LCLMsgFramePresented {
    uint32_t surfaceId{0};
    uint64_t timestampNs{0};
    uint64_t refreshIntervalNs{0};
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

struct LCLMsgRequestWindowAction {
    uint32_t surfaceId{0};
    LCLWindowAction action{LCLWindowAction::BeginDrag};
    // Used by BeginDrag only. Coordinates are client-local logical pixels.
    float localX{0.0f};
    float localY{0.0f};
};

struct LCLMsgSetInsetBorder {
    uint32_t surfaceId{0};
    uint8_t enabled{1};
};

struct LCLMsgSetWindowCornerRadius {
    uint32_t surfaceId{0};
    float radiusPx{0.0f};
};

struct LCLMsgSetWindowCornerStyle {
    uint32_t surfaceId{0};
    float radiusPx{0.0f};
    float roundness{2.0f};
};

/** Subscribe to compositor-owned scene/focus state from a known revision. */
struct LCLMsgSubscribeShellState {
    uint64_t lastKnownRevision{0};
};

/** Declared once before SurfaceCreate by the trusted desktop/mobile shell. */
struct LCLMsgSetSystemSurfaceKind {
    LCLSystemSurfaceKind kind{LCLSystemSurfaceKind::None};
};

/** One shell-visible scene. No renderer or SHM ownership crosses this boundary. */
struct LCLMsgShellScene {
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
    LCLSceneVisibility visibility{LCLSceneVisibility::Visible};
    char appId[64]{0};
    char title[128]{0};
};

/** Followed by sceneCount LCLMsgShellScene records. */
struct LCLMsgShellStateSnapshot {
    uint64_t revision{0};
    uint32_t sceneCount{0};
    uint32_t seatId{0};
    uint32_t displayId{0};
    uint32_t workspaceId{0};
    uint64_t activeSceneId{0};
};

/** A scene record is populated for scene changes; focus changes carry focus only. */
struct LCLMsgShellStateDelta {
    uint64_t revision{0};
    LCLShellStateDeltaKind kind{LCLShellStateDeltaKind::SceneUpdated};
    uint32_t seatId{0};
    uint32_t displayId{0};
    uint32_t workspaceId{0};
    uint64_t activeSceneId{0};
    LCLMsgShellScene scene{};
};

#pragma pack(pop)

/**
 * @brief Send an IPC packet with optional shared memory file descriptor (SCM_RIGHTS).
 */
bool sendMsgWithFd(int socketFd, const LCLHeader& header, const void* payload, int passedFd = -1);

/** Flush packets previously queued because a non-blocking socket returned EAGAIN. */
bool flushPendingWrites(int socketFd);

/** Release queued packets and duplicated descriptors owned for a disconnected socket. */
void discardPendingWrites(int socketFd);

enum class ReceiveStatus {
    Received,
    WouldBlock,
    Closed,
    Invalid,
    IoError
};

/** Receive and validate one complete SOCK_SEQPACKET protocol packet. */
ReceiveStatus recvPacketWithFd(int socketFd, LCLHeader& header,
                               std::vector<uint8_t>& payload, int& receivedFd);

/**
 * @brief Receive an IPC packet with optional shared memory file descriptor (SCM_RIGHTS).
 */
bool recvMsgWithFd(int socketFd, LCLHeader& header, std::vector<uint8_t>& payload, int& receivedFd);

/** Explicit v3 little-endian codec entry points used by transport and tests. */
bool encodePacket(const LCLHeader& header, const void* nativePayload,
                  std::vector<uint8_t>& packet);
bool decodePacket(const uint8_t* packet, size_t packetSize, LCLHeader& header,
                  std::vector<uint8_t>& nativePayload);

} // namespace lcl::protocol
