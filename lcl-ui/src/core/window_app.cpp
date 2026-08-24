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
#include <cstddef>
#include <cstdlib>

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

uint32_t toBufferPixels(float logical, float scale) {
    return std::max(1u, static_cast<uint32_t>(std::ceil(logical * scale)));
}

uint32_t crossfadePixel(uint32_t oldPixel, uint32_t newPixel, float progress) {
    const uint32_t newWeight = static_cast<uint32_t>(
        std::clamp(std::lround(progress * 256.0f), 0l, 256l));
    const uint32_t oldWeight = 256u - newWeight;
    const auto blendChannel = [oldWeight, newWeight](uint32_t oldValue, uint32_t newValue) {
        return (oldValue * oldWeight + newValue * newWeight + 128u) >> 8u;
    };
    return (blendChannel((oldPixel >> 24u) & 0xFFu, (newPixel >> 24u) & 0xFFu) << 24u) |
           (blendChannel((oldPixel >> 16u) & 0xFFu, (newPixel >> 16u) & 0xFFu) << 16u) |
           (blendChannel((oldPixel >> 8u) & 0xFFu, (newPixel >> 8u) & 0xFFu) << 8u) |
           blendChannel(oldPixel & 0xFFu, newPixel & 0xFFu);
}

bool frameTraceEnabled() {
    const char* value = std::getenv("LCL_TRACE_FRAMES");
    return value && value[0] != '\0' && value[0] != '0';
}

bool layoutOverlayEnabled() {
    const char* value = std::getenv("LCL_DEBUG_LAYOUT");
    return value && value[0] != '\0' && value[0] != '0';
}

void drawLayoutOverlay(const Widget& widget, graphics::Canvas& canvas, uint32_t depth = 0) {
    static constexpr graphics::Color kPalette[] = {
        {56, 189, 248, 220}, {74, 222, 128, 220}, {250, 204, 21, 220},
        {244, 114, 182, 220}, {167, 139, 250, 220},
    };
    const graphics::RectF bounds = widget.getAbsoluteBounds();
    if (!bounds.isEmpty()) {
        canvas.drawRoundedRect(bounds, 0.0f, {0, 0, 0, 0},
                               kPalette[depth % (sizeof(kPalette) / sizeof(kPalette[0]))],
                               1.0f, 2.0f);
    }
    for (const auto& child : widget.getChildren()) {
        drawLayoutOverlay(*child, canvas, depth + 1);
    }
}

} // namespace

WindowApp::WindowApp(std::unique_ptr<graphics::Canvas> canvas, float width, float height,
                     const std::string& title)
    : m_width(width), m_height(height), m_title(title), m_canvas(std::move(canvas)) {
    setupAppSignalHandlers();
    if (!m_canvas) return;
    m_frameTraceEnabled = frameTraceEnabled();
    m_layoutOverlayEnabled = layoutOverlayEnabled();
    m_traceLastLog = std::chrono::steady_clock::now();
    const uint32_t pixelWidth = toBufferPixels(width, m_bufferScale);
    const uint32_t pixelHeight = toBufferPixels(height, m_bufferScale);
    m_pixelBuffer.resize(static_cast<size_t>(pixelWidth) * pixelHeight, 0xFF000000);
    m_initialized = m_canvas->initialize(pixelWidth, pixelHeight, m_pixelBuffer.data());
    updateCanvasRenderTarget();
    m_motionCoordinator.setCallbacks(
        [this] { updateLayout(); },
        [this](const graphics::RectF& rect) { if (!rect.isEmpty()) m_renderPass.addDirtyRect(rect); });

    auto defaultRoot = std::make_unique<Container>();
    defaultRoot->getYogaNode().setWidth(static_cast<float>(width));
    defaultRoot->getYogaNode().setHeight(static_cast<float>(height));
    setRootWidget(std::move(defaultRoot));
    m_lastResizeApply = std::chrono::steady_clock::now();
    m_lastAnimationTick = std::chrono::steady_clock::now();
}

WindowApp::~WindowApp() {
    m_running = false;
    m_lifetimeToken.reset();
    // Widgets unregister their channels on destruction. Detach while the
    // coordinator member is still alive (member teardown runs in reverse).
    m_transients.clear();
    m_transients.setWindowRoot(nullptr);
    m_hostedSurfaces.clear();
    if (m_windowRoot) m_windowRoot->setMotionCoordinator(nullptr);
    if (m_shmPixels) {
        munmap(m_shmPixels, m_shmSize);
        m_shmPixels = nullptr;
    }
    if (m_shmFd >= 0) {
        close(m_shmFd);
        m_shmFd = -1;
    }
    if (m_socketFd >= 0 && m_ownsSocketFd) {
        lcl::protocol::discardPendingWrites(m_socketFd);
        close(m_socketFd);
        m_socketFd = -1;
    }
    if (m_nativeBufferSocketFd >= 0) {
        close(m_nativeBufferSocketFd);
        m_nativeBufferSocketFd = -1;
    }
}

uint32_t WindowApp::getPixelWidth() const {
    return toBufferPixels(m_width, m_bufferScale);
}

uint32_t WindowApp::getPixelHeight() const {
    return toBufferPixels(m_height, m_bufferScale);
}

void WindowApp::updateCanvasRenderTarget() {
    if (!m_canvas) return;
    m_canvas->setRenderTarget({
        {static_cast<float>(m_width), static_cast<float>(m_height)},
        {getPixelWidth(), getPixelHeight()},
        m_bufferScale,
    });
}

void WindowApp::setRootWidget(std::unique_ptr<Widget> root) {
    if (!root) return;
    m_dispatcher.cancelPointerCaptures();
    m_transients.setWindowRoot(nullptr);

    auto windowRoot = std::make_unique<Container>();
    windowRoot->setThemeContext(&m_themeContext);
    windowRoot->getYogaNode().setWidth(static_cast<float>(m_width));
    windowRoot->getYogaNode().setHeight(static_cast<float>(m_height));
    // A normal application root represents the complete client surface.
    // App-provided startup dimensions must not remain as fixed cross-axis
    // constraints after the compositor configures a new window size. Absolute
    // roots are intentional surface fragments (for example a titlebar-only
    // test host) and keep their own geometry.
    if (YGNodeStyleGetPositionType(root->getYogaNode().getRef()) !=
        YGPositionTypeAbsolute) {
        root->getYogaNode().setWidthAuto();
        root->getYogaNode().setHeightAuto();
        root->getYogaNode().setAlignSelf(YGAlignStretch);
    }
    root->getYogaNode().setFlexGrow(1.0f);
    root->getYogaNode().setFlexShrink(1.0f);
    Widget* contentRoot = root.get();
    windowRoot->addChild(std::move(root));
    windowRoot->setRenderPass(&m_renderPass);
    windowRoot->setMotionCoordinator(&m_motionCoordinator);

    m_windowRoot = std::move(windowRoot);
    m_rootWidget = contentRoot;
    m_transients.setWindowRoot(m_windowRoot.get());
    m_windowRoot->markLayoutDirty();
    m_windowRoot->markDirty();
    // A newly mounted tree has not been through Yoga/syncLayout yet, so its
    // absolute bounds are still empty and markDirty() cannot produce damage.
    // Force one full frame after layout; runtime root replacement must not
    // leave old pixels in the client buffer.
    m_firstFrame = true;
}

void WindowApp::setTheme(lcl::theme::Theme theme) {
    m_themeContext.setTheme(std::move(theme));
    if (m_windowRoot) {
        m_windowRoot->setThemeContext(&m_themeContext);
        m_windowRoot->markLayoutDirty();
        m_windowRoot->markDirty();
    }
    m_firstFrame = true;
}

TransientHandle WindowApp::registerLocalTransient(std::unique_ptr<Widget> widget,
                                                  TransientOptions options) {
    if (!widget) return 0;
    widget->getYogaNode().setPositionType(YGPositionTypeAbsolute);
    return m_transients.registerLocal(std::move(widget), std::move(options));
}

TransientHandle WindowApp::registerSurfaceTransient(
        uint32_t surfaceId, std::function<void()> destroySurface,
        TransientOptions options) {
    return m_transients.registerSurface(surfaceId, std::move(destroySurface),
                                        std::move(options));
}

TransientHandle WindowApp::registerSurfaceTransient(
        uint32_t surfaceId, TransientOptions options) {
    return m_transients.registerSurface(
        surfaceId,
        [this, surfaceId] { requestSurfaceDestroy(surfaceId); },
        std::move(options));
}

bool WindowApp::removeTransient(TransientHandle handle) {
    return m_transients.remove(handle);
}

void WindowApp::clearTransients() {
    m_transients.clear();
}

HostedSurfaceHandle WindowApp::hostSurface(
        std::unique_ptr<WindowApp> surface, std::function<void()> onClosed) {
    if (!surface || surface.get() == this) return 0;
    const HostedSurfaceHandle handle = m_nextHostedSurfaceHandle++;
    m_hostedSurfaces.push_back(HostedSurfaceEntry{
        handle, std::move(surface), std::move(onClosed), false});
    return handle;
}

bool WindowApp::removeHostedSurface(HostedSurfaceHandle handle) {
    const auto found = std::find_if(
        m_hostedSurfaces.begin(), m_hostedSurfaces.end(),
        [handle](const HostedSurfaceEntry& entry) { return entry.handle == handle; });
    if (found == m_hostedSurfaces.end()) return false;
    if (found->pendingRemoval) return true;
    found->pendingRemoval = true;
    if (found->surface) found->surface->requestWindowClose();
    if (!m_tickingHostedSurfaces) collectClosedHostedSurfaces();
    return true;
}

WindowApp* WindowApp::getHostedSurface(HostedSurfaceHandle handle) const noexcept {
    const auto found = std::find_if(
        m_hostedSurfaces.begin(), m_hostedSurfaces.end(),
        [handle](const HostedSurfaceEntry& entry) { return entry.handle == handle; });
    return found == m_hostedSurfaces.end() ? nullptr : found->surface.get();
}

void WindowApp::setInputEnabled(bool enabled) {
    if (m_inputEnabled == enabled) return;
    if (!enabled) m_dispatcher.cancelPointerCaptures();
    m_inputEnabled = enabled;
}

void WindowApp::allocateSHM(float width, float height) {
    const auto started = std::chrono::steady_clock::now();
    const uint32_t pixelWidth = toBufferPixels(width, m_bufferScale);
    const uint32_t pixelHeight = toBufferPixels(height, m_bufferScale);
    const uint32_t requestedCapacityWidth = toBufferPixels(
        std::max(width, m_backingWidth), m_bufferScale);
    const uint32_t requestedCapacityHeight = toBufferPixels(
        std::max(height, m_backingHeight), m_bufferScale);

    const bool canReuseMapping = m_shmPixels && m_shmFd >= 0 &&
        pixelWidth <= m_shmCapacityWidth && pixelHeight <= m_shmCapacityHeight;

    const size_t requestedCapacityPixels =
        static_cast<size_t>(requestedCapacityWidth) * requestedCapacityHeight;
    if (m_pixelBuffer.capacity() < requestedCapacityPixels) {
        m_pixelBuffer.reserve(requestedCapacityPixels);
    }
    m_pixelBuffer.resize(static_cast<size_t>(pixelWidth) * pixelHeight, 0xFF14161D);

    if (!canReuseMapping) {
        const size_t nextShmSize = requestedCapacityPixels * sizeof(uint32_t);
        int nextShmFd = memfd_create("lcl_ui_app_shm", MFD_CLOEXEC);
        uint32_t* nextShmPixels = nullptr;
        if (nextShmFd >= 0 && ftruncate(nextShmFd, nextShmSize) == 0) {
            nextShmPixels = reinterpret_cast<uint32_t*>(
                mmap(nullptr, nextShmSize, PROT_READ | PROT_WRITE,
                     MAP_SHARED, nextShmFd, 0));
            if (nextShmPixels == MAP_FAILED) nextShmPixels = nullptr;
        }
        if (!nextShmPixels) {
            if (nextShmFd >= 0) close(nextShmFd);
            // Do not publish the previous mapping with dimensions that exceed
            // its stride/capacity. Keeping it would make the next attach both
            // invalid and impossible to recover from presentation pacing.
            if (pixelWidth > m_shmCapacityWidth ||
                pixelHeight > m_shmCapacityHeight) {
                if (m_shmPixels) munmap(m_shmPixels, m_shmSize);
                if (m_shmFd >= 0) close(m_shmFd);
                m_shmPixels = nullptr;
                m_shmFd = -1;
                m_shmSize = 0;
                m_shmCapacityWidth = 0;
                m_shmCapacityHeight = 0;
            }
            std::cerr << "[lcl-ui ERROR] Failed to allocate retained SHM backing for "
                      << m_title << "\n";
        } else {
            std::fill_n(nextShmPixels, requestedCapacityPixels, 0xFF14161D);
            if (m_shmPixels) munmap(m_shmPixels, m_shmSize);
            if (m_shmFd >= 0) close(m_shmFd);
            m_shmPixels = nextShmPixels;
            m_shmFd = nextShmFd;
            m_shmSize = nextShmSize;
            m_shmCapacityWidth = requestedCapacityWidth;
            m_shmCapacityHeight = requestedCapacityHeight;
            m_shmNeedsAttach = true;
        }
    }

    // Resize path: keep existing renderer instance and only retarget the backing pixels.
    // Re-initializing renderer every configure event causes heavy stalls while dragging.
    m_canvas->setTargetPixels(m_pixelBuffer.data(), pixelWidth, pixelHeight);
    updateCanvasRenderTarget();
    if (m_frameTraceEnabled) {
        ++m_traceShmAllocations;
        m_traceShmMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
    }
}

bool WindowApp::connectCompositor(const std::string& socketPath) {
    if (m_appId.empty()) {
        std::cerr << "[lcl-ui ERROR] WindowApp requires a canonical app ID before connection\n";
        return false;
    }
    std::string effectiveSocketPath = socketPath;
    if (const char* envSocket = std::getenv("LCL_COMPOSITOR_SOCKET")) {
        if (envSocket[0] != '\0') {
            effectiveSocketPath = envSocket;
        }
    }
    for (int i = 0; i < 50; ++i) {
        m_socketFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (m_socketFd >= 0) {
            struct sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, effectiveSocketPath.c_str(), sizeof(addr.sun_path) - 1);
            if (connect(m_socketFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
                break;
            }
            close(m_socketFd);
            m_socketFd = -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (m_socketFd < 0) {
        std::cerr << "[lcl-ui ERROR] Could not connect to compositor IPC socket: " << effectiveSocketPath << "\n";
        return false;
    }
    m_compositorSocketPath = effectiveSocketPath;
    m_surfaceEnded = false;

    // The compositor is the output-scale authority. Initial creation is purely
    // logical; ConfigureBounds supplies the buffer mapping scale.
    m_bufferScale = 1.0f;
    updateCanvasRenderTarget();

    // Set non-blocking socket reads
    int flags = fcntl(m_socketFd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(m_socketFd, F_SETFL, flags | O_NONBLOCK);
    }

    // 1. System-surface policy is declared separately. The compositor verifies
    // the declaration against the trusted shell peer; normal clients have no
    // role-selection protocol.
    if (m_canvas->supportsNativeBufferTransport(
            lcl::graphics::NativeBufferTransport::AndroidHardwareBufferV1)) {
        lcl::protocol::LCLMsgQueryCapabilities capabilityQuery{};
        capabilityQuery.requested = lcl::protocol::LCL_CAPABILITY_AHB_V1;
        if (!sendProtocolMessage(lcl::protocol::LCLOpcode::QueryCapabilities,
                                 &capabilityQuery, sizeof(capabilityQuery))) {
            std::cerr << "[lcl-ui ERROR] Failed to query compositor capabilities\n";
            return false;
        }
    }

    if (!isPopupSurface() &&
        m_systemSurfaceKind != lcl::protocol::LCLSystemSurfaceKind::None) {
        lcl::protocol::LCLMsgSetSystemSurfaceKind systemSurface{};
        systemSurface.kind = m_systemSurfaceKind;
        if (!sendProtocolMessage(lcl::protocol::LCLOpcode::SetSystemSurfaceKind,
                                 &systemSurface, sizeof(systemSurface))) {
            std::cerr << "[lcl-ui ERROR] Failed to declare system-surface policy\n";
            return false;
        }
    }

    // 2. Request normal or parent-bound popup surface creation. Both continue
    // through the same configure, buffer transport, render, and input paths.
    m_waitingForInitialConfigure = true;
    bool createSent = false;
    if (isPopupSurface()) {
        lcl::protocol::LCLMsgPopupSurfaceCreate popup{};
        popup.surfaceId = m_surfaceId;
        popup.parentSurfaceId = m_popupParentSurfaceId;
        popup.role = m_popupRole;
        popup.x = m_popupX;
        popup.y = m_popupY;
        popup.width = m_width;
        popup.height = m_height;
        createSent = sendProtocolMessage(lcl::protocol::LCLOpcode::PopupSurfaceCreate,
                                         &popup, sizeof(popup));
    } else {
        lcl::protocol::LCLMsgSurfaceCreate surface{};
        surface.surfaceId = m_surfaceId;
        surface.x = m_initialX;
        surface.y = m_initialY;
        surface.width = m_width;
        surface.height = m_height;
        surface.resizePresentation = m_resizePresentationMode;
        std::strncpy(surface.title, m_title.c_str(), sizeof(surface.title) - 1);
        std::strncpy(surface.appId, m_appId.c_str(), sizeof(surface.appId) - 1);
        createSent = sendProtocolMessage(lcl::protocol::LCLOpcode::SurfaceCreate,
                                         &surface, sizeof(surface));
    }
    if (!createSent) {
        m_waitingForInitialConfigure = false;
        std::cerr << "[lcl-ui ERROR] Failed to create v15 surface\n";
        return false;
    }

    m_ipcConnected = true;
    m_ownsSocketFd = true;
    m_canvas->setDmaBufTransportEnabled(true);
    m_backingWidth = m_width;
    m_backingHeight = m_height;
    if (!m_canvas->hasDmaBufTransport() ||
        !m_canvas->configureDmaBufFrame(getPixelWidth(), getPixelHeight(),
                                        getPixelWidth(), getPixelHeight())) {
        // CPU storage is prepared only when GPU transport is genuinely absent.
        m_canvas->setDmaBufTransportEnabled(false);
        allocateSHM(m_width, m_height);
    }
    // Decoration state belongs to the surface contract, not to a later frame.
    // Send preconfigured values before the first effect graph/buffer attach so a
    // CSD client never flashes the compositor's default title chrome.
    if (!isPopupSurface() && m_hasRequestedDecorationMode) {
        setDecorationMode(m_requestedDecorationMode);
    }
    if (!isPopupSurface() && m_hasRequestedEdgeToEdge) {
        setEdgeToEdge(m_requestedEdgeToEdge);
    }
    if (m_hasRequestedCornerRadius) {
        setWindowCornerStyle(m_requestedCornerRadius, m_requestedCornerRoundness);
    }
    if (m_windowRoot) m_windowRoot->markDirty();
    m_firstFrame = true;

    std::cout << "[lcl-ui] Connected to Compositor IPC socket successfully (" << m_title << ").\n";
    return true;
}

void WindowApp::resize(float width, float height) {
    if (width == 0 || height == 0) return;
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;
    clearMorphCrossfade();

    if (m_windowRoot) {
        m_windowRoot->getYogaNode().setWidth(static_cast<float>(width));
        m_windowRoot->getYogaNode().setHeight(static_cast<float>(height));
        m_windowRoot->markDirty();
    }

    if (m_ipcConnected) {
        const uint32_t backingPixelWidth = toBufferPixels(
            std::max(width, m_backingWidth), m_bufferScale);
        const uint32_t backingPixelHeight = toBufferPixels(
            std::max(height, m_backingHeight), m_bufferScale);
        if (m_canvas->hasDmaBufTransport()) {
            if (!m_canvas->configureDmaBufFrame(getPixelWidth(), getPixelHeight(),
                                                backingPixelWidth, backingPixelHeight)) {
                m_canvas->setDmaBufTransportEnabled(false);
                allocateSHM(width, height);
            }
        } else {
            allocateSHM(width, height);
        }
        // Defer render+attach to the main loop's renderFrame() so each resize tick
        // produces at most one frame and one attach commit.
        m_firstFrame = true;
    }

    if (m_onResize) {
        m_onResize(width, height);
    }
}

void WindowApp::setInitialBounds(float x, float y, float width, float height) {
    if (m_ipcConnected || width == 0 || height == 0) return;

    m_initialX = x;
    m_initialY = y;
    m_width = width;
    m_height = height;
    const uint32_t pixelWidth = toBufferPixels(width, m_bufferScale);
    const uint32_t pixelHeight = toBufferPixels(height, m_bufferScale);
    m_pixelBuffer.resize(static_cast<size_t>(pixelWidth) * pixelHeight, 0xFF000000);
    m_canvas->setTargetPixels(m_pixelBuffer.data(), pixelWidth, pixelHeight);
    updateCanvasRenderTarget();
    if (m_windowRoot) {
        m_windowRoot->getYogaNode().setWidth(static_cast<float>(width));
        m_windowRoot->getYogaNode().setHeight(static_cast<float>(height));
        m_windowRoot->markDirty();
    }
}

void WindowApp::configurePopupSurface(uint32_t parentSurfaceId,
                                      lcl::protocol::LCLPopupRole role,
                                      float x, float y) {
    if (m_ipcConnected || parentSurfaceId == 0) return;
    m_popupParentSurfaceId = parentSurfaceId;
    m_popupRole = role;
    m_popupX = x;
    m_popupY = y;
}

void WindowApp::pollIPC() {
    if (!m_ipcConnected || m_socketFd < 0) return;

    float latestWidth = 0.0f;
    float latestHeight = 0.0f;
    float latestBackingWidth = 0.0f;
    float latestBackingHeight = 0.0f;
    float latestScale = m_bufferScale;
    lcl::protocol::LCLConfigureResizeReason latestResizeReason =
        lcl::protocol::LCLConfigureResizeReason::Initial;
    bool pendingResize = false;
    bool receivedInitialConfigure = false;

    while (true) {
        lcl::protocol::LCLHeader header{};
        std::vector<uint8_t> payload;
        int receivedFd = -1;
        const auto receiveStatus =
            lcl::protocol::recvPacketWithFd(m_socketFd, header, payload, receivedFd);
        if (receiveStatus == lcl::protocol::ReceiveStatus::Received) {
            bool receivedFdConsumed = false;
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
                    const auto source = (inputMsg->source == static_cast<uint8_t>(lcl::protocol::LCLPointerSource::Touch))
                        ? PointerSource::Touch
                        : PointerSource::Mouse;
                    sendPointerMove(inputMsg->x, inputMsg->y, source);
                } else if (inputMsg->type == 4) { // PointerButton
                    const auto source = (inputMsg->source == static_cast<uint8_t>(lcl::protocol::LCLPointerSource::Touch))
                        ? PointerSource::Touch
                        : PointerSource::Mouse;
                    if (inputMsg->pressed) {
                        sendPointerDown(inputMsg->x, inputMsg->y, inputMsg->key, source);
                    } else {
                        sendPointerUp(inputMsg->x, inputMsg->y, inputMsg->key, source);
                    }
                } else if (inputMsg->type == 6) { // PointerScroll
                    const auto source = (inputMsg->source == static_cast<uint8_t>(lcl::protocol::LCLPointerSource::Touch))
                        ? PointerSource::Touch
                        : PointerSource::Mouse;
                    sendPointerScroll(inputMsg->x, inputMsg->y, inputMsg->deltaX, inputMsg->deltaY, source);
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
                       payload.size() == sizeof(lcl::protocol::LCLMsgConfigureBounds)) {
                auto* cfg = reinterpret_cast<const lcl::protocol::LCLMsgConfigureBounds*>(payload.data());
                if (cfg->surfaceId != m_surfaceId) continue;
                if (cfg->width > 0 && cfg->height > 0) {
                    if (m_frameTraceEnabled) {
                        ++m_traceConfigureCount;
                        if (cfg->resizeReason == lcl::protocol::LCLConfigureResizeReason::Interactive) {
                            ++m_traceInteractiveConfigureCount;
                        } else if (cfg->resizeReason == lcl::protocol::LCLConfigureResizeReason::WindowStateTransition) {
                            ++m_traceTransitionConfigureCount;
                        }
                    }
                    latestWidth = cfg->width;
                    latestHeight = cfg->height;
                    latestBackingWidth = cfg->backingWidth;
                    latestBackingHeight = cfg->backingHeight;
                    latestScale = cfg->bufferScale;
                    latestResizeReason = cfg->resizeReason;
                    m_pendingConfigureSerial = cfg->configureSerial;
                    receivedInitialConfigure = receivedInitialConfigure || m_waitingForInitialConfigure;
                    pendingResize = true;
                }
            } else if (header.opcode == lcl::protocol::LCLOpcode::Capabilities &&
                       payload.size() == sizeof(lcl::protocol::LCLMsgCapabilities)) {
                const auto* capabilities = reinterpret_cast<
                    const lcl::protocol::LCLMsgCapabilities*>(payload.data());
                if ((capabilities->supported & lcl::protocol::LCL_CAPABILITY_AHB_V1) != 0 &&
                    receivedFd >= 0) {
                    if (m_nativeBufferSocketFd >= 0) close(m_nativeBufferSocketFd);
                    m_nativeBufferSocketFd = receivedFd;
                    receivedFdConsumed = true;
                } else if (m_canvas->hasDmaBufTransport()) {
#if defined(__ANDROID__)
                    m_canvas->setDmaBufTransportEnabled(false);
                    allocateSHM(m_width, m_height);
#endif
                }
            } else if (header.opcode == lcl::protocol::LCLOpcode::ReleaseDmaBuf &&
                       payload.size() == sizeof(lcl::protocol::LCLMsgReleaseDmaBuf)) {
                const auto* release = reinterpret_cast<const lcl::protocol::LCLMsgReleaseDmaBuf*>(payload.data());
                if (release->surfaceId == m_surfaceId) {
                    receivedFdConsumed = m_canvas->releaseDmaBufFrameWithFence(
                        release->bufferId, receivedFd);
                    if (release->bufferId == m_submittedDmaBufId) {
                        // The compositor returned the frame before presenting
                        // it (for example after a genuine transaction cancel).
                        // Return the presentation credit so pending damage can
                        // produce the newest frame instead of remaining gated.
                        m_submittedConfigureSerial = 0;
                        m_submittedDmaBufId = 0;
                        m_frameGateOpen = true;
                        m_firstFrame = true;
                        if (m_frameTraceEnabled) ++m_traceRejectedDmaBufFrames;
                    }
                }
            } else if (header.opcode == lcl::protocol::LCLOpcode::FramePresented &&
                       payload.size() == sizeof(lcl::protocol::LCLMsgFramePresented)) {
                const auto* presented = reinterpret_cast<const lcl::protocol::LCLMsgFramePresented*>(payload.data());
                if (presented->surfaceId == m_surfaceId) {
                    m_lastPresentedTimestampNs = presented->timestampNs;
                    m_refreshIntervalNs = presented->refreshIntervalNs;
                    m_submittedConfigureSerial = 0;
                    m_submittedDmaBufId = 0;
                    m_frameGateOpen = true;
                    if (m_frameTraceEnabled) ++m_tracePresentedFrames;
                }
            } else if (header.opcode == lcl::protocol::LCLOpcode::FrameDiscarded &&
                       payload.size() == sizeof(lcl::protocol::LCLMsgFrameDiscarded)) {
                const auto* discarded = reinterpret_cast<
                    const lcl::protocol::LCLMsgFrameDiscarded*>(payload.data());
                if (discarded->surfaceId == m_surfaceId &&
                    discarded->configureSerial == m_submittedConfigureSerial) {
                    // A newer configure overtook this SHM frame. It can never
                    // be presented, so restore the single-frame credit and
                    // repaint using the newest configure serial.
                    m_submittedConfigureSerial = 0;
                    m_submittedDmaBufId = 0;
                    m_frameGateOpen = true;
                    m_firstFrame = true;
                }
            } else if (header.opcode == lcl::protocol::LCLOpcode::AckResponse &&
                       payload.size() == sizeof(lcl::protocol::LCLMsgAckResponse)) {
                const auto* ack = reinterpret_cast<const lcl::protocol::LCLMsgAckResponse*>(payload.data());
                if (ack->status == 4 &&
                    std::strncmp(ack->message, "DMA-BUF import unavailable",
                                 sizeof(ack->message)) == 0 &&
                    m_canvas->hasDmaBufTransport()) {
                    m_canvas->setDmaBufTransportEnabled(false);
                    allocateSHM(m_width, m_height);
                    m_firstFrame = true;
                    m_submittedConfigureSerial = 0;
                    m_submittedDmaBufId = 0;
                    m_frameGateOpen = true;
                }
            } else if (header.opcode == lcl::protocol::LCLOpcode::SurfaceDestroy) {
                if (payload.size() >= sizeof(lcl::protocol::LCLMsgSurfaceDestroy)) {
                    auto* destroy = reinterpret_cast<const lcl::protocol::LCLMsgSurfaceDestroy*>(payload.data());
                    if (destroy->surfaceId == m_surfaceId) {
                        m_running = false;
                        m_surfaceEnded = true;
                    }
                }
            }
            if (receivedFd >= 0 && !receivedFdConsumed) close(receivedFd);
            if (m_onIpcMessage) m_onIpcMessage(header, payload);
        } else if (receiveStatus == lcl::protocol::ReceiveStatus::WouldBlock) {
            break;
        } else {
            std::cerr << "[lcl-ui ERROR] Compositor v15 connection closed or rejected\n";
            m_ipcConnected = false;
            m_running = false;
            m_surfaceEnded = true;
            break;
        }
    }

    if (pendingResize && (latestWidth > 0 && latestHeight > 0)) {
        if (m_resizeTransform) {
            const auto [transformedWidth, transformedHeight] =
                m_resizeTransform(latestWidth, latestHeight, latestResizeReason);
            latestWidth = transformedWidth;
            latestHeight = transformedHeight;
            if (latestWidth == 0 || latestHeight == 0) {
                pendingResize = false;
            }
        }

        m_backingWidth = std::max(latestWidth, latestBackingWidth);
        m_backingHeight = std::max(latestHeight, latestBackingHeight);
        m_liveResizeFramePacing =
            m_resizePresentationMode == lcl::protocol::LCLResizePresentationMode::Live &&
            (latestResizeReason == lcl::protocol::LCLConfigureResizeReason::Interactive ||
             latestResizeReason == lcl::protocol::LCLConfigureResizeReason::WindowStateTransition);

        latestScale = sanitizeBufferScale(latestScale);
        if (std::fabs(latestScale - m_bufferScale) > 0.0001f) {
            m_bufferScale = latestScale;
            updateCanvasRenderTarget();
            // A scale-only configure has identical logical bounds but needs a new buffer.
            if (latestWidth == m_width && latestHeight == m_height && m_ipcConnected) {
                if (m_canvas->hasDmaBufTransport()) {
                    m_canvas->configureDmaBufFrame(
                        getPixelWidth(), getPixelHeight(),
                        toBufferPixels(std::max(m_width, m_backingWidth), m_bufferScale),
                        toBufferPixels(std::max(m_height, m_backingHeight), m_bufferScale));
                } else {
                    allocateSHM(m_width, m_height);
                }
                m_firstFrame = true;
            }
        }
        m_pendingResizeWidth = latestWidth;
        m_pendingResizeHeight = latestHeight;
        m_hasPendingResize = true;
    }

    // Throttled resize apply: demos update every pixel while dragging; applying each
    // configure causes SHM recreate storms. Keep latest target and apply at a bounded rate.
    if (m_hasPendingResize && (!m_liveResizeFramePacing || m_frameGateOpen) &&
        (m_pendingResizeWidth != m_width || m_pendingResizeHeight != m_height)) {
        const auto now = std::chrono::steady_clock::now();
        constexpr auto kMinResizeInterval = std::chrono::milliseconds(22);

        const float dx = std::fabs(m_pendingResizeWidth - m_width);
        const float dy = std::fabs(m_pendingResizeHeight - m_height);

        const bool largeJump = (dx >= 48.0f) || (dy >= 48.0f);
        const bool intervalElapsed = (now - m_lastResizeApply) >= kMinResizeInterval;

        // The first configure establishes the only serial eligible for an
        // initial commit. It must never wait for interactive-resize throttling,
        // even when the compositor adjusted the requested bounds by a few px.
        if (m_liveResizeFramePacing || receivedInitialConfigure || largeJump || intervalElapsed) {
            m_configureSerial = m_pendingConfigureSerial;
            resize(m_pendingResizeWidth, m_pendingResizeHeight);
            m_waitingForInitialConfigure = false;
            if (m_frameTraceEnabled) ++m_traceResizeApplies;
            m_hasPendingResize = false;
            m_lastResizeApply = now;
        }
    } else if (m_hasPendingResize && (!m_liveResizeFramePacing || m_frameGateOpen)) {
        // The compositor often confirms the initial logical size verbatim.
        // It requires no SHM reallocation, but it still needs one commit to
        // acknowledge the configure serial and release compositor backpressure.
        m_hasPendingResize = false;
        m_configureSerial = m_pendingConfigureSerial;
        m_waitingForInitialConfigure = false;
        if (m_canvas->hasDmaBufTransport()) {
            m_canvas->configureDmaBufFrame(
                getPixelWidth(), getPixelHeight(),
                toBufferPixels(std::max(m_width, m_backingWidth), m_bufferScale),
                toBufferPixels(std::max(m_height, m_backingHeight), m_bufferScale));
        }
        m_firstFrame = true;
    }
}

void WindowApp::runEventLoop() {
    m_running = true;

    while (m_running && !g_appSignalReceived.load()) {
        auto frameStart = std::chrono::high_resolution_clock::now();

        bool rendered = tick();

        if (rendered && !m_liveResizeFramePacing) {
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
        lcl::protocol::discardPendingWrites(m_socketFd);
        close(m_socketFd);
        m_socketFd = -1;
    }
    if (m_nativeBufferSocketFd >= 0) {
        close(m_nativeBufferSocketFd);
        m_nativeBufferSocketFd = -1;
    }
}

bool WindowApp::tick() {
    m_transients.pruneExpiredOwners();
    pollIPC();
    if (m_onFrame) {
        m_onFrame();
    }
    const auto now = std::chrono::steady_clock::now();
    const float dtSec = std::chrono::duration<float>(now - m_lastAnimationTick).count();
    m_lastAnimationTick = now;
    advanceAnimations(dtSec);
    bool rendered = renderFrame();
    m_tickingHostedSurfaces = true;
    for (auto& entry : m_hostedSurfaces) {
        if (!entry.pendingRemoval && entry.surface) {
            rendered = entry.surface->tick() || rendered;
            if (entry.surface->m_surfaceEnded) entry.pendingRemoval = true;
        }
    }
    m_tickingHostedSurfaces = false;
    collectClosedHostedSurfaces();
    logFrameTraceIfDue();
    return rendered;
}

void WindowApp::collectClosedHostedSurfaces() {
    for (size_t index = 0; index < m_hostedSurfaces.size();) {
        if (!m_hostedSurfaces[index].pendingRemoval) {
            ++index;
            continue;
        }
        auto surface = std::move(m_hostedSurfaces[index].surface);
        auto onClosed = std::move(m_hostedSurfaces[index].onClosed);
        m_hostedSurfaces.erase(m_hostedSurfaces.begin() +
                               static_cast<std::ptrdiff_t>(index));
        surface.reset();
        if (onClosed) onClosed();
    }
}

namespace {
void collectWidgetBounds(Widget* widget, std::unordered_map<uint64_t, graphics::RectF>& bounds) {
    if (!widget) return;
    bounds[widget->getObjectId()] = widget->getAbsoluteBounds();
    for (const auto& child : widget->getChildren()) collectWidgetBounds(child.get(), bounds);
}

void startMorphs(Widget* widget, const std::unordered_map<uint64_t, graphics::RectF>& oldBounds,
                 MotionCoordinator& coordinator, const lcl::motion::Motion& motion,
                 bool& anyMorph) {
    if (!widget) return;
    const auto found = oldBounds.find(widget->getObjectId());
    if (found != oldBounds.end()) {
        const graphics::RectF before = found->second;
        const graphics::RectF after = widget->getAbsoluteBounds();
        if (!before.isEmpty() && !after.isEmpty() &&
            (std::fabs(before.x - after.x) > 0.01f || std::fabs(before.y - after.y) > 0.01f ||
             std::fabs(before.width - after.width) > 0.01f || std::fabs(before.height - after.height) > 0.01f)) {
            anyMorph = true;
            const PresentationState finalPresentation = widget->getPresentationState();
            const float startX = finalPresentation.translationX + before.x - after.x;
            const float startY = finalPresentation.translationY + before.y - after.y;
            const float startScaleX = finalPresentation.scaleX *
                before.width / std::max(0.001f, after.width);
            const float startScaleY = finalPresentation.scaleY *
                before.height / std::max(0.001f, after.height);
            widget->applyPresentationValue(AnimatableProperty::TranslationX, startX);
            widget->applyPresentationValue(AnimatableProperty::TranslationY, startY);
            widget->applyPresentationValue(AnimatableProperty::ScaleX, startScaleX);
            widget->applyPresentationValue(AnimatableProperty::ScaleY, startScaleY);
            coordinator.animateFloat(*widget, AnimatableProperty::TranslationX, startX,
                finalPresentation.translationX, motion,
                [widget](float value) { widget->applyPresentationValue(AnimatableProperty::TranslationX, value); });
            coordinator.animateFloat(*widget, AnimatableProperty::TranslationY, startY,
                finalPresentation.translationY, motion,
                [widget](float value) { widget->applyPresentationValue(AnimatableProperty::TranslationY, value); });
            coordinator.animateFloat(*widget, AnimatableProperty::ScaleX, startScaleX,
                finalPresentation.scaleX, motion,
                [widget](float value) { widget->applyPresentationValue(AnimatableProperty::ScaleX, value); });
            coordinator.animateFloat(*widget, AnimatableProperty::ScaleY, startScaleY,
                finalPresentation.scaleY, motion,
                [widget](float value) { widget->applyPresentationValue(AnimatableProperty::ScaleY, value); });
        }
    }
    for (const auto& child : widget->getChildren()) startMorphs(child.get(), oldBounds, coordinator, motion, anyMorph);
}
} // namespace

void WindowApp::animate(const lcl::motion::Motion& motion,
                        AnimationTransactionOptions options,
                        const std::function<void()>& changes) {
    if (!changes) return;
    std::unordered_map<uint64_t, graphics::RectF> oldBounds;
    std::vector<uint32_t> oldPixels;
    uint32_t snapshotWidth = 0;
    uint32_t snapshotHeight = 0;
    if (options.layout == LayoutMode::Morph) {
        // Flush pending old-state damage before copying the backing raster. The
        // snapshot owns its pixels, so later SHM reuse and resize cannot mutate it.
        renderFrame();
        snapshotWidth = getPixelWidth();
        snapshotHeight = getPixelHeight();
        if (const auto* pixels = m_canvas->rasterBuffer()) {
            const size_t pixelCount = static_cast<size_t>(snapshotWidth) * snapshotHeight;
            oldPixels.assign(pixels, pixels + pixelCount);
        }
        collectWidgetBounds(m_windowRoot.get(), oldBounds);
    }
    m_motionCoordinator.beginTransaction(motion, options);
    try {
        changes();
    } catch (...) {
        m_motionCoordinator.endTransaction();
        throw;
    }
    m_motionCoordinator.endTransaction();
    if (options.layout == LayoutMode::Morph) {
        updateLayout();
        bool anyMorph = false;
        startMorphs(m_windowRoot.get(), oldBounds, m_motionCoordinator, motion, anyMorph);
        startMorphCrossfade(std::move(oldPixels), snapshotWidth, snapshotHeight, motion);
        m_morphInputFrozen = anyMorph || m_morphBlendEngine.hasActiveAnimations();
        if (m_morphInputFrozen) m_dispatcher.cancelPointerCaptures();
    }
}

bool WindowApp::advanceAnimations(float dtSec) {
    const bool motionActive = m_motionCoordinator.tick(dtSec);
    const bool morphBlendActive = advanceMorphCrossfade(dtSec);
    if (m_morphInputFrozen && !motionActive && !morphBlendActive) m_morphInputFrozen = false;
    return motionActive || morphBlendActive;
}

bool WindowApp::hasActiveAnimations() const noexcept {
    return m_motionCoordinator.hasActiveAnimations() || m_morphBlendEngine.hasActiveAnimations();
}

void WindowApp::startMorphCrossfade(std::vector<uint32_t> snapshot,
                                    uint32_t pixelWidth, uint32_t pixelHeight,
                                    const lcl::motion::Motion& motion) {
    clearMorphCrossfade();
    const size_t expectedPixels = static_cast<size_t>(pixelWidth) * pixelHeight;
    if (snapshot.size() != expectedPixels || pixelWidth != getPixelWidth() ||
        pixelHeight != getPixelHeight()) {
        return;
    }

    m_morphSnapshotPixels = std::move(snapshot);
    m_morphSnapshotWidth = pixelWidth;
    m_morphSnapshotHeight = pixelHeight;
    m_morphBlendProgress = 0.0f;
    m_morphBlendChannel = m_morphBlendEngine.createChannel({1u, 1u}, 0.0f);
    m_morphBlendEngine.animateTo(m_morphBlendChannel, 1.0f, motion);
    const auto sample = m_morphBlendEngine.sample(m_morphBlendChannel);
    m_morphBlendProgress = std::clamp(sample.value, 0.0f, 1.0f);
    if (!sample.active) {
        clearMorphCrossfade();
        return;
    }
    m_renderPass.addDirtyRect({0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height)});
}

bool WindowApp::advanceMorphCrossfade(float dtSec) {
    if (m_morphBlendChannel == 0) return false;
    const auto changed = m_morphBlendEngine.tick(dtSec);
    const auto sample = m_morphBlendEngine.sample(m_morphBlendChannel);
    m_morphBlendProgress = std::clamp(sample.value, 0.0f, 1.0f);
    if (!changed.empty() || sample.active) {
        m_renderPass.addDirtyRect({0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height)});
    }
    if (!sample.active) {
        clearMorphCrossfade();
        m_renderPass.addDirtyRect({0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height)});
        return false;
    }
    return true;
}

void WindowApp::blendMorphSnapshot() {
    if (m_morphSnapshotPixels.empty() || m_morphBlendChannel == 0) return;
    if (m_morphSnapshotWidth != getPixelWidth() || m_morphSnapshotHeight != getPixelHeight()) {
        clearMorphCrossfade();
        return;
    }
    auto* newPixels = m_canvas->rasterBuffer();
    if (!newPixels) {
        clearMorphCrossfade();
        return;
    }
    const size_t pixelCount = static_cast<size_t>(m_morphSnapshotWidth) * m_morphSnapshotHeight;
    for (size_t index = 0; index < pixelCount; ++index) {
        newPixels[index] = crossfadePixel(m_morphSnapshotPixels[index], newPixels[index],
                                         m_morphBlendProgress);
    }
}

void WindowApp::clearMorphCrossfade() {
    m_morphBlendEngine.clearAll();
    m_morphBlendChannel = 0;
    m_morphSnapshotPixels.clear();
    m_morphSnapshotWidth = 0;
    m_morphSnapshotHeight = 0;
    m_morphBlendProgress = 1.0f;
}

void WindowApp::setExternalIpcSocket(int socketFd) {
    if (socketFd < 0) return;
    m_socketFd = socketFd;
    m_ipcConnected = true;
    m_surfaceEnded = false;
    m_ownsSocketFd = false;
    m_canvas->setDmaBufTransportEnabled(true);
}

bool WindowApp::requestWindowAction(lcl::protocol::LCLWindowAction action,
                                    float localX, float localY) {
    if (!m_ipcConnected || m_socketFd < 0 || isPopupSurface()) return false;

    lcl::protocol::LCLMsgRequestWindowAction msg{};
    msg.surfaceId = m_surfaceId;
    msg.action = action;
    msg.localX = localX;
    msg.localY = localY;

    return sendProtocolMessage(lcl::protocol::LCLOpcode::RequestWindowAction,
                               &msg, sizeof(msg));
}

bool WindowApp::requestSurfaceDestroy(uint32_t surfaceId) {
    if (!m_ipcConnected || m_socketFd < 0 || surfaceId == 0) return false;
    lcl::protocol::LCLMsgRequestSurfaceClose close{};
    close.surfaceId = surfaceId;
    return sendProtocolMessage(lcl::protocol::LCLOpcode::RequestSurfaceClose,
                               &close, sizeof(close));
}

bool WindowApp::sendProtocolMessage(lcl::protocol::LCLOpcode opcode, const void* payload,
                                    uint32_t payloadSize, int passedFd) {
    if (m_socketFd < 0) return false;
    lcl::protocol::LCLHeader header{};
    header.opcode = opcode;
    header.requestId = m_nextRequestId++;
    if (m_nextRequestId == 0) m_nextRequestId = 1;
    header.payloadSize = payloadSize;
    return lcl::protocol::sendMsgWithFd(m_socketFd, header, payload, passedFd);
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
    if (isPopupSurface()) {
        return requestSurfaceDestroy(m_surfaceId);
    }
    return requestWindowAction(lcl::protocol::LCLWindowAction::Close);
}

bool WindowApp::setDecorationMode(lcl::protocol::LCLDecorationMode mode) {
    m_requestedDecorationMode = mode;
    m_hasRequestedDecorationMode = true;
    if (!m_ipcConnected || m_socketFd < 0) return true;

    lcl::protocol::LCLMsgSetDecorationMode msg{};
    msg.surfaceId = m_surfaceId;
    msg.mode = mode;

    return sendProtocolMessage(lcl::protocol::LCLOpcode::SetDecorationMode,
                               &msg, sizeof(msg));
}

bool WindowApp::setEdgeToEdge(bool enabled) {
    m_requestedEdgeToEdge = enabled;
    m_hasRequestedEdgeToEdge = true;
    if (!m_ipcConnected || m_socketFd < 0) return true;

    lcl::protocol::LCLMsgSetEdgeToEdge msg{};
    msg.surfaceId = m_surfaceId;
    msg.enabled = enabled ? 1 : 0;
    return sendProtocolMessage(lcl::protocol::LCLOpcode::SetEdgeToEdge,
                               &msg, sizeof(msg));
}

bool WindowApp::setWindowLayer(lcl::protocol::LCLWindowLayer layer, bool unfocusable) {
    if (!m_ipcConnected || m_socketFd < 0) return false;

    lcl::protocol::LCLMsgSetWindowLayer msg{};
    msg.surfaceId = m_surfaceId;
    msg.layer = layer;
    msg.unfocusable = unfocusable ? 1 : 0;
    return sendProtocolMessage(lcl::protocol::LCLOpcode::SetWindowLayer,
                               &msg, sizeof(msg));
}

bool WindowApp::setReservedZone(float top, float bottom, float left, float right) {
    if (!m_ipcConnected || m_socketFd < 0) return false;

    lcl::protocol::LCLMsgSetReservedZone msg{};
    msg.surfaceId = m_surfaceId;
    msg.top = top;
    msg.bottom = bottom;
    msg.left = left;
    msg.right = right;
    return sendProtocolMessage(lcl::protocol::LCLOpcode::SetReservedZone,
                               &msg, sizeof(msg));
}

bool WindowApp::setWindowCornerStyle(float radius, float roundness) {
    m_requestedCornerRadius = std::max(0.0f, radius);
    m_requestedCornerRoundness = std::clamp(roundness, 2.0f, 8.0f);
    m_hasRequestedCornerRadius = true;
    if (!m_ipcConnected || m_socketFd < 0) return true;

    lcl::protocol::LCLMsgSetWindowCornerStyle msg{};
    msg.surfaceId = m_surfaceId;
    msg.radius = m_requestedCornerRadius;
    msg.roundness = m_requestedCornerRoundness;

    return sendProtocolMessage(lcl::protocol::LCLOpcode::SetWindowCornerStyle,
                               &msg, sizeof(msg));
}

bool WindowApp::setWindowCornerRadius(float radius) {
    return setWindowCornerStyle(radius, m_requestedCornerRoundness);
}

bool WindowApp::sendPointerMove(float x, float y, PointerSource source, uint32_t pointerId) {
    if (m_morphInputFrozen) return false;
    PointerEvent ev{x, y, 0, 0.0f, 0.0f, PointerEventType::Move, source, pointerId};
    if (!m_dispatcher.hasPointerCapture(pointerId) &&
        m_onRawPointer && m_onRawPointer(ev)) {
        return true;
    }
    return m_dispatcher.dispatchPointerEvent(m_windowRoot.get(), &m_transients, ev);
}

bool WindowApp::sendPointerDown(float x, float y, int button, PointerSource source,
                                uint32_t pointerId) {
    if (m_morphInputFrozen) return false;
    PointerEvent ev{x, y, button, 0.0f, 0.0f, PointerEventType::Down, source, pointerId};
    if (m_onRawPointer && m_onRawPointer(ev)) {
        return true;
    }
    return m_dispatcher.dispatchPointerEvent(m_windowRoot.get(), &m_transients, ev);
}

bool WindowApp::sendPointerUp(float x, float y, int button, PointerSource source,
                              uint32_t pointerId) {
    if (m_morphInputFrozen) return false;
    PointerEvent ev{x, y, button, 0.0f, 0.0f, PointerEventType::Up, source, pointerId};
    if (!m_dispatcher.hasPointerCapture(pointerId) &&
        m_onRawPointer && m_onRawPointer(ev)) {
        return true;
    }
    return m_dispatcher.dispatchPointerEvent(m_windowRoot.get(), &m_transients, ev);
}

bool WindowApp::sendPointerCancel(float x, float y, PointerSource source,
                                  uint32_t pointerId) {
    PointerEvent ev{x, y, 0, 0.0f, 0.0f, PointerEventType::Cancel, source, pointerId};
    if (!m_dispatcher.hasPointerCapture(pointerId) &&
        m_onRawPointer && m_onRawPointer(ev)) {
        return true;
    }
    return m_dispatcher.dispatchPointerEvent(m_windowRoot.get(), &m_transients, ev);
}

bool WindowApp::sendPointerScroll(float x, float y, float deltaX, float deltaY, PointerSource source) {
    if (m_morphInputFrozen) return false;
    PointerEvent ev{x, y, 0, deltaX, deltaY, PointerEventType::Scroll, source};
    if (m_onRawPointer && m_onRawPointer(ev)) {
        return true;
    }
    return m_dispatcher.dispatchPointerEvent(m_windowRoot.get(), &m_transients, ev);
}

bool WindowApp::sendKeyDown(int keyCode, char32_t codepoint, uint8_t modifiers) {
    KeyEvent ev{static_cast<lcl::platform::PhysicalKey>(keyCode), keyCode, codepoint, modifiers, KeyEventType::KeyDown};
    if (m_onRawKey && m_onRawKey(ev)) {
        return true;
    }
    return m_dispatcher.dispatchKeyEvent(m_windowRoot.get(), ev);
}

bool WindowApp::sendKeyUp(int keyCode, uint8_t modifiers) {
    KeyEvent ev{static_cast<lcl::platform::PhysicalKey>(keyCode), keyCode, 0, modifiers, KeyEventType::KeyUp};
    if (m_onRawKey && m_onRawKey(ev)) {
        return true;
    }
    return m_dispatcher.dispatchKeyEvent(m_windowRoot.get(), ev);
}

bool WindowApp::sendTextInput(const std::string& text) {
    TextInputEvent ev{text};
    if (m_onRawTextInput && m_onRawTextInput(ev)) {
        return true;
    }
    return m_dispatcher.dispatchTextInputEvent(ev);
}

void WindowApp::updateLayout() {
    if (m_windowRoot) {
        const auto started = std::chrono::steady_clock::now();
        // Clear the request being serviced before calculation. A widget that
        // performs a genuine Yoga mutation from syncLayout() will set it again
        // and receive another layout pass on the next frame.
        m_windowRoot->clearLayoutDirty();
        m_windowRoot->getYogaNode().calculateLayout(static_cast<float>(m_width), static_cast<float>(m_height));
        m_windowRoot->syncLayout(0.0f, 0.0f);
        if (m_frameTraceEnabled) {
            ++m_traceLayoutPasses;
            m_traceLayoutMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
        }
    }
}

bool WindowApp::renderFrame() {
    // The compositor owns configure serial assignment. A buffer rendered from
    // SurfaceCreate's requested geometry has no valid serial and would be
    // discarded as stale when the initial ConfigureBounds is already queued.
    if (m_ipcConnected && m_waitingForInitialConfigure) return false;
    if (m_ipcConnected && !m_frameGateOpen) return false;

    if (m_windowRoot && m_windowRoot->isLayoutDirty()) {
        updateLayout();
        // Layout can move several siblings without each new bound producing a
        // paint invalidation. It is intentionally the infrequent full-window
        // invalidation path; presentation-only motion remains region-scoped.
        m_renderPass.addDirtyRect(m_windowRoot->getAbsoluteBounds());
    }

    // A resize can leave old-layout damage queued before Yoga computes the
    // new child positions. Always include the complete post-layout root extent
    // on the first frame so children moved outside the old damage are painted.
    if (m_firstFrame) {
        if (m_windowRoot) m_renderPass.addDirtyRect(m_windowRoot->getAbsoluteBounds());
    }
    if (!m_renderPass.hasDamage()) return false;
    m_firstFrame = false;

    const graphics::RectF surfaceBounds{0.0f, 0.0f, static_cast<float>(m_width),
                             static_cast<float>(m_height)};
    // Path coverage can extend one physical pixel past its analytic bounds.
    // Keep widget damage logical and add that backend sampling margin only at
    // the raster boundary, before clear, paint clipping and buffer copy.
    const float deviceScale = sanitizeBufferScale(
        m_canvas->renderTarget().deviceScale);
    const float rasterOutset = 1.0f / deviceScale;
    RenderPass rasterDamage;
    for (const graphics::RectF& dirty : m_renderPass.getDirtyRects()) {
        const graphics::RectF expanded{
            dirty.x - rasterOutset,
            dirty.y - rasterOutset,
            dirty.width + rasterOutset * 2.0f,
            dirty.height + rasterOutset * 2.0f,
        };
        const graphics::RectF clipped = expanded.intersection(surfaceBounds);
        if (!clipped.isEmpty()) rasterDamage.addDirtyRect(clipped);
    }
    std::vector<graphics::RectF> damageRects = rasterDamage.getDirtyRects();
    m_renderPass.clear();
    if (damageRects.empty()) return false;

    struct PhysicalDamageRect {
        uint32_t left{0};
        uint32_t top{0};
        uint32_t right{0};
        uint32_t bottom{0};
    };
    const uint32_t activePixelWidth = getPixelWidth();
    const uint32_t activePixelHeight = getPixelHeight();
    std::vector<PhysicalDamageRect> physicalDamage;
    physicalDamage.reserve(damageRects.size());
    uint32_t damageLeft = activePixelWidth;
    uint32_t damageTop = activePixelHeight;
    uint32_t damageRight = 0;
    uint32_t damageBottom = 0;
    for (const graphics::RectF& damage : damageRects) {
        PhysicalDamageRect rect{};
        rect.left = std::min(
            activePixelWidth, static_cast<uint32_t>(std::floor(
                std::max(0.0f, damage.x * m_bufferScale))));
        rect.top = std::min(
            activePixelHeight, static_cast<uint32_t>(std::floor(
                std::max(0.0f, damage.y * m_bufferScale))));
        rect.right = std::min(
            activePixelWidth, static_cast<uint32_t>(std::ceil(
                std::max(0.0f, (damage.x + damage.width) * m_bufferScale))));
        rect.bottom = std::min(
            activePixelHeight, static_cast<uint32_t>(std::ceil(
                std::max(0.0f, (damage.y + damage.height) * m_bufferScale))));
        if (rect.left >= rect.right || rect.top >= rect.bottom) continue;
        damageLeft = std::min(damageLeft, rect.left);
        damageTop = std::min(damageTop, rect.top);
        damageRight = std::max(damageRight, rect.right);
        damageBottom = std::max(damageBottom, rect.bottom);
        physicalDamage.push_back(rect);
    }
    m_canvas->beginFrame();
    if (m_canvas->isDmaBufFrameBlocked()) {
        // Keep the last compositor-owned DMA-BUF visible until a release
        // arrives. Requeue the exact damage instead of CPU-rendering it into
        // SHM during configure/transition pressure.
        for (const graphics::RectF& damage : damageRects) {
            m_renderPass.addDirtyRect(damage);
        }
        return false;
    }

    const auto paintStarted = std::chrono::steady_clock::now();
    if (m_frameTraceEnabled) {
        ++m_traceRenderedFrames;
        for (const graphics::RectF& damage : damageRects) {
            m_traceDamagePixels +=
                static_cast<uint64_t>(std::max(0.0f, damage.width)) *
                static_cast<uint64_t>(std::max(0.0f, damage.height));
        }
    }

    const auto clearStarted = paintStarted;
    const bool dmaBufFrame = m_canvas->isDmaBufFrameActive();
    for (const graphics::RectF& damage : damageRects) {
        m_canvas->clearRect(damage, {0, 0, 0, 0});
        if (m_frameTraceEnabled) {
            m_traceClearedBytes +=
                static_cast<uint64_t>(std::ceil(damage.width * m_bufferScale)) *
                static_cast<uint64_t>(std::ceil(damage.height * m_bufferScale)) *
                sizeof(uint32_t);
        }
    }
    if (m_frameTraceEnabled) {
        m_traceClearMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - clearStarted).count();
    }

    const auto drawStarted = std::chrono::steady_clock::now();
    m_renderPass.begin(*m_canvas);

    for (const graphics::RectF& damage : damageRects) {
        m_canvas->saveState();
        m_canvas->clipRect(damage);
        if (m_windowRoot && m_windowRoot->isVisible()) {
            m_windowRoot->draw(*m_canvas, damage);
        }
        if (m_layoutOverlayEnabled && m_windowRoot) {
            drawLayoutOverlay(*m_windowRoot, *m_canvas);
            m_canvas->drawRoundedRect(damage, 0.0f, {0, 0, 0, 0},
                                      {239, 68, 68, 255}, 1.0f, 2.0f);
        }
        m_canvas->restoreState();
    }

    std::vector<EffectRegion> uiEffects;
    if (m_windowRoot) m_windowRoot->collectEffects(uiEffects);

    m_renderPass.end(*m_canvas);
    m_canvas->endFrame();
    if (!dmaBufFrame) blendMorphSnapshot();

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
        auto toProtoBounds = [](EffectBounds bounds) {
            return bounds == EffectBounds::OuterSurface
                ? lcl::protocol::EffectBoundsPolicy::OuterSurface
                : lcl::protocol::EffectBoundsPolicy::Local;
        };

        std::vector<lcl::protocol::EffectRegion> protoRegions;
        std::vector<lcl::protocol::FilterOp> flatFilters;
        protoRegions.reserve(uiEffects.size());

        for (const auto& effect : uiEffects) {
            if (effect.bounds.isEmpty() || effect.filters.empty()) continue;

            const float x = std::max(0.0f, effect.bounds.x);
            const float y = std::max(0.0f, effect.bounds.y);
            if (x >= m_width || y >= m_height) continue;
            const float w = std::min(std::max(0.0f, effect.bounds.width), m_width - x);
            const float h = std::min(std::max(0.0f, effect.bounds.height), m_height - y);
            if (w <= 0.0f || h <= 0.0f) continue;

            lcl::protocol::EffectRegion region{};
            region.x = x;
            region.y = y;
            region.width = w;
            region.height = h;
            region.cornerRadius = std::max(0.0f, effect.cornerRadius);
            region.cornerRoundness = std::clamp(effect.cornerRoundness, 2.0f, 8.0f);
            region.boundsPolicy = toProtoBounds(effect.boundsPolicy);
            region.source = toProtoSource(effect.source);
            region.blendMode = toProtoBlend(effect.blend);
            region.opacity = std::clamp(effect.opacity, 0.0f, 1.0f);
            region.filterOffset = static_cast<uint32_t>(flatFilters.size());
            region.filterCount = static_cast<uint16_t>(std::min<size_t>(effect.filters.size(), 65535));

            for (uint16_t i = 0; i < region.filterCount; ++i) {
                auto filter = effect.filters[i];
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
                sendProtocolMessage(lcl::protocol::LCLOpcode::SetEffectGraph,
                                    payload.data(), static_cast<uint32_t>(payload.size()));
                m_lastEffectGraphPayload = std::move(payload);
            }
            m_effectGraphActive = true;
        } else if (m_effectGraphActive) {
            lcl::protocol::LCLMsgClearEffectGraph clearMsg{};
            clearMsg.surfaceId = m_surfaceId;

            sendProtocolMessage(lcl::protocol::LCLOpcode::ClearEffectGraph,
                                &clearMsg, sizeof(clearMsg));
            m_effectGraphActive = false;
            m_lastEffectGraphPayload.clear();
        }
    }
    if (m_frameTraceEnabled) {
        m_traceDrawMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - drawStarted).count();
    }

    // The SHM mapping is retained as well. Copy only changed scanline spans;
    // the first frame and resize paths already invalidate the full surface.
    const auto copyStarted = std::chrono::steady_clock::now();
    if (!dmaBufFrame && m_shmPixels && !m_pixelBuffer.empty()) {
        const uint32_t shmStridePixels = m_shmCapacityWidth > 0
            ? m_shmCapacityWidth : activePixelWidth;
        for (const PhysicalDamageRect& damage : physicalDamage) {
            const size_t rowBytes =
                static_cast<size_t>(damage.right - damage.left) * sizeof(uint32_t);
            for (uint32_t y = damage.top; y < damage.bottom; ++y) {
                const size_t sourceOffset =
                    static_cast<size_t>(y) * activePixelWidth + damage.left;
                const size_t destinationOffset =
                    static_cast<size_t>(y) * shmStridePixels + damage.left;
                if ((destinationOffset + (damage.right - damage.left)) *
                        sizeof(uint32_t) > m_shmSize) break;
                std::memcpy(m_shmPixels + destinationOffset,
                            m_pixelBuffer.data() + sourceOffset,
                            rowBytes);
            }
            if (m_frameTraceEnabled) {
                m_traceCopiedBytes += rowBytes * (damage.bottom - damage.top);
            }
        }
    }
    if (m_frameTraceEnabled) {
        m_traceCopyMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - copyStarted).count();
    }

    // If connected over IPC, notify compositor of buffer commit
    const auto attachStarted = std::chrono::steady_clock::now();
    if (m_ipcConnected && m_socketFd >= 0 && dmaBufFrame) {
        if (auto frame = m_canvas->takeDmaBufFrame()) {
            bool dmaBufAttached = false;
            if (frame->transport ==
                lcl::graphics::NativeBufferTransport::AndroidHardwareBufferV1) {
                lcl::protocol::LCLMsgAttachNativeBuffer attachMsg{};
                attachMsg.surfaceId = m_surfaceId;
                attachMsg.configureSerial = m_configureSerial;
                attachMsg.bufferId = frame->bufferId;
                attachMsg.width = frame->width;
                attachMsg.height = frame->height;
                attachMsg.backingWidth = frame->backingWidth;
                attachMsg.backingHeight = frame->backingHeight;
                attachMsg.format = frame->format;
                attachMsg.transport =
                    lcl::protocol::LCLNativeBufferTransport::AndroidHardwareBufferV1;
                const bool handleSent = m_nativeBufferSocketFd >= 0 &&
                    m_canvas->sendNativeBufferHandle(
                        m_nativeBufferSocketFd, frame->bufferId);
                dmaBufAttached = handleSent && sendProtocolMessage(
                    lcl::protocol::LCLOpcode::AttachNativeBuffer,
                    &attachMsg, sizeof(attachMsg), frame->acquireFenceFd);
            } else {
                lcl::protocol::LCLMsgAttachDmaBuf attachMsg{};
                attachMsg.surfaceId = m_surfaceId;
                attachMsg.configureSerial = m_configureSerial;
                attachMsg.bufferId = frame->bufferId;
                attachMsg.width = frame->width;
                attachMsg.height = frame->height;
                attachMsg.backingWidth = frame->backingWidth;
                attachMsg.backingHeight = frame->backingHeight;
                attachMsg.stride = frame->stride;
                attachMsg.format = frame->format;
                attachMsg.modifier = frame->modifier;
                dmaBufAttached = sendProtocolMessage(
                    lcl::protocol::LCLOpcode::AttachDmaBuf,
                    &attachMsg, sizeof(attachMsg), frame->fd);
            }
            if (frame->fd >= 0) close(frame->fd);
            if (frame->acquireFenceFd >= 0) close(frame->acquireFenceFd);
            if (!dmaBufAttached) {
                m_canvas->cancelDmaBufFrame(frame->bufferId);
                if (frame->transport ==
                    lcl::graphics::NativeBufferTransport::AndroidHardwareBufferV1) {
                    m_canvas->setDmaBufTransportEnabled(false);
                    allocateSHM(m_width, m_height);
                    if (m_nativeBufferSocketFd >= 0) {
                        close(m_nativeBufferSocketFd);
                        m_nativeBufferSocketFd = -1;
                    }
                }
                m_firstFrame = true;
            } else {
                m_submittedConfigureSerial = m_configureSerial;
                m_submittedDmaBufId = frame->bufferId;
                m_frameGateOpen = false;
            }
            if (dmaBufAttached && m_frameTraceEnabled) ++m_traceDmaBufAttaches;
        } else {
            // The GPU pool may be temporarily full. Keep the currently shown
            // client buffer and retry after the compositor releases a slot.
            m_firstFrame = true;
            if (m_frameTraceEnabled) ++m_traceDmaBufPoolBlocks;
        }
    }
    if (!dmaBufFrame && m_ipcConnected && m_socketFd >= 0 && m_shmFd >= 0) {
        lcl::protocol::LCLMsgAttachBuffer attachMsg{};
        attachMsg.surfaceId = m_surfaceId;
        attachMsg.configureSerial = m_configureSerial;
        attachMsg.width = activePixelWidth;
        attachMsg.height = activePixelHeight;
        attachMsg.stride = (m_shmCapacityWidth > 0
            ? m_shmCapacityWidth : activePixelWidth) * sizeof(uint32_t);
        attachMsg.format = lcl::protocol::LCL_BUFFER_FORMAT_ARGB8888;
        if (damageRight > damageLeft && damageBottom > damageTop) {
            attachMsg.damageX = damageLeft;
            attachMsg.damageY = damageTop;
            attachMsg.damageWidth = damageRight - damageLeft;
            attachMsg.damageHeight = damageBottom - damageTop;
        }

        const int passFd = m_shmNeedsAttach ? m_shmFd : -1;
        if (sendProtocolMessage(lcl::protocol::LCLOpcode::AttachBuffer,
                                &attachMsg, sizeof(attachMsg), passFd)) {
            m_shmNeedsAttach = false;
            m_submittedConfigureSerial = m_configureSerial;
            m_frameGateOpen = false;
        } else if (m_shmNeedsAttach) {
            // A lazy-mapped surface has no fallback window to keep it alive.
            // Keep the first buffer eligible for another SCM_RIGHTS commit if
            // a non-blocking socket briefly rejects this send.
            m_firstFrame = true;
            std::cerr << "[lcl-ui ERROR] Initial SHM attach failed for " << m_title
                      << "; retrying\n";
        }
    }
    if (m_frameTraceEnabled) {
        m_traceAttachMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - attachStarted).count();
    }

    if (m_frameTraceEnabled) {
        m_tracePaintMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - paintStarted).count();
    }
    return true;
}

void WindowApp::logFrameTraceIfDue() {
    if (!m_frameTraceEnabled) return;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - m_traceLastLog).count();
    if (seconds < 1.0) return;
    const auto average = [](double total, uint64_t count) {
        return count == 0 ? 0.0 : total / static_cast<double>(count);
    };
    std::cerr << "[LCL TRACE " << m_title << "] frames=" << m_traceRenderedFrames
              << " layout=" << m_traceLayoutPasses << " (" << average(m_traceLayoutMs, m_traceLayoutPasses) << " ms)"
              << " paint=" << average(m_tracePaintMs, m_traceRenderedFrames) << " ms"
              << " stages=(clear=" << average(m_traceClearMs, m_traceRenderedFrames)
              << ", draw=" << average(m_traceDrawMs, m_traceRenderedFrames)
              << ", copy=" << average(m_traceCopyMs, m_traceRenderedFrames)
              << ", attach=" << average(m_traceAttachMs, m_traceRenderedFrames) << " ms)"
              << " damage=" << m_traceDamagePixels << " px"
              << " clear/copy=" << (m_traceClearedBytes / 1024) << '/' << (m_traceCopiedBytes / 1024) << " KiB"
              << " cfg=" << m_traceConfigureCount << " (interactive=" << m_traceInteractiveConfigureCount
              << ", transition=" << m_traceTransitionConfigureCount << ')'
              << " dma=(attach=" << m_traceDmaBufAttaches
              << ", presented=" << m_tracePresentedFrames
              << ", rejected=" << m_traceRejectedDmaBufFrames
              << ", pool-blocked=" << m_traceDmaBufPoolBlocks << ')'
              << " resize=" << m_traceResizeApplies
              << " shm=" << m_traceShmAllocations << " (" << average(m_traceShmMs, m_traceShmAllocations) << " ms)\n";
    m_traceLayoutPasses = 0;
    m_traceRenderedFrames = 0;
    m_traceConfigureCount = 0;
    m_traceInteractiveConfigureCount = 0;
    m_traceTransitionConfigureCount = 0;
    m_tracePresentedFrames = 0;
    m_traceDmaBufAttaches = 0;
    m_traceDmaBufPoolBlocks = 0;
    m_traceRejectedDmaBufFrames = 0;
    m_traceResizeApplies = 0;
    m_traceShmAllocations = 0;
    m_traceDamagePixels = 0;
    m_traceClearedBytes = 0;
    m_traceCopiedBytes = 0;
    m_traceLayoutMs = 0.0;
    m_tracePaintMs = 0.0;
    m_traceClearMs = 0.0;
    m_traceDrawMs = 0.0;
    m_traceCopyMs = 0.0;
    m_traceAttachMs = 0.0;
    m_traceShmMs = 0.0;
    m_traceLastLog = now;
}

} // namespace lcl::ui
