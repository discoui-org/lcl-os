#include "lcl-ui/core/window_app.hpp"
#include "core/ipc/lcl_protocol.hpp"
#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <cstring>
#include <signal.h>
#include <atomic>
#include <thread>
#include <chrono>

namespace lcl::ui {

static std::atomic<bool> g_appSignalReceived{false};

static void setupAppSignalHandlers() {
    static bool handlersSet = false;
    if (handlersSet) return;
    handlersSet = true;

    struct sigaction sa{};
    sa.sa_handler = [](int sig) {
        (void)sig;
        g_appSignalReceived = true;
    };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
}

WindowApp::WindowApp(uint32_t width, uint32_t height, const std::string& title)
    : m_width(width), m_height(height), m_title(title) {
    setupAppSignalHandlers();
    m_pixelBuffer.resize(width * height, 0xFF000000);
    m_initialized = m_renderer.initialize(width, height, nullptr, m_pixelBuffer.data());

    auto defaultRoot = std::make_unique<Container>();
    defaultRoot->getYogaNode().setWidth(static_cast<float>(width));
    defaultRoot->getYogaNode().setHeight(static_cast<float>(height));
    setRootWidget(std::move(defaultRoot));
}

WindowApp::~WindowApp() {
    m_running = false;
    if (m_shmPixels) {
        munmap(m_shmPixels, m_shmSize);
        m_shmPixels = nullptr;
    }
    if (m_shmFd >= 0) {
        close(m_shmFd);
        m_shmFd = -1;
    }
    if (m_socketFd >= 0) {
        close(m_socketFd);
        m_socketFd = -1;
    }
}

void WindowApp::setRootWidget(std::unique_ptr<Widget> root) {
    if (!root) return;
    m_rootWidget = std::move(root);
    m_rootWidget->setRenderPass(&m_renderPass);
    m_rootWidget->markDirty();
}

void WindowApp::allocateSHM(uint32_t width, uint32_t height) {
    if (m_shmPixels) {
        munmap(m_shmPixels, m_shmSize);
        m_shmPixels = nullptr;
    }
    if (m_shmFd >= 0) {
        close(m_shmFd);
        m_shmFd = -1;
    }

    m_shmSize = static_cast<size_t>(width) * height * 4;
    m_pixelBuffer.resize(width * height, 0xFF14161D);

    m_shmFd = memfd_create("lcl_ui_app_shm", MFD_CLOEXEC);
    if (m_shmFd >= 0) {
        ftruncate(m_shmFd, m_shmSize);
        m_shmPixels = reinterpret_cast<uint32_t*>(
            mmap(nullptr, m_shmSize, PROT_READ | PROT_WRITE, MAP_SHARED, m_shmFd, 0));
        if (m_shmPixels == MAP_FAILED) {
            m_shmPixels = nullptr;
        } else {
            std::fill_n(m_shmPixels, width * height, 0xFF14161D);
        }
    }

    m_renderer.setTargetPixels(m_pixelBuffer.data(), width, height);
    m_renderer.initialize(width, height, nullptr, m_pixelBuffer.data());
    m_shmNeedsAttach = true;
}

bool WindowApp::connectCompositor(const std::string& socketPath) {
    for (int i = 0; i < 50; ++i) {
        m_socketFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (m_socketFd >= 0) {
            struct sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);
            if (connect(m_socketFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
                break;
            }
            close(m_socketFd);
            m_socketFd = -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (m_socketFd < 0) {
        std::cerr << "[lcl-ui ERROR] Could not connect to compositor IPC socket: " << socketPath << "\n";
        return false;
    }

    // Set non-blocking socket reads
    int flags = fcntl(m_socketFd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(m_socketFd, F_SETFL, flags | O_NONBLOCK);
    }

    // 1. Register role as CLIENT_APP
    lcl::protocol::LCLHeader regHeader{};
    regHeader.opcode = lcl::protocol::LCLOpcode::RegisterRole;
    regHeader.payloadSize = sizeof(lcl::protocol::LCLMsgRegisterRole);

    lcl::protocol::LCLMsgRegisterRole regMsg{};
    regMsg.role = lcl::protocol::LCLRole::ClientApp;
    std::strncpy(regMsg.clientName, m_title.c_str(), sizeof(regMsg.clientName) - 1);
    lcl::protocol::sendMsgWithFd(m_socketFd, regHeader, &regMsg);

    // 2. Request Surface Creation
    lcl::protocol::LCLHeader surfHeader{};
    surfHeader.opcode = lcl::protocol::LCLOpcode::SurfaceCreate;
    surfHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSurfaceCreate);

    lcl::protocol::LCLMsgSurfaceCreate surfMsg{};
    surfMsg.surfaceId = 1;
    surfMsg.x = 80;
    surfMsg.y = 60;
    surfMsg.width = m_width;
    surfMsg.height = m_height;
    std::strncpy(surfMsg.title, m_title.c_str(), sizeof(surfMsg.title) - 1);
    lcl::protocol::sendMsgWithFd(m_socketFd, surfHeader, &surfMsg);

    // 3. Allocate SHM memory buffer and attach to compositor
    allocateSHM(m_width, m_height);

    m_ipcConnected = true;
    if (m_rootWidget) {
        m_rootWidget->markDirty();
    }
    m_firstFrame = true;
    renderFrame();

    std::cout << "[lcl-ui] Connected to Compositor IPC socket successfully (" << m_title << ").\n";
    return true;
}

void WindowApp::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;

    if (m_rootWidget) {
        m_rootWidget->getYogaNode().setWidth(static_cast<float>(width));
        m_rootWidget->getYogaNode().setHeight(static_cast<float>(height));
        m_rootWidget->markDirty();
    }

    if (m_ipcConnected) {
        allocateSHM(width, height);

        // Pre-render layout into m_pixelBuffer & copy to m_shmPixels BEFORE committing buffer to Compositor!
        renderFrame();

        if (m_socketFd >= 0 && m_shmFd >= 0) {
            lcl::protocol::LCLHeader attachHeader{};
            attachHeader.opcode = lcl::protocol::LCLOpcode::AttachBuffer;
            attachHeader.payloadSize = sizeof(lcl::protocol::LCLMsgAttachBuffer);

            lcl::protocol::LCLMsgAttachBuffer attachMsg{};
            attachMsg.surfaceId = 1;
            attachMsg.width = width;
            attachMsg.height = height;
            attachMsg.stride = width * 4;
            attachMsg.format = 1;

            lcl::protocol::sendMsgWithFd(m_socketFd, attachHeader, &attachMsg, m_shmFd);
            m_shmNeedsAttach = false;
        }
    }
}

void WindowApp::pollIPC() {
    if (!m_ipcConnected || m_socketFd < 0) return;

    uint32_t latestWidth = 0;
    uint32_t latestHeight = 0;
    bool pendingResize = false;

    while (true) {
        lcl::protocol::LCLHeader header{};
        std::vector<uint8_t> payload;
        int receivedFd = -1;
        if (lcl::protocol::recvMsgWithFd(m_socketFd, header, payload, receivedFd)) {
            if (header.opcode == lcl::protocol::LCLOpcode::InputEvent &&
                payload.size() >= sizeof(lcl::protocol::LCLMsgInputEvent)) {
                auto* inputMsg = reinterpret_cast<const lcl::protocol::LCLMsgInputEvent*>(payload.data());
                if (inputMsg->type == 1) { // KeyboardKey
                    if (inputMsg->pressed) {
                        sendKeyDown(inputMsg->key, 0, inputMsg->modifiers);
                    } else {
                        sendKeyUp(inputMsg->key, inputMsg->modifiers);
                    }
                } else if (inputMsg->type == 2) { // PointerMotion
                    sendPointerMove(inputMsg->x, inputMsg->y);
                } else if (inputMsg->type == 3) { // PointerButton
                    if (inputMsg->pressed) {
                        sendPointerDown(inputMsg->x, inputMsg->y, inputMsg->key);
                    } else {
                        sendPointerUp(inputMsg->x, inputMsg->y, inputMsg->key);
                    }
                }
            } else if (header.opcode == lcl::protocol::LCLOpcode::ConfigureBounds &&
                       payload.size() >= sizeof(lcl::protocol::LCLMsgConfigureBounds)) {
                auto* cfg = reinterpret_cast<const lcl::protocol::LCLMsgConfigureBounds*>(payload.data());
                if (cfg->width > 0 && cfg->height > 0) {
                    latestWidth = cfg->width;
                    latestHeight = cfg->height;
                    pendingResize = true;
                }
            } else if (header.opcode == lcl::protocol::LCLOpcode::SurfaceDestroy) {
                m_running = false;
            }
        } else {
            break;
        }
    }

    // Event Coalescing: Execute resize ONCE for the most recent dimensions in socket queue
    if (pendingResize && (latestWidth != m_width || latestHeight != m_height)) {
        resize(latestWidth, latestHeight);
    }
}

void WindowApp::runEventLoop() {
    m_running = true;
    while (m_running && !g_appSignalReceived.load()) {
        pollIPC();
        renderFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    m_running = false;
    if (m_socketFd >= 0) {
        close(m_socketFd);
        m_socketFd = -1;
    }
}

bool WindowApp::sendPointerMove(float x, float y) {
    PointerEvent ev{x, y, 0, 0.0f, 0.0f, PointerEventType::Move};
    if (m_onRawPointer && m_onRawPointer(ev)) {
        return true;
    }
    return m_dispatcher.dispatchPointerEvent(m_rootWidget.get(), ev);
}

bool WindowApp::sendPointerDown(float x, float y, int button) {
    PointerEvent ev{x, y, button, 0.0f, 0.0f, PointerEventType::Down};
    if (m_onRawPointer && m_onRawPointer(ev)) {
        return true;
    }
    return m_dispatcher.dispatchPointerEvent(m_rootWidget.get(), ev);
}

bool WindowApp::sendPointerUp(float x, float y, int button) {
    PointerEvent ev{x, y, button, 0.0f, 0.0f, PointerEventType::Up};
    if (m_onRawPointer && m_onRawPointer(ev)) {
        return true;
    }
    return m_dispatcher.dispatchPointerEvent(m_rootWidget.get(), ev);
}

bool WindowApp::sendKeyDown(int keyCode, char32_t codepoint, uint8_t modifiers) {
    KeyEvent ev{keyCode, codepoint, modifiers, KeyEventType::KeyDown};
    if (m_onRawKey && m_onRawKey(ev)) {
        return true;
    }
    return m_dispatcher.dispatchKeyEvent(ev);
}

bool WindowApp::sendKeyUp(int keyCode, uint8_t modifiers) {
    KeyEvent ev{keyCode, 0, modifiers, KeyEventType::KeyUp};
    if (m_onRawKey && m_onRawKey(ev)) {
        return true;
    }
    return m_dispatcher.dispatchKeyEvent(ev);
}

bool WindowApp::sendTextInput(const std::string& text) {
    TextInputEvent ev{text};
    if (m_onRawTextInput && m_onRawTextInput(ev)) {
        return true;
    }
    return m_dispatcher.dispatchTextInputEvent(ev);
}

void WindowApp::updateLayout() {
    if (m_rootWidget) {
        m_rootWidget->getYogaNode().calculateLayout(static_cast<float>(m_width), static_cast<float>(m_height));
        m_rootWidget->syncLayout(0.0f, 0.0f);
    }
}

bool WindowApp::renderFrame() {
    updateLayout();

    if (!m_renderPass.hasDamage()) {
        if (m_firstFrame && m_rootWidget) {
            m_renderPass.addDirtyRect(m_rootWidget->getAbsoluteBounds());
            m_firstFrame = false;
        } else {
            return false;
        }
    } else {
        m_firstFrame = false;
    }

    Rect damageRect = m_renderPass.getDamageRect();

    m_renderer.beginFrame();
    m_renderPass.begin(nullptr);

    if (m_rootWidget && m_rootWidget->isVisible()) {
        m_rootWidget->draw(reinterpret_cast<SkCanvas*>(&m_renderer), damageRect);
    }

    m_renderPass.end(nullptr);
    m_renderer.endFrame();

    m_renderPass.clear();

    // Copy 100% complete rendered frame to SHM buffer atomically
    if (m_shmPixels && !m_pixelBuffer.empty()) {
        size_t copyBytes = std::min(m_shmSize, m_pixelBuffer.size() * sizeof(uint32_t));
        std::memcpy(m_shmPixels, m_pixelBuffer.data(), copyBytes);
    }

    // If connected over IPC, notify compositor of buffer commit
    if (m_ipcConnected && m_socketFd >= 0 && m_shmFd >= 0) {
        lcl::protocol::LCLHeader attachHeader{};
        attachHeader.opcode = lcl::protocol::LCLOpcode::AttachBuffer;
        attachHeader.payloadSize = sizeof(lcl::protocol::LCLMsgAttachBuffer);

        lcl::protocol::LCLMsgAttachBuffer attachMsg{};
        attachMsg.surfaceId = 1;
        attachMsg.width = m_width;
        attachMsg.height = m_height;
        attachMsg.stride = m_width * 4;
        attachMsg.format = 1;

        int passFd = m_shmNeedsAttach ? m_shmFd : -1;
        lcl::protocol::sendMsgWithFd(m_socketFd, attachHeader, &attachMsg, passFd);
        m_shmNeedsAttach = false;
    }

    return true;
}

} // namespace lcl::ui
