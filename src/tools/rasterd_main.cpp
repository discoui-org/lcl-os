#include "core/ipc/raster_protocol.hpp"
#include "lcl-graphics/display_list_wire.hpp"
#include "render/client_egl_context.hpp"
#include "render/raster_renderer.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unordered_map>
#include <unistd.h>

#if defined(__linux__)
#include <linux/memfd.h>
#endif

namespace {

using lcl::raster_protocol::FrameDiscarded;
using lcl::raster_protocol::LayerReady;
using lcl::raster_protocol::Opcode;
using lcl::raster_protocol::SubmitFrame;
using lcl::raster_protocol::SurfaceGrant;

struct TokenKey {
    uint64_t high{0};
    uint64_t low{0};
    bool operator==(const TokenKey&) const = default;
};

struct TokenHash {
    size_t operator()(const TokenKey& key) const noexcept {
        return std::hash<uint64_t>{}(key.high) ^
            (std::hash<uint64_t>{}(key.low) << 1u);
    }
};

TokenKey tokenOf(const SurfaceGrant& grant) {
    return {grant.tokenHigh, grant.tokenLow};
}

struct ImageKey {
    uint64_t id{0};
    uint64_t revision{0};
    bool operator==(const ImageKey&) const = default;
};

struct ImageHash {
    size_t operator()(const ImageKey& key) const noexcept {
        return std::hash<uint64_t>{}(key.id) ^
            (std::hash<uint64_t>{}(key.revision) << 1u);
    }
};

struct ImageResource {
    uint64_t rendererId{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stridePixels{0};
    bool opaque{false};
    std::vector<uint32_t> pixels;
};

struct LayerSlot {
    uint64_t layerId{0};
    int fd{-1};
    uint32_t* pixels{nullptr};
    size_t bytes{0};
    uint32_t width{0};
    uint32_t height{0};
    bool busy{false};

    LayerSlot() = default;
    LayerSlot(const LayerSlot&) = delete;
    LayerSlot& operator=(const LayerSlot&) = delete;
    LayerSlot(LayerSlot&& other) noexcept { *this = std::move(other); }
    LayerSlot& operator=(LayerSlot&& other) noexcept {
        if (this == &other) return *this;
        release();
        layerId = other.layerId;
        fd = other.fd;
        pixels = other.pixels;
        bytes = other.bytes;
        width = other.width;
        height = other.height;
        busy = other.busy;
        other.fd = -1;
        other.pixels = nullptr;
        other.bytes = 0;
        other.busy = false;
        return *this;
    }
    ~LayerSlot() { release(); }

    void release() noexcept {
        if (pixels && bytes) munmap(pixels, bytes);
        if (fd >= 0) close(fd);
        fd = -1;
        pixels = nullptr;
        bytes = 0;
        busy = false;
    }
};

struct SurfaceState {
    SurfaceGrant grant{};
    std::unordered_map<ImageKey, ImageResource, ImageHash> images;
    std::unordered_map<uint64_t, uint64_t> cachedLayerNamespaces;
    std::array<LayerSlot, 3> slots;
    std::vector<uint32_t> softwarePixels;
    std::unique_ptr<lcl::render::RasterRenderer> softwareRenderer;
    uint64_t softwareFrameSerial{0};
    uint64_t softwareGeometryGeneration{0};
    uint32_t softwareWidth{0};
    uint32_t softwareHeight{0};
    float softwareScale{0.0f};
    std::unique_ptr<lcl::render::ClientEGLContext> gpuContext;
    std::unique_ptr<lcl::render::RasterRenderer> gpuRenderer;
    std::unordered_map<uint64_t, uint32_t> gpuLayers;
    std::unordered_map<uint32_t, uint64_t> gpuBufferIds;
    uint64_t gpuFrameSerial{0};
    uint64_t gpuGeometryGeneration{0};
    uint32_t gpuWidth{0};
    uint32_t gpuHeight{0};
    float gpuScale{0.0f};
    bool gpuUnavailable{false};
};

struct Client {
    int fd{-1};
    pid_t pid{0};
};

struct PendingFrame {
    int clientFd{-1};
    SubmitFrame submit{};
    int displayListFd{-1};
};

int createMemfd(const char* name, size_t bytes) {
#if defined(SYS_memfd_create)
    const int fd = static_cast<int>(syscall(
        SYS_memfd_create, name, MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
#else
    (void)name;
    (void)bytes;
    return -1;
#endif
}

bool isImmutableMemfd(int fd, size_t expectedBytes) {
    if (fd < 0 || expectedBytes == 0) return false;
    struct stat info{};
    if (fstat(fd, &info) != 0 ||
        static_cast<uint64_t>(info.st_size) != expectedBytes) return false;
#if defined(F_GET_SEALS) && defined(F_SEAL_WRITE) && defined(F_SEAL_GROW) && defined(F_SEAL_SHRINK)
    const int seals = fcntl(fd, F_GET_SEALS);
    constexpr int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
    return seals >= 0 && (seals & required) == required;
#else
    return true;
#endif
}

bool readExact(int fd, void* destination, size_t bytes) {
    if (fd < 0 || !destination || bytes == 0) return false;
    auto* cursor = static_cast<uint8_t*>(destination);
    size_t offset = 0;
    while (offset < bytes) {
        const ssize_t count = pread(
            fd, cursor + offset, bytes - offset, static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

bool replacesRetainedScene(const SubmitFrame& submit) noexcept {
    return (submit.flags & lcl::raster_protocol::kSubmitReplacesScene) != 0;
}

bool hasValidRetainedFrameMetadata(const SubmitFrame& submit) noexcept {
    constexpr float kTolerance = 0.001f;
    constexpr float kMaxPixelExtent = 16384.0f;
    const bool finite = std::isfinite(submit.logicalWidth) &&
        std::isfinite(submit.logicalHeight) &&
        std::isfinite(submit.bufferScale) &&
        std::isfinite(submit.damageX) && std::isfinite(submit.damageY) &&
        std::isfinite(submit.damageWidth) &&
        std::isfinite(submit.damageHeight);
    if (!finite || submit.logicalWidth <= 0.0f ||
        submit.logicalHeight <= 0.0f || submit.bufferScale < 0.5f ||
        submit.bufferScale > 4.0f || submit.damageX < 0.0f ||
        submit.logicalWidth * submit.bufferScale > kMaxPixelExtent ||
        submit.logicalHeight * submit.bufferScale > kMaxPixelExtent ||
        submit.damageY < 0.0f || submit.damageWidth <= 0.0f ||
        submit.damageHeight <= 0.0f ||
        submit.damageX + submit.damageWidth >
            submit.logicalWidth + kTolerance ||
        submit.damageY + submit.damageHeight >
            submit.logicalHeight + kTolerance ||
        (submit.flags & ~lcl::raster_protocol::kSubmitReplacesScene) != 0) {
        return false;
    }
    if (!replacesRetainedScene(submit)) return submit.baseFrameSerial != 0;
    return submit.baseFrameSerial == 0 &&
        submit.damageX <= kTolerance && submit.damageY <= kTolerance &&
        submit.damageX + submit.damageWidth >=
            submit.logicalWidth - kTolerance &&
        submit.damageY + submit.damageHeight >=
            submit.logicalHeight - kTolerance;
}

bool retainedBaseMatches(const SubmitFrame& submit, uint64_t frameSerial,
                         uint64_t geometryGeneration, uint32_t width,
                         uint32_t height, float scale) noexcept {
    if (replacesRetainedScene(submit)) return true;
    const uint32_t nextWidth = std::max(1u, static_cast<uint32_t>(
        std::ceil(submit.logicalWidth * submit.bufferScale)));
    const uint32_t nextHeight = std::max(1u, static_cast<uint32_t>(
        std::ceil(submit.logicalHeight * submit.bufferScale)));
    return frameSerial != 0 && submit.baseFrameSerial == frameSerial &&
        submit.geometryGeneration == geometryGeneration &&
        width == nextWidth && height == nextHeight &&
        std::fabs(scale - submit.bufferScale) <= 0.0001f;
}

struct PixelDamage {
    uint32_t x{0};
    uint32_t y{0};
    uint32_t width{0};
    uint32_t height{0};
};

PixelDamage pixelDamageFor(const SubmitFrame& submit, uint32_t width,
                           uint32_t height) noexcept {
    const uint32_t left = std::min(width, static_cast<uint32_t>(std::floor(
        submit.damageX * submit.bufferScale)));
    const uint32_t top = std::min(height, static_cast<uint32_t>(std::floor(
        submit.damageY * submit.bufferScale)));
    const uint32_t right = std::min(width, static_cast<uint32_t>(std::ceil(
        (submit.damageX + submit.damageWidth) * submit.bufferScale)));
    const uint32_t bottom = std::min(height, static_cast<uint32_t>(std::ceil(
        (submit.damageY + submit.damageHeight) * submit.bufferScale)));
    return {left, top, right > left ? right - left : 0,
            bottom > top ? bottom - top : 0};
}

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

int createListener(const std::string& path) {
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        close(fd);
        return -1;
    }
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    unlink(path.c_str());
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        chmod(path.c_str(), 0600) != 0 || listen(fd, 32) != 0) {
        close(fd);
        unlink(path.c_str());
        return -1;
    }
    return fd;
}

class RasterService {
public:
    RasterService(int compositorFd, std::string socketPath)
        : m_compositorFd(compositorFd), m_socketPath(std::move(socketPath)) {}

    ~RasterService() {
        for (auto& frame : m_systemFrames) if (frame.displayListFd >= 0) close(frame.displayListFd);
        for (auto& frame : m_appFrames) if (frame.displayListFd >= 0) close(frame.displayListFd);
        for (const auto& client : m_clients) if (client.fd >= 0) close(client.fd);
        if (m_listenerFd >= 0) close(m_listenerFd);
        if (m_compositorFd >= 0) close(m_compositorFd);
        unlink(m_socketPath.c_str());
    }

    bool initialize() {
        if (m_compositorFd < 0 || !setNonBlocking(m_compositorFd)) return false;
        m_listenerFd = createListener(m_socketPath);
        if (m_listenerFd < 0) return false;
        return lcl::raster_protocol::sendPacket(
            m_compositorFd, Opcode::Ready,
            static_cast<const void*>(nullptr), 0u, -1);
    }

    int run() {
        while (m_running) {
            acceptClients();
            pollCompositor();
            pollClients();
            renderOne();

            std::vector<pollfd> descriptors;
            descriptors.push_back({m_compositorFd, POLLIN, 0});
            descriptors.push_back({m_listenerFd, POLLIN, 0});
            for (const auto& client : m_clients) {
                descriptors.push_back({client.fd, POLLIN | POLLHUP, 0});
            }
            (void)poll(descriptors.data(), descriptors.size(),
                       hasPendingFrames() ? 0 : 4);
        }
        return 0;
    }

private:
    void acceptClients() {
        while (true) {
            const int fd = accept4(m_listenerFd, nullptr, nullptr,
                                   SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) break;
            ucred credentials{};
            socklen_t length = sizeof(credentials);
            if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
                close(fd);
                continue;
            }
            m_clients.push_back({fd, credentials.pid});
        }
    }

    void pollCompositor() {
        while (true) {
            lcl::raster_protocol::Header header{};
            std::vector<uint8_t> payload;
            int receivedFd = -1;
            const auto status = lcl::raster_protocol::receivePacket(
                m_compositorFd, header, payload, receivedFd);
            if (status == lcl::raster_protocol::ReceiveStatus::WouldBlock) break;
            if (status == lcl::raster_protocol::ReceiveStatus::Closed ||
                status == lcl::raster_protocol::ReceiveStatus::Error) {
                m_running = false;
                break;
            }
            if (receivedFd >= 0) close(receivedFd);
            if (status != lcl::raster_protocol::ReceiveStatus::Received) continue;
            if (const auto* grant = lcl::raster_protocol::payloadAs<SurfaceGrant>(
                    header, payload, Opcode::RegisterSurface)) {
                auto& surface = m_surfaces[tokenOf(*grant)];
                surface.grant = *grant;
            } else if (const auto* grant = lcl::raster_protocol::payloadAs<SurfaceGrant>(
                           header, payload, Opcode::RevokeSurface)) {
                revokeSurface(tokenOf(*grant));
            } else if (const auto* release = lcl::raster_protocol::payloadAs<
                           lcl::raster_protocol::ReleaseLayer>(
                           header, payload, Opcode::ReleaseLayer)) {
                releaseLayer(release->layerId, release->reason);
            }
        }
    }

    void pollClients() {
        for (auto it = m_clients.begin(); it != m_clients.end();) {
            bool alive = true;
            while (alive) {
                lcl::raster_protocol::Header header{};
                std::vector<uint8_t> payload;
                int receivedFd = -1;
                const auto status = lcl::raster_protocol::receivePacket(
                    it->fd, header, payload, receivedFd);
                if (status == lcl::raster_protocol::ReceiveStatus::WouldBlock) break;
                if (status == lcl::raster_protocol::ReceiveStatus::Closed ||
                    status == lcl::raster_protocol::ReceiveStatus::Error) {
                    if (receivedFd >= 0) close(receivedFd);
                    alive = false;
                    break;
                }
                if (status != lcl::raster_protocol::ReceiveStatus::Received) {
                    if (receivedFd >= 0) close(receivedFd);
                    continue;
                }
                handleClientPacket(*it, header, payload, receivedFd);
            }
            if (!alive) {
                close(it->fd);
                it = m_clients.erase(it);
            } else {
                ++it;
            }
        }
    }

    SurfaceState* authorizedSurface(const Client& client,
                                    const SurfaceGrant& grant) {
        const auto found = m_surfaces.find(tokenOf(grant));
        if (found == m_surfaces.end() || found->second.grant.surfaceId != grant.surfaceId ||
            found->second.grant.ownerPid != client.pid || grant.ownerPid != client.pid) {
            return nullptr;
        }
        return &found->second;
    }

    void handleClientPacket(const Client& client,
                            const lcl::raster_protocol::Header& header,
                            std::span<const uint8_t> payload, int receivedFd) {
        if (const auto* upload = lcl::raster_protocol::payloadAs<
                lcl::raster_protocol::UploadImage>(
                header, payload, Opcode::UploadImage)) {
            auto* surface = authorizedSurface(client, upload->grant);
            if (!surface || !isImmutableMemfd(receivedFd, upload->byteSize) ||
                upload->width == 0 || upload->height == 0 ||
                upload->stridePixels < upload->width ||
                upload->byteSize != static_cast<uint64_t>(upload->stridePixels) *
                    upload->height * sizeof(uint32_t)) {
                if (receivedFd >= 0) close(receivedFd);
                return;
            }
            ImageResource resource{};
            resource.rendererId = m_nextImageId++;
            resource.width = upload->width;
            resource.height = upload->height;
            resource.stridePixels = upload->stridePixels;
            resource.opaque = upload->opaque != 0;
            resource.pixels.resize(
                static_cast<size_t>(upload->stridePixels) * upload->height);
            // Android may reject a cross-domain MAP_SHARED mapping of a
            // sealed memfd even though SCM_RIGHTS and pread are permitted.
            // Reading the immutable payload also avoids retaining a mapping
            // to producer-owned input beyond this packet.
            if (readExact(receivedFd, resource.pixels.data(), upload->byteSize)) {
                surface->images[{upload->resourceId, upload->contentRevision}] =
                    std::move(resource);
            }
            close(receivedFd);
            return;
        }

        if (const auto* submit = lcl::raster_protocol::payloadAs<SubmitFrame>(
                header, payload, Opcode::SubmitFrame)) {
            auto* surface = authorizedSurface(client, submit->grant);
            if (!surface || submit->displayListSize == 0 ||
                !hasValidRetainedFrameMetadata(*submit) ||
                !isImmutableMemfd(receivedFd, submit->displayListSize)) {
                if (receivedFd >= 0) close(receivedFd);
                discard(client.fd, *submit,
                        surface ? lcl::raster_protocol::DiscardReason::InvalidFrame
                                : lcl::raster_protocol::DiscardReason::InvalidGrant);
                return;
            }
            auto& queue = (submit->grant.flags &
                           lcl::raster_protocol::kGrantInteractiveSystem) != 0
                ? m_systemFrames : m_appFrames;
            auto existing = std::find_if(queue.begin(), queue.end(), [submit](const auto& frame) {
                return tokenOf(frame.submit.grant) == tokenOf(submit->grant);
            });
            if (existing != queue.end()) {
                if (replacesRetainedScene(*submit)) {
                    // A complete replacement has no dependency on the queued
                    // base and may safely collapse obsolete work.
                    discard(existing->clientFd, existing->submit,
                            lcl::raster_protocol::DiscardReason::Superseded);
                    if (existing->displayListFd >= 0) {
                        close(existing->displayListFd);
                    }
                    *existing = {client.fd, *submit, receivedFd};
                } else {
                    // Retained patches are ordered against an exact base.
                    // Never drop the queued predecessor and apply this patch
                    // to a different scene.
                    discard(client.fd, *submit,
                            lcl::raster_protocol::DiscardReason::InvalidFrame);
                    close(receivedFd);
                }
            } else {
                queue.push_back({client.fd, *submit, receivedFd});
            }
            return;
        }

        if (receivedFd >= 0) close(receivedFd);
    }

    bool hasPendingFrames() const {
        return !m_systemFrames.empty() || !m_appFrames.empty();
    }

    void renderOne() {
        std::deque<PendingFrame>* queue = nullptr;
        if (!m_systemFrames.empty() && (m_systemBurst < 3 || m_appFrames.empty())) {
            queue = &m_systemFrames;
            ++m_systemBurst;
        } else if (!m_appFrames.empty()) {
            queue = &m_appFrames;
            m_systemBurst = 0;
        }
        if (!queue) return;

        auto frame = std::move(queue->front());
        queue->pop_front();
        auto surfaceIt = m_surfaces.find(tokenOf(frame.submit.grant));
        if (surfaceIt == m_surfaces.end()) {
            discard(frame.clientFd, frame.submit,
                    lcl::raster_protocol::DiscardReason::SurfaceRevoked);
            close(frame.displayListFd);
            return;
        }
        if (!rasterFrame(surfaceIt->second, frame.submit, frame.displayListFd)) {
            discard(frame.clientFd, frame.submit,
                    lcl::raster_protocol::DiscardReason::RasterFailure);
        }
        close(frame.displayListFd);
    }

    LayerSlot* acquireSlot(SurfaceState& surface, const SubmitFrame& submit) {
        const uint32_t width = std::max(1u, static_cast<uint32_t>(
            std::ceil(submit.logicalWidth * submit.bufferScale)));
        const uint32_t height = std::max(1u, static_cast<uint32_t>(
            std::ceil(submit.logicalHeight * submit.bufferScale)));
        const size_t bytes = static_cast<size_t>(width) * height * sizeof(uint32_t);
        for (auto& slot : surface.slots) {
            if (slot.busy) continue;
            if (slot.bytes != bytes) {
                slot.release();
                slot.fd = createMemfd("lcl-raster-layer", bytes);
                if (slot.fd < 0) return nullptr;
                void* mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, slot.fd, 0);
                if (mapping == MAP_FAILED) {
                    slot.release();
                    return nullptr;
                }
                slot.pixels = static_cast<uint32_t*>(mapping);
                slot.bytes = bytes;
                slot.width = width;
                slot.height = height;
                // A daemon restart must never recycle an identifier still
                // retained by the compositor from the previous process.
                slot.layerId =
                    (static_cast<uint64_t>(static_cast<uint32_t>(getpid())) << 32u) |
                    m_nextLayerId++;
            }
            slot.busy = true;
            return &slot;
        }
        return nullptr;
    }

    bool rasterFrame(SurfaceState& surface, const SubmitFrame& submit,
                     int displayListFd) {
        if (!replacesRetainedScene(submit) &&
            !retainedBaseMatches(
                submit, surface.gpuFrameSerial,
                surface.gpuGeometryGeneration, surface.gpuWidth,
                surface.gpuHeight, surface.gpuScale) &&
            !retainedBaseMatches(
                submit, surface.softwareFrameSerial,
                surface.softwareGeometryGeneration, surface.softwareWidth,
                surface.softwareHeight, surface.softwareScale)) {
            return false;
        }
        std::vector<uint8_t> wireBytes(submit.displayListSize);
        if (!readExact(displayListFd, wireBytes.data(), wireBytes.size())) {
            return false;
        }
        const auto wire = std::span<const uint8_t>(wireBytes);
        auto decoded = lcl::graphics::decodeDisplayList(wire, submit.displayListSize);
        if (!decoded) return false;

        auto commands = std::make_shared<std::vector<lcl::graphics::DisplayCommand>>(
            decoded.displayList.commands());
        for (auto& command : *commands) {
            if (auto* image = std::get_if<lcl::graphics::DrawImageCommand>(&command)) {
                const auto found = surface.images.find(
                    {image->resourceId, image->contentRevision});
                if (found == surface.images.end()) return false;
                auto& resource = found->second;
                image->resourceKey = reinterpret_cast<uintptr_t>(resource.pixels.data());
                image->resourceId = resource.rendererId;
                image->sourceWidth = static_cast<int>(resource.width);
                image->sourceHeight = static_cast<int>(resource.height);
                image->stridePixels = static_cast<int>(resource.stridePixels);
                image->opaque = resource.opaque;
            } else if (auto* begin = std::get_if<
                           lcl::graphics::BeginCachedLayerCommand>(&command)) {
                auto [found, inserted] = surface.cachedLayerNamespaces.try_emplace(
                    begin->id, 0);
                if (inserted) found->second = m_nextCachedLayerId++;
                begin->id = found->second;
            } else if (auto* draw = std::get_if<
                           lcl::graphics::DrawCachedLayerCommand>(&command)) {
                auto [found, inserted] = surface.cachedLayerNamespaces.try_emplace(
                    draw->id, 0);
                if (inserted) found->second = m_nextCachedLayerId++;
                draw->id = found->second;
            }
        }

        const auto displayCommands = std::shared_ptr<
            const std::vector<lcl::graphics::DisplayCommand>>(commands);
        const lcl::graphics::DisplayList displayList(displayCommands);
        if (rasterGpuFrame(surface, submit, displayList)) return true;

        if (!retainedBaseMatches(
                submit, surface.softwareFrameSerial,
                surface.softwareGeometryGeneration, surface.softwareWidth,
                surface.softwareHeight, surface.softwareScale)) {
            return false;
        }
        LayerSlot* slot = acquireSlot(surface, submit);
        if (!slot) return false;
        const size_t pixelCount =
            static_cast<size_t>(slot->width) * slot->height;
        const bool recreateSoftwareScene = !surface.softwareRenderer ||
            surface.softwareWidth != slot->width ||
            surface.softwareHeight != slot->height;
        if (recreateSoftwareScene) {
            surface.softwareRenderer =
                std::make_unique<lcl::render::RasterRenderer>();
            surface.softwarePixels.assign(pixelCount, 0u);
            if (!surface.softwareRenderer->initialize(
                    slot->width, slot->height, nullptr,
                    surface.softwarePixels.data())) {
                surface.softwareRenderer.reset();
                slot->busy = false;
                return false;
            }
            surface.softwareRenderer->setRetainsFrameBacking(true);
            surface.softwareWidth = slot->width;
            surface.softwareHeight = slot->height;
        }
        auto& renderer = *surface.softwareRenderer;
        renderer.setDeviceScale(submit.bufferScale);
        renderer.setFrameExtent(slot->width, slot->height);
        renderer.beginFrame();
        renderer.setFrameDamageRect(lcl::render::RasterRect{
            submit.damageX, submit.damageY,
            submit.damageWidth, submit.damageHeight});
        renderer.replayDisplayList(
            displayList,
            {{submit.logicalWidth, submit.logicalHeight},
             {slot->width, slot->height}, submit.bufferScale});
        renderer.endFrame();
        std::copy(surface.softwarePixels.begin(),
                  surface.softwarePixels.end(), slot->pixels);
        const PixelDamage damage = pixelDamageFor(
            submit, slot->width, slot->height);
        LayerReady ready{};
        ready.grant = submit.grant;
        ready.layerId = slot->layerId;
        ready.configureSerial = submit.configureSerial;
        ready.frameSerial = submit.frameSerial;
        ready.geometryGeneration = submit.geometryGeneration;
        ready.width = slot->width;
        ready.height = slot->height;
        ready.backingWidth = slot->width;
        ready.backingHeight = slot->height;
        ready.stride = slot->width * sizeof(uint32_t);
        ready.damageX = damage.x;
        ready.damageY = damage.y;
        ready.damageWidth = damage.width;
        ready.damageHeight = damage.height;
        ready.transport = lcl::raster_protocol::LayerTransport::Shm;
        ready.byteSize = slot->bytes;
        if (!lcl::raster_protocol::sendPacket(
                m_compositorFd, Opcode::LayerReady, ready, slot->fd)) {
            slot->busy = false;
            surface.softwareFrameSerial = 0;
            return false;
        }
        surface.softwareFrameSerial = submit.frameSerial;
        surface.softwareGeometryGeneration = submit.geometryGeneration;
        surface.softwareScale = submit.bufferScale;
        return true;
    }

    bool rasterGpuFrame(
            SurfaceState& surface, const SubmitFrame& submit,
            const lcl::graphics::DisplayList& displayList) {
#if defined(__ANDROID__)
        // The Android private channel needs an AHardwareBuffer handle transfer,
        // not an fd-only DMA-BUF packet. Keep the validated SHM fallback until
        // that platform-native transport is installed below this same ABI.
        (void)surface;
        (void)submit;
        (void)displayList;
        return false;
#else
        if (surface.gpuUnavailable) return false;
        const uint32_t width = std::max(1u, static_cast<uint32_t>(
            std::ceil(submit.logicalWidth * submit.bufferScale)));
        const uint32_t height = std::max(1u, static_cast<uint32_t>(
            std::ceil(submit.logicalHeight * submit.bufferScale)));
        if (!retainedBaseMatches(
                submit, surface.gpuFrameSerial,
                surface.gpuGeometryGeneration, surface.gpuWidth,
                surface.gpuHeight, surface.gpuScale)) {
            return false;
        }
        if (!surface.gpuContext) {
            auto context = std::make_unique<lcl::render::ClientEGLContext>();
            if (!context->initialize(width, height) ||
                !context->hasDmaBufPool()) {
                surface.gpuUnavailable = true;
                return false;
            }
            auto renderer = std::make_unique<lcl::render::RasterRenderer>();
            if (!renderer->initialize(width, height, context.get(), nullptr)) {
                surface.gpuUnavailable = true;
                return false;
            }
            renderer->setRetainsFrameBacking(true);
            surface.gpuContext = std::move(context);
            surface.gpuRenderer = std::move(renderer);
        }
        if (!surface.gpuContext->ensureDmaBufCapacity(width, height) ||
            !surface.gpuRenderer->ensureFrameBackingCapacity(width, height)) {
            return false;
        }
        const auto target = surface.gpuContext->acquireDmaBufTarget();
        if (!target) return false;
        surface.gpuRenderer->setExternalFrameTarget(
            target->framebuffer, target->texture,
            target->width, target->height);
        surface.gpuRenderer->setDeviceScale(submit.bufferScale);
        surface.gpuRenderer->setFrameExtent(width, height);
        surface.gpuRenderer->beginFrame();
        surface.gpuRenderer->setFrameDamageRect(lcl::render::RasterRect{
            submit.damageX, submit.damageY,
            submit.damageWidth, submit.damageHeight});
        surface.gpuRenderer->replayDisplayList(
            displayList,
            {{submit.logicalWidth, submit.logicalHeight},
             {width, height}, submit.bufferScale});
        surface.gpuRenderer->endFrame();
        const auto exported = surface.gpuContext->exportCurrentDmaBuf();
        surface.gpuRenderer->clearExternalFrameTarget();
        if (!exported || exported->fd < 0 || exported->androidHardwareBuffer) {
            surface.gpuContext->releaseDmaBuf(target->bufferId);
            return false;
        }
        const uint64_t layerId =
            (static_cast<uint64_t>(static_cast<uint32_t>(getpid())) << 32u) |
            m_nextLayerId++;
        auto [bufferIdentity, inserted] = surface.gpuBufferIds.try_emplace(
            target->bufferId, 0);
        if (inserted) {
            bufferIdentity->second =
                (static_cast<uint64_t>(static_cast<uint32_t>(getpid())) << 32u) |
                m_nextBufferId++;
        }
        LayerReady ready{};
        ready.grant = submit.grant;
        ready.layerId = layerId;
        ready.bufferId = bufferIdentity->second;
        ready.configureSerial = submit.configureSerial;
        ready.frameSerial = submit.frameSerial;
        ready.geometryGeneration = submit.geometryGeneration;
        ready.width = width;
        ready.height = height;
        ready.backingWidth = exported->width;
        ready.backingHeight = exported->height;
        ready.stride = exported->stride;
        const PixelDamage damage = pixelDamageFor(submit, width, height);
        ready.damageX = damage.x;
        ready.damageY = damage.y;
        ready.damageWidth = damage.width;
        ready.damageHeight = damage.height;
        ready.transport = lcl::raster_protocol::LayerTransport::DmaBuf;
        ready.format = exported->format;
        ready.modifier = exported->modifier;
        const bool sent = lcl::raster_protocol::sendPacket(
            m_compositorFd, Opcode::LayerReady, ready, exported->fd);
        close(exported->fd);
        if (!sent) {
            surface.gpuContext->releaseDmaBuf(target->bufferId);
            surface.gpuFrameSerial = 0;
            return false;
        }
        surface.gpuLayers.emplace(layerId, target->bufferId);
        surface.gpuFrameSerial = submit.frameSerial;
        surface.gpuGeometryGeneration = submit.geometryGeneration;
        surface.gpuWidth = width;
        surface.gpuHeight = height;
        surface.gpuScale = submit.bufferScale;
        return true;
#endif
    }

    void discard(int clientFd, const SubmitFrame& submit,
                 lcl::raster_protocol::DiscardReason reason) {
        FrameDiscarded discarded{};
        discarded.surfaceId = submit.grant.surfaceId;
        discarded.configureSerial = submit.configureSerial;
        discarded.frameSerial = submit.frameSerial;
        discarded.geometryGeneration = submit.geometryGeneration;
        discarded.reason = reason;
        (void)lcl::raster_protocol::sendPacket(
            clientFd, Opcode::FrameDiscarded, discarded);
    }

    void releaseLayer(
            uint64_t layerId,
            lcl::raster_protocol::LayerReleaseReason reason) {
        for (auto& [_, surface] : m_surfaces) {
            const auto gpu = surface.gpuLayers.find(layerId);
            if (gpu != surface.gpuLayers.end()) {
                if (surface.gpuContext) {
                    surface.gpuContext->releaseDmaBuf(gpu->second);
                }
                surface.gpuLayers.erase(gpu);
                if (reason == lcl::raster_protocol::LayerReleaseReason::RejectedTransport) {
                    // The compositor could not import this producer's DMA-BUF.
                    // Keep already-retained GPU layers alive, but publish all
                    // subsequent frames for this surface through the portable
                    // SHM transport instead of retrying an unusable transport.
                    surface.gpuUnavailable = true;
                }
                return;
            }
            for (auto& slot : surface.slots) {
                if (slot.layerId == layerId) {
                    slot.busy = false;
                    return;
                }
            }
        }
    }

    void revokeSurface(const TokenKey& token) {
        const auto found = m_surfaces.find(token);
        if (found == m_surfaces.end()) return;
        const auto removePending = [&token](auto& queue) {
            for (auto it = queue.begin(); it != queue.end();) {
                if (tokenOf(it->submit.grant) == token) {
                    if (it->displayListFd >= 0) close(it->displayListFd);
                    it = queue.erase(it);
                } else {
                    ++it;
                }
            }
        };
        removePending(m_systemFrames);
        removePending(m_appFrames);
        m_surfaces.erase(found);
    }

    int m_compositorFd{-1};
    std::string m_socketPath;
    int m_listenerFd{-1};
    bool m_running{true};
    std::vector<Client> m_clients;
    std::unordered_map<TokenKey, SurfaceState, TokenHash> m_surfaces;
    std::deque<PendingFrame> m_systemFrames;
    std::deque<PendingFrame> m_appFrames;
    unsigned m_systemBurst{0};
    uint64_t m_nextLayerId{1};
    uint32_t m_nextBufferId{1};
    uint64_t m_nextImageId{1};
    uint64_t m_nextCachedLayerId{1};
};

std::optional<int> parseFd(const char* value) {
    if (!value || !*value) return std::nullopt;
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < 0 || parsed > 1024) {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

} // namespace

int main(int argc, char** argv) {
    int compositorFd = -1;
    std::string socketPath = "/Runtime/lcl-raster.sock";
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--compositor-fd" && index + 1 < argc) {
            compositorFd = parseFd(argv[++index]).value_or(-1);
        } else if (argument == "--socket" && index + 1 < argc) {
            socketPath = argv[++index];
        }
    }
    RasterService service(compositorFd, std::move(socketPath));
    if (!service.initialize()) {
        std::cerr << "[LCL Rasterd ERROR] initialization failed\n";
        return 1;
    }
    std::cout << "[LCL Rasterd] retained layer producer ready\n";
    return service.run();
}
