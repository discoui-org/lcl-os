#include "lcl-ui/widgets/external_buffer.hpp"

#include "lcl-graphics/canvas.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace lcl::ui {
namespace {

int duplicateDescriptor(int fd) noexcept {
    return fd < 0 ? -1 : fcntl(fd, F_DUPFD_CLOEXEC, 0);
}

bool validFrame(const ExternalBufferFrame& frame) noexcept {
    constexpr uint32_t kMaxDimension = 16384;
    constexpr uint64_t kMaxByteSize = 512ull * 1024ull * 1024ull;
    if (frame.bufferId == 0 || frame.contentRevision == 0 ||
        frame.width == 0 || frame.height == 0 ||
        frame.width > kMaxDimension || frame.height > kMaxDimension ||
        frame.stride < frame.width * sizeof(uint32_t) ||
        frame.stride % sizeof(uint32_t) != 0 ||
        frame.stride > kMaxDimension * sizeof(uint32_t) ||
        frame.format != kExternalBufferFormatArgb8888 ||
        frame.byteSize > kMaxByteSize || frame.bufferFd < 0) {
        return false;
    }
    switch (frame.transport) {
        case ExternalBufferTransport::ImmutableShmArgb8888:
            return frame.byteSize ==
                static_cast<uint64_t>(frame.stride) * frame.height &&
                frame.acquireFenceFd < 0;
        case ExternalBufferTransport::DmaBufArgb8888:
            return frame.format == kExternalBufferFormatArgb8888 &&
                frame.byteSize == 0;
    }
    return false;
}

} // namespace

ExternalBufferView::ReleaseSignal::~ReleaseSignal() {
    if (callback) {
        callback(bufferId, contentRevision, releaseFenceFd);
    } else if (releaseFenceFd >= 0) {
        close(releaseFenceFd);
    }
}

ExternalBufferView::StoredFrame::~StoredFrame() {
    if (descriptor.bufferFd >= 0) close(descriptor.bufferFd);
    if (descriptor.acquireFenceFd >= 0) close(descriptor.acquireFenceFd);
}

ExternalBufferView::ExternalBufferView() {
    setClipsToBounds(true);
}

ExternalBufferView::~ExternalBufferView() = default;

bool ExternalBufferView::setFrame(const ExternalBufferFrame& frame,
                                  ReleaseCallback onRelease) {
    if (!validFrame(frame)) return false;
    // A storage identity cannot be rewritten while rasterd may still sample
    // its previous revision. Producers rotate buffers and reuse an id only
    // after its release callback.
    if (m_frame && m_frame->descriptor.bufferId == frame.bufferId) {
        return false;
    }
    const int bufferFd = duplicateDescriptor(frame.bufferFd);
    if (bufferFd < 0) return false;
    const int fenceFd = duplicateDescriptor(frame.acquireFenceFd);
    if (frame.acquireFenceFd >= 0 && fenceFd < 0) {
        close(bufferFd);
        return false;
    }

    auto stored = std::make_unique<StoredFrame>();
    stored->descriptor = frame;
    stored->descriptor.bufferFd = bufferFd;
    stored->descriptor.acquireFenceFd = fenceFd;
    stored->release = std::make_shared<ReleaseSignal>();
    stored->release->bufferId = frame.bufferId;
    stored->release->contentRevision = frame.contentRevision;
    stored->release->callback = std::move(onRelease);
    m_frame = std::move(stored);
    // Buffer identity is a retained presentation property. The stable
    // DisplayList placeholder is not repainted for every decoded frame.
    invalidatePresentation();
    return true;
}

void ExternalBufferView::clearFrame() {
    if (!m_frame) return;
    m_frame.reset();
    invalidatePresentation();
}

uint64_t ExternalBufferView::bufferId() const noexcept {
    return m_frame ? m_frame->descriptor.bufferId : 0;
}

uint64_t ExternalBufferView::contentRevision() const noexcept {
    return m_frame ? m_frame->descriptor.contentRevision : 0;
}

void ExternalBufferView::draw(graphics::Canvas& canvas,
                              const graphics::RectF& damageRect) {
    if (!m_visible || isCollapsed() || !getPresentationBounds().intersects(damageRect)) return;
    beginPresentation(canvas, damageRect);
    if (!m_absoluteBounds.isEmpty()) {
        canvas.drawExternalBufferPlaceholder(getObjectId(), m_absoluteBounds);
    }
    drawChildren(canvas, damageRect);
    endPresentation(canvas);
}

} // namespace lcl::ui
