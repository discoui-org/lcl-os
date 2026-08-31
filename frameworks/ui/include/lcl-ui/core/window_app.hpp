#pragma once

#include "lcl-graphics/geometry.hpp"
#include "lcl-graphics/canvas.hpp"
#include "lcl-ui/core/render_pass.hpp"
#include "lcl-ui/core/event_dispatcher.hpp"
#include "lcl-ui/core/layout_environment.hpp"
#include "lcl-ui/core/motion.hpp"
#include "lcl-ui/core/transient_controller.hpp"
#include "system/ipc/lcl_protocol.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-theme/theme.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <utility>
#include <unordered_map>

namespace lcl::ui {

class RasterServiceClient;

using RawKeyCallback = std::function<bool(const KeyEvent&)>;
using RawPointerCallback = std::function<bool(const PointerEvent&)>;
using RawTextInputCallback = std::function<bool(const TextInputEvent&)>;
using IpcMessageCallback = std::function<void(const lcl::protocol::LCLHeader&, const std::vector<uint8_t>&)>;
using ResizeCallback = std::function<void(float width, float height)>;
using LayoutEnvironmentChangedCallback = std::function<void(const LayoutEnvironment&)>;
using FrameCallback = std::function<void()>;
using HostedSurfaceHandle = uint64_t;

/** Compositor-owned interactive resize grid in logical content pixels. */
struct WindowResizeConstraints {
    float baseWidth{0.0f};
    float baseHeight{0.0f};
    float widthIncrement{0.0f};
    float heightIncrement{0.0f};
};

class WindowApp {
public:
    WindowApp(std::unique_ptr<graphics::Canvas> canvas, float width, float height,
              const std::string& title = "lcl-ui Application");
    ~WindowApp();

    WindowApp(const WindowApp&) = delete;
    WindowApp& operator=(const WindowApp&) = delete;

    float getWidth() const { return m_width; }
    float getHeight() const { return m_height; }
    /** Current safe-area-aware logical environment for adaptive layouts. */
    const LayoutEnvironment& getLayoutEnvironment() const noexcept {
        return m_layoutEnvironment;
    }
    void setSafeAreaInsets(LayoutInsets insets);
    void setLayoutSizeClassPolicy(LayoutSizeClassPolicy policy);
    const LayoutSizeClassPolicy& getLayoutSizeClassPolicy() const noexcept {
        return m_layoutSizeClassPolicy;
    }
    void setOnLayoutEnvironmentChanged(LayoutEnvironmentChangedCallback callback) {
        m_onLayoutEnvironmentChanged = std::move(callback);
    }
    /** Logical-pixel to shared-buffer-pixel ratio for this client surface. */
    float getBufferScale() const { return m_bufferScale; }
    uint32_t getPixelWidth() const;
    uint32_t getPixelHeight() const;
    const std::string& getTitle() const { return m_title; }
    const std::string& getAppId() const noexcept { return m_appId; }
    const std::string& getCompositorSocketPath() const noexcept {
        return m_compositorSocketPath;
    }
    bool isIpcConnected() const noexcept { return m_ipcConnected; }
    std::weak_ptr<uint8_t> getLifetimeToken() const noexcept {
        return m_lifetimeToken;
    }

    void setRootWidget(std::unique_ptr<Widget> root);
    Widget* getRootWidget() const { return m_rootWidget; }
    void setTheme(lcl::theme::Theme theme);
    const lcl::theme::Theme& getTheme() const noexcept {
        return m_themeContext.value();
    }
    TransientController& getTransientController() { return m_transients; }
    const TransientController& getTransientController() const { return m_transients; }
    TransientHandle registerLocalTransient(std::unique_ptr<Widget> widget,
                                           TransientOptions options = {});
    TransientHandle registerSurfaceTransient(uint32_t surfaceId,
                                             std::function<void()> destroySurface,
                                             TransientOptions options = {});
    TransientHandle registerSurfaceTransient(uint32_t surfaceId,
                                             TransientOptions options = {});
    bool removeTransient(TransientHandle handle);
    void clearTransients();

    /** Own and tick another generic surface from this app's event loop. */
    HostedSurfaceHandle hostSurface(std::unique_ptr<WindowApp> surface,
                                    std::function<void()> onClosed = {});
    bool removeHostedSurface(HostedSurfaceHandle handle);
    WindowApp* getHostedSurface(HostedSurfaceHandle handle) const noexcept;
    size_t hostedSurfaceCount() const noexcept { return m_hostedSurfaces.size(); }

    EventDispatcher& getDispatcher() { return m_dispatcher; }
    RenderPass& getRenderPass() { return m_renderPass; }
    graphics::Canvas& getCanvas() { return *m_canvas; }
    MotionCoordinator& getMotionCoordinator() { return m_motionCoordinator; }
    void setInteractionMotionTheme(InteractionMotionTheme theme) {
        m_motionCoordinator.setInteractionTheme(std::move(theme));
    }

    /** Capture model mutations and animate their presentation through one transaction. */
    void animate(const lcl::motion::Motion& motion,
                 AnimationTransactionOptions options,
                 const std::function<void()>& changes);
    void animate(const lcl::motion::Motion& motion,
                 const std::function<void()>& changes) {
        animate(motion, {}, changes);
    }
    /** Deterministic clock hook used by embedders and tests. */
    bool advanceAnimations(float dtSec);
    bool hasActiveAnimations() const noexcept;

    // Direct Window Raw Event Callbacks (bypasses/intercepts Widget tree if handled)
    void setOnRawKeyEvent(RawKeyCallback callback) { m_onRawKey = callback; }
    void setOnRawPointerEvent(RawPointerCallback callback) { m_onRawPointer = callback; }
    void setOnRawTextInputEvent(RawTextInputCallback callback) { m_onRawTextInput = callback; }

    // Native OS Input Dispatch Forwarders
    bool sendPointerMove(float x, float y, PointerSource source = PointerSource::Mouse,
                         uint32_t pointerId = 0);
    bool sendPointerDown(float x, float y, int button = 0,
                         PointerSource source = PointerSource::Mouse, uint32_t pointerId = 0);
    bool sendPointerUp(float x, float y, int button = 0,
                       PointerSource source = PointerSource::Mouse, uint32_t pointerId = 0);
    bool sendPointerCancel(float x, float y, PointerSource source = PointerSource::Mouse,
                           uint32_t pointerId = 0);
    bool sendPointerScroll(float x, float y, float deltaX, float deltaY,
                           PointerSource source = PointerSource::Mouse,
                           uint32_t pointerId = 0);
    bool sendKeyDown(int keyCode, char32_t codepoint = 0, uint8_t modifiers = 0);
    bool sendKeyUp(int keyCode, uint8_t modifiers = 0);
    bool sendTextInput(const std::string& text);

    // Compositor IPC Client Connection & Loop
    bool connectCompositor(const std::string& socketPath = "/Runtime/lcl-compositor.sock");
    void runEventLoop();
    /** Process compositor messages and render at most one frame; useful for multi-surface shells. */
    bool tick();
    void resize(float width, float height);
    /** Set logical surface bounds before connectCompositor(). */
    void setInitialBounds(float x, float y, float width, float height);
    /** Configure this WindowApp as a parent-bound popup before connecting. */
    void configurePopupSurface(uint32_t parentSurfaceId,
                               lcl::protocol::LCLPopupRole role,
                               float x, float y);
    bool isPopupSurface() const noexcept { return m_popupParentSurfaceId != 0; }
    /** Configure a trusted WM-owned child of another process' toplevel. */
    void configureAttachedSurface(uint32_t targetWindowId,
                                  lcl::protocol::LCLAttachedSurfaceRole role,
                                  float x, float y, float width, float height,
                                  bool followParentWidth,
                                  bool followParentHeight,
                                  bool acceptsInput);
    bool isAttachedSurface() const noexcept { return m_attachedWindowId != 0; }

    void setSurfaceId(uint32_t surfaceId) { if (!m_ipcConnected && surfaceId > 0) m_surfaceId = surfaceId; }
    uint32_t getSurfaceId() const { return m_surfaceId; }
    /** Canonical session/catalog application identity, supplied before connection. */
    void setAppId(std::string appId) { if (!m_ipcConnected) m_appId = std::move(appId); }
    /** Request compositor-owned policy for a trusted system surface. */
    void setSystemSurfaceKind(lcl::protocol::LCLSystemSurfaceKind kind) {
        if (!m_ipcConnected) m_systemSurfaceKind = kind;
    }
    /** Disable widget-event dispatch for visual-only surfaces such as shell panels. */
    void setInputEnabled(bool enabled);
    bool isInputEnabled() const { return m_inputEnabled; }
    void setOnIpcMessage(IpcMessageCallback callback) { m_onIpcMessage = std::move(callback); }
    /** Runs after a logical configure has allocated its new SHM buffer. */
    void setOnResize(ResizeCallback callback) { m_onResize = std::move(callback); }
    /** Declare the toplevel's interactive resize grid before connecting. */
    bool setResizeConstraints(WindowResizeConstraints constraints);
    /** Runs once per WindowApp event-loop tick before damage is rendered. */
    void setOnFrame(FrameCallback callback) { m_onFrame = std::move(callback); }
    void requestQuit() { m_running = false; }

    /** Ask the compositor to begin moving this window from a client-local point. */
    bool requestWindowDrag(float localX, float localY);
    /** Hide this window while keeping its client process and surface alive. */
    bool requestWindowMinimize();
    /** Expand this window to the compositor work area. */
    bool requestWindowMaximize();
    /** Restore this window from maximized or minimized state. */
    bool requestWindowRestore();
    /** Toggle between maximized and restored geometry. */
    bool requestWindowToggleMaximize();
    bool requestWindowClose();
    /** Request an action for the toplevel managed by this attached WM surface. */
    bool requestManagedWindowAction(lcl::protocol::LCLWindowAction action,
                                    float localX = 0.0f,
                                    float localY = 0.0f);
    /** Begin an immediate compositor-owned launch placeholder from HomeScreen. */
    bool beginLaunchPlaceholder(uint64_t launchToken, const std::string& appId,
                                const graphics::RectF& origin,
                                float cornerRadius,
                                uint32_t iconWidth,
                                uint32_t iconHeight,
                                const std::vector<uint32_t>& iconPixels);
    /** Bind session identity; reused instances are restored instead of spawned. */
    bool resolveLaunchPlaceholder(uint64_t launchToken, uint64_t appInstanceId,
                                  bool reused);
    bool cancelLaunchPlaceholder(uint64_t launchToken);
    bool setDecorationMode(lcl::protocol::LCLDecorationMode mode);
    /** Extend the surface material beneath compositor-owned system insets. */
    bool setEdgeToEdge(bool enabled);
    bool setWindowLayer(lcl::protocol::LCLWindowLayer layer, bool unfocusable = false);
    bool setReservedZone(float top, float bottom, float left = 0.0f, float right = 0.0f);
    /** Set the compositor-owned outer WindowGroup shape in logical pixels. */
    bool setWindowCornerStyle(float radius, float roundness = 2.0f);
    /** Compatibility shorthand that preserves the current WindowGroup roundness. */
    bool setWindowCornerRadius(float radius);
    void setExternalIpcSocket(int socketFd);

    // Frame Execution & Render Loop Pipeline
    void updateLayout();
    bool renderFrame();

    uint32_t* getPixelBuffer() { return m_pixelBuffer.data(); }

private:
    struct PrivateRenderState;

    void updateCanvasRenderTarget();
    void pollIPC();
    bool requestWindowAction(lcl::protocol::LCLWindowAction action,
                             float localX = 0.0f, float localY = 0.0f);
    bool requestSurfaceDestroy(uint32_t surfaceId);
    bool sendProtocolMessage(lcl::protocol::LCLOpcode opcode, const void* payload,
                             uint32_t payloadSize, int passedFd = -1);
    bool uploadImageResource(const graphics::ImageResourceView& resource);
    bool uploadExternalBufferResources(const Widget& widget);
    void invalidateRetainedWidgetCaches(Widget& widget) noexcept;
    void configureRetainedWidgetCaches(Widget& widget) noexcept;
    void logFrameTraceIfDue();
    void collectClosedHostedSurfaces();
    void updateLayoutEnvironment();

    struct HostedSurfaceEntry {
        HostedSurfaceHandle handle{0};
        std::unique_ptr<WindowApp> surface;
        std::function<void()> onClosed;
        bool pendingRemoval{false};
    };

    float m_width;
    float m_height;
    // Public layout/input coordinates remain logical. SHM is rasterized at this DPR.
    float m_bufferScale{1.0f};
    LayoutInsets m_safeAreaInsets{};
    LayoutSizeClassPolicy m_layoutSizeClassPolicy{};
    LayoutEnvironment m_layoutEnvironment{};
    std::string m_title;
    std::shared_ptr<uint8_t> m_lifetimeToken{std::make_shared<uint8_t>(0)};

    lcl::theme::ThemeContext m_themeContext;
    std::unique_ptr<Container> m_windowRoot;
    Widget* m_rootWidget{nullptr};
    RenderPass m_renderPass;
    EventDispatcher m_dispatcher;
    TransientController m_transients{m_dispatcher};
    MotionCoordinator m_motionCoordinator;
    // Opaque so the internal retained-render model never enters the public API.
    std::unique_ptr<PrivateRenderState> m_privateRenderState;
    std::unique_ptr<graphics::Canvas> m_canvas;
    // Logical frames are rasterized outside the compositor. This private
    // client owns only the producer grant and the raster-service connection;
    // widgets remain backend-neutral DisplayList producers.
    std::unique_ptr<RasterServiceClient> m_rasterClient;

    RawKeyCallback m_onRawKey{nullptr};
    RawPointerCallback m_onRawPointer{nullptr};
    RawTextInputCallback m_onRawTextInput{nullptr};
    IpcMessageCallback m_onIpcMessage{nullptr};
    ResizeCallback m_onResize{nullptr};
    LayoutEnvironmentChangedCallback m_onLayoutEnvironmentChanged{nullptr};
    FrameCallback m_onFrame{nullptr};
    WindowResizeConstraints m_resizeConstraints{};

    std::vector<uint32_t> m_pixelBuffer;
    int m_socketFd{-1};
    bool m_ipcConnected{false};
    bool m_ownsSocketFd{true};
    uint32_t m_surfaceId{1};
    std::string m_appId;
    std::string m_compositorSocketPath{"/Runtime/lcl-compositor.sock"};
    lcl::protocol::LCLSystemSurfaceKind m_systemSurfaceKind{lcl::protocol::LCLSystemSurfaceKind::None};
    uint32_t m_attachedWindowId{0};
    lcl::protocol::LCLAttachedSurfaceRole m_attachedRole{
        lcl::protocol::LCLAttachedSurfaceRole::Adornment};
    float m_attachedX{0.0f};
    float m_attachedY{0.0f};
    float m_attachedWidth{0.0f};
    float m_attachedHeight{0.0f};
    bool m_attachedFollowParentWidth{false};
    bool m_attachedFollowParentHeight{false};
    bool m_attachedAcceptsInput{false};
    uint32_t m_nextRequestId{1};
    uint64_t m_configureSerial{1};
    uint64_t m_pendingConfigureSerial{1};
    uint64_t m_geometryGeneration{0};
    float m_backingWidth{0.0f};
    float m_backingHeight{0.0f};
    // SurfaceCreate is only a request. Do not commit the provisional client
    // buffer until the compositor assigns the first configure serial.
    bool m_waitingForInitialConfigure{false};
    bool m_inputEnabled{true};
    float m_initialX{80.0f};
    float m_initialY{60.0f};
    uint32_t m_popupParentSurfaceId{0};
    lcl::protocol::LCLPopupRole m_popupRole{lcl::protocol::LCLPopupRole::Transient};
    float m_popupX{0.0f};
    float m_popupY{0.0f};

    bool m_initialized{false};
    bool m_firstFrame{true};
    bool m_effectGraphActive{false};
    std::vector<uint8_t> m_lastEffectGraphPayload;
    float m_pendingResizeWidth{0.0f};
    float m_pendingResizeHeight{0.0f};
    bool m_hasPendingResize{false};
    // Pointer resize and maximize/restore share rasterd presentation pacing.
    // Initial configure remains an immediate, content-sized transaction.
    bool m_atomicResizeFramePacing{false};
    // One compositor presentation credit applies to each rasterd layer frame.
    // This prevents clients from producing obsolete generations faster than
    // the display can present them.
    bool m_frameGateOpen{true};
    uint64_t m_lastPresentedTimestampNs{0};
    uint64_t m_refreshIntervalNs{0};
    uint64_t m_submittedConfigureSerial{0};
    uint64_t m_nextFrameSerial{1};
    uint64_t m_submittedFrameSerial{0};
    // Last raster frame known to be presented and therefore safe to patch.
    uint64_t m_retainedRasterFrameSerial{0};
    uint64_t m_submittedGeometryGeneration{0};
    uint64_t m_rasterConnectionGeneration{0};
    // A complete rasterd frame installs logical ScrollContent templates.
    // Property-only commits then move/replenish bounded rasterd tiles without
    // another client DisplayList.
    bool m_scrollTransformFastPathReady{false};
    bool m_retainedPresentationFastPathReady{false};
    bool m_externalBufferFastPathReady{false};
    std::unordered_map<uint64_t, uint64_t> m_uploadedImageRevisions;
    struct ExternalBufferUploadState {
        uint64_t contentRevision{0};
        std::function<void(int releaseFenceFd)> completeRelease{};
    };
    std::unordered_map<uint64_t, ExternalBufferUploadState>
        m_uploadedExternalBuffers;
    // A launch icon becomes compositor-visible only after the HomeScreen
    // buffer containing it has been committed on this same ordered socket.
    std::optional<lcl::protocol::LCLMsgLaunchIconVisibilityAck>
        m_pendingLaunchIconVisibilityAck;
    // Attached chrome actions are published only after the unpressed frame
    // generated by pointer-up has actually left presentation backpressure.
    std::optional<lcl::protocol::LCLMsgRequestManagedWindowAction>
        m_pendingManagedWindowAction;
    uint64_t m_pendingManagedActionAfterFrameSerial{0};
    std::chrono::steady_clock::time_point m_lastResizeApply{};
    bool m_running{false};
    bool m_surfaceEnded{false};
    bool m_tickingHostedSurfaces{false};
    std::vector<HostedSurfaceEntry> m_hostedSurfaces;
    HostedSurfaceHandle m_nextHostedSurfaceHandle{1};
    bool m_morphInputFrozen{false};
    std::chrono::steady_clock::time_point m_lastAnimationTick{};

    // Opt-in, aggregate diagnostics. Enabled with LCL_TRACE_FRAMES=1.
    bool m_frameTraceEnabled{false};
    bool m_layoutOverlayEnabled{false};
    uint64_t m_traceLayoutPasses{0};
    uint64_t m_traceRenderedFrames{0};
    uint64_t m_traceConfigureCount{0};
    uint64_t m_traceInteractiveConfigureCount{0};
    uint64_t m_traceTransitionConfigureCount{0};
    uint64_t m_tracePresentedFrames{0};
    uint64_t m_traceResizeApplies{0};
    uint64_t m_traceDamagePixels{0};
    uint64_t m_traceClearedBytes{0};
    uint64_t m_traceRenderCreates{0};
    uint64_t m_traceRenderContentUpdates{0};
    uint64_t m_traceRenderPropertyUpdates{0};
    uint64_t m_traceRenderRemovals{0};
    uint64_t m_traceScrollTransformTransactions{0};
    uint64_t m_traceRetainedPresentationTransactions{0};
    double m_traceLayoutMs{0.0};
    double m_tracePaintMs{0.0};
    double m_traceRenderTreeMs{0.0};
    double m_traceClearMs{0.0};
    double m_traceDrawMs{0.0};
    double m_traceRasterSubmitMs{0.0};
    std::vector<double> m_traceClientBuildSamplesMs;
    std::vector<double> m_traceRasterQueueSamplesMs;
    std::vector<double> m_traceRasterSamplesMs;
    std::vector<double> m_traceComposeQueueSamplesMs;
    std::vector<double> m_traceComposeSubmitSamplesMs;
    std::vector<double> m_traceEndToEndSubmitSamplesMs;
    std::chrono::steady_clock::time_point m_traceLastLog{};

    lcl::protocol::LCLDecorationMode m_requestedDecorationMode{lcl::protocol::LCLDecorationMode::None};
    bool m_hasRequestedDecorationMode{false};
    bool m_requestedEdgeToEdge{false};
    bool m_hasRequestedEdgeToEdge{false};
    float m_requestedCornerRadius{0.0f};
    float m_requestedCornerRoundness{2.0f};
    bool m_hasRequestedCornerRadius{false};
};

} // namespace lcl::ui
