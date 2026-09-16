#include "frame_transport.hpp"

#include "platforms/common/native_buffer.hpp"
#include "system/render/client_egl_context.hpp"

#include <GLES2/gl2.h>

#include <unordered_map>
#include <utility>

namespace lcl::qt::detail {
namespace {

class LinuxDmaBufFrameTransport final : public FrameTransport {
public:
    bool configure(uint32_t width, uint32_t height, QString& error) override;
    bool submit(const QImage& renderedFrame, client::SurfaceClient& surface,
                QString& error) override;
    void release(uint64_t bufferId, client::OwnedFd releaseFence) override;
    void reset() override;

private:
    lcl::render::ClientEGLContext gpu_;
    std::unordered_map<uint32_t, uint64_t> revisions_;
};

QString transportError(const lcl::render::ClientEGLContext& gpu) {
    QString message = QStringLiteral("No exportable DMA-BUF render target");
    if (!gpu.dmaBufError().empty()) {
        message += QStringLiteral(": ") +
                   QString::fromStdString(gpu.dmaBufError());
    } else if (!gpu.initializationError().empty()) {
        message += QStringLiteral(": ") +
                   QString::fromStdString(gpu.initializationError());
    } else {
        message += QStringLiteral(": client EGL initialization failed");
    }
    return message;
}

} // namespace

bool LinuxDmaBufFrameTransport::configure(uint32_t width, uint32_t height,
                                          QString& error) {
    const bool ready = gpu_.isInitialized()
        ? gpu_.ensureDmaBufCapacity(width, height)
        : gpu_.initialize(width, height);
    if (ready && gpu_.hasDmaBufPool()) return true;

    error = transportError(gpu_);
    return false;
}

bool LinuxDmaBufFrameTransport::submit(const QImage& renderedFrame,
                                       client::SurfaceClient& surface,
                                       QString& error) {
    const auto target = gpu_.acquireDmaBufTarget();
    if (!target) return false;

    const QImage upload = renderedFrame.mirrored(false, true).convertToFormat(
        QImage::Format_RGBA8888_Premultiplied);
    glBindTexture(GL_TEXTURE_2D, target->texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    static_cast<GLsizei>(upload.width()),
                    static_cast<GLsizei>(upload.height()), GL_RGBA,
                    GL_UNSIGNED_BYTE, upload.constBits());
    glBindTexture(GL_TEXTURE_2D, 0);
    if (glGetError() != GL_NO_ERROR) {
        gpu_.cancelCurrentDmaBuf();
        error = QStringLiteral("Qt Quick native-buffer upload failed");
        return false;
    }

    const auto exported = gpu_.exportCurrentDmaBuf();
    if (!exported) {
        gpu_.cancelCurrentDmaBuf();
        return false;
    }

    uint64_t& revision = revisions_[exported->bufferId];
    ++revision;
    if (revision == 0) revision = 1;
    const auto& configure = surface.configure();
    client::DmaBufFrame frame{};
    frame.bufferId = exported->bufferId;
    frame.contentRevision = revision;
    frame.width = exported->width;
    frame.height = exported->height;
    frame.stride = exported->stride;
    frame.format = exported->format;
    frame.modifier = exported->modifier;
    frame.damage = {0.0f, 0.0f, configure.bounds.width,
                    configure.bounds.height};
    frame.buffer = client::OwnedFd(exported->fd);
    frame.acquireFence = client::OwnedFd(exported->acquireFenceFd);
    if (surface.submitFrame(std::move(frame))) return true;

    gpu_.releaseDmaBuf(exported->bufferId);
    return false;
}

void LinuxDmaBufFrameTransport::release(
        uint64_t bufferId, client::OwnedFd releaseFence) {
    gpu_.releaseDmaBuf(static_cast<uint32_t>(bufferId),
                       releaseFence.release());
}

void LinuxDmaBufFrameTransport::reset() {
    gpu_.shutdown();
    revisions_.clear();
}

std::unique_ptr<FrameTransport> makeFrameTransport() {
    return std::make_unique<LinuxDmaBufFrameTransport>();
}

} // namespace lcl::qt::detail
