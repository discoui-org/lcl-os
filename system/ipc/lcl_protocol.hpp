#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <sys/types.h>
#include <sys/socket.h>

namespace lcl::protocol {

constexpr uint32_t LCL_PROTOCOL_MAGIC = 0x4C434C50; // "LCLP"
constexpr uint32_t LCL_PROTOCOL_VERSION = 29;
constexpr uint32_t LCL_PROTOCOL_MAX_PAYLOAD = 1024u * 1024u;
constexpr uint32_t LCL_PROTOCOL_WIRE_HEADER_SIZE = 24u;
constexpr uint32_t LCL_LAUNCH_ICON_MAX_DIMENSION = 256u;

enum class LCLOpcode : uint32_t {
    SurfaceCreate = 2,
    SurfaceDestroy = 3,
    ConfigureBounds = 4,
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
    // Presentation timing returns the app/raster producer's frame credit.
    FramePresented = 26,
    SetEdgeToEdge = 27,
    PopupSurfaceCreate = 28,
    // Returns the single-frame presentation credit when a commit cannot be
    // presented, commonly because a newer configure serial superseded it.
    FrameDiscarded = 29,
    BeginLaunchPlaceholder = 33,
    ResolveLaunchPlaceholder = 34,
    CancelLaunchPlaceholder = 35,
    LaunchIconVisibility = 36,
    LaunchIconVisibilityAck = 37,
    // Trusted window-manager surface composited as a child of an existing
    // toplevel.  The compositor treats its contents as opaque UI policy.
    AttachedSurfaceCreate = 40,
    // Trusted window-manager action targeting the parent toplevel rather than
    // the manager-owned attached surface that originated the interaction.
    RequestManagedWindowAction = 41,
    // Grants one surface-specific capability for the out-of-process raster
    // service. Application content never carries this token to the compositor.
    SurfaceProducerGrant = 42
};

/** Minimal v1 popup role. Feature semantics remain in client-side UI policy. */
enum class LCLPopupRole : uint32_t {
    Transient = 1,
};

/** Generic relationship between a trusted WM surface and a toplevel group. */
enum class LCLAttachedSurfaceRole : uint32_t {
    Frame = 1,
    Adornment = 2,
};

enum class LCLSystemSurfaceKind : uint32_t {
    None = 0,
    Wallpaper = 1,
    MenuBar = 2,
    Dock = 3,
    HomeScreen = 4,
    PermissionPrompt = 5,
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
    Glass = 7,
    Tint = 8
};

enum class GlassProfile : uint8_t {
    Auto = 0,
    Clear = 1,
    Frosted = 2,
    Dense = 3
};

enum class EffectSourceType : uint8_t {
    /** Filter the application's own presented layer. */
    Layer = 0,
    /** Compatibility spelling for the old foreground source. */
    Foreground = Layer,
    /** Backdrop from the compositor scene behind an application surface. */
    SurfaceBackdrop = 1,
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
    OuterSurface = 1
};

#pragma pack(push, 1)

struct FilterOp {
    FilterType type{FilterType::None};
    float value{0.0f};
    uint8_t profile{static_cast<uint8_t>(GlassProfile::Auto)};

    // Reserved for future per-filter metadata without protocol reshaping.
    uint8_t reserved0{0};
    uint16_t reserved1{0};

    // Optional custom parameters (used by advanced filters like Glass and Tint).
    // Glass mapping:
    // params[0] = logical thickness (zero disables Glass)
    // params[1] = refractionFactor (zero disables Glass)
    // params[2] = dispersionGain (zero keeps refraction without RGB separation)
    // Tint mapping:
    // value = alpha in [0, 1]
    // params[0..2] = red, green, blue in [0, 255]
    // Blur uses value as its logical radius. Raster execution may choose a
    // continuously varying working scale, but the filtered result remains
    // fully opaque and no params entry changes the visual radius.
    float params[3]{0.0f, 0.0f, 0.0f};
};

struct EffectRegion {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
    float cornerRadius{0.0f};
    float cornerRoundness{2.0f};
    EffectBoundsPolicy boundsPolicy{EffectBoundsPolicy::Local};
    EffectSourceType source{EffectSourceType::SurfaceBackdrop};
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
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
    char title[128]{0};
    char appId[64]{0};
    uint8_t hasLaunchOrigin{0};
    float launchOriginX{0.0f};
    float launchOriginY{0.0f};
    float launchOriginWidth{0.0f};
    float launchOriginHeight{0.0f};
    float launchOriginCornerRadius{0.0f};
    uint64_t launchToken{0};
    uint64_t appInstanceId{0};
    // Interactive toplevel resize increments in logical content pixels.
    // Zero increments disable quantization on the corresponding axis.
    float resizeBaseWidth{0.0f};
    float resizeBaseHeight{0.0f};
    float resizeWidthIncrement{0.0f};
    float resizeHeightIncrement{0.0f};
};

/**
 * Trusted HomeScreen request for an immediate compositor-owned launch visual.
 * Followed in the native payload by iconWidth * iconHeight ARGB32 pixels.
 */
struct LCLMsgBeginLaunchPlaceholder {
    uint32_t homeSurfaceId{0};
    uint64_t launchToken{0};
    char appId[64]{0};
    float originX{0.0f};
    float originY{0.0f};
    float originWidth{0.0f};
    float originHeight{0.0f};
    float originCornerRadius{0.0f};
    uint32_t iconWidth{0};
    uint32_t iconHeight{0};
};

/** Binds session identity and optionally activates an already-running scene. */
struct LCLMsgResolveLaunchPlaceholder {
    uint32_t homeSurfaceId{0};
    uint64_t launchToken{0};
    uint64_t appInstanceId{0};
    uint8_t reused{0};
};

struct LCLMsgCancelLaunchPlaceholder {
    uint32_t homeSurfaceId{0};
    uint64_t launchToken{0};
};

/** Compositor-owned launch lifecycle notification for the trusted HomeScreen. */
struct LCLMsgLaunchIconVisibility {
    uint64_t launchToken{0};
    char appId[64]{0};
    uint8_t visible{0};
};

/**
 * Sent by HomeScreen after committing the frame that applies a launch-icon
 * visibility notification. Socket ordering makes the preceding buffer attach
 * the exact handoff point for the compositor-owned icon proxy.
 */
struct LCLMsgLaunchIconVisibilityAck {
    uint64_t launchToken{0};
    char appId[64]{0};
};

/**
 * Creates a surface bound to a same-process parent surface. Position is in
 * parent-window logical coordinates; width/height are popup logical pixels.
 */
struct LCLMsgPopupSurfaceCreate {
    uint32_t surfaceId{0};
    uint32_t parentSurfaceId{0};
    LCLPopupRole role{LCLPopupRole::Transient};
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
};

/**
 * Creates a trusted cross-process child of an existing toplevel WindowGroup.
 * Bounds are parent-group logical coordinates.  Follow flags let one immutable
 * WM surface track the parent without per-frame geometry IPC.
 */
struct LCLMsgAttachedSurfaceCreate {
    uint32_t surfaceId{0};
    uint32_t targetWindowId{0};
    LCLAttachedSurfaceRole role{LCLAttachedSurfaceRole::Adornment};
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
    uint8_t followParentWidth{0};
    uint8_t followParentHeight{0};
    uint8_t acceptsInput{0};
};

static_assert(sizeof(LCLMsgAttachedSurfaceCreate) == 31);

struct LCLMsgSurfaceDestroy {
    uint32_t surfaceId{0};
};

struct LCLMsgConfigureBounds {
    uint32_t surfaceId{0};
    uint64_t configureSerial{0};
    // All surfaces participating in one WindowGroup geometry transaction
    // receive the same generation. Zero is reserved for non-geometric chrome
    // refreshes which keep the currently presented group geometry.
    uint64_t geometryGeneration{0};
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
    // Logical capacity hint for a GPU backing allocation. Content remains
    // width x height and must fit inside this extent without being scaled.
    float backingWidth{0.0f};
    float backingHeight{0.0f};
    uint8_t isFocused{0};
    char title[128]{0};
    float bufferScale{1.0f}; // v14 buffer mapping; bounds and input are logical.
    LCLConfigureResizeReason resizeReason{LCLConfigureResizeReason::Initial};
};

struct LCLMsgFramePresented {
    // Sent after the compositor returns from submitting the latest accepted
    // buffer commit to its platform presenter. This is not a physical-display
    // scanout completion signal.
    // Clients keep at most one frame in flight and coalesce newer damage until
    // this acknowledgement returns the presentation credit.
    uint32_t surfaceId{0};
    uint64_t configureSerial{0};
    uint64_t frameSerial{0};
    uint64_t geometryGeneration{0};
    // Monotonic compositor display-frame identity. Every surface promoted in
    // one atomic WindowGroup snapshot receives the same sequence.
    uint64_t displaySequence{0};
    uint64_t timestampNs{0};
    uint64_t refreshIntervalNs{0};
    // Zero unless the client enabled frame tracing. Every value shares a
    // monotonic steady-clock epoch. composeStartNs is the start of the output
    // compose pass that carried this layer; timestampNs is its present-submit
    // completion time.
    uint64_t clientFrameStartNs{0};
    uint64_t clientSubmitNs{0};
    uint64_t rasterStartNs{0};
    uint64_t rasterReadyNs{0};
    uint64_t composeStartNs{0};
};

enum class LCLFrameDiscardReason : uint32_t {
    Superseded = 1,
    InvalidFrame = 2,
    SurfaceClosed = 3,
};

struct LCLMsgFrameDiscarded {
    uint32_t surfaceId{0};
    uint64_t configureSerial{0};
    uint64_t frameSerial{0};
    uint64_t geometryGeneration{0};
    LCLFrameDiscardReason reason{LCLFrameDiscardReason::InvalidFrame};
};

struct LCLMsgSurfaceProducerGrant {
    uint32_t surfaceId{0};
    int32_t ownerPid{0};
    uint32_t flags{0};
    uint32_t reserved{0};
    uint64_t tokenHigh{0};
    uint64_t tokenLow{0};
};

struct LCLMsgAckResponse {
    uint32_t status{0}; // 0 = OK, >0 = error code
    char message[128]{0};
};

enum class LCLPointerSource : uint8_t {
    Mouse = 0,
    Touch = 1
};

enum class LCLInputEventType : uint32_t {
    KeyDown = 1,
    KeyUp = 2,
    PointerMotion = 3,
    PointerButton = 4,
    TextInput = 5,
    PointerScroll = 6,
    PointerCancel = 7
};

struct LCLMsgInputEvent {
    uint32_t surfaceId{0};
    uint32_t type{0};      // LCLInputEventType
    uint32_t key{0};       // Linux evdev keycode (e.g. KEY_A, KEY_ENTER)
    uint8_t  pressed{0};   // 1 = Down, 0 = Up
    uint8_t  modifiers{0}; // Bitmask: 0x01=Shift, 0x02=Ctrl, 0x04=Alt, 0x08=CapsLock, 0x10=Super
    uint8_t  source{0};    // 0 = Mouse, 1 = Touch (LCLPointerSource)
    uint32_t pointerId{0}; // Stable identity for the lifetime of a pointer stream
    uint32_t codepoint{0}; // Translated UTF-8 / ASCII codepoint (e.g. 'A', 'a', '1', '\n')
    float    x{0.0f};
    float    y{0.0f};
    float    deltaX{0.0f};
    float    deltaY{0.0f};
};

struct LCLMsgSetDecorationMode {
    uint32_t surfaceId{0};
    LCLDecorationMode mode{LCLDecorationMode::SSD};
};

struct LCLMsgSetEdgeToEdge {
    uint32_t surfaceId{0};
    uint8_t enabled{0};
};

struct LCLMsgSetWindowLayer {
    uint32_t surfaceId{0};
    LCLWindowLayer layer{LCLWindowLayer::Normal};
    uint8_t unfocusable{0}; // 1 = unfocusable (does not steal focus), 0 = focusable
};

struct LCLMsgSetReservedZone {
    uint32_t surfaceId{0};
    float top{0.0f};    // Logical inset from top of the output.
    float bottom{0.0f}; // Logical inset from bottom of the output.
    float left{0.0f};
    float right{0.0f};
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

struct LCLMsgRequestManagedWindowAction {
    uint32_t targetWindowId{0};
    LCLWindowAction action{LCLWindowAction::BeginDrag};
    // Parent-group local logical coordinates, used by BeginDrag only.
    float localX{0.0f};
    float localY{0.0f};
};

struct LCLMsgSetInsetBorder {
    uint32_t surfaceId{0};
    uint8_t enabled{1};
};

struct LCLMsgSetWindowCornerRadius {
    uint32_t surfaceId{0};
    float radius{0.0f};
};

struct LCLMsgSetWindowCornerStyle {
    uint32_t surfaceId{0};
    float radius{0.0f};
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
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
    LCLSceneVisibility visibility{LCLSceneVisibility::Visible};
    LCLDecorationMode decorationMode{LCLDecorationMode::SSD};
    uint8_t edgeToEdge{0};
    char appId[64]{0};
    char title[128]{0};
};

static_assert(sizeof(LCLMsgShellScene) == 246);

/** Followed by sceneCount LCLMsgShellScene records. */
struct LCLMsgShellStateSnapshot {
    uint64_t revision{0};
    uint32_t sceneCount{0};
    uint32_t seatId{0};
    uint32_t displayId{0};
    uint32_t workspaceId{0};
    uint64_t activeSceneId{0};
};

static_assert(sizeof(LCLMsgShellStateSnapshot) == 32);

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

static_assert(sizeof(LCLMsgShellStateDelta) == 278);

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

/** Explicit versioned little-endian codec entry points used by transport and tests. */
bool encodePacket(const LCLHeader& header, const void* nativePayload,
                  std::vector<uint8_t>& packet);
bool decodePacket(const uint8_t* packet, size_t packetSize, LCLHeader& header,
                  std::vector<uint8_t>& nativePayload);

} // namespace lcl::protocol
