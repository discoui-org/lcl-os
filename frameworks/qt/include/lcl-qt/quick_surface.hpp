#pragma once

#include "lcl-client/surface_client.hpp"

#include <QObject>
#include <QSizeF>
#include <QString>
#include <QUrl>

#include <memory>

namespace lcl::qt {

/**
 * Hosts one QML scene on an LCL surface without a Qt platform window.
 *
 * QQuickRenderControl drives the scene graph from Qt's event loop. The
 * resulting pixels are submitted through SurfaceClient, so Qt never talks to
 * the compositor or publishes LayerReady descriptors directly.
 */
class QuickSurface final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)
    Q_PROPERTY(bool configured READ isConfigured NOTIFY configuredChanged)
    Q_PROPERTY(QSizeF logicalSize READ logicalSize NOTIFY configuredChanged)
    Q_PROPERTY(bool activeFocus READ hasActiveFocus NOTIFY activeFocusChanged)

public:
    explicit QuickSurface(QObject* parent = nullptr);
    ~QuickSurface() override;

    QuickSurface(const QuickSurface&) = delete;
    QuickSurface& operator=(const QuickSurface&) = delete;

    bool start(const client::SurfaceOptions& options, const QUrl& qmlSource,
               QString compositorSocket = {}, QString rasterSocket = {});
    void stop();

    bool isConnected() const noexcept;
    bool isConfigured() const noexcept;
    QSizeF logicalSize() const noexcept;
    bool hasActiveFocus() const noexcept;

    client::SurfaceClient& surfaceClient() noexcept;
    const client::SurfaceClient& surfaceClient() const noexcept;

    Q_INVOKABLE bool requestClose();
    Q_INVOKABLE bool requestWindowAction(int action, qreal localX = 0.0,
                                         qreal localY = 0.0);
    Q_INVOKABLE bool setEdgeToEdge(bool enabled);
    Q_INVOKABLE void requestFrame();

signals:
    void connectedChanged();
    void configuredChanged();
    void activeFocusChanged();
    void framePresented(quint64 frameSerial);
    void frameDiscarded(quint64 frameSerial);
    void closeRequested();
    void errorOccurred(const QString& message);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace lcl::qt
