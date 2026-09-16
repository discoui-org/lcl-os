#include "lcl-qt/quick_surface.hpp"

#include "private/frame_transport.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QFocusEvent>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSocketNotifier>
#include <QStringList>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

namespace lcl::qt {
namespace {

Qt::KeyboardModifiers qtModifiers(uint8_t modifiers) {
    Qt::KeyboardModifiers result = Qt::NoModifier;
    if ((modifiers & 0x01u) != 0) result |= Qt::ShiftModifier;
    if ((modifiers & 0x02u) != 0) result |= Qt::ControlModifier;
    if ((modifiers & 0x04u) != 0) result |= Qt::AltModifier;
    if ((modifiers & 0x10u) != 0) result |= Qt::MetaModifier;
    return result;
}

int qtKey(uint32_t key, char32_t codepoint) {
    switch (key) {
        case 1: return Qt::Key_Escape;
        case 14: return Qt::Key_Backspace;
        case 15: return Qt::Key_Tab;
        case 28: return Qt::Key_Return;
        case 57: return Qt::Key_Space;
        case 102: return Qt::Key_Home;
        case 103: return Qt::Key_Up;
        case 104: return Qt::Key_PageUp;
        case 105: return Qt::Key_Left;
        case 106: return Qt::Key_Right;
        case 107: return Qt::Key_End;
        case 108: return Qt::Key_Down;
        case 109: return Qt::Key_PageDown;
        case 110: return Qt::Key_Insert;
        case 111: return Qt::Key_Delete;
        default: break;
    }
    if (codepoint > 0 && codepoint <= 0x10ffff) {
        return static_cast<int>(codepoint);
    }
    return Qt::Key_unknown;
}

Qt::MouseButton qtButton(uint32_t key) {
    switch (key) {
        // LCL's public pointer ABI uses normalized buttons, not evdev BTN_*.
        case 0: return Qt::LeftButton;
        case 1: return Qt::MiddleButton;
        case 2: return Qt::RightButton;
        default: return Qt::NoButton;
    }
}

QString codepointText(char32_t codepoint) {
    if (codepoint == 0 || codepoint > 0x10ffff) return {};
    const char32_t value[] = {codepoint};
    return QString::fromUcs4(value, 1);
}

QString qmlErrors(const QQmlComponent& component) {
    QStringList lines;
    for (const auto& error : component.errors()) lines.push_back(error.toString());
    return lines.join(QLatin1Char('\n'));
}

} // namespace

class QuickSurface::Impl {
public:
    explicit Impl(QuickSurface* owner) : q(owner) {}

    bool start(const client::SurfaceOptions& options, const QUrl& qmlSource,
               const QString& compositorSocket, const QString& rasterSocket);
    void stop();
    void refreshNotifiers();
    void dispatchEvents();
    void applyConfigure(const client::ConfigureEvent& event);
    void deliverInput(const client::InputEvent& event);
    void scheduleFrame();
    void renderFrame();
    bool uploadFrame();
    void reportError(const QString& message);

    QuickSurface* q{nullptr};
    client::SurfaceClient surface;
    std::unique_ptr<QQuickRenderControl> renderControl;
    std::unique_ptr<QQuickWindow> window;
    std::unique_ptr<QQmlEngine> engine;
    QQuickItem* rootItem{nullptr};
    QObject* rootObject{nullptr};
    QSocketNotifier* compositorNotifier{nullptr};
    QSocketNotifier* rasterNotifier{nullptr};
    int watchedCompositorFd{-1};
    int watchedRasterFd{-1};
    QImage staging;
    std::unique_ptr<detail::FrameTransport> frameTransport;
    QSizeF size;
    qreal scale{1.0};
    Qt::MouseButtons mouseButtons{Qt::NoButton};
    bool focused{false};
    bool rendererInitialized{false};
    bool frameDirty{false};
    bool frameScheduled{false};
    bool stopping{false};
};

bool QuickSurface::Impl::start(
        const client::SurfaceOptions& options, const QUrl& qmlSource,
        const QString& compositorSocket, const QString& rasterSocket) {
    stop();
    stopping = false;
    frameTransport = detail::makeFrameTransport();
    if (!qmlSource.isValid() || qmlSource.isEmpty()) {
        reportError(QStringLiteral("Invalid QML source URL"));
        return false;
    }

    const std::string compositor = compositorSocket.isEmpty()
        ? std::string("/Runtime/lcl-compositor.sock")
        : compositorSocket.toStdString();
    const std::string raster = rasterSocket.isEmpty()
        ? std::string("/Runtime/lcl-raster.sock")
        : rasterSocket.toStdString();
    if (!surface.connect(options, compositor, raster)) {
        reportError(QStringLiteral("Could not connect the LCL surface"));
        return false;
    }
    qInfo() << "lcl-qt: surface transport connected";

    // The initial bridge deliberately uses Qt Quick's software render target
    // and then uploads it into LCL's native buffer pool. This needs no QPA
    // window and keeps all compositor-facing transport in lcl-client.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    renderControl = std::make_unique<QQuickRenderControl>();
    qInfo() << "lcl-qt: render control created";
    window = std::make_unique<QQuickWindow>(renderControl.get());
    qInfo() << "lcl-qt: quick window created";
    window->setColor(Qt::transparent);
    engine = std::make_unique<QQmlEngine>();
    qInfo() << "lcl-qt: QML engine created";
    engine->rootContext()->setContextProperty(
        QStringLiteral("lclSurface"), q);

    QQmlComponent component(engine.get(), qmlSource);
    qInfo() << "lcl-qt: QML component loaded" << component.status();
    if (component.status() != QQmlComponent::Ready) {
        reportError(qmlErrors(component));
        stop();
        return false;
    }
    rootObject = component.create(engine->rootContext());
    qInfo() << "lcl-qt: QML root created";
    rootItem = qobject_cast<QQuickItem*>(rootObject);
    if (!rootItem) {
        reportError(QStringLiteral("The QML root object must be a QQuickItem"));
        delete rootObject;
        rootObject = nullptr;
        stop();
        return false;
    }
    QQmlEngine::setObjectOwnership(rootObject, QQmlEngine::CppOwnership);
    rootItem->setParentItem(window->contentItem());

    QObject::connect(renderControl.get(), &QQuickRenderControl::renderRequested,
                     q, [this] { scheduleFrame(); });
    QObject::connect(renderControl.get(), &QQuickRenderControl::sceneChanged,
                     q, [this] { scheduleFrame(); });
    refreshNotifiers();
    qInfo() << "lcl-qt: socket notifiers ready";
    Q_EMIT q->connectedChanged();
    qInfo() << "lcl-qt: dispatching initial events";
    dispatchEvents();
    qInfo() << "lcl-qt: initial events dispatched";
    return true;
}

void QuickSurface::Impl::stop() {
    if (stopping) return;
    stopping = true;
    const bool wasConnected = surface.connected();
    const bool wasConfigured = surface.hasConfigure();
    const bool wasFocused = focused;
    if (compositorNotifier) {
        compositorNotifier->setEnabled(false);
        compositorNotifier->deleteLater();
        compositorNotifier = nullptr;
    }
    if (rasterNotifier) {
        rasterNotifier->setEnabled(false);
        rasterNotifier->deleteLater();
        rasterNotifier = nullptr;
    }
    watchedCompositorFd = -1;
    watchedRasterFd = -1;
    rendererInitialized = false;
    if (rootItem) rootItem->setParentItem(nullptr);
    delete rootObject;
    rootObject = nullptr;
    rootItem = nullptr;
    engine.reset();
    window.reset();
    renderControl.reset();
    staging = {};
    if (frameTransport) frameTransport->reset();
    frameTransport.reset();
    surface.disconnect();
    size = {};
    scale = 1.0;
    focused = false;
    frameDirty = false;
    frameScheduled = false;
    mouseButtons = Qt::NoButton;
    stopping = false;
    if (wasConnected) Q_EMIT q->connectedChanged();
    if (wasConfigured) Q_EMIT q->configuredChanged();
    if (wasFocused) Q_EMIT q->activeFocusChanged();
}

void QuickSurface::Impl::refreshNotifiers() {
    const auto replace = [this](QSocketNotifier*& notifier, int& watched,
                                int descriptor) {
        if (descriptor == watched) return;
        if (notifier) {
            notifier->setEnabled(false);
            notifier->deleteLater();
            notifier = nullptr;
        }
        watched = descriptor;
        if (descriptor < 0) return;
        notifier = new QSocketNotifier(
            descriptor, QSocketNotifier::Read, q);
        QObject::connect(notifier, &QSocketNotifier::activated,
                         q, [this](QSocketDescriptor, QSocketNotifier::Type) {
                             dispatchEvents();
                         });
    };
    replace(compositorNotifier, watchedCompositorFd, surface.compositorFd());
    replace(rasterNotifier, watchedRasterFd, surface.rasterFd());
}

void QuickSurface::Impl::dispatchEvents() {
    if (stopping) return;
    const bool wasConnected = surface.connected();
    for (auto& event : surface.dispatch()) {
        if (const auto* configure =
                std::get_if<client::ConfigureEvent>(&event)) {
            applyConfigure(*configure);
        } else if (const auto* input =
                       std::get_if<client::InputEvent>(&event)) {
            deliverInput(*input);
        } else if (const auto* focus =
                       std::get_if<client::FocusEvent>(&event)) {
            if (focused != focus->focused) {
                focused = focus->focused;
                QFocusEvent focusEvent(
                    focused ? QEvent::FocusIn : QEvent::FocusOut,
                    Qt::ActiveWindowFocusReason);
                if (window) QCoreApplication::sendEvent(window.get(), &focusEvent);
                Q_EMIT q->activeFocusChanged();
            }
        } else if (const auto* presented =
                       std::get_if<client::FramePresentedEvent>(&event)) {
            Q_EMIT q->framePresented(presented->message.frameSerial);
            if (frameDirty) scheduleFrame();
        } else if (const auto* discarded =
                       std::get_if<client::FrameDiscardedEvent>(&event)) {
            Q_EMIT q->frameDiscarded(discarded->frameSerial);
            frameDirty = true;
            scheduleFrame();
        } else if (auto* released =
                       std::get_if<client::BufferReleasedEvent>(&event)) {
            if (frameTransport) {
                frameTransport->release(released->bufferId,
                                        std::move(released->releaseFence));
            }
            if (frameDirty) scheduleFrame();
        } else if (std::get_if<client::SurfaceClosedEvent>(&event)) {
            Q_EMIT q->closeRequested();
        } else if (const auto* connection =
                       std::get_if<client::RasterConnectionEvent>(&event)) {
            if (connection->connected && frameDirty) scheduleFrame();
        }
    }
    refreshNotifiers();
    if (wasConnected != surface.connected()) Q_EMIT q->connectedChanged();
}

void QuickSurface::Impl::applyConfigure(const client::ConfigureEvent& event) {
    qInfo() << "lcl-qt: applying configure" << event.bounds.width
            << event.bounds.height << event.bufferScale;
    const QSizeF nextSize(event.bounds.width, event.bounds.height);
    const qreal nextScale = std::clamp<qreal>(event.bufferScale, 0.5, 4.0);
    const int pixelWidth = std::max(
        1, static_cast<int>(std::ceil(nextSize.width() * nextScale)));
    const int pixelHeight = std::max(
        1, static_cast<int>(std::ceil(nextSize.height() * nextScale)));
    if (!nextSize.isValid() || pixelWidth > 16384 || pixelHeight > 16384) {
        reportError(QStringLiteral("Compositor returned invalid surface geometry"));
        return;
    }

    size = nextSize;
    scale = nextScale;
    staging = QImage(pixelWidth, pixelHeight,
                     QImage::Format_RGBA8888_Premultiplied);
    if (staging.isNull()) {
        reportError(QStringLiteral("Could not allocate the Qt Quick render target"));
        return;
    }
    staging.setDevicePixelRatio(scale);
    staging.fill(Qt::transparent);
    window->setGeometry(0, 0, std::max(1, qRound(size.width())),
                        std::max(1, qRound(size.height())));
    window->contentItem()->setSize(size);
    rootItem->setSize(size);
    window->setRenderTarget(QQuickRenderTarget::fromPaintDevice(&staging));
    qInfo() << "lcl-qt: software render target ready" << pixelWidth << pixelHeight;
    if (!rendererInitialized) {
        // QQuickRenderControl::initialize() is explicitly not used by Qt's
        // software adaptation. fromPaintDevice() makes the target ready.
        rendererInitialized = true;
    }

    const uint32_t width = static_cast<uint32_t>(pixelWidth);
    const uint32_t height = static_cast<uint32_t>(pixelHeight);
    QString transportError;
    if (!frameTransport ||
        !frameTransport->configure(width, height, transportError)) {
        reportError(transportError.isEmpty()
            ? QStringLiteral("No Qt frame transport is available")
            : transportError);
        return;
    }
    frameDirty = true;
    Q_EMIT q->configuredChanged();
    scheduleFrame();
}

void QuickSurface::Impl::deliverInput(const client::InputEvent& event) {
    if (!window) return;
    const auto modifiers = qtModifiers(event.modifiers);
    const QPointF local(event.x, event.y);
    switch (event.type) {
        case protocol::LCLInputEventType::KeyDown:
        case protocol::LCLInputEventType::KeyUp: {
            const auto type = event.type == protocol::LCLInputEventType::KeyDown
                ? QEvent::KeyPress : QEvent::KeyRelease;
            QKeyEvent keyEvent(type, qtKey(event.key, event.codepoint), modifiers,
                               event.key, 0, 0,
                               codepointText(event.codepoint));
            QCoreApplication::sendEvent(window.get(), &keyEvent);
            break;
        }
        case protocol::LCLInputEventType::TextInput: {
            QInputMethodEvent textEvent;
            textEvent.setCommitString(codepointText(event.codepoint));
            QCoreApplication::sendEvent(window.get(), &textEvent);
            break;
        }
        case protocol::LCLInputEventType::PointerMotion: {
            QMouseEvent mouseEvent(QEvent::MouseMove, local, local, local,
                                   Qt::NoButton, mouseButtons, modifiers);
            QCoreApplication::sendEvent(window.get(), &mouseEvent);
            break;
        }
        case protocol::LCLInputEventType::PointerButton: {
            const Qt::MouseButton button = qtButton(event.key);
            if (event.pressed) mouseButtons |= button;
            else mouseButtons &= ~button;
            QMouseEvent mouseEvent(
                event.pressed ? QEvent::MouseButtonPress
                              : QEvent::MouseButtonRelease,
                local, local, local, button, mouseButtons, modifiers);
            QCoreApplication::sendEvent(window.get(), &mouseEvent);
            break;
        }
        case protocol::LCLInputEventType::PointerScroll: {
            QWheelEvent wheelEvent(
                local, local, QPoint(),
                QPoint(qRound(event.deltaX * 120.0f),
                       qRound(event.deltaY * 120.0f)),
                mouseButtons, modifiers, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(window.get(), &wheelEvent);
            break;
        }
        case protocol::LCLInputEventType::PointerCancel: {
            QEvent cancel(QEvent::UngrabMouse);
            QCoreApplication::sendEvent(window.get(), &cancel);
            mouseButtons = Qt::NoButton;
            break;
        }
    }
}

void QuickSurface::Impl::scheduleFrame() {
    frameDirty = true;
    if (frameScheduled || stopping) return;
    frameScheduled = true;
    QTimer::singleShot(0, q, [this] {
        frameScheduled = false;
        renderFrame();
    });
}

void QuickSurface::Impl::renderFrame() {
    if (stopping || !frameDirty || !rendererInitialized ||
        !surface.hasConfigure() || !surface.hasFrameCredit()) return;
    staging.fill(Qt::transparent);
    renderControl->polishItems();
    renderControl->beginFrame();
    renderControl->sync();
    renderControl->render();
    renderControl->endFrame();
    if (uploadFrame()) frameDirty = false;
}

bool QuickSurface::Impl::uploadFrame() {
    if (!frameTransport) return false;
    QString transportError;
    const bool submitted = frameTransport->submit(staging, surface,
                                                   transportError);
    if (!submitted && !transportError.isEmpty()) reportError(transportError);
    return submitted;
}

void QuickSurface::Impl::reportError(const QString& message) {
    Q_EMIT q->errorOccurred(message.isEmpty()
        ? QStringLiteral("Unknown lcl-qt error") : message);
}

QuickSurface::QuickSurface(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(this)) {}

QuickSurface::~QuickSurface() { m_impl->stop(); }

bool QuickSurface::start(const client::SurfaceOptions& options,
                         const QUrl& qmlSource, QString compositorSocket,
                         QString rasterSocket) {
    return m_impl->start(options, qmlSource, compositorSocket, rasterSocket);
}

void QuickSurface::stop() { m_impl->stop(); }

bool QuickSurface::isConnected() const noexcept {
    return m_impl->surface.connected();
}

bool QuickSurface::isConfigured() const noexcept {
    return m_impl->surface.hasConfigure();
}

QSizeF QuickSurface::logicalSize() const noexcept { return m_impl->size; }

bool QuickSurface::hasActiveFocus() const noexcept { return m_impl->focused; }

client::SurfaceClient& QuickSurface::surfaceClient() noexcept {
    return m_impl->surface;
}

const client::SurfaceClient& QuickSurface::surfaceClient() const noexcept {
    return m_impl->surface;
}

bool QuickSurface::requestClose() {
    return m_impl->surface.requestSurfaceClose();
}

bool QuickSurface::requestWindowAction(int action, qreal localX,
                                       qreal localY) {
    if (action < static_cast<int>(protocol::LCLWindowAction::BeginDrag) ||
        action > static_cast<int>(protocol::LCLWindowAction::Close)) {
        return false;
    }
    const auto requested = static_cast<protocol::LCLWindowAction>(action);
    if (m_impl->surface.requestWindowAction(
            requested, static_cast<float>(localX),
            static_cast<float>(localY))) {
        return true;
    }
    return m_impl->surface.requestManagedWindowAction(
        requested, static_cast<float>(localX), static_cast<float>(localY));
}

bool QuickSurface::setEdgeToEdge(bool enabled) {
    return m_impl->surface.setEdgeToEdge(enabled);
}

void QuickSurface::requestFrame() { m_impl->scheduleFrame(); }

} // namespace lcl::qt
