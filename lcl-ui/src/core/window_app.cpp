#include "lcl-ui/core/window_app.hpp"
#include "core/display/display_scale.hpp"
#include "core/ipc/lcl_protocol.hpp"
#include "render/skia_canvas.hpp"
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
#include <cmath>
#include <algorithm>
#include <cstddef>

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

namespace {

float sanitizeBufferScale(float scale) {
    if (!std::isfinite(scale) || scale < 0.5f || scale > 4.0f) {
        return 1.0f;
    }
    return scale;
}

uint32_t toBufferPixels(uint32_t logical, float scale) {
    return std::max(1u, static_cast<uint32_t>(std::ceil(static_cast<float>(logical) * scale)));
}

} // namespace

WindowApp::WindowApp(uint32_t width, uint32_t height, const std::string& title)
    : WindowApp(lcl::render::makeSkiaCanvas(), width, height, title) {}

WindowApp::WindowApp(std::unique_ptr<Canvas> canvas, uint32_t width, uint32_t height,
                     const std::string& title)
    : m_width(width), m_height(height), m_title(title), m_canvas(std::move(canvas)) {
    setupAppSignalHandlers();
    if (!m_canvas) return;
    m_pixelBuffer.resize(width * height, 0xFF000000);
    m_initialized = m_canvas->initialize(width, height, m_pixelBuffer.data());

    auto defaultRoot = std::make_unique<Container>();
    defaultRoot->getYogaNode().setWidth(static_cast<float>(width));
    defaultRoot->getYogaNode().setHeight(static_cast<float>(height));
    setRootWidget(std::move(defaultRoot));
    m_lastResizeApply = std::chrono::steady_clock::now();
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
    if (m_socketFd >= 0 && m_ownsSocketFd) {
        close(m_socketFd);
        m_socketFd = -1;
    }
}

uint32_t WindowApp::getPixelWidth() const {
    return toBufferPixels(m_width, m_bufferScale);
}

uint32_t WindowApp::getPixelHeight() const {
    return toBufferPixels(m_height, m_bufferScale);
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

    const uint32_t pixelWidth = toBufferPixels(width, m_bufferScale);
    const uint32_t pixelHeight = toBufferPixels(height, m_bufferScale);
    m_shmSize = static_cast<size_t>(pixelWidth) * pixelHeight * 4;
    m_pixelBuffer.resize(static_cast<size_t>(pixelWidth) * pixelHeight, 0xFF14161D);

    m_shmFd = memfd_create("lcl_ui_app_shm", MFD_CLOEXEC);
    if (m_shmFd >= 0) {
        ftruncate(m_shmFd, m_shmSize);
        m_shmPixels = reinterpret_cast<uint32_t*>(
            mmap(nullptr, m_shmSize, PROT_READ | PROT_WRITE, MAP_SHARED, m_shmFd, 0));
        if (m_shmPixels == MAP_FAILED) {
            m_shmPixels = nullptr;
        } else {
            std::fill_n(m_shmPixels, static_cast<size_t>(pixelWidth) * pixelHeight, 0xFF14161D);
        }
    }

    // Resize path: keep existing renderer instance and only retarget the backing pixels.
    // Re-initializing renderer every configure event causes heavy stalls while dragging.
    m_canvas->setTargetPixels(m_pixelBuffer.data(), pixelWidth, pixelHeight);
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

    // WindowApp exposes CSS-like logical pixels. The process-local boot scale is
    // its device pixel ratio; raw-pixel clients do not use this class and retain 1x.
    m_bufferScale = sanitizeBufferScale(lcl::core::DisplayScale::factor());
    m_canvas->setContentScale(m_bufferScale);

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
    regMsg.role = m_role;
    std::strncpy(regMsg.clientName, m_title.c_str(), sizeof(regMsg.clientName) - 1);
    lcl::protocol::sendMsgWithFd(m_socketFd, regHeader, &regMsg);

    // 2. Request Surface Creation
    lcl::protocol::LCLHeader surfHeader{};
    surfHeader.opcode = lcl::protocol::LCLOpcode::SurfaceCreate;
    surfHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSurfaceCreate);

    lcl::protocol::LCLMsgSurfaceCreate surfMsg{};
    surfMsg.surfaceId = m_surfaceId;
    surfMsg.x = m_initialX;
    surfMsg.y = m_initialY;
    surfMsg.width = m_width;
    surfMsg.height = m_height;
    surfMsg.bufferScale = m_bufferScale;
    std::strncpy(surfMsg.title, m_title.c_str(), sizeof(surfMsg.title) - 1);
    lcl::protocol::sendMsgWithFd(m_socketFd, surfHeader, &surfMsg);

    // 3. Allocate SHM memory buffer and attach to compositor
    allocateSHM(m_width, m_height);

    m_ipcConnected = true;
    m_ownsSocketFd = true;
    // Decoration state belongs to the surface contract, not to a later frame.
    // Send preconfigured values before the first effect graph/buffer attach so a
    // CSD client never flashes the compositor's default title chrome.
    if (m_hasRequestedDecorationMode) {
        setDecorationMode(m_requestedDecorationMode);
    }
    if (m_hasRequestedCornerRadius) {
        setWindowCornerRadius(m_requestedCornerRadius);
    }
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
        // Defer render+attach to the main loop's renderFrame() so each resize tick
        // produces at most one frame and one attach commit.
        m_firstFrame = true;
    }

    if (m_onResize) {
        m_onResize(width, height);
    }
}

void WindowApp::setInitialBounds(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    if (m_ipcConnected || width == 0 || height == 0) return;

    m_initialX = x;
    m_initialY = y;
    m_width = width;
    m_height = height;
    m_pixelBuffer.resize(static_cast<size_t>(width) * height, 0xFF000000);
    m_canvas->setTargetPixels(m_pixelBuffer.data(), width, height);
    if (m_rootWidget) {
        m_rootWidget->getYogaNode().setWidth(static_cast<float>(width));
        m_rootWidget->getYogaNode().setHeight(static_cast<float>(height));
        m_rootWidget->markDirty();
    }
}

void WindowApp::pollIPC() {
    if (!m_ipcConnected || m_socketFd < 0) return;

    uint32_t latestWidth = 0;
    uint32_t latestHeight = 0;
    float latestScale = m_bufferScale;
    bool pendingResize = false;

    while (true) {
        lcl::protocol::LCLHeader header{};
        std::vector<uint8_t> payload;
        int receivedFd = -1;
        if (lcl::protocol::recvMsgWithFd(m_socketFd, header, payload, receivedFd)) {
            if (header.opcode == lcl::protocol::LCLOpcode::InputEvent &&
                payload.size() >= sizeof(lcl::protocol::LCLMsgInputEvent)) {
                auto* inputMsg = reinterpret_cast<const lcl::protocol::LCLMsgInputEvent*>(payload.data());
                if (inputMsg->surfaceId != m_surfaceId) continue;
                // A shell panel can be visually topmost without being interactive.
                // Do not let hover handling mutate its widget tree until it explicitly
                // opts into input (dock activation is intentionally future work).
                if (!m_inputEnabled) continue;
                if (inputMsg->type == 1) { // KeyDown
                    sendKeyDown(inputMsg->key, static_cast<char32_t>(inputMsg->codepoint), inputMsg->modifiers);
                } else if (inputMsg->type == 2) { // KeyUp
                    sendKeyUp(inputMsg->key, inputMsg->modifiers);
                } else if (inputMsg->type == 3) { // PointerMotion
                    sendPointerMove(inputMsg->x, inputMsg->y);
                } else if (inputMsg->type == 4) { // PointerButton
                    if (inputMsg->pressed) {
                        sendPointerDown(inputMsg->x, inputMsg->y, inputMsg->key);
                    } else {
                        sendPointerUp(inputMsg->x, inputMsg->y, inputMsg->key);
                    }
                } else if (inputMsg->type == 5) { // KeyPress / TextInput
                    if (inputMsg->codepoint > 0) {
                        std::string utf8;
                        char32_t cp = inputMsg->codepoint;
                        if (cp <= 0x7F) {
                            utf8.push_back(static_cast<char>(cp));
                        } else if (cp <= 0x7FF) {
                            utf8.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
                            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else if (cp <= 0xFFFF) {
                            utf8.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
                            utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        sendTextInput(utf8);
                    }
                }
            } else if (header.opcode == lcl::protocol::LCLOpcode::ConfigureBounds &&
                       payload.size() >= offsetof(lcl::protocol::LCLMsgConfigureBounds, bufferScale)) {
                auto* cfg = reinterpret_cast<const lcl::protocol::LCLMsgConfigureBounds*>(payload.data());
                if (cfg->surfaceId != m_surfaceId) continue;
                if (cfg->width > 0 && cfg->height > 0) {
                    latestWidth = cfg->width;
                    latestHeight = cfg->height;
                    latestScale = (payload.size() >= sizeof(lcl::protocol::LCLMsgConfigureBounds))
                        ? sanitizeBufferScale(cfg->bufferScale)
                        : 1.0f;
                    pendingResize = true;
                }
            } else if (header.opcode == lcl::protocol::LCLOpcode::SurfaceDestroy) {
                if (payload.size() >= sizeof(lcl::protocol::LCLMsgSurfaceDestroy)) {
                    auto* destroy = reinterpret_cast<const lcl::protocol::LCLMsgSurfaceDestroy*>(payload.data());
                    if (destroy->surfaceId == m_surfaceId) m_running = false;
                }
            }
            if (m_onIpcMessage) m_onIpcMessage(header, payload);
        } else {
            break;
        }
    }

    if (pendingResize && (latestWidth > 0 && latestHeight > 0)) {
        if (m_resizeTransform) {
            const auto [transformedWidth, transformedHeight] =
                m_resizeTransform(latestWidth, latestHeight);
            latestWidth = transformedWidth;
            latestHeight = transformedHeight;
            if (latestWidth == 0 || latestHeight == 0) {
                pendingResize = false;
            }
        }

        if (std::fabs(latestScale - m_bufferScale) > 0.0001f) {
            m_bufferScale = latestScale;
            m_canvas->setContentScale(m_bufferScale);
            // A scale-only configure has identical logical bounds but needs a new buffer.
            if (latestWidth == m_width && latestHeight == m_height && m_ipcConnected) {
                allocateSHM(m_width, m_height);
                m_firstFrame = true;
            }
        }
        m_pendingResizeWidth = latestWidth;
        m_pendingResizeHeight = latestHeight;
        m_hasPendingResize = true;
    }

    // Throttled resize apply: demos update every pixel while dragging; applying each
    // configure causes SHM recreate storms. Keep latest target and apply at a bounded rate.
    if (m_hasPendingResize &&
        (m_pendingResizeWidth != m_width || m_pendingResizeHeight != m_height)) {
        const auto now = std::chrono::steady_clock::now();
        constexpr auto kMinResizeInterval = std::chrono::milliseconds(22);

        const uint32_t dx = (m_pendingResizeWidth > m_width)
            ? (m_pendingResizeWidth - m_width)
            : (m_width - m_pendingResizeWidth);
        const uint32_t dy = (m_pendingResizeHeight > m_height)
            ? (m_pendingResizeHeight - m_height)
            : (m_height - m_pendingResizeHeight);

        const bool largeJump = (dx >= 48u) || (dy >= 48u);
        const bool intervalElapsed = (now - m_lastResizeApply) >= kMinResizeInterval;

        if (largeJump || intervalElapsed) {
            resize(m_pendingResizeWidth, m_pendingResizeHeight);
            m_hasPendingResize = false;
            m_lastResizeApply = now;
        }
    } else if (m_hasPendingResize) {
        // The compositor often confirms the initial logical size verbatim.
        // It requires no SHM reallocation, so do not retain it indefinitely.
        m_hasPendingResize = false;
    }
}

void WindowApp::runEventLoop() {
    m_running = true;

    while (m_running && !g_appSignalReceived.load()) {
        auto frameStart = std::chrono::high_resolution_clock::now();

        bool rendered = tick();

        if (rendered) {
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now() - frameStart);
            // Keep app-side pacing close to terminal loop to avoid resize thrash.
            constexpr auto minFramePeriod = std::chrono::microseconds(6900);
            if (elapsed < minFramePeriod) {
                std::this_thread::sleep_for(minFramePeriod - elapsed);
            }
        } else {
            // Idle state: sleep 2ms when no redraws are needed to avoid CPU busy spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    m_running = false;
    if (m_socketFd >= 0 && m_ownsSocketFd) {
        close(m_socketFd);
        m_socketFd = -1;
    }
}

bool WindowApp::tick() {
    pollIPC();
    if (m_onFrame) {
        m_onFrame();
    }
    return renderFrame();
}

void WindowApp::setExternalIpcSocket(int socketFd) {
    if (socketFd < 0) return;
    m_socketFd = socketFd;
    m_ipcConnected = true;
    m_ownsSocketFd = false;
}

bool WindowApp::requestWindowAction(lcl::protocol::LCLWindowAction action,
                                    float localX, float localY) {
    if (!m_ipcConnected || m_socketFd < 0) return false;

    lcl::protocol::LCLHeader header{};
    header.opcode = lcl::protocol::LCLOpcode::RequestWindowAction;
    header.payloadSize = sizeof(lcl::protocol::LCLMsgRequestWindowAction);

    lcl::protocol::LCLMsgRequestWindowAction msg{};
    msg.surfaceId = m_surfaceId;
    msg.action = action;
    msg.localX = localX;
    msg.localY = localY;

    return lcl::protocol::sendMsgWithFd(m_socketFd, header, &msg);
}

bool WindowApp::requestWindowDrag(float localX, float localY) {
    return requestWindowAction(lcl::protocol::LCLWindowAction::BeginDrag, localX, localY);
}

bool WindowApp::requestWindowMinimize() {
    return requestWindowAction(lcl::protocol::LCLWindowAction::Minimize);
}

bool WindowApp::requestWindowMaximize() {
    return requestWindowAction(lcl::protocol::LCLWindowAction::Maximize);
}

bool WindowApp::requestWindowRestore() {
    return requestWindowAction(lcl::protocol::LCLWindowAction::Restore);
}

bool WindowApp::requestWindowToggleMaximize() {
    return requestWindowAction(lcl::protocol::LCLWindowAction::ToggleMaximize);
}

bool WindowApp::requestWindowClose() {
    return requestWindowAction(lcl::protocol::LCLWindowAction::Close);
}

bool WindowApp::setDecorationMode(lcl::protocol::LCLDecorationMode mode) {
    m_requestedDecorationMode = mode;
    m_hasRequestedDecorationMode = true;
    if (!m_ipcConnected || m_socketFd < 0) return true;

    lcl::protocol::LCLHeader header{};
    header.opcode = lcl::protocol::LCLOpcode::SetDecorationMode;
    header.payloadSize = sizeof(lcl::protocol::LCLMsgSetDecorationMode);

    lcl::protocol::LCLMsgSetDecorationMode msg{};
    msg.surfaceId = m_surfaceId;
    msg.mode = mode;

    return lcl::protocol::sendMsgWithFd(m_socketFd, header, &msg);
}

bool WindowApp::setWindowLayer(lcl::protocol::LCLWindowLayer layer, bool unfocusable) {
    if (!m_ipcConnected || m_socketFd < 0) return false;

    lcl::protocol::LCLHeader header{};
    header.opcode = lcl::protocol::LCLOpcode::SetWindowLayer;
    header.payloadSize = sizeof(lcl::protocol::LCLMsgSetWindowLayer);

    lcl::protocol::LCLMsgSetWindowLayer msg{};
    msg.surfaceId = m_surfaceId;
    msg.layer = layer;
    msg.unfocusable = unfocusable ? 1 : 0;
    return lcl::protocol::sendMsgWithFd(m_socketFd, header, &msg);
}

bool WindowApp::setReservedZone(uint32_t top, uint32_t bottom, uint32_t left, uint32_t right) {
    if (!m_ipcConnected || m_socketFd < 0) return false;

    lcl::protocol::LCLHeader header{};
    header.opcode = lcl::protocol::LCLOpcode::SetReservedZone;
    header.payloadSize = sizeof(lcl::protocol::LCLMsgSetReservedZone);

    lcl::protocol::LCLMsgSetReservedZone msg{};
    msg.surfaceId = m_surfaceId;
    msg.top = static_cast<uint32_t>(std::lround(static_cast<float>(top) * m_bufferScale));
    msg.bottom = static_cast<uint32_t>(std::lround(static_cast<float>(bottom) * m_bufferScale));
    msg.left = static_cast<uint32_t>(std::lround(static_cast<float>(left) * m_bufferScale));
    msg.right = static_cast<uint32_t>(std::lround(static_cast<float>(right) * m_bufferScale));
    return lcl::protocol::sendMsgWithFd(m_socketFd, header, &msg);
}

bool WindowApp::setWindowCornerRadius(float radiusPx) {
    m_requestedCornerRadius = std::max(0.0f, radiusPx);
    m_hasRequestedCornerRadius = true;
    if (!m_ipcConnected || m_socketFd < 0) return true;

    lcl::protocol::LCLHeader header{};
    header.opcode = lcl::protocol::LCLOpcode::SetWindowCornerRadius;
    header.payloadSize = sizeof(lcl::protocol::LCLMsgSetWindowCornerRadius);

    lcl::protocol::LCLMsgSetWindowCornerRadius msg{};
    msg.surfaceId = m_surfaceId;
    msg.radiusPx = m_requestedCornerRadius;

    return lcl::protocol::sendMsgWithFd(m_socketFd, header, &msg);
}

void WindowApp::configureCsdTitlebar(float height, float controlLeft, float controlTop,
                                     float controlSize, float controlGap) {
    m_csdTitlebarHeight = std::max(0.0f, height);
    m_csdControlLeft = controlLeft;
    m_csdControlTop = controlTop;
    m_csdControlSize = std::max(0.0f, controlSize);
    m_csdControlGap = std::max(0.0f, controlGap);
}

bool WindowApp::sendPointerMove(float x, float y) {
    PointerEvent ev{x, y, 0, 0.0f, 0.0f, PointerEventType::Move};
    if (m_onRawPointer && m_onRawPointer(ev)) {
        return true;
    }
    return m_dispatcher.dispatchPointerEvent(m_rootWidget.get(), ev);
}

bool WindowApp::sendPointerDown(float x, float y, int button) {
    if (m_csdTitlebarEnabled && button == 0 && y >= 0.0f && y <= m_csdTitlebarHeight) {
        const bool inControlY = y >= m_csdControlTop && y <= (m_csdControlTop + m_csdControlSize);
        const auto isControl = [&](int index) {
            const float left = m_csdControlLeft +
                static_cast<float>(index) * (m_csdControlSize + m_csdControlGap);
            return inControlY && x >= left && x <= (left + m_csdControlSize);
        };
        if (isControl(0)) {
            requestWindowClose();
            return true;
        }
        if (isControl(1)) {
            requestWindowMinimize();
            return true;
        }
        if (isControl(2)) {
            requestWindowToggleMaximize();
            return true;
        }
        requestWindowDrag(x, y);
        return true;
    }

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
    m_renderPass.clear();

    m_canvas->beginFrame();
    if (auto* pixels = m_canvas->rasterBuffer()) {
        std::fill_n(pixels, static_cast<size_t>(getPixelWidth()) * static_cast<size_t>(getPixelHeight()), 0x00000000);
    }
    m_renderPass.begin(*m_canvas);

    if (m_rootWidget && m_rootWidget->isVisible()) {
        m_rootWidget->draw(*m_canvas, damageRect);
    }

    std::vector<EffectRegion> uiEffects;
    if (m_rootWidget) {
        m_rootWidget->collectEffects(uiEffects);
    }

    m_renderPass.end(*m_canvas);
    m_canvas->endFrame();

    if (m_ipcConnected && m_socketFd >= 0) {
        auto toProtoSource = [](EffectSource source) {
            return (source == EffectSource::Foreground)
                ? lcl::protocol::EffectSourceType::Foreground
                : lcl::protocol::EffectSourceType::Backdrop;
        };

        auto toProtoBlend = [](EffectBlend blend) {
            switch (blend) {
                case EffectBlend::Screen:   return lcl::protocol::EffectBlendMode::Screen;
                case EffectBlend::Multiply: return lcl::protocol::EffectBlendMode::Multiply;
                case EffectBlend::Overlay:  return lcl::protocol::EffectBlendMode::Overlay;
                case EffectBlend::Plus:     return lcl::protocol::EffectBlendMode::Plus;
                case EffectBlend::Normal:
                default:                    return lcl::protocol::EffectBlendMode::Normal;
            }
        };

        std::vector<lcl::protocol::EffectRegion> protoRegions;
        std::vector<lcl::protocol::FilterOp> flatFilters;
        protoRegions.reserve(uiEffects.size());

        for (const auto& effect : uiEffects) {
            if (effect.bounds.isEmpty() || effect.filters.empty()) continue;

            int x = std::max(0, static_cast<int>(std::lround(effect.bounds.x * m_bufferScale)));
            int y = std::max(0, static_cast<int>(std::lround(effect.bounds.y * m_bufferScale)));
            int w = std::max(0, static_cast<int>(std::lround(effect.bounds.width * m_bufferScale)));
            int h = std::max(0, static_cast<int>(std::lround(effect.bounds.height * m_bufferScale)));

            if (x >= static_cast<int>(getPixelWidth()) || y >= static_cast<int>(getPixelHeight())) continue;
            w = std::min(w, static_cast<int>(getPixelWidth()) - x);
            h = std::min(h, static_cast<int>(getPixelHeight()) - y);
            if (w <= 0 || h <= 0) continue;

            lcl::protocol::EffectRegion region{};
            region.x = x;
            region.y = y;
            region.width = static_cast<uint32_t>(w);
            region.height = static_cast<uint32_t>(h);
            region.cornerRadius = std::max(0.0f, effect.cornerRadius * m_bufferScale);
            region.source = toProtoSource(effect.source);
            region.blendMode = toProtoBlend(effect.blend);
            region.opacity = std::clamp(effect.opacity, 0.0f, 1.0f);
            region.filterOffset = static_cast<uint32_t>(flatFilters.size());
            region.filterCount = static_cast<uint16_t>(std::min<size_t>(effect.filters.size(), 65535));

            for (uint16_t i = 0; i < region.filterCount; ++i) {
                auto filter = effect.filters[i];
                // These values are specified by widgets in logical px as well.
                if (filter.type == lcl::protocol::FilterType::Blur) {
                    filter.value *= m_bufferScale;
                } else if (filter.type == lcl::protocol::FilterType::Glass) {
                    filter.params[0] *= m_bufferScale; // thicknessPx
                }
                flatFilters.push_back(filter);
            }
            protoRegions.push_back(region);
        }

        if (!protoRegions.empty()) {
            lcl::protocol::LCLMsgSetEffectGraphHeader graphMsg{};
            graphMsg.surfaceId = m_surfaceId;
            graphMsg.regionCount = static_cast<uint32_t>(protoRegions.size());
            graphMsg.filterCount = static_cast<uint32_t>(flatFilters.size());

            const size_t payloadSize =
                sizeof(graphMsg) +
                protoRegions.size() * sizeof(lcl::protocol::EffectRegion) +
                flatFilters.size() * sizeof(lcl::protocol::FilterOp);

            std::vector<uint8_t> payload(payloadSize);
            uint8_t* dst = payload.data();
            std::memcpy(dst, &graphMsg, sizeof(graphMsg));
            dst += sizeof(graphMsg);
            std::memcpy(dst, protoRegions.data(), protoRegions.size() * sizeof(lcl::protocol::EffectRegion));
            dst += protoRegions.size() * sizeof(lcl::protocol::EffectRegion);
            if (!flatFilters.empty()) {
                std::memcpy(dst, flatFilters.data(), flatFilters.size() * sizeof(lcl::protocol::FilterOp));
            }

            if (payload != m_lastEffectGraphPayload) {
                lcl::protocol::LCLHeader graphHeader{};
                graphHeader.opcode = lcl::protocol::LCLOpcode::SetEffectGraph;
                graphHeader.payloadSize = static_cast<uint32_t>(payload.size());
                lcl::protocol::sendMsgWithFd(m_socketFd, graphHeader, payload.data());
                m_lastEffectGraphPayload = std::move(payload);
            }
            m_effectGraphActive = true;
        } else if (m_effectGraphActive) {
            lcl::protocol::LCLMsgClearEffectGraph clearMsg{};
            clearMsg.surfaceId = m_surfaceId;

            lcl::protocol::LCLHeader clearHeader{};
            clearHeader.opcode = lcl::protocol::LCLOpcode::ClearEffectGraph;
            clearHeader.payloadSize = sizeof(clearMsg);
            lcl::protocol::sendMsgWithFd(m_socketFd, clearHeader, &clearMsg);
            m_effectGraphActive = false;
            m_lastEffectGraphPayload.clear();
        }
    }

    // Double Buffering: Copy 100% complete rendered frame to SHM buffer atomically
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
        attachMsg.surfaceId = m_surfaceId;
        attachMsg.width = getPixelWidth();
        attachMsg.height = getPixelHeight();
        attachMsg.stride = getPixelWidth() * 4;
        attachMsg.format = 1;

        int passFd = m_shmNeedsAttach ? m_shmFd : -1;
        lcl::protocol::sendMsgWithFd(m_socketFd, attachHeader, &attachMsg, passFd);
        m_shmNeedsAttach = false;
    }

    return true;
}

} // namespace lcl::ui
