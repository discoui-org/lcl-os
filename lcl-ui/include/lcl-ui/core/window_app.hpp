#pragma once

#include "lcl-ui/core/rect.hpp"
#include "lcl-ui/core/canvas.hpp"
#include "lcl-ui/core/render_pass.hpp"
#include "lcl-ui/core/event_dispatcher.hpp"
#include "core/ipc/lcl_protocol.hpp"
#include "lcl-ui/widgets/container.hpp"
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <chrono>

namespace lcl::ui {

using RawKeyCallback = std::function<bool(const KeyEvent&)>;
using RawPointerCallback = std::function<bool(const PointerEvent&)>;
using RawTextInputCallback = std::function<bool(const TextInputEvent&)>;
using IpcMessageCallback = std::function<void(const lcl::protocol::LCLHeader&, const std::vector<uint8_t>&)>;

class WindowApp {
public:
    WindowApp(uint32_t width, uint32_t height, const std::string& title = "lcl-ui Application");
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
    bool connectCompositor(const std::string& socketPath = "/tmp/lcl_compositor.sock");
    void runEventLoop();
    /** Process compositor messages and render at most one frame; useful for multi-surface shells. */
    bool tick();
    void resize(uint32_t width, uint32_t height);
    /** Set logical surface bounds before connectCompositor(). */
    void setInitialBounds(int32_t x, int32_t y, uint32_t width, uint32_t height);

    void setSurfaceId(uint32_t surfaceId) { if (!m_ipcConnected && surfaceId > 0) m_surfaceId = surfaceId; }
    uint32_t getSurfaceId() const { return m_surfaceId; }
    void setRole(lcl::protocol::LCLRole role) { if (!m_ipcConnected) m_role = role; }
    /** Disable widget-event dispatch for visual-only surfaces such as shell panels. */
    void setInputEnabled(bool enabled) { m_inputEnabled = enabled; }
    bool isInputEnabled() const { return m_inputEnabled; }
    void setOnIpcMessage(IpcMessageCallback callback) { m_onIpcMessage = std::move(callback); }

    bool requestWindowMove(float localX, float localY);
    bool requestWindowClose();
    bool setDecorationMode(lcl::protocol::LCLDecorationMode mode);
    bool setWindowLayer(lcl::protocol::LCLWindowLayer layer, bool unfocusable = false);
    bool setReservedZone(uint32_t top, uint32_t bottom, uint32_t left = 0, uint32_t right = 0);
    bool setWindowCornerRadius(float radiusPx);
    void setExternalIpcSocket(int socketFd);
    void setCsdTitlebarEnabled(bool enabled) { m_csdTitlebarEnabled = enabled; }
    void configureCsdTitlebar(float height, float closeLeft, float closeTop, float closeSize);

    // Frame Execution & Render Loop Pipeline
    void updateLayout();
    bool renderFrame();

    uint32_t* getPixelBuffer() { return m_shmPixels ? m_shmPixels : m_pixelBuffer.data(); }

private:
    void pollIPC();
    void allocateSHM(uint32_t width, uint32_t height);

    uint32_t m_width;
    uint32_t m_height;
    // Public layout/input coordinates remain logical. SHM is rasterized at this DPR.
    float m_bufferScale{1.0f};
    std::string m_title;

    std::unique_ptr<Widget> m_rootWidget;
    RenderPass m_renderPass;
    EventDispatcher m_dispatcher;
    std::unique_ptr<Canvas> m_canvas;

    RawKeyCallback m_onRawKey{nullptr};
    RawPointerCallback m_onRawPointer{nullptr};
    RawTextInputCallback m_onRawTextInput{nullptr};
    IpcMessageCallback m_onIpcMessage{nullptr};

    std::vector<uint32_t> m_pixelBuffer;
    int m_socketFd{-1};
    int m_shmFd{-1};
    size_t m_shmSize{0};
    uint32_t* m_shmPixels{nullptr};
    bool m_ipcConnected{false};
    bool m_ownsSocketFd{true};
    uint32_t m_surfaceId{1};
    lcl::protocol::LCLRole m_role{lcl::protocol::LCLRole::ClientApp};
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

    bool m_csdTitlebarEnabled{false};
    float m_csdTitlebarHeight{32.0f};
    float m_csdCloseLeft{10.0f};
    float m_csdCloseTop{8.0f};
    float m_csdCloseSize{16.0f};
};

} // namespace lcl::ui
