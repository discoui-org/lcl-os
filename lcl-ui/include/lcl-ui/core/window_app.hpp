#pragma once

#include "lcl-ui/core/rect.hpp"
#include "lcl-ui/core/canvas.hpp"
#include "lcl-ui/core/render_pass.hpp"
#include "lcl-ui/core/event_dispatcher.hpp"
#include "lcl-ui/core/motion.hpp"
#include "lcl-ui/core/transient_controller.hpp"
#include "core/ipc/lcl_protocol.hpp"
#include "lcl-ui/widgets/container.hpp"
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <utility>

namespace lcl::ui {

using RawKeyCallback = std::function<bool(const KeyEvent&)>;
using RawPointerCallback = std::function<bool(const PointerEvent&)>;
using RawTextInputCallback = std::function<bool(const TextInputEvent&)>;
using IpcMessageCallback = std::function<void(const lcl::protocol::LCLHeader&, const std::vector<uint8_t>&)>;
using ResizeCallback = std::function<void(uint32_t width, uint32_t height)>;
using FrameCallback = std::function<void()>;
using ResizeTransform = std::function<std::pair<uint32_t, uint32_t>(
    uint32_t width, uint32_t height, lcl::protocol::LCLConfigureResizeReason reason)>;

class WindowApp {
public:
    WindowApp(std::unique_ptr<Canvas> canvas, uint32_t width, uint32_t height,
              const std::string& title = "lcl-ui Application");
    ~WindowApp();

    WindowApp(const WindowApp&) = delete;
    WindowApp& operator=(const WindowApp&) = delete;

    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    /** Logical-pixel to shared-buffer-pixel ratio for this client surface. */
    float getBufferScale() const { return m_bufferScale; }
    uint32_t getPixelWidth() const;
    uint32_t getPixelHeight() const;
    const std::string& getTitle() const { return m_title; }

    void setRootWidget(std::unique_ptr<Widget> root);
    Widget* getRootWidget() const { return m_rootWidget; }
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

    EventDispatcher& getDispatcher() { return m_dispatcher; }
    RenderPass& getRenderPass() { return m_renderPass; }
    Canvas& getCanvas() { return *m_canvas; }
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
    bool sendPointerScroll(float x, float y, float deltaX, float deltaY, PointerSource source = PointerSource::Mouse);
    bool sendKeyDown(int keyCode, char32_t codepoint = 0, uint8_t modifiers = 0);
    bool sendKeyUp(int keyCode, uint8_t modifiers = 0);
    bool sendTextInput(const std::string& text);

    // Compositor IPC Client Connection & Loop
    bool connectCompositor(const std::string& socketPath = "/Runtime/lcl-compositor.sock");
    void runEventLoop();
    /** Process compositor messages and render at most one frame; useful for multi-surface shells. */
    bool tick();
    void resize(uint32_t width, uint32_t height);
    /** Set logical surface bounds before connectCompositor(). */
    void setInitialBounds(int32_t x, int32_t y, uint32_t width, uint32_t height);
    /** Configure this WindowApp as a parent-bound popup before connecting. */
    void configurePopupSurface(uint32_t parentSurfaceId,
                               lcl::protocol::LCLPopupRole role,
                               int32_t x, int32_t y);
    bool isPopupSurface() const noexcept { return m_popupParentSurfaceId != 0; }

    void setSurfaceId(uint32_t surfaceId) { if (!m_ipcConnected && surfaceId > 0) m_surfaceId = surfaceId; }
    uint32_t getSurfaceId() const { return m_surfaceId; }
    /** Canonical session/catalog application identity, supplied before connection. */
    void setAppId(std::string appId) { if (!m_ipcConnected) m_appId = std::move(appId); }
    /** Request compositor-owned policy for a trusted system surface. */
    void setSystemSurfaceKind(lcl::protocol::LCLSystemSurfaceKind kind) {
        if (!m_ipcConnected) m_systemSurfaceKind = kind;
    }
    /** Choose direct live resize or compositor-retained-buffer morph before connect. */
    void setResizePresentationMode(lcl::protocol::LCLResizePresentationMode mode) {
        if (!m_ipcConnected) m_resizePresentationMode = mode;
    }
    lcl::protocol::LCLResizePresentationMode getResizePresentationMode() const {
        return m_resizePresentationMode;
    }
    /** Disable widget-event dispatch for visual-only surfaces such as shell panels. */
    void setInputEnabled(bool enabled);
    bool isInputEnabled() const { return m_inputEnabled; }
    void setOnIpcMessage(IpcMessageCallback callback) { m_onIpcMessage = std::move(callback); }
    /** Runs after a logical configure has allocated its new SHM buffer. */
    void setOnResize(ResizeCallback callback) { m_onResize = std::move(callback); }
    /** Coalesces an incoming logical configure before WindowApp reallocates SHM. */
    void setResizeTransform(ResizeTransform transform) { m_resizeTransform = std::move(transform); }
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
    bool setDecorationMode(lcl::protocol::LCLDecorationMode mode);
    /** Extend the surface material beneath compositor-owned system insets. */
    bool setEdgeToEdge(bool enabled);
    bool setWindowLayer(lcl::protocol::LCLWindowLayer layer, bool unfocusable = false);
    bool setReservedZone(uint32_t top, uint32_t bottom, uint32_t left = 0, uint32_t right = 0);
    /** Set the compositor-owned outer WindowGroup shape in logical pixels. */
    bool setWindowCornerStyle(float radiusPx, float roundness = 2.0f);
    /** Compatibility shorthand that preserves the current WindowGroup roundness. */
    bool setWindowCornerRadius(float radiusPx);
    void setExternalIpcSocket(int socketFd);
    void setCsdTitlebarEnabled(bool enabled) {
        m_csdTitlebarEnabled = enabled;
        if (!enabled) m_csdPressedControl = -1;
    }
    /**
     * Enable default CSD chrome behavior for the shared three-control layout.
     * Custom CSD controls can leave this disabled and call the request methods
     * above directly from any widget or user-area gesture.
     */
    void configureCsdTitlebar(float height, float controlLeft, float controlTop,
                              float controlSize, float controlGap = 6.0f);

    // Frame Execution & Render Loop Pipeline
    void updateLayout();
    bool renderFrame();

    uint32_t* getPixelBuffer() { return m_shmPixels ? m_shmPixels : m_pixelBuffer.data(); }

private:
    void pollIPC();
    void allocateSHM(uint32_t width, uint32_t height);
    void startMorphCrossfade(std::vector<uint32_t> snapshot,
                             uint32_t pixelWidth, uint32_t pixelHeight,
                             const lcl::motion::Motion& motion);
    bool advanceMorphCrossfade(float dtSec);
    void blendMorphSnapshot();
    void clearMorphCrossfade();
    bool requestWindowAction(lcl::protocol::LCLWindowAction action,
                             float localX = 0.0f, float localY = 0.0f);
    bool requestSurfaceDestroy(uint32_t surfaceId);
    int hitCsdControl(float x, float y) const noexcept;
    bool sendProtocolMessage(lcl::protocol::LCLOpcode opcode, const void* payload,
                             uint32_t payloadSize, int passedFd = -1);
    void logFrameTraceIfDue();

    uint32_t m_width;
    uint32_t m_height;
    // Public layout/input coordinates remain logical. SHM is rasterized at this DPR.
    float m_bufferScale{1.0f};
    std::string m_title;

    std::unique_ptr<Container> m_windowRoot;
    Widget* m_rootWidget{nullptr};
    RenderPass m_renderPass;
    EventDispatcher m_dispatcher;
    TransientController m_transients{m_dispatcher};
    MotionCoordinator m_motionCoordinator;
    std::unique_ptr<Canvas> m_canvas;

    RawKeyCallback m_onRawKey{nullptr};
    RawPointerCallback m_onRawPointer{nullptr};
    RawTextInputCallback m_onRawTextInput{nullptr};
    IpcMessageCallback m_onIpcMessage{nullptr};
    ResizeCallback m_onResize{nullptr};
    FrameCallback m_onFrame{nullptr};
    ResizeTransform m_resizeTransform{nullptr};

    std::vector<uint32_t> m_pixelBuffer;
    int m_socketFd{-1};
    int m_shmFd{-1};
    size_t m_shmSize{0};
    uint32_t* m_shmPixels{nullptr};
    bool m_ipcConnected{false};
    bool m_ownsSocketFd{true};
    uint32_t m_surfaceId{1};
    std::string m_appId;
    lcl::protocol::LCLSystemSurfaceKind m_systemSurfaceKind{lcl::protocol::LCLSystemSurfaceKind::None};
    lcl::protocol::LCLResizePresentationMode m_resizePresentationMode{
        lcl::protocol::LCLResizePresentationMode::CompositorMorph};
    uint32_t m_nextRequestId{1};
    uint64_t m_configureSerial{1};
    uint64_t m_pendingConfigureSerial{1};
    uint32_t m_backingWidth{0};
    uint32_t m_backingHeight{0};
    // SurfaceCreate is only a request. Do not commit the provisional client
    // buffer until the compositor assigns the first configure serial.
    bool m_waitingForInitialConfigure{false};
    bool m_inputEnabled{true};
    int32_t m_initialX{80};
    int32_t m_initialY{60};
    uint32_t m_popupParentSurfaceId{0};
    lcl::protocol::LCLPopupRole m_popupRole{lcl::protocol::LCLPopupRole::Transient};
    int32_t m_popupX{0};
    int32_t m_popupY{0};

    bool m_initialized{false};
    bool m_firstFrame{true};
    bool m_shmNeedsAttach{true};
    bool m_effectGraphActive{false};
    std::vector<uint8_t> m_lastEffectGraphPayload;
    uint32_t m_pendingResizeWidth{0};
    uint32_t m_pendingResizeHeight{0};
    bool m_hasPendingResize{false};
    // Both pointer resize and maximize/restore are frame-paced in Live mode.
    // Initial configure remains an immediate, content-sized transaction.
    bool m_liveResizeFramePacing{false};
    bool m_liveFrameGateOpen{true};
    uint64_t m_lastPresentedTimestampNs{0};
    uint64_t m_refreshIntervalNs{0};
    uint32_t m_liveSubmittedBufferId{0};
    std::chrono::steady_clock::time_point m_lastResizeApply{};
    bool m_running{false};
    bool m_morphInputFrozen{false};
    lcl::motion::AnimationEngine m_morphBlendEngine;
    lcl::motion::ChannelId m_morphBlendChannel{0};
    std::vector<uint32_t> m_morphSnapshotPixels;
    uint32_t m_morphSnapshotWidth{0};
    uint32_t m_morphSnapshotHeight{0};
    float m_morphBlendProgress{1.0f};
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
    uint64_t m_traceDmaBufAttaches{0};
    uint64_t m_traceDmaBufPoolBlocks{0};
    uint64_t m_traceRejectedLiveFrames{0};
    uint64_t m_traceResizeApplies{0};
    uint64_t m_traceShmAllocations{0};
    uint64_t m_traceDamagePixels{0};
    uint64_t m_traceClearedBytes{0};
    uint64_t m_traceCopiedBytes{0};
    double m_traceLayoutMs{0.0};
    double m_tracePaintMs{0.0};
    double m_traceClearMs{0.0};
    double m_traceDrawMs{0.0};
    double m_traceCopyMs{0.0};
    double m_traceAttachMs{0.0};
    double m_traceShmMs{0.0};
    std::chrono::steady_clock::time_point m_traceLastLog{};

    bool m_csdTitlebarEnabled{false};
    float m_csdTitlebarHeight{32.0f};
    float m_csdControlLeft{10.0f};
    float m_csdControlTop{8.0f};
    float m_csdControlSize{16.0f};
    float m_csdControlGap{6.0f};
    int m_csdPressedControl{-1};
    lcl::protocol::LCLDecorationMode m_requestedDecorationMode{lcl::protocol::LCLDecorationMode::None};
    bool m_hasRequestedDecorationMode{false};
    bool m_requestedEdgeToEdge{false};
    bool m_hasRequestedEdgeToEdge{false};
    float m_requestedCornerRadius{0.0f};
    float m_requestedCornerRoundness{2.0f};
    bool m_hasRequestedCornerRadius{false};
};

} // namespace lcl::ui
