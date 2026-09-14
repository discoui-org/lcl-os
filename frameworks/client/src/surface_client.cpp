#include "lcl-client/surface_client.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

#if defined(__linux__)
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif

namespace lcl::client {
namespace {

constexpr uint32_t kMaxDimension = 16384;

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool setCloseOnExec(int fd) {
    const int flags = fcntl(fd, F_GETFD, 0);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

OwnedFd connectSeqPacket(const std::string& path) {
    if (path.empty()) return {};
    OwnedFd fd(socket(AF_UNIX, SOCK_SEQPACKET, 0));
    if (!fd) return {};
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) return {};
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (!setCloseOnExec(fd.get()) ||
        ::connect(fd.get(), reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) != 0 || !setNonBlocking(fd.get())) {
        return {};
    }
    return OwnedFd(fd.release());
}

template <size_t N>
void copyString(char (&target)[N], std::string_view value) {
    const size_t count = std::min(value.size(), N - 1);
    std::memcpy(target, value.data(), count);
    target[count] = '\0';
}

std::string fixedString(const char* value, size_t capacity) {
    return std::string(value, strnlen(value, capacity));
}

uint64_t monotonicNowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

int createSealedMemfd(const void* data, size_t bytes) {
    if (!data || bytes == 0) return -1;
#if defined(SYS_memfd_create)
    OwnedFd fd(static_cast<int>(syscall(
        SYS_memfd_create, "lcl-client-display-list",
        MFD_CLOEXEC | MFD_ALLOW_SEALING)));
    if (!fd || ftruncate(fd.get(), static_cast<off_t>(bytes)) != 0) return -1;
    void* mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd.get(), 0);
    if (mapping == MAP_FAILED) return -1;
    std::memcpy(mapping, data, bytes);
    munmap(mapping, bytes);
#if defined(F_ADD_SEALS) && defined(F_SEAL_WRITE) && defined(F_SEAL_GROW) && defined(F_SEAL_SHRINK)
    constexpr int seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
    if (fcntl(fd.get(), F_ADD_SEALS, seals) != 0) return -1;
#endif
    return fd.release();
#else
    (void)data;
    (void)bytes;
    return -1;
#endif
}

bool validFrameGeometry(uint32_t width, uint32_t height, uint32_t stride,
                        const Rect& damage) {
    return width > 0 && height > 0 && width <= kMaxDimension &&
        height <= kMaxDimension && stride >= width * sizeof(uint32_t) &&
        stride <= kMaxDimension * sizeof(uint32_t) &&
        (stride % sizeof(uint32_t)) == 0 &&
        std::isfinite(damage.x) && std::isfinite(damage.y) &&
        std::isfinite(damage.width) && std::isfinite(damage.height) &&
        damage.width > 0.0f && damage.height > 0.0f;
}

} // namespace

class SurfaceClient::Impl {
public:
    bool connectClient(const SurfaceOptions& newOptions,
                       std::string compositorSocket,
                       std::string rasterSocket);
    void disconnectClient() noexcept;
    bool sendCompositor(protocol::LCLOpcode opcode, const void* payload,
                        uint32_t bytes, int passedFd = -1);
    bool connectRaster(bool emitEvent);
    void disconnectRaster(bool restart, bool emitEvent);
    bool sendRaster(raster_protocol::Opcode opcode, const void* payload,
                    uint32_t bytes, int passedFd = -1);
    bool submitNative(uint64_t bufferId, uint64_t revision, uint32_t width,
                      uint32_t height, uint32_t stride, uint32_t format,
                      uint64_t modifier, const Rect& damage, bool opaque,
                      raster_protocol::ExternalBufferTransport transport,
                      int bufferFd, int acquireFenceFd,
                      const std::function<bool(int)>& writeHandle);

    SurfaceOptions options{};
    std::string compositorPath;
    std::string rasterPath;
    OwnedFd compositor;
    OwnedFd raster;
    OwnedFd nativeSideband;
    raster_protocol::SurfaceGrant grant{};
    ConfigureEvent currentConfigure{};
    bool configured{false};
    bool focused{false};
    bool frameCredit{true};
    uint64_t nextFrameSerial{1};
    uint64_t inFlightFrameSerial{0};
    uint64_t inFlightConfigureSerial{0};
    uint64_t inFlightGeometryGeneration{0};
    uint64_t rasterGeneration{0};
    uint32_t nextRequestId{1};
    std::vector<Event> pendingEvents;
};

bool SurfaceClient::Impl::sendCompositor(protocol::LCLOpcode opcode,
                                         const void* payload, uint32_t bytes,
                                         int passedFd) {
    if (!compositor) return false;
    protocol::LCLHeader header{};
    header.opcode = opcode;
    header.requestId = nextRequestId++;
    if (nextRequestId == 0) nextRequestId = 1;
    header.payloadSize = bytes;
    const bool sent = protocol::sendMsgWithFd(
        compositor.get(), header, payload, passedFd);
    if (!sent && errno != EAGAIN && errno != EWOULDBLOCK) {
        disconnectClient();
    }
    return sent;
}

bool SurfaceClient::Impl::connectClient(const SurfaceOptions& newOptions,
                                        std::string compositorSocket,
                                        std::string rasterSocket) {
    disconnectClient();
    if (newOptions.surfaceId == 0 || newOptions.bounds.width <= 0.0f ||
        newOptions.bounds.height <= 0.0f ||
        ((newOptions.role == SurfaceRole::Toplevel ||
          newOptions.role == SurfaceRole::System) && newOptions.appId.empty()) ||
        (newOptions.role == SurfaceRole::System &&
         newOptions.systemKind == protocol::LCLSystemSurfaceKind::None) ||
        (newOptions.role == SurfaceRole::Popup &&
         newOptions.parentSurfaceId == 0) ||
        (newOptions.role == SurfaceRole::Attached &&
         newOptions.targetWindowId == 0)) {
        return false;
    }
    options = newOptions;
    compositorPath = std::move(compositorSocket);
    rasterPath = std::move(rasterSocket);
    if (const char* path = std::getenv("LCL_COMPOSITOR_SOCKET");
        path && path[0] != '\0') compositorPath = path;
    if (const char* path = std::getenv("LCL_RASTER_SOCKET");
        path && path[0] != '\0') rasterPath = path;
    compositor = connectSeqPacket(compositorPath);
    if (!compositor) return false;

    if (options.role == SurfaceRole::System) {
        protocol::LCLMsgSetSystemSurfaceKind system{};
        system.kind = options.systemKind;
        if (!sendCompositor(protocol::LCLOpcode::SetSystemSurfaceKind,
                            &system, sizeof(system))) return false;
    }

    bool created = false;
    if (options.role == SurfaceRole::Popup) {
        protocol::LCLMsgPopupSurfaceCreate message{};
        message.surfaceId = options.surfaceId;
        message.parentSurfaceId = options.parentSurfaceId;
        message.role = options.popupRole;
        message.x = options.bounds.x;
        message.y = options.bounds.y;
        message.width = options.bounds.width;
        message.height = options.bounds.height;
        created = sendCompositor(protocol::LCLOpcode::PopupSurfaceCreate,
                                 &message, sizeof(message));
    } else if (options.role == SurfaceRole::Attached) {
        protocol::LCLMsgAttachedSurfaceCreate message{};
        message.surfaceId = options.surfaceId;
        message.targetWindowId = options.targetWindowId;
        message.role = options.attachedRole;
        message.x = options.bounds.x;
        message.y = options.bounds.y;
        message.width = options.bounds.width;
        message.height = options.bounds.height;
        message.followParentWidth = options.followParentWidth ? 1 : 0;
        message.followParentHeight = options.followParentHeight ? 1 : 0;
        message.acceptsInput = options.acceptsInput ? 1 : 0;
        created = sendCompositor(protocol::LCLOpcode::AttachedSurfaceCreate,
                                 &message, sizeof(message));
    } else {
        protocol::LCLMsgSurfaceCreate message{};
        message.surfaceId = options.surfaceId;
        message.x = options.bounds.x;
        message.y = options.bounds.y;
        message.width = options.bounds.width;
        message.height = options.bounds.height;
        message.resizeBaseWidth = options.resizeBaseWidth;
        message.resizeBaseHeight = options.resizeBaseHeight;
        message.resizeWidthIncrement = options.resizeWidthIncrement;
        message.resizeHeightIncrement = options.resizeHeightIncrement;
        message.launchToken = options.launchToken;
        message.appInstanceId = options.appInstanceId;
        copyString(message.title, options.title);
        copyString(message.appId, options.appId);
        created = sendCompositor(protocol::LCLOpcode::SurfaceCreate,
                                 &message, sizeof(message));
    }
    if (!created) {
        disconnectClient();
        return false;
    }
    return true;
}

void SurfaceClient::Impl::disconnectClient() noexcept {
    if (compositor) protocol::discardPendingWrites(compositor.get());
    compositor.reset();
    disconnectRaster(false, false);
    grant = {};
    configured = false;
    focused = false;
    frameCredit = true;
    inFlightFrameSerial = 0;
    inFlightConfigureSerial = 0;
    inFlightGeometryGeneration = 0;
    pendingEvents.clear();
}

bool SurfaceClient::Impl::connectRaster(bool emitEvent) {
    if (raster) return true;
    if (grant.surfaceId == 0 ||
        (grant.tokenHigh == 0 && grant.tokenLow == 0)) return false;
    raster = connectSeqPacket(rasterPath);
    if (!raster) return false;

    int pair[2]{-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) != 0) {
        raster.reset();
        return false;
    }
    OwnedFd clientSide(pair[0]);
    OwnedFd serviceSide(pair[1]);
    if (!setCloseOnExec(clientSide.get()) ||
        !setCloseOnExec(serviceSide.get()) ||
        !setNonBlocking(clientSide.get()) || !setNonBlocking(serviceSide.get())) {
        raster.reset();
        return false;
    }
    raster_protocol::RegisterNativeBufferChannel registration{};
    if (!raster_protocol::sendPacket(
            raster.get(), raster_protocol::Opcode::RegisterNativeBufferChannel,
            registration, serviceSide.get())) {
        raster.reset();
        return false;
    }
    nativeSideband = std::move(clientSide);
    ++rasterGeneration;
    if (rasterGeneration == 0) rasterGeneration = 1;
    if (emitEvent) pendingEvents.emplace_back(
        RasterConnectionEvent{true, rasterGeneration});
    return true;
}

void SurfaceClient::Impl::disconnectRaster(bool restart, bool emitEvent) {
    const bool wasConnected = static_cast<bool>(raster);
    raster.reset();
    nativeSideband.reset();
    if (restart && inFlightFrameSerial != 0) {
        pendingEvents.emplace_back(FrameDiscardedEvent{
            inFlightConfigureSerial, inFlightFrameSerial,
            inFlightGeometryGeneration,
            raster_protocol::DiscardReason::RasterFailure});
        frameCredit = true;
        inFlightFrameSerial = 0;
        inFlightConfigureSerial = 0;
        inFlightGeometryGeneration = 0;
    }
    if (emitEvent && wasConnected) pendingEvents.emplace_back(
        RasterConnectionEvent{false, rasterGeneration});
}

bool SurfaceClient::Impl::sendRaster(raster_protocol::Opcode opcode,
                                     const void* payload, uint32_t bytes,
                                     int passedFd) {
    if (!connectRaster(true)) return false;
    const bool sent = raster_protocol::sendPacket(
        raster.get(), opcode, payload, bytes, passedFd);
    if (!sent && errno != EAGAIN && errno != EWOULDBLOCK) {
        disconnectRaster(true, true);
    }
    return sent;
}

bool SurfaceClient::Impl::submitNative(
        uint64_t bufferId, uint64_t revision, uint32_t width,
        uint32_t height, uint32_t stride, uint32_t format, uint64_t modifier,
        const Rect& damage, bool opaque,
        raster_protocol::ExternalBufferTransport transport, int bufferFd,
        int acquireFenceFd, const std::function<bool(int)>& writeHandle) {
    if (!configured || !frameCredit || bufferId == 0 || revision == 0 ||
        format == 0 || !validFrameGeometry(width, height, stride, damage) ||
        !connectRaster(true)) return false;
    const bool isNative = transport ==
        raster_protocol::ExternalBufferTransport::AndroidHardwareBufferRgba8888;
    if ((!isNative && bufferFd < 0) || (isNative && !writeHandle)) return false;

    if (isNative && !writeHandle(nativeSideband.get())) {
        disconnectRaster(true, true);
        return false;
    }

    raster_protocol::UploadExternalBuffer upload{};
    upload.grant = grant;
    upload.bufferId = bufferId;
    upload.contentRevision = revision;
    upload.transport = transport;
    upload.width = width;
    upload.height = height;
    upload.stride = stride;
    upload.format = format;
    upload.modifier = modifier;
    if (!sendRaster(raster_protocol::Opcode::UploadExternalBuffer,
                    &upload, sizeof(upload), bufferFd)) {
        // Once an Android handle has entered FIFO, metadata failure makes the
        // stream ambiguous. Both channels are discarded as one connection.
        if (isNative) disconnectRaster(true, true);
        return false;
    }
    if (acquireFenceFd >= 0) {
        raster_protocol::SetExternalBufferFence fence{};
        fence.grant = grant;
        fence.bufferId = bufferId;
        fence.contentRevision = revision;
        if (!sendRaster(raster_protocol::Opcode::SetExternalBufferFence,
                        &fence, sizeof(fence), acquireFenceFd)) {
            // Upload has already transferred a resource identity to rasterd.
            // Reset the connection so a partial submission cannot pin it or
            // make the next use of the same id look like a duplicate.
            disconnectRaster(true, true);
            return false;
        }
    }

    raster_protocol::CommitBufferFrame commit{};
    commit.grant = grant;
    commit.bufferId = bufferId;
    commit.contentRevision = revision;
    commit.configureSerial = currentConfigure.configureSerial;
    commit.frameSerial = nextFrameSerial++;
    if (nextFrameSerial == 0) nextFrameSerial = 1;
    commit.geometryGeneration = currentConfigure.geometryGeneration;
    commit.logicalWidth = currentConfigure.bounds.width;
    commit.logicalHeight = currentConfigure.bounds.height;
    commit.bufferScale = currentConfigure.bufferScale;
    commit.damageX = damage.x;
    commit.damageY = damage.y;
    commit.damageWidth = damage.width;
    commit.damageHeight = damage.height;
    commit.flags = opaque ? raster_protocol::kBufferFrameOpaque : 0;
    commit.clientFrameStartNs = monotonicNowNs();
    commit.clientSubmitNs = monotonicNowNs();
    if (!sendRaster(raster_protocol::Opcode::CommitBufferFrame,
                    &commit, sizeof(commit))) {
        disconnectRaster(true, true);
        return false;
    }
    frameCredit = false;
    inFlightFrameSerial = commit.frameSerial;
    inFlightConfigureSerial = commit.configureSerial;
    inFlightGeometryGeneration = commit.geometryGeneration;
    return true;
}

SurfaceClient::SurfaceClient() : m_impl(new Impl) {}
SurfaceClient::~SurfaceClient() { delete m_impl; }
SurfaceClient::SurfaceClient(SurfaceClient&& other) noexcept
    : m_impl(std::exchange(other.m_impl, nullptr)) {}
SurfaceClient& SurfaceClient::operator=(SurfaceClient&& other) noexcept {
    if (this != &other) {
        delete m_impl;
        m_impl = std::exchange(other.m_impl, nullptr);
    }
    return *this;
}

bool SurfaceClient::connect(const SurfaceOptions& options,
                            std::string compositorSocket,
                            std::string rasterSocket) {
    return m_impl && m_impl->connectClient(
        options, std::move(compositorSocket), std::move(rasterSocket));
}

void SurfaceClient::disconnect() noexcept {
    if (m_impl) m_impl->disconnectClient();
}

bool SurfaceClient::connected() const noexcept {
    return m_impl && static_cast<bool>(m_impl->compositor);
}
int SurfaceClient::compositorFd() const noexcept {
    return m_impl ? m_impl->compositor.get() : -1;
}
int SurfaceClient::rasterFd() const noexcept {
    return m_impl ? m_impl->raster.get() : -1;
}
uint64_t SurfaceClient::rasterConnectionGeneration() const noexcept {
    return m_impl ? m_impl->rasterGeneration : 0;
}
uint32_t SurfaceClient::surfaceId() const noexcept {
    return m_impl ? m_impl->options.surfaceId : 0;
}
bool SurfaceClient::hasConfigure() const noexcept {
    return m_impl && m_impl->configured;
}
const ConfigureEvent& SurfaceClient::configure() const noexcept {
    static const ConfigureEvent empty{};
    return m_impl ? m_impl->currentConfigure : empty;
}
bool SurfaceClient::hasFrameCredit() const noexcept {
    return m_impl && m_impl->frameCredit;
}

std::vector<Event> SurfaceClient::dispatch() {
    std::vector<Event> events;
    if (!m_impl) return events;
    events.swap(m_impl->pendingEvents);
    std::optional<ConfigureEvent> latestConfigure;
    std::optional<FocusEvent> latestFocus;

    if (m_impl->compositor) {
        (void)protocol::flushPendingWrites(m_impl->compositor.get());
        while (m_impl->compositor) {
            protocol::LCLHeader header{};
            std::vector<uint8_t> payload;
            int receivedFd = -1;
            const auto status = protocol::recvPacketWithFd(
                m_impl->compositor.get(), header, payload, receivedFd);
            OwnedFd received(receivedFd);
            if (status == protocol::ReceiveStatus::WouldBlock) break;
            if (status == protocol::ReceiveStatus::Closed ||
                status == protocol::ReceiveStatus::IoError) {
                m_impl->disconnectClient();
                break;
            }
            if (status != protocol::ReceiveStatus::Received) continue;
            if (header.opcode == protocol::LCLOpcode::ConfigureBounds &&
                payload.size() == sizeof(protocol::LCLMsgConfigureBounds)) {
                protocol::LCLMsgConfigureBounds wire{};
                std::memcpy(&wire, payload.data(), sizeof(wire));
                if (wire.surfaceId != m_impl->options.surfaceId) continue;
                ConfigureEvent value{};
                value.surfaceId = wire.surfaceId;
                value.configureSerial = wire.configureSerial;
                value.geometryGeneration = wire.geometryGeneration;
                value.bounds = {wire.x, wire.y, wire.width, wire.height};
                value.backingWidth = wire.backingWidth;
                value.backingHeight = wire.backingHeight;
                value.bufferScale = wire.bufferScale;
                value.resizeReason = wire.resizeReason;
                value.focused = wire.isFocused != 0;
                value.title = fixedString(wire.title, sizeof(wire.title));
                if (m_impl->inFlightFrameSerial != 0 &&
                    value.geometryGeneration >
                        m_impl->inFlightGeometryGeneration) {
                    events.emplace_back(FrameDiscardedEvent{
                        m_impl->inFlightConfigureSerial,
                        m_impl->inFlightFrameSerial,
                        m_impl->inFlightGeometryGeneration,
                        raster_protocol::DiscardReason::Superseded});
                    m_impl->frameCredit = true;
                    m_impl->inFlightFrameSerial = 0;
                    m_impl->inFlightConfigureSerial = 0;
                    m_impl->inFlightGeometryGeneration = 0;
                }
                if (!m_impl->configured || value.focused != m_impl->focused) {
                    latestFocus = FocusEvent{value.focused};
                }
                m_impl->focused = value.focused;
                m_impl->currentConfigure = value;
                m_impl->configured = true;
                latestConfigure = std::move(value);
            } else if (header.opcode == protocol::LCLOpcode::InputEvent &&
                       payload.size() == sizeof(protocol::LCLMsgInputEvent)) {
                protocol::LCLMsgInputEvent wire{};
                std::memcpy(&wire, payload.data(), sizeof(wire));
                if (wire.surfaceId != m_impl->options.surfaceId) continue;
                events.emplace_back(InputEvent{
                    static_cast<protocol::LCLInputEventType>(wire.type),
                    wire.key, wire.pressed != 0, wire.modifiers,
                    static_cast<protocol::LCLPointerSource>(wire.source),
                    wire.pointerId, static_cast<char32_t>(wire.codepoint),
                    wire.x, wire.y, wire.deltaX, wire.deltaY});
            } else if (header.opcode == protocol::LCLOpcode::FramePresented &&
                       payload.size() == sizeof(protocol::LCLMsgFramePresented)) {
                protocol::LCLMsgFramePresented wire{};
                std::memcpy(&wire, payload.data(), sizeof(wire));
                if (wire.surfaceId != m_impl->options.surfaceId) continue;
                if (wire.frameSerial == m_impl->inFlightFrameSerial) {
                    m_impl->frameCredit = true;
                    m_impl->inFlightFrameSerial = 0;
                    m_impl->inFlightConfigureSerial = 0;
                    m_impl->inFlightGeometryGeneration = 0;
                }
                events.emplace_back(FramePresentedEvent{wire});
            } else if (header.opcode == protocol::LCLOpcode::FrameDiscarded &&
                       payload.size() == sizeof(protocol::LCLMsgFrameDiscarded)) {
                protocol::LCLMsgFrameDiscarded wire{};
                std::memcpy(&wire, payload.data(), sizeof(wire));
                if (wire.surfaceId != m_impl->options.surfaceId) continue;
                if (wire.frameSerial == m_impl->inFlightFrameSerial) {
                    m_impl->frameCredit = true;
                    m_impl->inFlightFrameSerial = 0;
                }
                events.emplace_back(FrameDiscardedEvent{
                    wire.configureSerial, wire.frameSerial,
                    wire.geometryGeneration,
                    wire.reason == protocol::LCLFrameDiscardReason::Superseded
                        ? raster_protocol::DiscardReason::Superseded
                        : raster_protocol::DiscardReason::InvalidFrame});
            } else if (header.opcode ==
                           protocol::LCLOpcode::SurfaceProducerGrant &&
                       payload.size() ==
                           sizeof(protocol::LCLMsgSurfaceProducerGrant)) {
                protocol::LCLMsgSurfaceProducerGrant wire{};
                std::memcpy(&wire, payload.data(), sizeof(wire));
                if (wire.surfaceId != m_impl->options.surfaceId) continue;
                m_impl->grant = {wire.surfaceId, wire.ownerPid, wire.flags,
                                 wire.tokenHigh, wire.tokenLow};
                events.emplace_back(ProducerGrantEvent{m_impl->grant});
                (void)m_impl->connectRaster(true);
            } else if (header.opcode == protocol::LCLOpcode::SurfaceDestroy &&
                       payload.size() == sizeof(protocol::LCLMsgSurfaceDestroy)) {
                protocol::LCLMsgSurfaceDestroy wire{};
                std::memcpy(&wire, payload.data(), sizeof(wire));
                if (wire.surfaceId == m_impl->options.surfaceId) {
                    events.emplace_back(SurfaceClosedEvent{wire.surfaceId});
                    protocol::discardPendingWrites(m_impl->compositor.get());
                    m_impl->compositor.reset();
                }
            } else if (header.opcode == protocol::LCLOpcode::AckResponse &&
                       payload.size() == sizeof(protocol::LCLMsgAckResponse)) {
                protocol::LCLMsgAckResponse wire{};
                std::memcpy(&wire, payload.data(), sizeof(wire));
                events.emplace_back(RequestResultEvent{
                    header.requestId, wire.status,
                    fixedString(wire.message, sizeof(wire.message))});
            } else if (header.opcode ==
                           protocol::LCLOpcode::LaunchIconVisibility &&
                       payload.size() ==
                           sizeof(protocol::LCLMsgLaunchIconVisibility)) {
                protocol::LCLMsgLaunchIconVisibility wire{};
                std::memcpy(&wire, payload.data(), sizeof(wire));
                events.emplace_back(LaunchIconVisibilityEvent{
                    wire.launchToken,
                    fixedString(wire.appId, sizeof(wire.appId)),
                    wire.visible != 0});
            }
        }
    }

    if (m_impl->raster) {
        while (m_impl->raster) {
            raster_protocol::Header header{};
            std::vector<uint8_t> payload;
            int receivedFd = -1;
            const auto status = raster_protocol::receivePacket(
                m_impl->raster.get(), header, payload, receivedFd);
            OwnedFd received(receivedFd);
            if (status == raster_protocol::ReceiveStatus::WouldBlock) break;
            if (status == raster_protocol::ReceiveStatus::Closed ||
                status == raster_protocol::ReceiveStatus::Error) {
                m_impl->disconnectRaster(true, true);
                break;
            }
            if (status != raster_protocol::ReceiveStatus::Received) continue;
            if (const auto* discarded = raster_protocol::payloadAs<
                    raster_protocol::FrameDiscarded>(
                    header, payload,
                    raster_protocol::Opcode::FrameDiscarded)) {
                if (discarded->surfaceId != m_impl->options.surfaceId) continue;
                if (discarded->frameSerial == m_impl->inFlightFrameSerial) {
                    m_impl->frameCredit = true;
                    m_impl->inFlightFrameSerial = 0;
                }
                events.emplace_back(FrameDiscardedEvent{
                    discarded->configureSerial, discarded->frameSerial,
                    discarded->geometryGeneration, discarded->reason});
            } else if (const auto* released = raster_protocol::payloadAs<
                           raster_protocol::ExternalBufferReleased>(
                           header, payload,
                           raster_protocol::Opcode::ExternalBufferReleased)) {
                events.emplace_back(BufferReleasedEvent{
                    released->bufferId, released->contentRevision,
                    released->reason, std::move(received)});
            }
        }
    } else if (m_impl->grant.surfaceId != 0) {
        (void)m_impl->connectRaster(true);
    }

    if (latestConfigure) events.emplace_back(std::move(*latestConfigure));
    if (latestFocus) events.emplace_back(*latestFocus);
    if (!m_impl->pendingEvents.empty()) {
        for (auto& event : m_impl->pendingEvents) {
            events.emplace_back(std::move(event));
        }
        m_impl->pendingEvents.clear();
    }
    return events;
}

bool SurfaceClient::submitFrame(DmaBufFrame&& frame) {
    OwnedFd buffer = std::move(frame.buffer);
    OwnedFd acquireFence = std::move(frame.acquireFence);
    return m_impl && m_impl->submitNative(
        frame.bufferId, frame.contentRevision, frame.width, frame.height,
        frame.stride, frame.format, frame.modifier, frame.damage, frame.opaque,
        raster_protocol::ExternalBufferTransport::DmaBufArgb8888,
        buffer.get(), acquireFence.get(), {});
}

bool SurfaceClient::submitFrame(PlatformNativeFrame&& frame) {
    OwnedFd acquireFence = std::move(frame.acquireFence);
    auto writeHandle = std::move(frame.writeHandle);
    return m_impl && m_impl->submitNative(
        frame.bufferId, frame.contentRevision, frame.width, frame.height,
        frame.stride, frame.format, ~uint64_t{0}, frame.damage, frame.opaque,
        raster_protocol::ExternalBufferTransport::AndroidHardwareBufferRgba8888,
        -1, acquireFence.get(), writeHandle);
}

#define LCL_SIMPLE_COMMAND(method, opcode, Type, body)                         \
    bool SurfaceClient::method {                                               \
        if (!m_impl || !m_impl->compositor) return false;                       \
        Type message{};                                                         \
        body                                                                    \
        return m_impl->sendCompositor(opcode, &message, sizeof(message));       \
    }

bool SurfaceClient::requestWindowAction(protocol::LCLWindowAction action,
                                        float localX, float localY) {
    if (!m_impl || !m_impl->compositor ||
        (m_impl->options.role != SurfaceRole::Toplevel &&
         m_impl->options.role != SurfaceRole::System)) return false;
    protocol::LCLMsgRequestWindowAction message{};
    message.surfaceId = m_impl->options.surfaceId;
    message.action = action;
    message.localX = localX;
    message.localY = localY;
    return m_impl->sendCompositor(protocol::LCLOpcode::RequestWindowAction,
                                  &message, sizeof(message));
}

bool SurfaceClient::requestManagedWindowAction(
        protocol::LCLWindowAction action, float localX, float localY) {
    if (!m_impl || !m_impl->compositor ||
        m_impl->options.role != SurfaceRole::Attached) return false;
    protocol::LCLMsgRequestManagedWindowAction message{};
    message.targetWindowId = m_impl->options.targetWindowId;
    message.action = action;
    message.localX = localX;
    message.localY = localY;
    return m_impl->sendCompositor(
        protocol::LCLOpcode::RequestManagedWindowAction,
        &message, sizeof(message));
}

bool SurfaceClient::requestSurfaceClose(uint32_t id) {
    if (!m_impl || !m_impl->compositor) return false;
    protocol::LCLMsgRequestSurfaceClose message{};
    message.surfaceId = id == 0 ? m_impl->options.surfaceId : id;
    return m_impl->sendCompositor(protocol::LCLOpcode::RequestSurfaceClose,
                                  &message, sizeof(message));
}

LCL_SIMPLE_COMMAND(setDecorationMode(protocol::LCLDecorationMode mode),
    protocol::LCLOpcode::SetDecorationMode,
    protocol::LCLMsgSetDecorationMode,
    message.surfaceId = m_impl->options.surfaceId; message.mode = mode;)
LCL_SIMPLE_COMMAND(setEdgeToEdge(bool enabled),
    protocol::LCLOpcode::SetEdgeToEdge,
    protocol::LCLMsgSetEdgeToEdge,
    message.surfaceId = m_impl->options.surfaceId; message.enabled = enabled;)

bool SurfaceClient::setWindowLayer(protocol::LCLWindowLayer layer,
                                   bool unfocusable) {
    if (!m_impl || !m_impl->compositor) return false;
    protocol::LCLMsgSetWindowLayer message{};
    message.surfaceId = m_impl->options.surfaceId;
    message.layer = layer;
    message.unfocusable = unfocusable ? 1 : 0;
    return m_impl->sendCompositor(protocol::LCLOpcode::SetWindowLayer,
                                  &message, sizeof(message));
}

bool SurfaceClient::setReservedZone(float top, float bottom, float left,
                                    float right) {
    if (!m_impl || !m_impl->compositor) return false;
    protocol::LCLMsgSetReservedZone message{};
    message.surfaceId = m_impl->options.surfaceId;
    message.top = top;
    message.bottom = bottom;
    message.left = left;
    message.right = right;
    return m_impl->sendCompositor(protocol::LCLOpcode::SetReservedZone,
                                  &message, sizeof(message));
}

LCL_SIMPLE_COMMAND(setInsetBorder(bool enabled),
    protocol::LCLOpcode::SetInsetBorder,
    protocol::LCLMsgSetInsetBorder,
    message.surfaceId = m_impl->options.surfaceId; message.enabled = enabled;)

bool SurfaceClient::setWindowCornerStyle(float radius, float roundness) {
    if (!m_impl || !m_impl->compositor || !std::isfinite(radius) ||
        !std::isfinite(roundness)) return false;
    protocol::LCLMsgSetWindowCornerStyle message{};
    message.surfaceId = m_impl->options.surfaceId;
    message.radius = std::max(0.0f, radius);
    message.roundness = std::clamp(roundness, 2.0f, 8.0f);
    return m_impl->sendCompositor(protocol::LCLOpcode::SetWindowCornerStyle,
                                  &message, sizeof(message));
}

bool SurfaceClient::setEffectGraph(
        std::span<const protocol::EffectRegion> regions,
        std::span<const protocol::FilterOp> filters) {
    if (!m_impl || !m_impl->compositor || regions.empty() ||
        regions.size() > std::numeric_limits<uint32_t>::max() ||
        filters.size() > std::numeric_limits<uint32_t>::max()) return false;
    const size_t size = sizeof(protocol::LCLMsgSetEffectGraphHeader) +
        regions.size_bytes() + filters.size_bytes();
    if (size > protocol::LCL_PROTOCOL_MAX_PAYLOAD) return false;
    protocol::LCLMsgSetEffectGraphHeader header{};
    header.surfaceId = m_impl->options.surfaceId;
    header.regionCount = static_cast<uint32_t>(regions.size());
    header.filterCount = static_cast<uint32_t>(filters.size());
    std::vector<uint8_t> payload(size);
    std::memcpy(payload.data(), &header, sizeof(header));
    std::memcpy(payload.data() + sizeof(header), regions.data(),
                regions.size_bytes());
    std::memcpy(payload.data() + sizeof(header) + regions.size_bytes(),
                filters.data(), filters.size_bytes());
    return m_impl->sendCompositor(protocol::LCLOpcode::SetEffectGraph,
        payload.data(), static_cast<uint32_t>(payload.size()));
}

LCL_SIMPLE_COMMAND(clearEffectGraph(), protocol::LCLOpcode::ClearEffectGraph,
    protocol::LCLMsgClearEffectGraph,
    message.surfaceId = m_impl->options.surfaceId;)

bool SurfaceClient::beginLaunchPlaceholder(
        uint64_t launchToken, std::string_view appId, Rect origin,
        float cornerRadius, uint32_t iconWidth, uint32_t iconHeight,
        std::span<const uint32_t> argbPixels) {
    const size_t expected = static_cast<size_t>(iconWidth) * iconHeight;
    if (!m_impl || !m_impl->compositor || launchToken == 0 || appId.empty() ||
        iconWidth == 0 || iconHeight == 0 ||
        iconWidth > protocol::LCL_LAUNCH_ICON_MAX_DIMENSION ||
        iconHeight > protocol::LCL_LAUNCH_ICON_MAX_DIMENSION ||
        argbPixels.size() != expected) return false;
    protocol::LCLMsgBeginLaunchPlaceholder message{};
    message.homeSurfaceId = m_impl->options.surfaceId;
    message.launchToken = launchToken;
    copyString(message.appId, appId);
    message.originX = origin.x;
    message.originY = origin.y;
    message.originWidth = origin.width;
    message.originHeight = origin.height;
    message.originCornerRadius = cornerRadius;
    message.iconWidth = iconWidth;
    message.iconHeight = iconHeight;
    std::vector<uint8_t> payload(sizeof(message) + argbPixels.size_bytes());
    std::memcpy(payload.data(), &message, sizeof(message));
    std::memcpy(payload.data() + sizeof(message), argbPixels.data(),
                argbPixels.size_bytes());
    return m_impl->sendCompositor(
        protocol::LCLOpcode::BeginLaunchPlaceholder, payload.data(),
        static_cast<uint32_t>(payload.size()));
}

bool SurfaceClient::resolveLaunchPlaceholder(uint64_t launchToken,
                                             uint64_t appInstanceId,
                                             bool reused) {
    if (!m_impl || !m_impl->compositor || launchToken == 0 ||
        appInstanceId == 0) return false;
    protocol::LCLMsgResolveLaunchPlaceholder message{};
    message.homeSurfaceId = m_impl->options.surfaceId;
    message.launchToken = launchToken;
    message.appInstanceId = appInstanceId;
    message.reused = reused ? 1 : 0;
    return m_impl->sendCompositor(
        protocol::LCLOpcode::ResolveLaunchPlaceholder,
        &message, sizeof(message));
}

bool SurfaceClient::cancelLaunchPlaceholder(uint64_t launchToken) {
    if (!m_impl || !m_impl->compositor || launchToken == 0) return false;
    protocol::LCLMsgCancelLaunchPlaceholder message{};
    message.homeSurfaceId = m_impl->options.surfaceId;
    message.launchToken = launchToken;
    return m_impl->sendCompositor(
        protocol::LCLOpcode::CancelLaunchPlaceholder,
        &message, sizeof(message));
}

bool SurfaceClient::acknowledgeLaunchIconVisibility(
        uint64_t launchToken, std::string_view appId) {
    if (!m_impl || !m_impl->compositor || launchToken == 0 || appId.empty()) {
        return false;
    }
    protocol::LCLMsgLaunchIconVisibilityAck message{};
    message.launchToken = launchToken;
    copyString(message.appId, appId);
    return m_impl->sendCompositor(
        protocol::LCLOpcode::LaunchIconVisibilityAck,
        &message, sizeof(message));
}

bool SurfaceClient::commitLegacyRetainedFrame(
        raster_protocol::CommitTransaction transaction,
        std::span<const raster_protocol::NodeMutation> mutations,
        std::span<const uint8_t> displayList) {
    if (!m_impl || !m_impl->connectRaster(true) ||
        transaction.configureSerial == 0 || transaction.frameSerial == 0 ||
        mutations.size() > std::numeric_limits<uint32_t>::max() ||
        displayList.size() > std::numeric_limits<uint32_t>::max()) return false;
    transaction.grant = m_impl->grant;
    transaction.mutationCount = static_cast<uint32_t>(mutations.size());
    transaction.displayListSize = static_cast<uint32_t>(displayList.size());
    OwnedFd memfd(displayList.empty()
        ? -1 : createSealedMemfd(displayList.data(), displayList.size()));
    if (!displayList.empty() && !memfd) return false;
    const bool sent = raster_protocol::sendCommitTransaction(
        m_impl->raster.get(), transaction, mutations, memfd.get());
    if (!sent && errno != EAGAIN && errno != EWOULDBLOCK) {
        m_impl->disconnectRaster(true, true);
    }
    return sent;
}

#undef LCL_SIMPLE_COMMAND

} // namespace lcl::client
