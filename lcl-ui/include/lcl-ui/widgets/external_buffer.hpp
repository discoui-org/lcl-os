#pragma once

#include "lcl-ui/widgets/widget.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace lcl::ui {

/** Transport accepted by the private lcl-ui -> rasterd resource channel. */
enum class ExternalBufferTransport : uint32_t {
    ImmutableShmArgb8888 = 0,
    DmaBufArgb8888 = 1,
};

/** DRM_FORMAT_ARGB8888 ("AR24"), the initial external-buffer pixel contract. */
inline constexpr uint32_t kExternalBufferFormatArgb8888 = 0x34325241u;

/**
 * Borrowed frame descriptor. setFrame() duplicates both descriptors before it
 * returns; ownership of the supplied fds remains with the caller.
 */
struct ExternalBufferFrame {
    uint64_t bufferId{0};
    uint64_t contentRevision{0};
    ExternalBufferTransport transport{ExternalBufferTransport::DmaBufArgb8888};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride{0};
    uint32_t format{kExternalBufferFormatArgb8888};
    uint64_t modifier{~uint64_t{0}};
    uint64_t byteSize{0};
    int bufferFd{-1};
    int acquireFenceFd{-1};
};

/**
 * A retained video/camera/web-content surface. Pixel storage is never copied
 * into a public Canvas command and is never submitted directly to compositor.
 */
class ExternalBufferView final : public Widget {
public:
    // The callback owns releaseFenceFd when it is non-negative.
    using ReleaseCallback = std::function<void(uint64_t bufferId,
                                               uint64_t contentRevision,
                                               int releaseFenceFd)>;

    ExternalBufferView();
    ~ExternalBufferView() override;

    bool setFrame(const ExternalBufferFrame& frame,
                  ReleaseCallback onRelease = {});
    void clearFrame();
    bool hasFrame() const noexcept { return static_cast<bool>(m_frame); }
    uint64_t bufferId() const noexcept;
    uint64_t contentRevision() const noexcept;

    void draw(graphics::Canvas& canvas,
              const graphics::RectF& damageRect) override;

private:
    friend class WindowApp;

    struct ReleaseSignal {
        uint64_t bufferId{0};
        uint64_t contentRevision{0};
        int releaseFenceFd{-1};
        ReleaseCallback callback{};
        ~ReleaseSignal();
    };
    struct StoredFrame {
        ExternalBufferFrame descriptor{};
        std::shared_ptr<ReleaseSignal> release{};
        StoredFrame() = default;
        StoredFrame(const StoredFrame&) = delete;
        StoredFrame& operator=(const StoredFrame&) = delete;
        ~StoredFrame();
    };
    const StoredFrame* storedFrame() const noexcept { return m_frame.get(); }

    std::unique_ptr<StoredFrame> m_frame;
};

} // namespace lcl::ui
