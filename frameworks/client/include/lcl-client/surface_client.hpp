#pragma once

#include "lcl-client/owned_fd.hpp"
#include "system/ipc/lcl_protocol.hpp"
#include "system/ipc/raster_protocol.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace lcl::client {

struct Rect {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
};

enum class SurfaceRole : uint8_t { Toplevel, Popup, Attached, System };

struct SurfaceOptions {
    SurfaceRole role{SurfaceRole::Toplevel};
    uint32_t surfaceId{1};
    std::string appId;
    std::string title;
    Rect bounds{80.0f, 60.0f, 800.0f, 600.0f};
    protocol::LCLSystemSurfaceKind systemKind{
        protocol::LCLSystemSurfaceKind::None};

    uint32_t parentSurfaceId{0};
    protocol::LCLPopupRole popupRole{protocol::LCLPopupRole::Transient};

    uint32_t targetWindowId{0};
    protocol::LCLAttachedSurfaceRole attachedRole{
        protocol::LCLAttachedSurfaceRole::Adornment};
    bool followParentWidth{false};
    bool followParentHeight{false};
    bool acceptsInput{false};

    float resizeBaseWidth{0.0f};
    float resizeBaseHeight{0.0f};
    float resizeWidthIncrement{0.0f};
    float resizeHeightIncrement{0.0f};
    uint64_t launchToken{0};
    uint64_t appInstanceId{0};
};

struct ConfigureEvent {
    uint32_t surfaceId{0};
    uint64_t configureSerial{0};
    uint64_t geometryGeneration{0};
    Rect bounds{};
    float backingWidth{0.0f};
    float backingHeight{0.0f};
    float bufferScale{1.0f};
    protocol::LCLConfigureResizeReason resizeReason{
        protocol::LCLConfigureResizeReason::Initial};
    bool focused{false};
    std::string title;
};

struct FocusEvent { bool focused{false}; };

struct InputEvent {
    protocol::LCLInputEventType type{protocol::LCLInputEventType::KeyDown};
    uint32_t key{0};
    bool pressed{false};
    uint8_t modifiers{0};
    protocol::LCLPointerSource source{protocol::LCLPointerSource::Mouse};
    uint32_t pointerId{0};
    char32_t codepoint{0};
    float x{0.0f};
    float y{0.0f};
    float deltaX{0.0f};
    float deltaY{0.0f};
};

struct FramePresentedEvent { protocol::LCLMsgFramePresented message{}; };
struct FrameDiscardedEvent {
    uint64_t configureSerial{0};
    uint64_t frameSerial{0};
    uint64_t geometryGeneration{0};
    raster_protocol::DiscardReason reason{
        raster_protocol::DiscardReason::InvalidFrame};
};
struct BufferReleasedEvent {
    uint64_t bufferId{0};
    uint64_t contentRevision{0};
    raster_protocol::ExternalBufferReleaseReason reason{
        raster_protocol::ExternalBufferReleaseReason::Superseded};
    OwnedFd releaseFence{};
};
struct SurfaceClosedEvent { uint32_t surfaceId{0}; };
struct RequestResultEvent {
    uint32_t requestId{0};
    uint32_t status{0};
    std::string message;
};
struct ProducerGrantEvent { raster_protocol::SurfaceGrant grant{}; };
struct RasterConnectionEvent { bool connected{false}; uint64_t generation{0}; };
struct LaunchIconVisibilityEvent {
    uint64_t launchToken{0};
    std::string appId;
    bool visible{false};
};

using Event = std::variant<ConfigureEvent, FocusEvent, InputEvent,
                           FramePresentedEvent, FrameDiscardedEvent,
                           BufferReleasedEvent, SurfaceClosedEvent,
                           RequestResultEvent, ProducerGrantEvent,
                           RasterConnectionEvent, LaunchIconVisibilityEvent>;

struct DmaBufFrame {
    uint64_t bufferId{0};
    uint64_t contentRevision{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride{0};
    uint32_t format{0};
    uint64_t modifier{~uint64_t{0}};
    Rect damage{};
    bool opaque{false};
    OwnedFd buffer{};
    OwnedFd acquireFence{};
};

struct PlatformNativeFrame {
    uint64_t bufferId{0};
    uint64_t contentRevision{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride{0};
    uint32_t format{0};
    Rect damage{};
    bool opaque{false};
    /** Must synchronously queue exactly one native handle on sidebandFd. */
    std::function<bool(int sidebandFd)> writeHandle;
    OwnedFd acquireFence{};
};

/**
 * Toolkit-independent owner of one LCL surface and its raster producer link.
 * It never runs an event loop; embedders poll compositorFd()/rasterFd() and
 * call non-blocking dispatch().
 */
class SurfaceClient {
public:
    SurfaceClient();
    ~SurfaceClient();
    SurfaceClient(const SurfaceClient&) = delete;
    SurfaceClient& operator=(const SurfaceClient&) = delete;
    SurfaceClient(SurfaceClient&&) noexcept;
    SurfaceClient& operator=(SurfaceClient&&) noexcept;

    bool connect(const SurfaceOptions& options,
                 std::string compositorSocket = "/Runtime/lcl-compositor.sock",
                 std::string rasterSocket = "/Runtime/lcl-raster.sock");
    void disconnect() noexcept;
    bool connected() const noexcept;
    int compositorFd() const noexcept;
    int rasterFd() const noexcept;
    uint64_t rasterConnectionGeneration() const noexcept;
    uint32_t surfaceId() const noexcept;
    bool hasConfigure() const noexcept;
    const ConfigureEvent& configure() const noexcept;
    bool hasFrameCredit() const noexcept;

    /** Drains currently available packets only; never blocks. */
    std::vector<Event> dispatch();

    bool submitFrame(DmaBufFrame&& frame);
    bool submitFrame(PlatformNativeFrame&& frame);

    bool requestWindowAction(protocol::LCLWindowAction action,
                             float localX = 0.0f, float localY = 0.0f);
    bool requestManagedWindowAction(protocol::LCLWindowAction action,
                                    float localX = 0.0f, float localY = 0.0f);
    bool requestSurfaceClose(uint32_t surfaceId = 0);
    bool setDecorationMode(protocol::LCLDecorationMode mode);
    bool setEdgeToEdge(bool enabled);
    bool setWindowLayer(protocol::LCLWindowLayer layer,
                        bool unfocusable = false);
    bool setReservedZone(float top, float bottom, float left = 0.0f,
                         float right = 0.0f);
    bool setInsetBorder(bool enabled);
    bool setWindowCornerStyle(float radius, float roundness = 2.0f);
    bool setEffectGraph(std::span<const protocol::EffectRegion> regions,
                        std::span<const protocol::FilterOp> filters);
    bool clearEffectGraph();
    bool beginLaunchPlaceholder(uint64_t launchToken, std::string_view appId,
                                Rect origin, float cornerRadius,
                                uint32_t iconWidth, uint32_t iconHeight,
                                std::span<const uint32_t> argbPixels);
    bool resolveLaunchPlaceholder(uint64_t launchToken, uint64_t appInstanceId,
                                  bool reused);
    bool cancelLaunchPlaceholder(uint64_t launchToken);
    bool acknowledgeLaunchIconVisibility(uint64_t launchToken,
                                         std::string_view appId);

    /** Temporary adapter used by lcl-ui's retained DisplayList producer. */
    bool commitLegacyRetainedFrame(
        raster_protocol::CommitTransaction transaction,
        std::span<const raster_protocol::NodeMutation> mutations,
        std::span<const uint8_t> displayList);

private:
    class Impl;
    Impl* m_impl{nullptr};
};

} // namespace lcl::client
