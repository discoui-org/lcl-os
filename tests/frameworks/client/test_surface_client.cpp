#include <gtest/gtest.h>

#include "lcl-client/surface_client.hpp"
#include "platforms/common/native_buffer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

namespace client = lcl::client;
namespace protocol = lcl::protocol;
namespace raster = lcl::raster_protocol;

class SeqPacketListener {
public:
    explicit SeqPacketListener(std::string name) {
        path = (std::filesystem::path("/tmp") /
            (std::move(name) + "-" + std::to_string(getpid()) + "-" +
             std::to_string(++nextId))).string();
        fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd < 0) return;
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, path.c_str(),
                     sizeof(address.sun_path) - 1);
        if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(fd, 2) != 0) {
            close(fd);
            fd = -1;
        }
    }

    ~SeqPacketListener() {
        if (peer >= 0) close(peer);
        if (fd >= 0) close(fd);
        unlink(path.c_str());
    }

    bool valid() const { return fd >= 0; }
    bool acceptPeer() {
        peer = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
        return peer >= 0;
    }

    std::string path;
    int fd{-1};
    int peer{-1};
    static inline uint64_t nextId{0};
};

bool receiveCompositorPacket(int fd, protocol::LCLHeader& header,
                             std::vector<uint8_t>& payload) {
    pollfd descriptor{fd, POLLIN, 0};
    if (poll(&descriptor, 1, 1000) <= 0) return false;
    int receivedFd = -1;
    const auto status = protocol::recvPacketWithFd(
        fd, header, payload, receivedFd);
    if (receivedFd >= 0) close(receivedFd);
    return status == protocol::ReceiveStatus::Received;
}

template <typename T>
bool sendCompositorPacket(int fd, protocol::LCLOpcode opcode,
                          const T& message) {
    protocol::LCLHeader header{};
    header.opcode = opcode;
    header.payloadSize = sizeof(message);
    return protocol::sendMsgWithFd(fd, header, &message);
}

client::SurfaceOptions basicOptions(client::SurfaceRole role) {
    client::SurfaceOptions options{};
    options.role = role;
    options.surfaceId = 7;
    options.appId = "org.lcl.test.client";
    options.title = "client test";
    options.bounds = {10.0f, 20.0f, 320.0f, 200.0f};
    return options;
}

TEST(SurfaceClientTest, CreatesEverySurfaceRole) {
    for (const auto role : {client::SurfaceRole::Toplevel,
                            client::SurfaceRole::Popup,
                            client::SurfaceRole::Attached}) {
        SeqPacketListener compositor("lcl-client-role");
        ASSERT_TRUE(compositor.valid());
        auto options = basicOptions(role);
        options.parentSurfaceId = 3;
        options.targetWindowId = 4;
        options.followParentWidth = true;
        options.acceptsInput = true;
        client::SurfaceClient surface;
        ASSERT_TRUE(surface.connect(options, compositor.path, "/tmp/missing-raster"));
        ASSERT_TRUE(compositor.acceptPeer());
        protocol::LCLHeader header{};
        std::vector<uint8_t> payload;
        ASSERT_TRUE(receiveCompositorPacket(compositor.peer, header, payload));
        const auto expected = role == client::SurfaceRole::Toplevel
            ? protocol::LCLOpcode::SurfaceCreate
            : role == client::SurfaceRole::Popup
                ? protocol::LCLOpcode::PopupSurfaceCreate
                : protocol::LCLOpcode::AttachedSurfaceCreate;
        EXPECT_EQ(header.opcode, expected);
    }
}

TEST(SurfaceClientTest, CreatesSystemSurfaceWithDeclaredKind) {
    SeqPacketListener compositor("lcl-client-system-role");
    ASSERT_TRUE(compositor.valid());
    auto options = basicOptions(client::SurfaceRole::System);
    options.systemKind = protocol::LCLSystemSurfaceKind::Dock;
    client::SurfaceClient surface;
    ASSERT_TRUE(surface.connect(options, compositor.path, "/tmp/missing-raster"));
    ASSERT_TRUE(compositor.acceptPeer());

    protocol::LCLHeader header{};
    std::vector<uint8_t> payload;
    ASSERT_TRUE(receiveCompositorPacket(compositor.peer, header, payload));
    ASSERT_EQ(header.opcode, protocol::LCLOpcode::SetSystemSurfaceKind);
    ASSERT_EQ(payload.size(), sizeof(protocol::LCLMsgSetSystemSurfaceKind));
    protocol::LCLMsgSetSystemSurfaceKind kind{};
    std::memcpy(&kind, payload.data(), sizeof(kind));
    EXPECT_EQ(kind.kind, protocol::LCLSystemSurfaceKind::Dock);

    ASSERT_TRUE(receiveCompositorPacket(compositor.peer, header, payload));
    EXPECT_EQ(header.opcode, protocol::LCLOpcode::SurfaceCreate);
}

TEST(SurfaceClientTest, DispatchCoalescesConfigureAndTypesInputAndFocus) {
    SeqPacketListener compositor("lcl-client-events");
    ASSERT_TRUE(compositor.valid());
    client::SurfaceClient surface;
    ASSERT_TRUE(surface.connect(basicOptions(client::SurfaceRole::Toplevel),
                                compositor.path, "/tmp/missing-raster"));
    ASSERT_TRUE(compositor.acceptPeer());
    protocol::LCLHeader ignoredHeader{};
    std::vector<uint8_t> ignoredPayload;
    ASSERT_TRUE(receiveCompositorPacket(
        compositor.peer, ignoredHeader, ignoredPayload));

    protocol::LCLMsgConfigureBounds first{};
    first.surfaceId = 7;
    first.configureSerial = 10;
    first.geometryGeneration = 4;
    first.width = first.backingWidth = 300.0f;
    first.height = first.backingHeight = 180.0f;
    first.bufferScale = 1.0f;
    first.isFocused = 0;
    ASSERT_TRUE(sendCompositorPacket(
        compositor.peer, protocol::LCLOpcode::ConfigureBounds, first));
    auto latest = first;
    latest.configureSerial = 11;
    latest.geometryGeneration = 5;
    latest.width = latest.backingWidth = 320.0f;
    latest.height = latest.backingHeight = 200.0f;
    latest.isFocused = 1;
    ASSERT_TRUE(sendCompositorPacket(
        compositor.peer, protocol::LCLOpcode::ConfigureBounds, latest));
    protocol::LCLMsgInputEvent input{};
    input.surfaceId = 7;
    input.type = static_cast<uint32_t>(protocol::LCLInputEventType::PointerMotion);
    input.source = static_cast<uint8_t>(protocol::LCLPointerSource::Touch);
    input.pointerId = 8;
    input.x = 12.0f;
    input.y = 13.0f;
    ASSERT_TRUE(sendCompositorPacket(
        compositor.peer, protocol::LCLOpcode::InputEvent, input));

    const auto events = surface.dispatch();
    size_t configureCount = 0;
    size_t inputCount = 0;
    size_t focusCount = 0;
    for (const auto& event : events) {
        if (const auto* configure = std::get_if<client::ConfigureEvent>(&event)) {
            ++configureCount;
            EXPECT_EQ(configure->configureSerial, 11u);
            EXPECT_EQ(configure->geometryGeneration, 5u);
            EXPECT_EQ(configure->bounds.width, 320.0f);
        } else if (const auto* typed = std::get_if<client::InputEvent>(&event)) {
            ++inputCount;
            EXPECT_EQ(typed->source, protocol::LCLPointerSource::Touch);
            EXPECT_EQ(typed->pointerId, 8u);
        } else if (const auto* focus = std::get_if<client::FocusEvent>(&event)) {
            ++focusCount;
            EXPECT_TRUE(focus->focused);
        }
    }
    EXPECT_EQ(configureCount, 1u);
    EXPECT_EQ(inputCount, 1u);
    EXPECT_EQ(focusCount, 1u);
}

TEST(SurfaceClientTest, NativeHandleIsQueuedBeforeBufferMetadata) {
    SeqPacketListener compositor("lcl-client-sideband-compositor");
    SeqPacketListener rasterListener("lcl-client-sideband-raster");
    ASSERT_TRUE(compositor.valid());
    ASSERT_TRUE(rasterListener.valid());
    client::SurfaceClient surface;
    ASSERT_TRUE(surface.connect(basicOptions(client::SurfaceRole::Toplevel),
                                compositor.path, rasterListener.path));
    ASSERT_TRUE(compositor.acceptPeer());
    protocol::LCLHeader ignoredHeader{};
    std::vector<uint8_t> ignoredPayload;
    ASSERT_TRUE(receiveCompositorPacket(
        compositor.peer, ignoredHeader, ignoredPayload));

    protocol::LCLMsgSurfaceProducerGrant grant{};
    grant.surfaceId = 7;
    grant.ownerPid = static_cast<int32_t>(getpid());
    grant.tokenHigh = 107;
    grant.tokenLow = 109;
    ASSERT_TRUE(sendCompositorPacket(
        compositor.peer, protocol::LCLOpcode::SurfaceProducerGrant, grant));
    protocol::LCLMsgConfigureBounds configure{};
    configure.surfaceId = 7;
    configure.configureSerial = 3;
    configure.geometryGeneration = 1;
    configure.width = configure.backingWidth = 32.0f;
    configure.height = configure.backingHeight = 24.0f;
    configure.bufferScale = 1.0f;
    ASSERT_TRUE(sendCompositorPacket(
        compositor.peer, protocol::LCLOpcode::ConfigureBounds, configure));
    (void)surface.dispatch();
    ASSERT_TRUE(rasterListener.acceptPeer());

    raster::Header rasterHeader{};
    std::vector<uint8_t> rasterPayload;
    int sidebandFd = -1;
    ASSERT_EQ(raster::receivePacket(rasterListener.peer, rasterHeader,
                                    rasterPayload, sidebandFd),
              raster::ReceiveStatus::Received);
    ASSERT_NE(raster::payloadAs<raster::RegisterNativeBufferChannel>(
                  rasterHeader, rasterPayload,
                  raster::Opcode::RegisterNativeBufferChannel),
              nullptr);
    ASSERT_GE(sidebandFd, 0);

    client::PlatformNativeFrame frame{};
    frame.bufferId = 31;
    frame.contentRevision = 1;
    frame.width = 32;
    frame.height = 24;
    frame.stride = 32 * sizeof(uint32_t);
    frame.format = 1;
    frame.damage = {0.0f, 0.0f, 32.0f, 24.0f};
    frame.writeHandle = [](int fd) {
        constexpr uint8_t marker = 0xA5;
        return send(fd, &marker, sizeof(marker), MSG_NOSIGNAL) ==
            static_cast<ssize_t>(sizeof(marker));
    };
    ASSERT_TRUE(surface.submitFrame(std::move(frame)));
    EXPECT_FALSE(frame.writeHandle);

    uint8_t marker = 0;
    ASSERT_EQ(recv(sidebandFd, &marker, sizeof(marker), MSG_DONTWAIT),
              static_cast<ssize_t>(sizeof(marker)));
    EXPECT_EQ(marker, 0xA5);
    close(sidebandFd);

    int receivedFd = -1;
    ASSERT_EQ(raster::receivePacket(rasterListener.peer, rasterHeader,
                                    rasterPayload, receivedFd),
              raster::ReceiveStatus::Received);
    EXPECT_NE(raster::payloadAs<raster::UploadExternalBuffer>(
                  rasterHeader, rasterPayload,
                  raster::Opcode::UploadExternalBuffer),
              nullptr);
    EXPECT_LT(receivedFd, 0);
    ASSERT_EQ(raster::receivePacket(rasterListener.peer, rasterHeader,
                                    rasterPayload, receivedFd),
              raster::ReceiveStatus::Received);
    EXPECT_NE(raster::payloadAs<raster::CommitBufferFrame>(
                  rasterHeader, rasterPayload,
                  raster::Opcode::CommitBufferFrame),
              nullptr);
    if (receivedFd >= 0) close(receivedFd);
}

TEST(SurfaceClientTest, NativeSubmissionUsesOneCreditAndReleaseOwnsFence) {
    SeqPacketListener compositor("lcl-client-credit-compositor");
    SeqPacketListener rasterListener("lcl-client-credit-raster");
    ASSERT_TRUE(compositor.valid());
    ASSERT_TRUE(rasterListener.valid());
    client::SurfaceClient surface;
    ASSERT_TRUE(surface.connect(basicOptions(client::SurfaceRole::Toplevel),
                                compositor.path, rasterListener.path));
    ASSERT_TRUE(compositor.acceptPeer());
    protocol::LCLHeader ignoredHeader{};
    std::vector<uint8_t> ignoredPayload;
    ASSERT_TRUE(receiveCompositorPacket(
        compositor.peer, ignoredHeader, ignoredPayload));

    protocol::LCLMsgSurfaceProducerGrant grant{};
    grant.surfaceId = 7;
    grant.ownerPid = static_cast<int32_t>(getpid());
    grant.tokenHigh = 101;
    grant.tokenLow = 103;
    ASSERT_TRUE(sendCompositorPacket(
        compositor.peer, protocol::LCLOpcode::SurfaceProducerGrant, grant));
    protocol::LCLMsgConfigureBounds configure{};
    configure.surfaceId = 7;
    configure.configureSerial = 9;
    configure.geometryGeneration = 2;
    configure.width = configure.backingWidth = 64.0f;
    configure.height = configure.backingHeight = 32.0f;
    configure.bufferScale = 1.0f;
    ASSERT_TRUE(sendCompositorPacket(
        compositor.peer, protocol::LCLOpcode::ConfigureBounds, configure));
    (void)surface.dispatch();
    ASSERT_TRUE(rasterListener.acceptPeer());

    raster::Header rasterHeader{};
    std::vector<uint8_t> rasterPayload;
    int receivedFd = -1;
    ASSERT_EQ(raster::receivePacket(rasterListener.peer, rasterHeader,
                                    rasterPayload, receivedFd),
              raster::ReceiveStatus::Received);
    EXPECT_NE(raster::payloadAs<raster::RegisterNativeBufferChannel>(
                  rasterHeader, rasterPayload,
                  raster::Opcode::RegisterNativeBufferChannel),
              nullptr);
    ASSERT_GE(receivedFd, 0);
    close(receivedFd);

    client::DmaBufFrame frame{};
    frame.bufferId = 21;
    frame.contentRevision = 1;
    frame.width = 64;
    frame.height = 32;
    frame.stride = 64 * sizeof(uint32_t);
    frame.format = lcl::platform::kDmaBufFormatArgb8888;
    frame.damage = {0.0f, 0.0f, 64.0f, 32.0f};
    frame.buffer = client::OwnedFd(open("/dev/null", O_RDONLY | O_CLOEXEC));
    ASSERT_TRUE(surface.submitFrame(std::move(frame)));
    EXPECT_FALSE(frame.buffer);
    EXPECT_FALSE(surface.hasFrameCredit());

    client::DmaBufFrame blocked{};
    blocked.bufferId = 22;
    blocked.contentRevision = 1;
    blocked.width = 64;
    blocked.height = 32;
    blocked.stride = 64 * sizeof(uint32_t);
    blocked.format = lcl::platform::kDmaBufFormatArgb8888;
    blocked.damage = {0.0f, 0.0f, 64.0f, 32.0f};
    blocked.buffer = client::OwnedFd(open("/dev/null", O_RDONLY | O_CLOEXEC));
    EXPECT_FALSE(surface.submitFrame(std::move(blocked)));

    raster::CommitBufferFrame committed{};
    for (int packet = 0; packet < 2; ++packet) {
        receivedFd = -1;
        ASSERT_EQ(raster::receivePacket(rasterListener.peer, rasterHeader,
                                        rasterPayload, receivedFd),
                  raster::ReceiveStatus::Received);
        if (const auto* value = raster::payloadAs<raster::CommitBufferFrame>(
                rasterHeader, rasterPayload,
                raster::Opcode::CommitBufferFrame)) committed = *value;
        if (receivedFd >= 0) close(receivedFd);
    }
    ASSERT_NE(committed.frameSerial, 0u);
    raster::FrameDiscarded discarded{};
    discarded.surfaceId = 7;
    discarded.configureSerial = committed.configureSerial;
    discarded.frameSerial = committed.frameSerial;
    discarded.geometryGeneration = committed.geometryGeneration;
    discarded.reason = raster::DiscardReason::Superseded;
    ASSERT_TRUE(raster::sendPacket(
        rasterListener.peer, raster::Opcode::FrameDiscarded, discarded));
    const auto events = surface.dispatch();
    EXPECT_TRUE(surface.hasFrameCredit());
    EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return std::get_if<client::FrameDiscardedEvent>(&event) != nullptr;
    }));

    raster::ExternalBufferReleased released{};
    released.bufferId = 21;
    released.contentRevision = 1;
    const int fence = open("/dev/null", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(fence, 0);
    ASSERT_TRUE(raster::sendPacket(
        rasterListener.peer, raster::Opcode::ExternalBufferReleased,
        released, fence));
    close(fence);
    const auto releaseEvents = surface.dispatch();
    bool ownsFence = false;
    for (const auto& event : releaseEvents) {
        if (const auto* value = std::get_if<client::BufferReleasedEvent>(&event)) {
            ownsFence = value->releaseFence.get() >= 0;
        }
    }
    EXPECT_TRUE(ownsFence);

    const uint64_t generation = surface.rasterConnectionGeneration();
    close(rasterListener.peer);
    rasterListener.peer = -1;
    const auto disconnected = surface.dispatch();
    EXPECT_TRUE(std::any_of(
        disconnected.begin(), disconnected.end(), [](const auto& event) {
            const auto* connection =
                std::get_if<client::RasterConnectionEvent>(&event);
            return connection && !connection->connected;
        }));
    (void)surface.dispatch();
    ASSERT_TRUE(rasterListener.acceptPeer());
    EXPECT_GT(surface.rasterConnectionGeneration(), generation);
}

} // namespace
