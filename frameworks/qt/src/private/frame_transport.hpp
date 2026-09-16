#pragma once

#include "lcl-client/owned_fd.hpp"
#include "lcl-client/surface_client.hpp"

#include <QImage>
#include <QString>

#include <cstdint>
#include <memory>

namespace lcl::qt::detail {

// QuickSurface owns QML, input, and the LCL connection.  A frame transport
// owns only the platform-native buffer lifetime and its private upload path.
// Future platform implementations use this same boundary without changing
// QuickSurface's public API or the SurfaceClient protocol.
class FrameTransport {
public:
    virtual ~FrameTransport() = default;

    virtual bool configure(uint32_t width, uint32_t height,
                           QString& error) = 0;
    virtual bool submit(const QImage& renderedFrame,
                        client::SurfaceClient& surface,
                        QString& error) = 0;
    virtual void release(uint64_t bufferId,
                         client::OwnedFd releaseFence) = 0;
    virtual void reset() = 0;
};

std::unique_ptr<FrameTransport> makeFrameTransport();

} // namespace lcl::qt::detail
