#pragma once

#include "lcl-ui/core/rect.hpp"
#include "lcl-ui/core/canvas.hpp"
#include "lcl-ui/core/render_pass.hpp"
#include "lcl-ui/core/event_dispatcher.hpp"
#include "lcl-ui/core/motion.hpp"
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
using ResizeTransform = std::function<std::pair<uint32_t, uint32_t>(uint32_t width, uint32_t height)>;

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
    Widget* getRootWidget() const { return m_rootWidget.get(); }

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
    bool sendPointerMove(float x, float y);
    bool sendPointerDown(float x, float y, int button = 0);
    bool sendPointerUp(float x, float y, int button = 0);
    bool sendKeyDown(int keyCode, char32_t codepoint = 0, uint8_t modifiers = 0);
    bool sendKeyUp(int keyCode, uint8_t modifiers = 0);
    bool sendTextInput(const std::string& text);

    // Compositor IPC Client Connection & Loop
    bool connectCompositor(const std::string& socketPath = "/run/user/1000/lcl-compositor.sock");
    void runEventLoop();
    /** Process compositor messages and render at most one frame; useful for multi-surface shells. */
    bool tick();
    void resize(uint32_t width, uint32_t height);
    /** Set logical surface bounds before connectCompositor(). */
    void setInitialBounds(int32_t x, int32_t y, uint32_t width, uint32_t height);

    void setSurfaceId(uint32_t surfaceId) { if (!m_ipcConnected && surfaceId > 0) m_surfaceId = surfaceId; }
    uint32_t getSurfaceId() const { return m_surfaceId; }
    /** Canonical session/catalog application identity, supplied before connection. */
    void setAppId(std::string appId) { if (!m_ipcConnected) m_appId = std::move(appId); }
    /** Request compositor-owned policy for a trusted system surface. */
    void setSystemSurfaceKind(lcl::protocol::LCLSystemSurfaceKind kind) {
        if (!m_ipcConnected) m_systemSurfaceKind = kind;
    }
    /** Disable widget-event dispatch for visual-only surfaces such as shell panels. */
    void setInputEnabled(bool enabled) { m_inputEnabled = enabled; }
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
    int hitCsdControl(float x, float y) const noexcept;
    bool sendProtocolMessage(lcl::protocol::LCLOpcode opcode, const void* payload,
                             uint32_t payloadSize, int passedFd = -1);

    uint32_t m_width;
    uint32_t m_height;
    // Public layout/input coordinates remain logical. SHM is rasterized at this DPR.
    float m_bufferScale{1.0f};
    std::string m_title;

    std::unique_ptr<Widget> m_rootWidget;
    RenderPass m_renderPass;
    EventDispatcher m_dispatcher;
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
    uint32_t m_nextRequestId{1};
    uint64_t m_configureSerial{1};
    uint64_t m_pendingConfigureSerial{1};
    bool m_inputEnabled{true};
    int32_t m_initialX{80};
    int32_t m_initialY{60};

    bool m_initialized{false};
    bool m_firstFrame{true};
    bool m_shmNeedsAttach{true};
    bool m_effectGraphActive{false};
    std::vector<uint8_t> m_lastEffectGraphPayload;
    uint32_t m_pendingResizeWidth{0};
    uint32_t m_pendingResizeHeight{0};
    bool m_hasPendingResize{false};
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

    bool m_csdTitlebarEnabled{false};
    float m_csdTitlebarHeight{32.0f};
    float m_csdControlLeft{10.0f};
    float m_csdControlTop{8.0f};
    float m_csdControlSize{16.0f};
    float m_csdControlGap{6.0f};
    int m_csdPressedControl{-1};
    lcl::protocol::LCLDecorationMode m_requestedDecorationMode{lcl::protocol::LCLDecorationMode::None};
    bool m_hasRequestedDecorationMode{false};
    float m_requestedCornerRadius{0.0f};
    float m_requestedCornerRoundness{2.0f};
    bool m_hasRequestedCornerRadius{false};
};

} // namespace lcl::ui
