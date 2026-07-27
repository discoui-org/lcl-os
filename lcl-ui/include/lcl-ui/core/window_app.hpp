#pragma once

#include "lcl-ui/core/rect.hpp"
#include "lcl-ui/core/render_pass.hpp"
#include "lcl-ui/core/event_dispatcher.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "render/skia_renderer.hpp"
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace lcl::ui {

using RawKeyCallback = std::function<bool(const KeyEvent&)>;
using RawPointerCallback = std::function<bool(const PointerEvent&)>;
using RawTextInputCallback = std::function<bool(const TextInputEvent&)>;

class WindowApp {
public:
    WindowApp(uint32_t width, uint32_t height, const std::string& title = "lcl-ui Application");
    ~WindowApp();

    WindowApp(const WindowApp&) = delete;
    WindowApp& operator=(const WindowApp&) = delete;

    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    const std::string& getTitle() const { return m_title; }

    void setRootWidget(std::unique_ptr<Widget> root);
    Widget* getRootWidget() const { return m_rootWidget.get(); }

    EventDispatcher& getDispatcher() { return m_dispatcher; }
    RenderPass& getRenderPass() { return m_renderPass; }
    lcl::render::SkiaRenderer& getRenderer() { return m_renderer; }

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
    void resize(uint32_t width, uint32_t height);

    // Frame Execution & Render Loop Pipeline
    void updateLayout();
    bool renderFrame();

    uint32_t* getPixelBuffer() { return m_shmPixels ? m_shmPixels : m_pixelBuffer.data(); }

private:
    void pollIPC();
    void allocateSHM(uint32_t width, uint32_t height);

    uint32_t m_width;
    uint32_t m_height;
    std::string m_title;

    std::unique_ptr<Widget> m_rootWidget;
    RenderPass m_renderPass;
    EventDispatcher m_dispatcher;
    lcl::render::SkiaRenderer m_renderer;

    RawKeyCallback m_onRawKey{nullptr};
    RawPointerCallback m_onRawPointer{nullptr};
    RawTextInputCallback m_onRawTextInput{nullptr};

    std::vector<uint32_t> m_pixelBuffer;
    int m_socketFd{-1};
    int m_shmFd{-1};
    size_t m_shmSize{0};
    uint32_t* m_shmPixels{nullptr};
    bool m_ipcConnected{false};

    bool m_initialized{false};
    bool m_firstFrame{true};
    bool m_shmNeedsAttach{true};
    bool m_running{false};
};

} // namespace lcl::ui
