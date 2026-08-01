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
#include <cmath>
#include <algorithm>

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

    // Resize path: keep existing renderer instance and only retarget the backing pixels.
    // Re-initializing renderer every configure event causes heavy stalls while dragging.
    m_renderer.setTargetPixels(m_pixelBuffer.data(), width, height);
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
        // Defer render+attach to the main loop's renderFrame() so each resize tick
        // produces at most one frame and one attach commit.
        m_firstFrame = true;
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

    if (pendingResize && (latestWidth > 0 && latestHeight > 0)) {
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
    }
}

void WindowApp::runEventLoop() {
    m_running = true;

    while (m_running && !g_appSignalReceived.load()) {
        auto frameStart = std::chrono::high_resolution_clock::now();

        pollIPC();
        bool rendered = renderFrame();

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
    if (m_socketFd >= 0) {
        close(m_socketFd);
        m_socketFd = -1;
    }
}

bool WindowApp::requestWindowMove(float localX, float localY) {
    if (!m_ipcConnected || m_socketFd < 0) return false;

    lcl::protocol::LCLHeader header{};
    header.opcode = lcl::protocol::LCLOpcode::BeginWindowMove;
    header.payloadSize = sizeof(lcl::protocol::LCLMsgBeginWindowMove);

    lcl::protocol::LCLMsgBeginWindowMove msg{};
    msg.surfaceId = 1;
    msg.localX = localX;
    msg.localY = localY;

    return lcl::protocol::sendMsgWithFd(m_socketFd, header, &msg);
}

bool WindowApp::requestWindowClose() {
    if (!m_ipcConnected || m_socketFd < 0) return false;

    lcl::protocol::LCLHeader header{};
    header.opcode = lcl::protocol::LCLOpcode::RequestSurfaceClose;
    header.payloadSize = sizeof(lcl::protocol::LCLMsgRequestSurfaceClose);

    lcl::protocol::LCLMsgRequestSurfaceClose msg{};
    msg.surfaceId = 1;

    return lcl::protocol::sendMsgWithFd(m_socketFd, header, &msg);
}

bool WindowApp::setDecorationMode(lcl::protocol::LCLDecorationMode mode) {
    if (!m_ipcConnected || m_socketFd < 0) return false;

    lcl::protocol::LCLHeader header{};
    header.opcode = lcl::protocol::LCLOpcode::SetDecorationMode;
    header.payloadSize = sizeof(lcl::protocol::LCLMsgSetDecorationMode);

    lcl::protocol::LCLMsgSetDecorationMode msg{};
    msg.surfaceId = 1;
    msg.mode = mode;

    return lcl::protocol::sendMsgWithFd(m_socketFd, header, &msg);
}

void WindowApp::configureCsdTitlebar(float height, float closeLeft, float closeTop, float closeSize) {
    m_csdTitlebarHeight = std::max(0.0f, height);
    m_csdCloseLeft = closeLeft;
    m_csdCloseTop = closeTop;
    m_csdCloseSize = std::max(0.0f, closeSize);
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
        const bool inCloseX = x >= m_csdCloseLeft && x <= (m_csdCloseLeft + m_csdCloseSize);
        const bool inCloseY = y >= m_csdCloseTop && y <= (m_csdCloseTop + m_csdCloseSize);
        if (inCloseX && inCloseY) {
            requestWindowClose();
            return true;
        }
        requestWindowMove(x, y);
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

    m_renderer.beginFrame();
    if (auto* pixels = m_renderer.getRasterBuffer()) {
        std::fill_n(pixels, static_cast<size_t>(m_width) * static_cast<size_t>(m_height), 0x00000000);
    }
    m_renderPass.begin(nullptr);

    if (m_rootWidget && m_rootWidget->isVisible()) {
        m_rootWidget->draw(reinterpret_cast<SkCanvas*>(&m_renderer), damageRect);
    }

    std::vector<EffectRegion> uiEffects;
    if (m_rootWidget) {
        m_rootWidget->collectEffects(uiEffects);
    }

    m_renderPass.end(nullptr);
    m_renderer.endFrame();

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

            int x = std::max(0, static_cast<int>(std::lround(effect.bounds.x)));
            int y = std::max(0, static_cast<int>(std::lround(effect.bounds.y)));
            int w = std::max(0, static_cast<int>(std::lround(effect.bounds.width)));
            int h = std::max(0, static_cast<int>(std::lround(effect.bounds.height)));

            if (x >= static_cast<int>(m_width) || y >= static_cast<int>(m_height)) continue;
            w = std::min(w, static_cast<int>(m_width) - x);
            h = std::min(h, static_cast<int>(m_height) - y);
            if (w <= 0 || h <= 0) continue;

            lcl::protocol::EffectRegion region{};
            region.x = x;
            region.y = y;
            region.width = static_cast<uint32_t>(w);
            region.height = static_cast<uint32_t>(h);
            region.cornerRadius = std::max(0.0f, effect.cornerRadius);
            region.source = toProtoSource(effect.source);
            region.blendMode = toProtoBlend(effect.blend);
            region.opacity = std::clamp(effect.opacity, 0.0f, 1.0f);
            region.filterOffset = static_cast<uint32_t>(flatFilters.size());
            region.filterCount = static_cast<uint16_t>(std::min<size_t>(effect.filters.size(), 65535));

            flatFilters.insert(
                flatFilters.end(),
                effect.filters.begin(),
                effect.filters.begin() + region.filterCount);
            protoRegions.push_back(region);
        }

        if (!protoRegions.empty()) {
            lcl::protocol::LCLMsgSetEffectGraphHeader graphMsg{};
            graphMsg.surfaceId = 1;
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
            clearMsg.surfaceId = 1;

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
